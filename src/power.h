// Power latch, power button, idle tracking and deep sleep.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace power {

void earlyInit();     // first thing in setup(): latch on, undo deep-sleep holds
void begin();         // start watching the power button (hold 1.5 s = off)
bool offRequested();  // power button held

// Idle tracking: call on any sign of a person (tap, played note, ...).
void noteActivity();
uint32_t idleMs();

// Prepare the board's rails and sleep; wakes (= reboots) on the power button.
// batteryPercent is remembered to report the drain after waking.
[[noreturn]] void deepSleep(int batteryPercent);

// After waking from deep sleep: e.g. "Was off 14h05, used 2% battery".
// Returns false on a normal (cold) boot.
bool wakeReport(int batteryPercent, char* buf, size_t n);

}  // namespace power
