# Family practice timer (v1 design, rev 3)

Status: **design converged (3 review rounds), UI reworked after hardware
review — not yet implemented.** Rev 3 changes vs rev 2: buckets are session /
week / all-time (per-day buckets and bar graphs dropped); **new home screen**
("Piano") is the primary display, the voice grid + metronome bar live on a
**settings screen**; both screens share the same header chrome (title left,
nav box right) and the same status footer; no names — six fixed kid-friendly
personas with **game-icons.net artwork** (CC-BY); stat digits render bold via
double-draw and with the classic font's dotted-zero avoided by using the
letter 'O' for zeros.

## Goal

Several people share the piano. Each picks their persona on the remote; the
device shows how long the current session has run and records per-user
practice: this session, this week (Mon–Sun), and all time.

## Device context (constraints this design respects)

- 800×480 monochrome e-ink: full refresh 1.8 s, fast ~0.5 s; the panel
  flashes on refresh. UIs tolerate a few updates per minute at most and
  never continuous redraw.
- The piano streams everything played on it over BLE-MIDI, ~30 ms latency:
  Note On/Off (key 21–108, velocity) on the voice channel (ch1), pedals as
  CC (sustain = 64), panel voice changes (CC0/CC32/PC ch1 + layer PC ch2),
  Active Sensing ~5/s. Received on the remote as the BLE central.
- Storage: NVS key-value flash only. Battery-backed PCF8563 RTC for wall
  clock across power loss (currently not driven by the app).
- 750 mAh battery: off ~0.5 mA, runtime ~60 mA; auto-offs after 10 min
  without use (taps or any piano MIDI).

## Personas (replaces user names)

- Exactly **6 fixed personas**, compiled in, kid-friendly and fictional:
  **Prince, Princess, Dragon, Wizard, Frog, Robot**.
- Icons are **game-icons.net artwork** (1-bpp rasterized): crown, queen-crown,
  dragon-head, wizard-face, frog-prince, robot-golem. Authors Lorc /
  Delapouite, **CC-BY 3.0** — attribution to be added to the README/LICENSE
  section when adopted. Icons live as C bitmap arrays beside the UI code;
  swappable without touching logic.
