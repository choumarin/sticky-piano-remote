// Sticky Piano Remote — an e-ink touch remote for a Yamaha SILENT/TransAcoustic
// piano (YUS3 SH-TA), talking BLE-MIDI to a CME WIDI Master plugged into the
// piano's MIDI ports. Runs on a Seeed reTerminal Sticky (ESP32-S3).
//
// See README.md for features and docs/ for protocol and hardware notes.

#include <Arduino.h>
#include <WiFi.h>

#include "battery.h"
#include "config.h"
#include "ble_midi.h"
#include "metronome.h"
#include "power.h"
#include "touch.h"
#include "ui.h"

static void powerOff(bool idle) {
  Serial.println(idle ? "power off (idle)" : "power off");
  metronome::setRunning(false);
  ble::shutdown();
  battery::update();
  const int pct = battery::percent();
  ui::showOffScreen(idle, pct);
  touch::powerDown();
  power::deepSleep(pct, pct, battery::charging());
}

// Battery check: the timer woke us while off. Read the gauge and, if the
// charge % moved, redraw the off screen; then sleep again. No BLE, touch or
// metronome — just the gauge and, when the picture has to change, the panel.
// Whether the charger is in only changes how often we check.
static void chargeCheck() {
  // A press that lands here would otherwise be swallowed by the release wait
  // in deepSleep() — it should turn the device on.
  if (digitalRead(cfg::kPinPowerButton) == LOW) return;
  battery::begin();
  const bool gaugeOk = battery::update();
  battery::noteCheckSample();
  if (!gaugeOk) Serial.println("battery check: GAUGE READ FAILED");
  const int pct = battery::percent();
  Serial.printf("battery check: %d%%, %s\n", pct, battery::charging() ? "charging" : "idle");
  int shown = power::shownPercent();
  if (pct >= 0 && pct != shown) {
    if (ui::begin(false)) {  // no main-screen baseline; we overwrite it anyway
      ui::showOffScreen(power::shownIdle(), pct);
    }
    shown = pct;  // on failure too: a broken display must not be retried at
                  // every check (it would drain the battery while off)
  }
  power::deepSleep(pct, shown, battery::charging(), power::shownIdle());
}

void setup() {
  power::earlyInit();
  Serial.begin(115200);
  Serial.println("\nSticky Piano Remote");
  if (power::chargeCheckWake()) chargeCheck();
  WiFi.mode(WIFI_OFF);

  if (!ui::begin()) {
    while (true) delay(1000);
  }
  battery::begin();
  ble::begin();
  touch::begin();
  metronome::begin();
  power::begin();

  char report[64];
  battery::printCheckSamples();   // drain timeline (silent when empty)
  battery::printRuntimeSamples(); // runtime current vs conn state
  if (power::offGlitchBoots())
    Serial.printf("WARNING: %u glitch reboot(s) during the last off period\n",
                  power::offGlitchBoots());
  if (power::wakeReport(battery::percent(), report, sizeof(report))) {
    Serial.println(report);
    ui::showNotice(report, 60000);
  }
}

void loop() {
  touch::Tap tap;
  while (touch::nextTap(tap)) {
    power::noteActivity();
    ui::handleTap(tap.x, tap.y);
  }
  if (power::offRequested()) powerOff(false);
  if (power::idleMs() > cfg::kIdleOffMs) powerOff(true);
  // Runtime drain forensics: gauge current every 15 s, tagged with how busy
  // the BLE link is (the /10-scaled gauge avg is steady enough to A/B this).
  static uint32_t lastRuntimeMs = 0;
  if (millis() - lastRuntimeMs > 15000) {
    lastRuntimeMs = millis();
    battery::update();
    battery::noteRuntimeSample(metronome::running() ? 1
                               : ble::connected()   ? 0
                                                    : 2);
  }
  ui::tick();
}
