#include "rtc.h"

#include <Arduino.h>

#include "RtcBackend.h"

namespace {

papyrix::board::RtcBackend backend;
bool ok_ = false;
int lastYear = 2000, lastMonth = 1, lastDay = 1, lastHour = 0, lastMinute = 0, lastSecond = 0;

// days since 1970-01-01 for a proleptic-Gregorian date (Howard Hinnant)
uint32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<uint32_t>(era * 146097L + static_cast<long>(doe) - 719468L);
}

}  // namespace

namespace rtc {

bool begin() {
  ok_ = backend.begin() && backend.available();
  return ok_;
}

bool valid() { return ok_; }

bool read(int& year, int& month, int& day, int& hour, int& minute, int& second) {
  papyrix::board::DateTime dt;
  ok_ = backend.read(dt);
  if (!ok_) return false;
  year = dt.year;
  month = dt.month;
  day = dt.day;
  hour = dt.hour;
  minute = dt.minute;
  second = dt.second;
  lastYear = year;
  lastMonth = month;
  lastDay = day;
  lastHour = hour;
  lastMinute = minute;
  lastSecond = second;
  return true;
}

bool set(int year, int month, int day, int hour, int minute, int second) {
  papyrix::board::DateTime dt;
  dt.year = static_cast<uint16_t>(year);
  dt.month = static_cast<uint8_t>(month);
  dt.day = static_cast<uint8_t>(day);
  dt.hour = static_cast<uint8_t>(hour);
  dt.minute = static_cast<uint8_t>(minute);
  dt.second = static_cast<uint8_t>(second);
  dt.weekday = 0;  // computed from the date, unused by the app
  ok_ = backend.write(dt);
  if (ok_) read(year, month, day, hour, minute, second);
  return ok_;
}

uint32_t dayIndex() {
  const uint32_t since1970 = daysFromCivil(lastYear, lastMonth, lastDay);
  return since1970 - 10957U;  // 2000-01-01 is day 10957
}

uint32_t weekIndex() { return (dayIndex() + 3) / 7; }  // Monday-start weeks

}  // namespace rtc
