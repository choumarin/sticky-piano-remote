# MIDI and BLE protocol notes

Everything here was verified on the actual setup (WIDI Master fw 2.6.0, YUS3 SH-TA)
on 2026-11-22 unless marked otherwise. Earlier assumptions that turned out wrong
are listed at the end; they cost real time, so don't reintroduce them.

## WIDI Master (CME)

- Exposes the **standard BLE-MIDI 1.0 service** `03B80E5A-EDE8-4B33-A751-6CE34EC4C700`,
  characteristic `7772E5DB-3868-4112-A1A9-F2669D106BF3` (read, write-without-response,
  notify), plus a CME control service (`ffd0`/`ffd1`) and device info.
- **Requires bonding:** a connection that can't pair is dropped. NimBLE: `setSecurityAuth(true, false, true)`,
  IO capability `NO_INPUT_OUTPUT`.
- **Accepts one central at a time.** While connected it stops advertising.
  Linux PipeWire (`bluez_midi`) auto-connects to it, which blocks other devices.
- Sometimes advertises with an **all-zero address**. Connecting to that fails;
  use its public address instead (set `WIDI_ADDRESS` in `src/config_local.h`).
- Doesn't auto-pair with BLE-MIDI peripherals, so the Sticky acts as the **central**.
- The official Yamaha app does **not** accept the WIDI ("unsupported device"),
  presumably because it wants a Yamaha with built-in Bluetooth.

## BLE-MIDI framing

Every write must be a framed BLE-MIDI 1.0 packet. The WIDI **silently drops**
anything else.

```
[header] [ts] status d1 d2 [ts] status d1 d2 ...
header = 0x80 | (ms >> 7 & 0x3F)      ts = 0x80 | (ms & 0x7F)
```

Example: Electric Piano 3 on ch1 is `80 80 B0 00 6C 80 B0 20 01 80 C0 04`.

What the piano sends back:
- Active Sensing (~5 packets/s) arrives as `[hdr][ts] FE`.
- It uses **running status**: `b7 a0 b0 00 6c 20 00 a0 c0 30` = CC0 108, CC32 0, PC 48.
  A receiver must handle running status, or voices that share a program number get mixed up.

## Yamaha YUS3 SH-TA control unit

- **Channel 1** for both receive and transmit. (The plain SH3 manual says
  channel 5; that doesn't apply to this unit.)
- **Voices:** CC0 = 108, CC32 = bank LSB, then Program Change. The table is in
  `src/config.h` (SH-TA owner's manual, "Program Change" list).
- **The piano's display doesn't change** when a voice arrives over MIDI, but the
  sound changes, including for the keys played by hand.
- **Panel voice changes are transmitted:** CC0/CC32/PC on ch1. Dual voices 17–19
  also send a PC on **ch2** for the layer (Piano + Strings: ch2 PC 49).
- **XG Parameter Request** `F0 43 3n 4C 08 00 {01,02,03} F7` (Multi Part 1 bank
  MSB / LSB / program) is answered, but the answer doesn't follow the panel voice.
  The firmware doesn't use it.
- **Acoustic-only mode:** with the digital sound off, MIDI notes and voices are silent.
- The **built-in metronome can't be controlled over MIDI**. Nothing is
  transmitted when it's toggled or its tempo changes, and the manual lists no
  SysEx for it.
- The tone generator is XG-compatible with drum kits. **Standard Kit 1**
  (bank MSB 127, LSB 0, PC 0) has note **21 "Metronome Click"** and note **22
  "Metronome Bell"**. Those are the metronome's own sounds, so the Sticky plays
  them on ch10.
- Channels other than 1 have their own voices (ch2 played choir, ch10 a piano-like
  default before the drum kit was selected), so the metronome on ch10 doesn't
  disturb the voice on ch1.

## Assumptions that were wrong

| Assumption | Reality |
|---|---|
| The WIDI uses a proprietary service carrying raw MIDI | Standard BLE-MIDI; raw bytes are dropped |
| 3-byte notifications are raw MIDI | Framed Active Sensing `[hdr][ts][FE]` |
| The piano listens on ch5 (SH3 manual) | The SH-TA uses ch1 |
| Silence means the voice/channel is wrong | Check that the digital sound is on, and that only one central holds the WIDI |
