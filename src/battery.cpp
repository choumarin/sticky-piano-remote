#include "battery.h"

#include <Arduino.h>
#include <Wire.h>

#include "config.h"

// BQ27220 on its own I2C bus (Wire1), address 0x55. On this unit the gauge's
// current and capacity read ~10x too high (it reports 7500 mAh for the 750 mAh
// cell), but their ratio is right — so only charge % and the gauge's own
// time-to-empty are shown, never mA/mAh.

namespace {

constexpr uint8_t kAddr = 0x55;
constexpr uint8_t kRegVoltage = 0x08, kRegAvgCurrent = 0x14, kRegTimeToEmpty = 0x16,
                  kRegSoc = 0x2C;

bool ok = false;
int soc = 0, avgCurrent = 0, timeToEmptyMin = 0;
char lastText[48] = "";

bool read16(uint8_t reg, uint16_t& v) {
  Wire1.beginTransmission(kAddr);
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0 || Wire1.requestFrom(kAddr, (uint8_t)2) != 2) return false;
  v = Wire1.read() | (Wire1.read() << 8);
  return true;
}

}  // namespace

namespace battery {

void begin() {
  Wire1.begin(cfg::kPinGaugeSda, cfg::kPinGaugeScl, 100000);
  update();
}

bool update() {
  uint16_t s, avg, tte, mv;
  ok = read16(kRegSoc, s) && read16(kRegAvgCurrent, avg) && read16(kRegTimeToEmpty, tte) &&
       read16(kRegVoltage, mv);
  if (ok) {
    soc = s;
    avgCurrent = (int16_t)avg;
    timeToEmptyMin = tte;
  }
  char text[sizeof(lastText)];
  statusText(text, sizeof(text));
  if (strcmp(text, lastText) == 0) return false;
  strlcpy(lastText, text, sizeof(lastText));
  return true;
}

int percent() { return ok ? soc : -1; }

void statusText(char* buf, size_t n) {
  if (!ok) {
    if (n) buf[0] = 0;
  } else if (avgCurrent < 0 && timeToEmptyMin > 0 && timeToEmptyMin < 65535) {
    // round to 10 min so the e-ink line doesn't redraw every minute
    const int m = (timeToEmptyMin + 5) / 10 * 10;
    snprintf(buf, n, "Battery %d%%  ~%dh%02d left", soc, m / 60, m % 60);
  } else {
    snprintf(buf, n, "Battery %d%%%s", soc, avgCurrent > 0 ? "  charging" : "");
  }
}

}  // namespace battery