- **No names, no PII anywhere** — not even build-time config. (On-device
  renaming stays deferred; unrelated to PII now, it's a UX cost.)
- Selection state = persona index 0–5 plus "nobody" (time not counted).

## 1. The rules: what "playing" means

- **Play event** = Note On with velocity ≥ 1 on the voice channel
  (`cfg::kVoiceChannel`). The channel filter is the rule, not a filter:
  layer (ch2) and metronome (ch10) messages never count.
- A vel-0 Note-On under running status is a **Note-Off**, never a play event.
- Only Note-On timestamps feed timing; Note-Offs on ch1 are otherwise
  ignored.
- **Playing state** (per main-loop pass, ~5 ms; real Note-On ticks):
  - starts at the first play event;
  - continues while the last play event is ≤ **15 s** old (page turns, brief
    pauses keep counting);
- **Session = played-time accumulator** (rev 3 feedback: a break should pause
  the timer, not reset it):
  - only actively-played seconds accrue (talking between pieces is free);
  - a break of up to **5 minutes** freezes the counter without closing the
    session — resuming play continues the same total;
  - a break longer than 5 minutes closes the session: its whole-played
    minutes are credited, and the next play starts a fresh session at zero;
- **Buckets match the screen exactly:** every loop credits the displayed
  whole-minute floor — the "screen said 3m, the week had 2m" divergence is
  structurally impossible now.
- **User switch mid-session:** the open session is closed and credited to
  the previous persona (week + all time), and the timer restarts at zero for
  the next player — the one-tap switch is the attribution moment.
- **Pedals** (e.g. sustain CC64): never play events, never touch the gap
  timer. They DO keep the auto-off timer fed — two distinct "activity"
  concepts, two distinct functions (`power::noteActivity()` vs the play
  detector). Keep them separate on purpose.
- The remote's own sends (voice picks, metronome clicks) are not known to
  echo back; verified during pre-ship testing (checklist). If an echo were
  discovered, the channel + event-type filters still exclude voice and
  metronome counts.
- **TransAcoustic built-in song playback** may stream as ordinary ch1
  Note-Ons and would credit the currently selected persona. Documented
  limitation; empirical test on the pre-ship checklist.
- Four-hands / teacher duets: all minutes go to the selected persona. Accepted.

Constants (all `constexpr` in `src/config.h`): `kPlayGapSec = 15`,
`kPlayMinVelocity = 1`.

## 2. Buckets (per persona)

| bucket | what | structure |
|---|---|---|
| **this session** | minutes in the current playing state, live on screen | RAM only |
| **this week** | Mon–Sun minutes, current calendar week | single `uint32`, cleared Monday |
| **all time** | cumulative minutes ever | `uint32` |

- No per-day buckets or bar graphs (dropped in rev 3 — not needed for the
  feature, and the home screen reads cleaner without them).
- **Week rollover:** Sunday 23:59 → Monday 00:00 clears the week counter.
  All time accumulates forever.
- **Midnight span:** a session crossing midnight credits all of it to the day
  it started on (within the week; the Sunday-night case credits Sunday).
  Simple, stated.
- **Display convention:** everything is plain minutes — never hours ("I've
  played 1000 minutes"). Session, week and all time all use the same unit.

## 3. Screens: home is the primary display, aligned chrome on both

Both screens share the **same header chrome** (title left, navigation box
right — identical geometry, `652/6/144x54`) and the **same status footer**
(BLE state left, notices center, battery right — no user UI in the footer):

```
PRACTICE (home, new, default)  SETTINGS (voice grid etc.)      BLUETOOTH
header: Practice | [Settings]  header: Settings | [Practice]  as today,
persona tiles (select, 6)      voice grid (2x8)               reached
big session time               metronome bar (-5 -1 bpm       from settings
this week / all time           +1 +5, beats, START, BT)
notice row                     [BACK: Practice box in header]
footer (same)
```

- Navigation: header box on home → settings; header box ("Piano") on
  settings → home.
- **Voice switching is one step worse than today** (home → settings → tap a
  voice). Accepted; voices are also set from the piano's own panel and sync
  back to wherever the grid is visible.
- The status footer is present on settings too (review note: user asked for
  the status line in the settings menu).

### Home layout (current implementation: see `docs/mockups/` renders)

```
[P1] [P2] [P3] [P4] [P5] [P6]   <- persona tiles, 124x128, icon + persona name
         23                     <- THIS SESSION, big numerals
  session minutes
this week   O minutes    all time   O minutes   <- stats row, minutes only
[notice row]
[status footer]
```

- Session display cadence: whole minutes; refresh when the displayed value
  changes while playing (≈ once/min during practice), never when idle. The
  flash-per-minute trade-off is explicit; 5-minute granularity is a constant
  flip away.
- **Bold/no-dot:** all text is single-draw (faux-bold double-draw was tried and
  removed — it smeared tile labels). The classic font's '0' has a center dot
  (reviewer disliked it) — stat values render every '0' as the letter 'O'
  (clean oval, visually identical to a plain zero).
- Persona tiles: tap = select (session timer restarts for that persona).

## 4. Last-session record (the mis-attribute fix)

- **Created** when a playing session ends (15 s gap expiry, auto-off, manual
  power-off) with ≥ 1 accrued minute: NVS record
  `{version, weekIndex, dayIndex, minutes, personaId}`. Sub-minute sessions
  create nothing.
- **Power-off ordering:** final flush → record written → deep sleep; a torn
  write is caught by the blob CRC.
- **Purpose:** when the next person picks the remote and selects their
  persona while an unclaimed record from the *same day* exists, offer the
  re-redirect **as a notice on the home screen**:
  "Last session 23m (Dragon) — tap here to give it to Princess", single tap
  applies, notice auto-expires after 60 s. The decision is presented exactly
  when it matters (the moment of switching), not as a permanent screen row.
- **Redirect = merge**, one main-loop transaction, per target persona:
  1. subtract the record's minutes from the leaver's week bucket
     + all time (week/all **clamped at 0**);
  2. add the same to the target persona's week + all time;
  3. clear the record (RAM + NVS) in the same step. Double-apply impossible.
- **Eligible** only while the record's day matches the current day; cleared
  at day rollover otherwise (later fixes need the v1.1 editing tool).
- **Same-persona "redirect"**: no-op that just clears the record.
- Crash window: the ordering can only over-credit (never lose); worst case is
  a one-time double-apply of one session's minutes after a crash between the
  last blob write and the record clear. Accepted; CRC bounds torn writes.

## 5. Ownership & concurrency

- **Single writer rule:** the main loop alone mutates session/week/all-time
  counters, lastSession, and performs all NVS flushes. It runs a
  1 s housekeeping tick (new, cheap) evaluating the play state machine.
- **The NimBLE receive task** does exactly two things per MIDI message:
  `power::noteActivity()` (auto-off, existing) and stores the last Note-On
  tick into a single-word volatile atomic. No NVS, no I²C, no bucket access
  from that task.
- Switch taps and notifications arrive via the main loop — same writer. The
  metronome task has no bucket access. Minute math that must not drift uses
  the RTC-backed wall clock each tick, never accumulated `millis()`.

## 6. Storage model

- One NVS key **per persona** (never one giant blob):
  `{version, weekIndex, weekMinutes (uint32), allTime (uint32), CRC}`.
- Flush cadence: at most every 5 minutes during a session, at session end,
  at power-off (ordering per §4). **Bounded worst-case loss: ~5 min per
  persona**, stated as a design decision.
- **Clock-validity gates:**
  - PCF8563 VL flag set (fresh/dead backup cell): no accrual, no flush, no
    notices; home shows a "clock not set" state where the session time
    normally sits. Recording minutes under an unknown date would poison the
    buckets.
  - **Clock moved backwards** (valid→valid, earlier date): practice data is
    discarded (fresh start), warning logged.
  - **Clockset with a session open:** the in-progress session is discarded;
    the play state machine restarts. Logged.
  - **Clockset forward mid-day/week** is treated exactly like a rollover.
- Day/week rollover while on: Sunday→Monday clears the week counter;
  all time persists; selection persists; notices re-arm.
- Battery-pull cold boot with an intact backup cell resumes cleanly.

## 7. Clock mechanics

- Wall clock = PCF8563, read at boot into the system time (also makes the
  wake report survive battery pulls).
- **Setting the clock:** minimal in-firmware serial command
  `clockset YYYY-MM-DD HH:MM:SS` parsed by the main loop (single-line format,
  no other commands), writing the PCF8563 and re-reading;
  `tools/widi_voice.py` gains a `clockset` wrapper. On-device date picker is
  deferred (a keyboard UI on e-ink is its own project).
- **Timezone policy:** naive local time; DST folds accepted and documented.

## 8. Notices & footer

- Notice mechanism unchanged (single slot, auto-expiring, partial refresh,
  priority notice > BLE state; BLE text truncated to remaining width).
- Any state where recording was gated or a mis-attribution risk exists is
  communicated by a home-screen notice (redirect offer, clock-not-set,
  new-day reminder); the footer status line stays pure status.
- No user UI in the status line, per decision.

## 9. PII policy

- The personas are fictional and compiled in; **no names, no config of
  names, no PII anywhere** in git. Future on-device renaming (if ever) must
  navigate the PII rule before shipping.

## 10. Deferred to v1.1+ (explicit, re-addable without data-model changes)

- **A way to reset counters** (user-requested): in-UI reset (hold persona tile
  2 s → confirm) or the `tools/` serial wipe (`wipepractice` exists already).
- **On-device time setting** (user-requested — the device must be
  laptop-independent): replaces the `clockset` serial command as the setup
  path. Sketch: a simple touch picker screen ( rollers for date/time, OK) in
  the settings flow; the clock driver's `rtc::set` already exists, so it's
  pure UI work. Until then `clockset` is THE setup path (needed once per
  battery condition: the PCF8563 keeps time only while VSYS has power).
- **Tap to wake** (researched — see §11): opt-in only, after the GT911
  Normal-mode / reduced-rate draw is measured on hardware.
- On-device persona renaming / customizing icon art.
- Per-day buckets and bar graphs (rev 3 dropped them; revisit only if
  requested).
- Physical Up/Down button persona cycling (invisible switching rejected as
  unsafe; revisit only with strong visible feedback).
- In-UI practice-data wipe / factory reset (a `tools/` serial wipe of the
  users namespace covers the need today).
- "Ask every day" selection mode.
- Built-in-song-playback fingerprinting (suppress credit).
- Goals / streaks / history beyond the current week.
- History beyond the current week; hiding the session view when voice
  grid is shown.

## 11. Touch wake from off (planned, opt-in — researched 2026-11)

**Goal:** tap the e-ink to wake the device ("the screen is the button").

**Research findings (GT911 datasheet + ESP32-S3 docs):**
- The GT911 has **no scan-in-sleep mode**: its Sleep mode stops touch
  detection entirely, and it wakes **only when the host drives INT high for
  2–5 ms** — a touch cannot wake it from true sleep. Cheap wake-on-tap is
  out.
- Therefore tap-to-wake requires the touch controller **kept in Normal
  scanning while off**: ~3–6 mA continuous (panel rail TP_3V3 + GT911), i.e.
  off-standby drops from weeks to ~5–6 days. A reduced scan-rate config may
  bring that toward 1–2 mA — to be measured, not promised.
- **ESP32-S3 ext1 limitation:** the wake mask accepts ONE trigger level for
  all pins. Power button = active-low; a scanning GT911 pulses INT *high* on
  touches. Likely resolution: configure the GT911 INT to **polling/level
  mode** (low while touched, high otherwise — needs an experiment on our
  touch backend), making both wake sources compatible with ext1 ANY_LOW.
  (No per-pin-level ext1 on the S3.)

**Planned shape (opt-in "Tap to wake" setting, stored in NVS):**
1. Measure the GT911 Normal-mode draw + reduced-rate draw on our unit (gauge
   timeline).
2. Touch prep at power-off: rail + EN stay HIGH, GT911 normal (or
   reduced-rate) mode, INT in level mode (low-while-touch).
3. Ext1 mask = {power button, touch INT} ANY_LOW; wake cause tells button vs
   touch (both are wanted: tap = see home).
4. Sleep-leak regression check via the established off-drain timeline.
5. Settings UI toggle; ship OFF by default until measured as acceptable.

**Cost/benefit:** ~0.1%/day-ish if the reduced-rate path lands near 1 mA —
worthwhile; ~1%/day at full scan — optional convenience only.

## 12. Pre-ship hardware verification checklist

1. **RtcBackend rides the existing Wire1 bus** shared with the BQ27220
   (schematic: MISC_I2C nets join BFG_I2C on GPIO0/1; addresses 0x51 vs
   0x55). All I²C is main-loop same-thread — verified by reading call sites.
2. **Monitor session:** does the SH-TA stream built-in song playback as ch1
   Note-Ons? Do our own sends (voice picks / metronome) echo back?
   (`tools/widi_voice.py monitor`.)
3. **Metronome rhythm regression** — the BLE connection-speed stretch was
   reverted for breaking it; confirm no timing side effects from the new 1 s
   housekeeping tick.
4. **End-to-end persona flow:** select Dragon, play 2 min, switch to Princess
   (redirect notice appears, tap applies), overnight week-rollover test,
   wake-report unaffected.
5. **Icon legibility pass** on the physical e-ink (pixel art reads very
   differently on 1-bpp than on a screen).

## 13. Implementation order (each step independently testable)

1. PCF8563 reads via the existing `BoardSupport` RtcBackend on Wire1, boot
   wall-clock, the `clockset` serial command, VL gating.
2. Play detector + session/week/all-time accounting + NVS blobs (invisible in
   UI; serial printouts verify counting before any screen work).
3. Persona assets (six 1-bpp icons) + persona selection state.
4. New home screen + prototype tile art; settings screen split; navigation.
5. Multi-bucket redirect at switch, notices, checks from §12.

## 14. Open decisions taken by default (flag for reviewer)

| decision | default | alternative |
|---|---|---|
| personas & icons | Prince/Princess/Dragon/Wizard/Frog/Robot (crown, queen-crown, dragon-head, wizard-face, frog-prince, robot-golem) | any six from game-icons.net |
| session display cadence | refresh at minute changes while playing | 5-minute granularity for fewer flashes |
| nobody selected | time not counted | Guest persona that always counts |
| pause grace | 15 s | shorter = stricter, longer = friendlier |
| faux bold / zero fix | single-draw text everywhere; '0' rendered as 'O' | custom digit font later |
| session spanning midnight | all minutes to the start day | split at midnight |
