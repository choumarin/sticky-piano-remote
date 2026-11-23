#include "ui.h"

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <Display.h>

#include "BoardSelector.h"
#include "HardwareIdentity.h"
#include "battery.h"
#include "ble_midi.h"
#include "config.h"
#include "metronome.h"

// Driver tweak in lib/EInkDisplay (see docs/HARDWARE.md): keep the panel's
// drive voltages on between quick refreshes (695 -> ~515 ms per refresh).
namespace papyrix::hal {
extern uint8_t stickyFastTemp;
extern bool stickyKeepPowered;
}  // namespace papyrix::hal

namespace {

using Panel = papyrix::hal::Display;
Panel display;
uint8_t* fb = nullptr;

// 1 bpp framebuffer, 100 bytes per row, 800x480 landscape. Bit = 0 is black.
class FrameGfx : public Adafruit_GFX {
 public:
  FrameGfx() : Adafruit_GFX(800, 480) {}
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || y < 0 || x >= 800 || y >= 480 || !fb) return;
    const uint8_t mask = 0x80 >> (x % 8);
    uint8_t& b = fb[y * 100 + x / 8];
    b = color ? (b | mask) : (b & ~mask);
  }
};
FrameGfx gfx;
constexpr uint16_t kBlack = 0, kWhite = 1;

// Handlers only change state and the framebuffer; tick() does one panel refresh
// for everything that happened meanwhile.
bool needRefresh = false;
bool needDeepRefresh = false;  // HALF_REFRESH: used when switching full screens
void requestRefresh(bool deep = false) {
  needRefresh = true;
  needDeepRefresh |= deep;
}

enum class Screen { Main, Keypad, Bluetooth };
Screen screen = Screen::Main;

char notice[64] = "";
uint32_t noticeUntilMs = 0;

int selectedVoice = 0;
int activeLayer = -1;  // dual voice set on the piano: layer program on ch2 (-1 = none)

// ---- Drawing helpers ----------------------------------------------------------

void drawBox(int16_t x, int16_t y, int16_t w, int16_t h, const char* label, bool filled,
             uint8_t size = 3) {
  gfx.fillRoundRect(x, y, w, h, 8, filled ? kBlack : kWhite);
  gfx.drawRoundRect(x, y, w, h, 8, kBlack);
  gfx.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 7, kBlack);
  while (size > 1 && (int16_t)strlen(label) * 6 * size > w - 10) size--;
  gfx.setTextColor(filled ? kWhite : kBlack);
  gfx.setTextSize(size);
  gfx.setCursor(x + (w - (int16_t)strlen(label) * 6 * size) / 2, y + (h - 7 * size) / 2);
  gfx.print(label);
}

void drawText(int16_t x, int16_t y, uint8_t size, const char* text) {
  gfx.setTextColor(kBlack);
  gfx.setTextSize(size);
  gfx.setCursor(x, y);
  gfx.print(text);
}

// ---- Main screen: 2x8 voice grid + metronome bar ------------------------------

constexpr int16_t kCellW = 400, kCellH = 50, kBarY = 404;

struct Btn {
  int16_t x, w;
};
constexpr int16_t kBtnY = 410, kBtnH = 52;
constexpr Btn kMinus5{8, 80}, kMinus1{96, 72}, kBpm{176, 152}, kPlus1{336, 72}, kPlus5{416, 80},
    kBeats{504, 100}, kStart{612, 120}, kBt{740, 52};
bool hit(const Btn& b, int16_t x) { return x >= b.x && x < b.x + b.w; }

const char* layerName(int program) {
  switch (program) {
    case 48: case 49: return "Strings";
    case 88: case 89: return "Synth Pad";
    case 5: return "E.Piano 1";
    default: return "layer";
  }
}

void drawVoice(int i, bool selected) {
  const int16_t x = (i / 8) * kCellW, y = (i % 8) * kCellH;
  gfx.fillRect(x, y, kCellW, kCellH, selected ? kBlack : kWhite);
  if (!selected) gfx.drawFastHLine(x, y + kCellH - 1, kCellW, kBlack);
  if (i < 8) gfx.drawFastVLine(kCellW - 1, y, kCellH, kBlack);
  gfx.setTextColor(selected ? kWhite : kBlack);
  gfx.setTextSize(3);
  gfx.setCursor(x + 16, y + 14);
  if (selected && activeLayer >= 0) gfx.printf("%s + %s", cfg::kVoices[i].name, layerName(activeLayer));
  else gfx.print(cfg::kVoices[i].name);
}

