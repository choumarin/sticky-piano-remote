// Practice accounting: who is playing now, and how many minutes.
// Buckets per persona: this week (Mon-Sun) and all time; the live session is
// RAM-only. One writer: the main loop (pulse each pass, clock reads on a 1 s
// tick); the BLE task only reports the last Note-On tick. See docs/PLAN.md.
#pragma once

#include <cstdint>

namespace practice {

void begin();                     // clock init + load persona buckets
void pulse();                     // play accounting + credit (every main-loop pass)
void tick();                    // 1 s clock and rollover housekeeping
void setPersona(int index);       // end the current session, restart fresh
int selectedPersona();            // -1 = nobody

uint32_t sessionMinutes();        // live display value (this session)
uint32_t week(int persona);       // this week's minutes
uint32_t allTime(int persona);    // all-time minutes
bool clockValid();                // false: nothing accrues, chip says "clock not set"

void powerOffFlush();             // credit + store (called before deep sleep)
void wipe();                      // reset all practice counters (serial command)
void dumpState();                 // internal flags for diagnostics (serial)

}  // namespace practice
