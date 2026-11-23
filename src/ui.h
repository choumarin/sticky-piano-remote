// E-ink UI: main screen (voices + metronome bar), tempo keypad, Bluetooth screen.
#pragma once

#include <stdint.h>

namespace ui {

bool begin();                      // init the panel and draw the main screen
void handleTap(int16_t x, int16_t y);
void tick();                       // apply sync/status changes, refresh the panel if needed
void showOffScreen(bool idle);     // "Off" picture + panel deep sleep (before power::deepSleep)
void showNotice(const char* text, uint32_t forMs);  // temporary text in the status line

}  // namespace ui