void drawBar() {
  gfx.fillRect(0, kBarY - 4, 800, 480 - (kBarY - 4), kWhite);
  gfx.drawFastHLine(0, kBarY - 4, 800, kBlack);
  gfx.drawFastHLine(0, kBarY - 3, 800, kBlack);

  drawBox(kMinus5.x, kBtnY, kMinus5.w, kBtnH, "-5", false);
  drawBox(kMinus1.x, kBtnY, kMinus1.w, kBtnH, "-1", false);
  drawBox(kPlus1.x, kBtnY, kPlus1.w, kBtnH, "+1", false);
  drawBox(kPlus5.x, kBtnY, kPlus5.w, kBtnH, "+5", false);

  // bpm readout (tap → keypad), boxed so it reads as tappable
  char buf[48];
  gfx.drawRoundRect(kBpm.x, kBtnY, kBpm.w, kBtnH, 8, kBlack);
  snprintf(buf, sizeof(buf), "%d", metronome::bpm());
  drawText(kBpm.x + (kBpm.w - (int16_t)strlen(buf) * 24) / 2, kBtnY + 4, 4, buf);
  drawText(kBpm.x + (kBpm.w - 18) / 2, kBtnY + 42, 1, "bpm");

  const uint8_t beats = metronome::beatsPerBar();
  if (beats == 0) snprintf(buf, sizeof(buf), "click");
  else snprintf(buf, sizeof(buf), "%u/4", beats);
  drawBox(kBeats.x, kBtnY, kBeats.w, kBtnH, buf, false);
  drawBox(kStart.x, kBtnY, kStart.w, kBtnH, metronome::running() ? "STOP" : "START",
          metronome::running());
  drawBox(kBt.x, kBtnY, kBt.w, kBtnH, "BT", ble::connected());  // filled = connected

  ble::statusText(buf, sizeof(buf));
  drawText(8, 470, 1, buf);
  if (notice[0]) drawText(300, 470, 1, notice);  // free middle of the status line
  battery::statusText(buf, sizeof(buf));
  drawText(560, 470, 1, buf);
}

void drawMain() {
  gfx.fillRect(0, 0, 800, 480, kWhite);
  for (int i = 0; i < cfg::kVoiceCount; i++) drawVoice(i, i == selectedVoice);
  drawBar();
}

void selectVoice(int v) {
  const int prev = selectedVoice;
  selectedVoice = v;
  if (screen != Screen::Main) return;
  drawVoice(prev, false);
  drawVoice(v, true);
  requestRefresh();
}

// ---- Tempo keypad ---------------------------------------------------------------

