#include "metronome.h"

#include <Arduino.h>
#include <Preferences.h>

#include "ble_midi.h"
#include "config.h"

namespace {

constexpr uint8_t kBeatOptions[] = {0, 4, 3, 2, 6};

volatile bool on = false;
volatile int tempo = cfg::kDefaultBpm;
volatile int beatIdx = 0;

// Own task with vTaskDelayUntil: no drift, and e-ink refreshes in loop() can't
// delay a beat.
void metronomeTask(void*) {
  bool wasRunning = false;
  uint32_t beat = 0;
  TickType_t next = xTaskGetTickCount();
  for (;;) {
    if (!on || !ble::connected()) {
      wasRunning = false;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (!wasRunning) {  // (re)start: select the drum kit; beat 1 right away
      wasRunning = true;
      beat = 0;
      midi::sendBankProgram(cfg::kMetroChannel, 127, 0, 0);
      vTaskDelay(pdMS_TO_TICKS(30));
      next = xTaskGetTickCount();
    }
    const uint8_t beats = kBeatOptions[beatIdx];
    const uint8_t note = (beats != 0 && beat % beats == 0) ? cfg::kNoteBell : cfg::kNoteClick;
    const uint8_t m[] = {(uint8_t)(0x90 | cfg::kMetroChannel), note, 100,
                         (uint8_t)(0x80 | cfg::kMetroChannel), note, 0};
    midi::send(m, sizeof(m));
    beat++;
    xTaskDelayUntil(&next, pdMS_TO_TICKS(60000 / tempo));
  }
}

}  // namespace

namespace metronome {

void begin() {
  // Remember tempo and beats-per-bar across power off/on (running state does
  // not survive — a fresh boot starts stopped).
  Preferences prefs;
  prefs.begin("sticky", true);
  tempo = constrain((int)prefs.getUShort("bpm", cfg::kDefaultBpm), cfg::kMinBpm, cfg::kMaxBpm);
  const int beats = prefs.getUChar(
      "beats", kBeatOptions[beatIdx]);  // click or one of 4/3/2/6
  for (int i = 0; i < (int)sizeof(kBeatOptions); i++)
    if (kBeatOptions[i] == beats) beatIdx = i;
  prefs.end();
  xTaskCreate(metronomeTask, "metronome", 4096, nullptr, 5, nullptr);
}

bool running() { return on; }
void setRunning(bool r) { on = r; }
int bpm() { return tempo; }
void setBpm(int b) {
  const int v = constrain(b, cfg::kMinBpm, cfg::kMaxBpm);
  if (v == tempo) return;
  tempo = v;
  Preferences prefs;
  prefs.begin("sticky", false);
  prefs.putUShort("bpm", (uint16_t)v);
  prefs.end();
}
uint8_t beatsPerBar() { return kBeatOptions[beatIdx]; }
void cycleBeatsPerBar() {
  beatIdx = (beatIdx + 1) % sizeof(kBeatOptions);
  Preferences prefs;
  prefs.begin("sticky", false);
  prefs.putUChar("beats", kBeatOptions[beatIdx]);
  prefs.end();
}

}  // namespace metronome
