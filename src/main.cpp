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
#include "rtc.h"
#include "metronome.h"
#include "practice.h"
#include "power.h"
#include "touch.h"
#include "ui.h"

static void powerOff(bool idle) {
  Serial.println(idle ? "power off (idle)" : "power off");
  practice::powerOffFlush();
  metronome::setRunning(false);
  ble::shutdown();
  battery::update();
  const int pct = battery::percent();
  ui::showOffScreen(idle, pct);
  touch::powerDown();
  power::deepSleep(pct, pct, battery::charging());
}

// Tiny dev console on the USB serial: clockset / wipepractice / dump.
// (An on-device time picker will replace this eventually — the device should
// be laptop-independent; see docs/PLAN.md. Until then this is THE setup path.)
static void handleCommand(const String& cmd) {
  int y = 0, mo = 0, dd = 0, hh = 0, mi = 0, ss = 0;
  if (sscanf(cmd.c_str(), "clockset %d-%d-%d %d:%d:%d", &y, &mo, &dd, &hh, &mi, &ss) == 6) {
    rtc::set(y, mo, dd, hh, mi, ss);
    Serial.printf("clock set: %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, dd, hh, mi, ss);
  } else if (cmd == "wipepractice") {
    practice::wipe();
    Serial.println("practice counters wiped");
  } else if (cmd == "dumpclock") {
    int y, mo, dd, hh, mi, ss;
    const bool ok = rtc::read(y, mo, dd, hh, mi, ss);
    Serial.printf("rtc: valid=%s, %04d-%02d-%02d %02d:%02d:%02d, dayIndex=%lu, weekIndex=%lu\n",
                  ok ? "yes" : "NO", y, mo, dd, hh, mi, ss,
                  (unsigned long)(ok ? rtc::dayIndex() : 0),
                  (unsigned long)(ok ? rtc::weekIndex() : 0));
  } else if (cmd == "dumpble") {
    Serial.printf("ble: state=%d connected=%s, Note-Ons=%lu, last=%lu ms ago\n",
                  static_cast<int>(ble::state()), ble::connected() ? "yes" : "no",
                  (unsigned long)ble::noteOns(),
                  (unsigned long)(millis() - ble::lastPlayTick()));
  } else if (cmd == "dumpstate") {
    practice::dumpState();
  } else if (cmd == "dumppractice") {
    for (int i = 0; i < 6; i++)
      Serial.printf("persona %d: week %lu min, all time %lu min\n", i,
                    (unsigned long)practice::week(i), (unsigned long)practice::allTime(i));
  }
}

static void serialConsole() {
  static String cmd;
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (cmd.length()) {
        Serial.println(">" + cmd);
        handleCommand(cmd);
        cmd = "";
      }
      continue;
    }
    cmd += c;
  }
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
  practice::begin();  // loads the clock + persona buckets (after ble: Note-On ticks)
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
  practice::pulse();  // play accounting + bucket credit (every pass)
  practice::tick();   // 1 s clock/rollover housekeeping
  serialConsole();
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
