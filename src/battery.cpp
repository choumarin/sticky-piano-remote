#include "battery.h"

#include <Arduino.h>
#include <Wire.h>
#include <sys/time.h>

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

// BQ25616 STAT pin: low while charging, high when complete/disabled/unplugged.
bool statCharging() { return digitalRead(cfg::kPinChargeState) == LOW; }

bool read16(uint8_t reg, uint16_t& v) {
  Wire1.beginTransmission(kAddr);
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0 || Wire1.requestFrom(kAddr, (uint8_t)2) != 2) return false;
  v = Wire1.read() | (Wire1.read() << 8);
  return true;
}

}  // namespace

namespace battery {

// Off-period drain samples (survive deep sleep): one per timer check. The
// gauge's avg current is a rolling ~60 s average, so at a 1 s check it mostly
// measures the sleep before it — and the timestamps expose any hours where
// the device was NOT asleep (no samples). Not cleared at power-off: a rolling
// window, so the data survives even if the device auto-offs before it's read.
constexpr int kMaxSamples = 24;
struct OffSample {
  uint32_t at;  // epoch seconds
  int16_t pct;
  int16_t avg;  // gauge avg current, ~10x inflated
};
RTC_DATA_ATTR OffSample samples[kMaxSamples];
RTC_DATA_ATTR uint8_t sampleCount = 0, sampleNext = 0;

void noteCheckSample() {
  // Record even failed reads: the point is to know the check ran at all.
  timeval tv;
  gettimeofday(&tv, nullptr);
  samples[sampleNext] = {(uint32_t)tv.tv_sec, (int16_t)(ok ? soc : -1),
                         (int16_t)(ok ? avgCurrent : 0)};
  sampleNext = (uint8_t)((sampleNext + 1) % kMaxSamples);
  if (sampleCount < kMaxSamples) sampleCount++;
}

void printCheckSamples() {
  if (!sampleCount) return;
  Serial.printf("off period, oldest to newest (-1%% = gauge read failed; mA ~10x):\n");
  for (int i = 0; i < sampleCount; i++) {
    const OffSample& s = samples[(sampleNext + kMaxSamples - sampleCount + i) % kMaxSamples];
    Serial.printf("  t=%lu (+%lus) %d%% %+dmA\n", (unsigned long)s.at,
                  (unsigned long)(i ? s.at - samples[(sampleNext + kMaxSamples - sampleCount + i - 1) %
                                                                     kMaxSamples]
                                                       .at
                                    : 0),
                  s.pct, s.avg);
  }
  const OffSample& first = samples[(sampleNext + kMaxSamples - sampleCount) % kMaxSamples];
  const OffSample& last = samples[(sampleNext + kMaxSamples - 1) % kMaxSamples];
  Serial.printf("  span %lus, %d%% -> %d%%\n", (unsigned long)(last.at - first.at), first.pct,
                last.pct);
}

// Runtime (device on): gauge rolling current with a conn-state tag; the
// gauge's ~60 s window smooths the sample nicely.
constexpr int kMaxRuntime = 96;
struct RuntimeSample {
  uint32_t at;
  int16_t avg;
  int8_t tag;
};
RTC_DATA_ATTR RuntimeSample runtime[kMaxRuntime];
RTC_DATA_ATTR uint8_t runtimeCount = 0, runtimeNext = 0;

void noteRuntimeSample(int tag) {
  if (!ok) return;
  timeval tv;
  gettimeofday(&tv, nullptr);
  runtime[runtimeNext] = {(uint32_t)tv.tv_sec, (int16_t)avgCurrent, (int8_t)tag};
  runtimeNext = (uint8_t)((runtimeNext + 1) % kMaxRuntime);
  if (runtimeCount < kMaxRuntime) runtimeCount++;
}

void printRuntimeSamples() {
  if (!runtimeCount) return;
  Serial.printf("runtime gauge current (mA ~10x; tag 0=idle 1=active 2=no BLE):\n");
  for (int i = 0; i < runtimeCount; i++) {
    const RuntimeSample& s = runtime[(runtimeNext + kMaxRuntime - runtimeCount + i) % kMaxRuntime];
    Serial.printf("  t=%lu tag=%d %+dmA\n", (unsigned long)s.at, s.tag, s.avg);
  }
  const RuntimeSample& first = runtime[(runtimeNext + kMaxRuntime - runtimeCount) % kMaxRuntime];
  const RuntimeSample& last = runtime[(runtimeNext + kMaxRuntime - 1) % kMaxRuntime];
  Serial.printf("  span %lus\n", (unsigned long)(last.at - first.at));
}

void begin() {
  Wire1.begin(cfg::kPinGaugeSda, cfg::kPinGaugeScl, 100000);
  pinMode(cfg::kPinChargeState, INPUT);  // external 1.2k pull-up on STAT
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
  // Edge-triggered log: shows STAT vs the gauge's current sign while charging.
  static int lastStat = -1;
  const int stat = digitalRead(cfg::kPinChargeState);
  if (stat != lastStat) {
    lastStat = stat;
    Serial.printf("charge: STAT=%d gauge=%+dmA %d%%\n", stat, ok ? avgCurrent : 0, soc);
  }
  char text[sizeof(lastText)];
  statusText(text, sizeof(text));
  if (strcmp(text, lastText) == 0) return false;
  strlcpy(lastText, text, sizeof(lastText));
  return true;
}

int percent() { return ok ? soc : -1; }

bool charging() { return statCharging(); }

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
