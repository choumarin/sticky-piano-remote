# reTerminal Sticky hardware notes

Board: Seeed reTerminal Sticky. ESP32-S3 (rev 0.2), 32 MB flash, 8 MB PSRAM,
3.97" 800×480 monochrome e-ink, GT911 touch, BQ27220 fuel gauge, 750 mAh battery.
USB is a CH343 UART (shows up as `/dev/ttyACM0` on Linux).

## Pins

| Function | GPIO |
|---|---|
| Display SPI SCLK / MOSI / CS / DC / RST / BUSY / panel enable | 13 / 14 / 15 / 16 / 17 / 18 / 47 |
| Touch I²C SDA / SCL, INT, RST, power enable | 3 / 2, 21, 41, 42 |
| Fuel gauge I²C SDA / SCL (BQ27220 @ 0x55) | 1 / 0 |
| Power button (active low, deep-sleep wake) | 4 |
| Up / Down buttons (unused) | 5 / 6 |
| Power latch (drive HIGH) | 45 |

## Charger (BQ25616) pins — from the official schematic, rev 01 (2026-06)

- USB VBUS feeds only the charger and the CH343 UART — **no VBUS GPIO**. The
  charger's power-good output (VSYS_PG) is a test point (TP5) only, not routed
  to the ESP32. Hence "no USB detection pin": plugging in can't wake the board.
- But the charger's **STAT output is on GPIO40** (`CHARGE_STATE`): open-drain,
  pulled up 1.2k — LOW while charging, HIGH when complete or disabled. Not an
  RTC GPIO, so it can't wake from deep sleep, but the battery checks could read
  it instead of the gauge. Verified on hardware: STAT=0 while charging at 94%.
- The charger enable (`EN_BAT_CHGn`, active low) is on GPIO39 — the same pin
  papyrix's profile calls `chargeEnablePin`. papyrix only drives it during
  deep-sleep prep, so `power::earlyInit()` now drives it LOW at every boot;
  before that, the battery charged only while the device was off.

## Display (SSD1677)

- The panel controller is an **SSD1677**. papyrix-reader's probe can also report
  an "SSD2677"; this unit is the SSD1677. GxEPD2 does not support this panel.
- Before `display.begin()`, call
  `papyrix::board::selectPanel(papyrix::board::HardwareIdentity::instance())`;
  otherwise `begin()` returns `UnsupportedPanel`.
- Framebuffer: 1 bpp, 100 bytes per row; bit `0x80 >> (x % 8)` of `fb[y*100 + x/8]`.
  **0 = black.** Get the pointer *after* `begin()`.
- `displayWindow()` is not actually windowed on this target (it falls back to a
  full-frame fast refresh).
- Refresh times:

  | Refresh | Time |
  |---|---|
  | FULL | 1.8 s |
  | FAST, stock | 695 ms |
  | FAST, with our tweak | ~515 ms |

  The panel's built-in quick waveform takes ~470 ms at every forced temperature
  (swept 0–100 °C), so the remaining gain would need a custom LUT (not done).

## Touch (GT911)

The init sequence must match papyrix's `TouchBackend::enableTouch()`:

1. EN high, then wait 50 ms.
2. RST low and INT low, then wait 10 ms.
3. RST high, then wait 10 ms.
4. INT low for 50 ms.
5. INT as input, then wait 50 ms.
6. Probe 0x5D (fallback 0x14).

Shorter timings leave the chip ACKing I²C without reporting touches. I²C runs at
100 kHz. Point data starts at byte 0. Mapping: swap X/Y, then flip both
(`x = 799 − rawY`, `y = 479 − rawX`).

## Power

- Measured with the gauge timeline: about **60 mA** awake, connected and idle
  (rock steady across 20 minutes; the trace also shows metronome running
  adds ~3 mA, and the gauge's charging readings match the plug state).
  That's the ESP32-S3 staying awake: panel drive, touch, Bluetooth toggle
  and an 80 MHz CPU each moved this by less than the measurement noise in
  bench tests, so ~12.5 h runtime on the 750 mAh cell is the current
  ceiling. The next lever would be FreeRTOS light sleep between events,
  which the prebuilt Arduino core doesn't support well.
  - Turning off the panel drive, touch, Bluetooth or dropping the CPU to 80 MHz
    each changed this by less than the measurement noise.
  - Bluetooth connection-interval stretching while idle was also tried and
    reverted: the WIDI wouldn't take (or took seconds to honor) a fast
    update when the metronome started, and clicks quantized to the slow
    connection events — ±0.5 s jitter. See the TODO in `src/ble_midi.cpp`.
  - The remaining draw is the ESP32-S3 staying awake.
- The **BQ27220 reports current and capacity ~10× too high** (7500 mAh for the
  750 mAh cell). The ratio is right, so charge % and time-to-empty are usable,
  but mA/mAh are not shown in the UI.
- Deep sleep (power button, or auto-off after 10 min idle):
  - Runs papyrix's `prepareDeepSleepPins()`, which switches off touch, SD, the
    microphone and panel power.
  - Holds GPIO45 high and wakes on GPIO4 low (ext1).
  - Battery % and time are kept in RTC memory, and the status line reports the
    drain after wake ("Was off 14h05, used 2% battery").
  - Sleep draw, measured with the gauge-sample timeline (see below): about
  **0.4–0.5 mA** after the reset-pin fix below — ESP32 RTC domain,
  buck-boost, fuel gauge and sensors — under 2% of the battery per day.
  Before the fix it was ~14 mA (36% overnight).
- While off, a timer also wakes it for **battery checks** (2 min while
  charging, 1 h otherwise): read the gauge, redraw the off screen if the
  charge % changed, sleep again. Each check is a reboot with no BLE or
  touch; the e-ink keeps its picture between checks. There is no USB-detect
  pin, so plugging the charger in is only noticed at the next check.
- **Back-feed bug (found by pad bisection):** papyrix's sleep prep holds the
  display reset line (GPIO17) **HIGH** while cutting the panel rail. The
  3.3 V back-feeds the unpowered SSD1677 through its input protection and
  burns ~13.7 mA all night. Our `power::deepSleep()` overrides it to LOW
  (the chip is unpowered; low reset is its natural idle). Seeed's own
  firmware avoids the same trap by keeping `EPD_EN` on while the controller
  sleeps.
- Drain forensics: every battery check samples the gauge into an RTC ring;
  each boot prints the off-period timeline (time, charge %, avg current) on
  serial, plus reboot-during-sleep (`glitchBoots`) counting. The check
  cadence is configurable via `BATTERY_CHECK_IDLE_SEC` /
  `BATTERY_CHECK_CHARGING_SEC` in `src/config.h`.
- Waking means a reboot and a BLE reconnect, ~4 s in total.
- Keeping the connection while lightly sleeping would need ESP-IDF power
  management (Arduino's prebuilt core doesn't support it well).

## Changes to the vendored papyrix-reader libraries

| File | Change |
|---|---|
| `lib/BoardSupport/src/BoardProfiles.cpp` | Sticky display SPI clock 10 → 20 MHz |
| `lib/EInkDisplay/src/Display.cpp` | Sticky FAST refresh: when `stickyFastTemp` is set, write a fixed temperature and keep the panel's clock/analog on between refreshes (`stickyKeepPowered`), unless `turnOffScreen` is requested |
| `lib/SDPowerControl/` | Extracted from papyrix-reader's SDCardManager (needed by BoardSupport) |

The libraries were copied from github.com/bigbag/papyrix-reader (MIT; see the
`FREEINK_LICENSE` files).
