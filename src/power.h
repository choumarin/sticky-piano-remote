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

// Prepare the board's rails and sleep; wakes (= reboots) on the power button,
// plus periodic timer wakes that refresh the battery display (chargeCheck).
// batteryPercent is remembered to report the drain after waking. The shown*
// values describe the off screen: charge % (-1: plain "Off"), whether the
// charger is in (picks the check interval) and whether it auto-offed (the
// "10 minutes without use" line).
[[noreturn]] void deepSleep(int batteryPercent, int shownPercent = -1,
                            bool shownCharging = false, bool shownIdle = false);

// True when this boot is one of those periodic battery checks.
bool chargeCheckWake();
// What the off screen currently shows.
int shownPercent();
bool shownCharging();
bool shownIdle();

// After waking from deep sleep: e.g. "Was off 14h05, used 2% battery".
// Returns false on a normal (cold) boot.
bool wakeReport(int batteryPercent, char* buf, size_t n);

}  // namespace power
