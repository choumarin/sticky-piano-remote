#include "touch.h"

#include <Arduino.h>
#include <Wire.h>

#include "config.h"

// Init sequence and coordinate mapping mirror papyrix-reader's proven
// BoardSupport TouchBackend for the Sticky (see docs/HARDWARE.md): shorter
// reset timings leave the GT911 half-alive (ACKs, but no touch reports).

namespace {

uint8_t addr = 0x5D;
bool ok = false;
QueueHandle_t taps = nullptr;

bool probe() {
  for (uint8_t a : {0x5D, 0x14}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      addr = a;
      return true;
    }
  }
  return false;
}

// Raw → screen: swapXY, flipX, flipY (Sticky profile; raw range is 0..799/0..479).
bool toScreen(uint16_t rawX, uint16_t rawY, int16_t& sx, int16_t& sy) {
  if (rawY > 799 || rawX > 479) return false;
  sx = 799 - (int16_t)rawY;
  sy = 479 - (int16_t)rawX;
  return true;
}

// Returns true if exactly one contact point is reported.
bool poll(int16_t& sx, int16_t& sy) {
  Wire.beginTransmission(addr);
  Wire.write(0x81);
  Wire.write(0x4E);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((int)addr, 1) != 1) return false;
  const uint8_t status = Wire.read();
  if (!(status & 0x80)) return false;  // no new data

  bool got = false;
  if ((status & 0x0F) == 1) {
    Wire.beginTransmission(addr);
    Wire.write(0x81);
    Wire.write(0x50);
    if (Wire.endTransmission(false) == 0 && Wire.requestFrom((int)addr, 8) == 8) {
      uint8_t b[8];
      for (auto& v : b) v = Wire.read();
      got = toScreen(b[0] | (b[1] << 8), b[2] | (b[3] << 8), sx, sy);  // coords at byte 0
    }
  }
  Wire.beginTransmission(addr);  // clear the status so the next point is reported
  Wire.write(0x81);
  Wire.write(0x4E);
  Wire.write(0x00);
  Wire.endTransmission();
  return got;
}

// Polls every 10 ms; a contact becomes one tap (released after 60 ms without
// reports; taps closer than 120 ms are ignored as bounces).
void touchTask(void*) {
  bool down = false;
  uint32_t lastSeenMs = 0, lastTapMs = 0;
  for (;;) {
    int16_t x, y;
    const uint32_t now = millis();
    if (poll(x, y)) {
      lastSeenMs = now;
      if (!down) {
        down = true;
        if (now - lastTapMs >= 120) {
          lastTapMs = now;
          touch::Tap t{x, y};
          xQueueSend(taps, &t, 0);
        }
      }
    } else if (down && now - lastSeenMs > 60) {
      down = false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

}  // namespace

namespace touch {

bool begin() {
  // prepareDeepSleepPins() holds these low while asleep; release after wake.
  for (int pin : {cfg::kPinTouchEn, cfg::kPinTouchRst, cfg::kPinTouchInt}) gpio_hold_dis((gpio_num_t)pin);
  pinMode(cfg::kPinTouchEn, OUTPUT);
  digitalWrite(cfg::kPinTouchEn, HIGH);
  delay(50);
  pinMode(cfg::kPinTouchInt, OUTPUT);
  pinMode(cfg::kPinTouchRst, OUTPUT);
  digitalWrite(cfg::kPinTouchRst, LOW);
  digitalWrite(cfg::kPinTouchInt, LOW);
  delay(10);
  digitalWrite(cfg::kPinTouchRst, HIGH);
  delay(10);
  digitalWrite(cfg::kPinTouchInt, LOW);
  delay(50);
  pinMode(cfg::kPinTouchInt, INPUT);
  delay(50);

  Wire.begin(cfg::kPinTouchSda, cfg::kPinTouchScl, 100000);
  Wire.setTimeOut(10);
  ok = probe();
  Serial.printf("touch: GT911 at 0x%02X %s\n", addr, ok ? "ok" : "NOT FOUND");

  taps = xQueueCreate(16, sizeof(Tap));
  if (ok) xTaskCreate(touchTask, "touch", 4096, nullptr, 4, nullptr);
  return ok;
}

bool nextTap(Tap& tap) { return taps && xQueueReceive(taps, &tap, 0) == pdTRUE; }

void powerDown() { digitalWrite(cfg::kPinTouchEn, LOW); }

}  // namespace touch
