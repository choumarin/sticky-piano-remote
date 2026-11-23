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
  ui::showOffScreen(idle);
  touch::powerDown();
  power::deepSleep(battery::percent());
}

void setup() {
  power::earlyInit();
  Serial.begin(115200);
  Serial.println("\nSticky Piano Remote");
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
  ui::tick();
}
