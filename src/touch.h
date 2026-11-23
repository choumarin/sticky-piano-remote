// GT911 capacitive touch: init, background polling, tap queue.
#pragma once

#include <stdint.h>

namespace touch {

struct Tap {
  int16_t x, y;  // screen coordinates (800x480 landscape)
};

bool begin();             // power up + init the GT911, start the polling task
bool nextTap(Tap& tap);   // non-blocking; taps made during e-ink refreshes are queued
void powerDown();         // before deep sleep

}  // namespace touch
