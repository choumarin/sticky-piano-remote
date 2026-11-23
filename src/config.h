// Build-time configuration: pins, MIDI constants, the voice table.
#pragma once

#include <stdint.h>

// Optional, git-ignored per-setup overrides (copy config_local.h.example).
#if __has_include("config_local.h")
#include "config_local.h"
#endif

#ifndef WIDI_ADDRESS
#define WIDI_ADDRESS ""  // empty: connect to the first BLE-MIDI device found
#endif

#ifndef MIDI_CHANNEL
#define MIDI_CHANNEL 1  // 1-based; the YUS3 SH-TA sends and receives on ch1
#endif

namespace cfg {

// ---- MIDI -----------------------------------------------------------------
inline constexpr uint8_t kVoiceChannel = (MIDI_CHANNEL - 1) & 0x0F;  // 0-based
inline constexpr uint8_t kLayerChannel = (kVoiceChannel + 1) & 0x0F;  // dual-voice layer part
inline constexpr uint8_t kBankMsb = 108;  // all SH-TA panel voices

struct Voice {
  const char* name;
  uint8_t bankLsb;
  uint8_t program;
};

// SH-TA owner's manual, "Program Change" table (bank MSB is always 108).
inline constexpr Voice kVoices[] = {
    {"Piano", 0, 0},          {"Electric Piano 1", 0, 5}, {"Electric Piano 2", 0, 4},
    {"Electric Piano 3", 1, 4}, {"Harpsichord 1", 0, 6},  {"Harpsichord 2", 1, 6},
    {"Vibraphone", 0, 11},    {"Celesta", 0, 8},          {"Pipe Organ 1", 1, 19},
    {"Pipe Organ 2", 0, 19},  {"Pipe Organ 3", 2, 19},    {"Pipe Organ 4", 3, 19},
    {"Jazz Organ", 0, 16},    {"Strings", 0, 48},         {"Choir", 0, 52},
    {"Synth Pad", 0, 89},
};
inline constexpr int kVoiceCount = sizeof(kVoices) / sizeof(kVoices[0]);

// Metronome: XG Standard Kit 1 (bank MSB 127, LSB 0, PC 0) on ch10. Notes 21/22
// are "Metronome Click"/"Metronome Bell" — the piano's own metronome sounds.
inline constexpr uint8_t kMetroChannel = 10 - 1;
inline constexpr uint8_t kNoteClick = 21;
inline constexpr uint8_t kNoteBell = 22;
inline constexpr int kMinBpm = 5, kMaxBpm = 500, kDefaultBpm = 80;  // piano's own range

// ---- Power ------------------------------------------------------------------
// Turn off after this long without a tap, a button press or anything played on
// the piano. A running metronome or a live BLE connection doesn't count.
inline constexpr uint32_t kIdleOffMs = 10UL * 60 * 1000;

// ---- Bluetooth ------------------------------------------------------------
inline constexpr char kDeviceName[] = "Sticky Piano Remote";
// Standard BLE-MIDI 1.0 service / characteristic.
inline constexpr char kMidiServiceUuid[] = "03B80E5A-EDE8-4B33-A751-6CE34EC4C700";
inline constexpr char kMidiCharUuid[] = "7772E5DB-3868-4112-A1A9-F2669D106BF3";
// Default target until another device is picked on the Bluetooth screen. With
// no address configured, the Sticky scans and connects to the first BLE-MIDI
// device (preferring one named "WIDI Master") and remembers it. Setting the
// address helps with WIDIs that sometimes advertise an all-zero address.
inline constexpr char kDefaultTargetName[] = "WIDI Master";
inline constexpr char kDefaultTargetAddr[] = WIDI_ADDRESS;

// ---- Pins (Seeed reTerminal Sticky) --------------------------------------
inline constexpr int kPinPowerButton = 4;   // active low; deep-sleep wake source
inline constexpr int kPinPowerLatch = 45;   // keep HIGH
inline constexpr int kPinChargerEnable = 39;  // BQ25616 CE, active low: drive LOW to charge
inline constexpr int kPinChargeState = 40;    // BQ25616 STAT: LOW while charging
inline constexpr int kPinTouchEn = 42, kPinTouchRst = 41, kPinTouchInt = 21;
inline constexpr int kPinTouchSda = 3, kPinTouchScl = 2;
inline constexpr int kPinGaugeSda = 1, kPinGaugeScl = 0;  // BQ27220 fuel gauge

}  // namespace cfg