constexpr int16_t kKpTop = 100, kKpRowH = 92, kKpColW = 180, kKpLeft = 20;
constexpr int16_t kKpSideX = 580, kKpSideW = 200;
constexpr const char* kKpKeys[4][3] = {{"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}, {"C", "0", "<"}};
char kpEntry[4] = "";

void drawKey(int16_t x, int16_t y, int16_t w, int16_t h, const char* label, bool filled) {
  drawBox(x + 4, y + 4, w - 8, h - 8, label, filled, 4);
}

void drawKeypadEntry() {
  gfx.fillRect(0, 0, 800, kKpTop, kWhite);
  char range[24];
  snprintf(range, sizeof(range), "Tempo (%d-%d):", cfg::kMinBpm, cfg::kMaxBpm);
  drawText(kKpLeft + 4, 34, 3, range);
  gfx.drawRoundRect(300, 14, 280, 72, 10, kBlack);
  gfx.drawRoundRect(301, 15, 278, 70, 9, kBlack);
  drawText(320, 26, 6, kpEntry[0] ? kpEntry : "_");
  drawText(600, 34, 3, "bpm");
}

void openKeypad() {
  screen = Screen::Keypad;
  kpEntry[0] = 0;
  gfx.fillRect(0, 0, 800, 480, kWhite);
  drawKeypadEntry();
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 3; c++)
      drawKey(kKpLeft + c * kKpColW, kKpTop + r * kKpRowH, kKpColW, kKpRowH, kKpKeys[r][c], false);
  drawKey(kKpSideX, kKpTop, kKpSideW, kKpRowH * 2, "CANCEL", false);
  drawKey(kKpSideX, kKpTop + kKpRowH * 2, kKpSideW, kKpRowH * 2, "OK", true);
  requestRefresh(true);
}

void backToMain() {
  screen = Screen::Main;
  drawMain();
  requestRefresh(true);
}

void keypadTap(int16_t x, int16_t y) {
  if (y < kKpTop) return;
  const int r = (y - kKpTop) / kKpRowH;
  if (r > 3) return;
  if (x >= kKpSideX) {
    if (r >= 2) {  // OK
      const int v = atoi(kpEntry);
      if (v >= cfg::kMinBpm && v <= cfg::kMaxBpm) metronome::setBpm(v);
    }
    backToMain();  // OK (valid or not) and CANCEL
    return;
  }
  if (x < kKpLeft) return;
  const int c = (x - kKpLeft) / kKpColW;
  if (c > 2) return;
  const char k = kKpKeys[r][c][0];
  const size_t len = strlen(kpEntry);
  if (k == 'C') kpEntry[0] = 0;
  else if (k == '<') { if (len) kpEntry[len - 1] = 0; }
  else if (len < 3 && !(len == 0 && k == '0')) { kpEntry[len] = k; kpEntry[len + 1] = 0; }
  drawKeypadEntry();
  requestRefresh();
}

// ---- Bluetooth screen --------------------------------------------------------------

constexpr int16_t kBtListY = 204, kBtRowH = 54;

void drawBluetooth() {
  gfx.fillRect(0, 0, 800, 480, kWhite);
  drawText(16, 20, 4, "Bluetooth");
  drawBox(632, 8, 160, 56, "BACK", false);

  char buf[64];
  ble::statusText(buf, sizeof(buf));
  drawText(16, 80, 2, buf);

  const bool on = ble::enabled();
  drawBox(16, 110, 240, 56, on ? "BT: ON" : "BT: OFF", on);
  drawBox(272, 110, 180, 56, ble::state() == ble::State::Scanning ? "..." : "SCAN", false);
  drawBox(468, 110, 260, 56, "DISCONNECT", false);

  const char* hint = !on ? "Turn Bluetooth on to connect."
                     : ble::state() == ble::State::Scanning ? "Scanning for devices (4 s)..."
                     : !ble::scanDone()                     ? "Tap SCAN to find devices."
                     : ble::deviceCount() == 0              ? "No devices found. Tap SCAN to retry."
                                                            : "Tap a device to connect:";
  drawText(16, 180, 2, hint);
  if (!on) return;

  for (int i = 0; i < ble::deviceCount(); i++) {
    const ble::Device& d = ble::device(i);
    const int16_t y = kBtListY + i * kBtRowH;
    const bool target = ble::isTarget(d);
    gfx.fillRoundRect(16, y, 768, kBtRowH - 4, 8, target ? kBlack : kWhite);
    gfx.drawRoundRect(16, y, 768, kBtRowH - 4, 8, kBlack);
    gfx.setTextColor(target ? kWhite : kBlack);
    gfx.setTextSize(3);
    gfx.setCursor(30, y + 6);
    gfx.print(d.name);
    gfx.setTextSize(1);
    gfx.setCursor(30, y + 36);
    gfx.printf("%s   %d dBm%s%s", d.addr, d.rssi, d.midi ? "   MIDI" : "",
               target ? (ble::connected() ? "   connected" : "   selected") : "");
  }
}

void openBluetooth() {
  screen = Screen::Bluetooth;
  if (ble::enabled() && !ble::scanDone()) ble::requestScan();  // first visit: scan right away
  drawBluetooth();
  requestRefresh(true);
}

void bluetoothTap(int16_t x, int16_t y) {
  if (y < 72) {
    if (x >= 632) backToMain();
    return;
  }
  if (y >= 110 && y < 166) {
    if (x >= 16 && x < 256) ble::setEnabled(!ble::enabled());
    else if (x >= 272 && x < 452) ble::requestScan();
    else if (x >= 468 && x < 728) ble::requestDisconnect();
    drawBluetooth();
    requestRefresh();
    return;
  }
  if (y >= kBtListY && ble::enabled()) {
    const int i = (y - kBtListY) / kBtRowH;
    if (i < ble::deviceCount()) ble::requestConnect(i);
  }
}

// ---- Main-screen taps -------------------------------------------------------------

void barTap(int16_t x) {
  if (hit(kBpm, x)) return openKeypad();
  if (hit(kBt, x)) return openBluetooth();
  if (hit(kMinus5, x)) metronome::setBpm(metronome::bpm() - 5);
  else if (hit(kMinus1, x)) metronome::setBpm(metronome::bpm() - 1);
  else if (hit(kPlus1, x)) metronome::setBpm(metronome::bpm() + 1);
  else if (hit(kPlus5, x)) metronome::setBpm(metronome::bpm() + 5);
  else if (hit(kBeats, x)) metronome::cycleBeatsPerBar();
  else if (hit(kStart, x)) metronome::setRunning(!metronome::running());
  else return;
  drawBar();
  requestRefresh();
}

void mainTap(int16_t x, int16_t y) {
  if (y >= kBarY) return barTap(x);
  const int v = (x / kCellW) * 8 + y / kCellH;
  if (v < 0 || v >= cfg::kVoiceCount || v == selectedVoice) return;
  const cfg::Voice& voice = cfg::kVoices[v];
  Serial.printf("select '%s'\n", voice.name);
  // Send first so the piano switches immediately; the e-ink refresh follows.
  midi::sendBankProgram(cfg::kVoiceChannel, cfg::kBankMsb, voice.bankLsb, voice.program);
  activeLayer = -1;
  selectVoice(v);
}

}  // namespace

