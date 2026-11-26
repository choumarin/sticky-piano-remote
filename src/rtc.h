// Wall clock from the PCF8563 (via the vendored RtcBackend, same Wire1 bus
// as the fuel gauge — all I2C stays on the main loop thread).
#pragma once

#include <cstdint>

namespace rtc {

bool begin();   // backend init; returns the first validity check
bool valid();   // true when the wall clock is known-good (VL clear, last read ok)
bool read(int& year, int& month, int& day, int& hour, int& minute, int& second);
bool set(int year, int month, int day, int hour, int minute, int second);

uint32_t dayIndex();   // whole days since 2000-01-01 (from the last read)
uint32_t weekIndex();  // Monday-based weeks: (dayIndex + 3) / 7

}  // namespace rtc
