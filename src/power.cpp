#include "power.h"

#include <Arduino.h>
#include <driver/rtc_io.h>
#include <sys/time.h>

#include "HardwareIdentity.h"
#include "PowerPolicy.h"
#include "config.h"

namespace {

volatile bool offReq = false;
volatile uint32_t lastActivityMs = 0;

// Survives deep sleep (RTC slow memory); the RTC clock keeps running too.
struct SleepRecord {
  uint32_t magic;
  int batteryPercent;
  time_t at;
};
RTC_DATA_ATTR SleepRecord sleepRecord;
constexpr uint32_t kMagic = 0x51EE9u;
bool wokeFromSleep = false;

// What the off screen shows (charge %, -1 = plain "Off", whether the charger
// is in, whether it auto-offed), and how often the timer wakes us to refresh
// it: often while charging, rarely otherwise (the battery drains very slowly
// when off, and the checks themselves cost more than the sleep does). E-ink
// keeps its picture with no power, so between checks the screen just sits
// there.
struct OffDisplay {
  int percent;
  bool charging;
  bool idle;
};
RTC_DATA_ATTR OffDisplay offDisplay = {-1, false, false};
constexpr uint64_t kChargeCheckUsCharging = 2ULL * 60 * 1000000;
constexpr uint64_t kChargeCheckUsIdle = 60ULL * 60 * 1000000;

time_t nowSeconds() {
  timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec;
}

void buttonTask(void*) {
  uint32_t pressedAt = 0;
  bool armed = false;  // ignore the press that woke us until it's released
  for (;;) {
    const uint32_t now = millis();
    if (!armed) {
      armed = digitalRead(cfg::kPinPowerButton) == HIGH;
    } else if (digitalRead(cfg::kPinPowerButton) == LOW) {
      if (!pressedAt) {
        pressedAt = now ? now : 1;
        lastActivityMs = now;
      } else if (now - pressedAt >= 1500) {
        offReq = true;
      }
    } else {
      pressedAt = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

}  // namespace

namespace power {

void earlyInit() {
  // GPIO45 is the board's power latch (driven HIGH, as papyrix-reader does).
  gpio_hold_dis((gpio_num_t)cfg::kPinPowerLatch);
  gpio_deep_sleep_hold_dis();
  pinMode(cfg::kPinPowerLatch, OUTPUT);
  digitalWrite(cfg::kPinPowerLatch, HIGH);
  // Enable the BQ25616 charger (CE is active low). papyrix only drives this in
  // its own earlyInit() — which we don't use — and holds it low for deep sleep;
  // without this the battery charges only while the device is off.
  gpio_hold_dis((gpio_num_t)cfg::kPinChargerEnable);
  pinMode(cfg::kPinChargerEnable, OUTPUT);
  digitalWrite(cfg::kPinChargerEnable, LOW);
  rtc_gpio_deinit((gpio_num_t)cfg::kPinPowerButton);  // was an RTC wake pin while asleep
  pinMode(cfg::kPinPowerButton, INPUT_PULLUP);
  wokeFromSleep = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1 && sleepRecord.magic == kMagic;
  lastActivityMs = millis();
}

void begin() { xTaskCreate(buttonTask, "powerbtn", 2048, nullptr, 2, nullptr); }

bool offRequested() { return offReq; }

void noteActivity() { lastActivityMs = millis(); }
uint32_t idleMs() { return millis() - lastActivityMs; }

bool chargeCheckWake() { return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER; }

int shownPercent() { return offDisplay.percent; }

bool shownCharging() { return offDisplay.charging; }

bool shownIdle() { return offDisplay.idle; }

bool wakeReport(int batteryPercent, char* buf, size_t n) {
  if (!wokeFromSleep) return false;
  wokeFromSleep = false;
  sleepRecord.magic = 0;
  const long mins = (long)(nowSeconds() - sleepRecord.at) / 60;
  const int used = sleepRecord.batteryPercent - batteryPercent;
  if (used >= 0)
    snprintf(buf, n, "Was off %ldh%02ld, used %d%% battery", mins / 60, mins % 60, used);
  else
    snprintf(buf, n, "Was off %ldh%02ld (charged meanwhile)", mins / 60, mins % 60);
  return true;
}

void deepSleep(int batteryPercent, int shownPercent, bool shownCharging, bool shownIdle) {
  // A charging check keeps the original record, so the wake report still
  // covers the whole time the device was off.
  if (!chargeCheckWake()) sleepRecord = {kMagic, batteryPercent, nowSeconds()};
  offDisplay = {shownPercent, shownCharging, shownIdle};
  while (digitalRead(cfg::kPinPowerButton) == LOW) delay(10);  // else we'd wake at once
  delay(50);
  // Same rail shutdown as papyrix-reader: touch reset + power, SD card power,
  // microphone, panel power off (latch kept HIGH so the button can wake us).
  papyrix::board::prepareDeepSleepPins(papyrix::board::HardwareIdentity::instance().profile(), false);
  rtc_gpio_pullup_en((gpio_num_t)cfg::kPinPowerButton);
  rtc_gpio_pulldown_dis((gpio_num_t)cfg::kPinPowerButton);
  esp_sleep_enable_ext1_wakeup(1ULL << cfg::kPinPowerButton, ESP_EXT1_WAKEUP_ANY_LOW);
  // Periodic battery checks on top of the power-button wake: often while
  // charging (the % moves fast), rarely otherwise.
  esp_sleep_enable_timer_wakeup(shownCharging ? kChargeCheckUsCharging
                                              : kChargeCheckUsIdle);
  gpio_hold_en((gpio_num_t)cfg::kPinPowerLatch);
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_start();
}

}  // namespace power