namespace ui {

bool begin(bool baseline) {
  papyrix::board::selectPanel(papyrix::board::HardwareIdentity::instance());
  const auto init = display.begin();
  if (init != Panel::InitResult::Ok) {
    Serial.printf("display init failed (%u)\n", (unsigned)init);
    return false;
  }
  fb = display.getFrameBuffer();  // only valid after begin()
  if (baseline) {
    drawMain();
    display.displayBuffer(Panel::FULL_REFRESH);  // clean baseline
  }
  papyrix::hal::stickyFastTemp = 0x19;          // fixed 25 C enables the keep-powered path
  papyrix::hal::stickyKeepPowered = true;
  return true;
}

void handleTap(int16_t x, int16_t y) {
  switch (screen) {
    case Screen::Main: mainTap(x, y); break;
    case Screen::Keypad: keypadTap(x, y); break;
    case Screen::Bluetooth: bluetoothTap(x, y); break;
  }
}

void tick() {
  // Voice picked on the piano's own panel.
  const int v = midi::takeSyncedVoice();
  if (v >= 0 && v != selectedVoice) selectVoice(v);

  int layer;
  if (midi::takeLayerUpdate(layer) && layer != activeLayer) {
    activeLayer = layer;
    if (screen == Screen::Main) {
      drawVoice(selectedVoice, true);
      requestRefresh();
    }
  }

  // Bluetooth state and battery line.
  if (ble::takeChanged()) {
    if (screen == Screen::Bluetooth) drawBluetooth(), requestRefresh();
    else if (screen == Screen::Main) drawBar(), requestRefresh();
  }
  static uint32_t lastBatteryMs = 0;
  if (millis() - lastBatteryMs > 60000) {
    lastBatteryMs = millis();
    if (battery::update() && screen == Screen::Main) drawBar(), requestRefresh();
  }

  if (notice[0] && (int32_t)(millis() - noticeUntilMs) > 0) {
    notice[0] = 0;
    if (screen == Screen::Main) drawBar(), requestRefresh();
  }

  if (needRefresh) {
    const bool deep = needDeepRefresh;
    needRefresh = needDeepRefresh = false;
    display.displayBuffer(deep ? Panel::HALF_REFRESH : Panel::FAST_REFRESH);
    return;  // straight back to the loop: taps made during the refresh are queued
  }
  delay(5);
}

void showNotice(const char* text, uint32_t forMs) {
  strlcpy(notice, text, sizeof(notice));
  noticeUntilMs = millis() + forMs;
  if (screen == Screen::Main) drawBar(), requestRefresh();
}

void showOffScreen(bool idle, int percent) {
  gfx.fillRect(0, 0, 800, 480, kWhite);
  drawText((800 - 3 * 36) / 2, 170, 6, "Off");
  const char* hint = "Press the power button to turn on";
  drawText((800 - (int16_t)strlen(hint) * 12) / 2, 260, 2, hint);
  if (idle) {
    const char* why = "(turned off after 10 minutes without use)";
    drawText((800 - (int16_t)strlen(why) * 12) / 2, 300, 2, why);
  }
  if (percent >= 0) {
    constexpr int16_t kW = 320, kH = 28;
    const int16_t x = (800 - kW) / 2, y = idle ? 344 : 324;
    gfx.drawRoundRect(x, y, kW, kH, 8, kBlack);
    gfx.drawRoundRect(x + 1, y + 1, kW - 2, kH - 2, 7, kBlack);
    gfx.fillRect(x + 5, y + 5, (kW - 10) * percent / 100, kH - 10, kBlack);
    char label[24];
    snprintf(label, sizeof(label), "Battery %d%%", percent);
    drawText((800 - (int16_t)strlen(label) * 12) / 2, y + kH + 14, 2, label);
  }
  display.displayBuffer(Panel::FULL_REFRESH, true);
  display.deepSleep();
}

}  // namespace ui
