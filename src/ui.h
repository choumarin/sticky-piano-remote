// E-ink UI: main screen (voices + metronome bar), tempo keypad, Bluetooth screen.
#pragma once

#include <stdint.h>

namespace ui {

bool begin(bool baseline = true);  // init the panel and draw the main screen
                                   // (baseline=false: off-screen redraws only)
void handleTap(int16_t x, int16_t y);
void tick();                       // apply sync/status changes, refresh the panel if needed
// "Off" picture + battery bar + panel deep sleep (before power::deepSleep).
// percent < 0 means the gauge is unreadable: plain "Off" picture.
void showOffScreen(bool idle, int percent = -1);
void showNotice(const char* text, uint32_t forMs);  // temporary text in the status line

}  // namespace ui
