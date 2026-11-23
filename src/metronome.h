// Metronome played on the piano: XG metronome click/bell drum notes over MIDI.
#pragma once

#include <stdint.h>

namespace metronome {

void begin();  // starts the timing task

bool running();
void setRunning(bool on);
int bpm();
void setBpm(int bpm);      // clamped to cfg::kMinBpm..kMaxBpm
uint8_t beatsPerBar();     // 0 = clicks only (no bell), like the piano's default
void cycleBeatsPerBar();   // 0 -> 4 -> 3 -> 2 -> 6 -> 0

}  // namespace metronome
