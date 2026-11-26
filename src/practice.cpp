#include "practice.h"

#include <Arduino.h>
#include <Preferences.h>

#include "ble_midi.h"
#include "rtc.h"

// Practice accounting (accumulator model, see docs/PLAN.md):
//  - a session opens on the first piano Note-On and accumulates PLAYED time;
//  - a break of up to 5 minutes pauses the timer without closing it;
//  - a longer break closes the session (whole played minutes credited);
//  - persona switches close it and the next player starts at zero;
//  - buckets credit the exact value the screen shows (no display-lead).
// Storage: one CRC'd blob per persona in the "practice" NVS namespace
// (weekIndex, weekMinutes, allTime, crc). Single writer: the main loop.

namespace {

constexpr int kPersonaCount = 6;
constexpr uint32_t kActiveMs = 15000;         // actively playing window
constexpr uint32_t kBreakMs = 5UL * 60 * 1000;  // session survives a break this long
constexpr uint32_t kStoreMs = 5UL * 60 * 1000;  // mid-session store cadence

struct Bucket {
  uint32_t weekIndex = 0;
  uint32_t weekMinutes = 0;
  uint32_t allTime = 0;
};

Bucket buckets[kPersonaCount];
int persona = -1;
bool loaded_ = false;

// session state: one writer, the main loop
bool sessionOpen = false;
bool playing = false;
uint32_t playedMs = 0;     // played time in the open session (pauses excluded)
uint32_t creditedMin = 0;  // minutes of the open session already credited
uint32_t lastSessionMin = 0;  // screen value when no session is open
uint32_t lastFlush = 0;
uint32_t lastPulseMs = 0, lastTickMs = 0;
uint32_t lastDayIndex = 0, lastWeekSeen = 0;
bool clockOk = false;

uint32_t crc(const Bucket& b) {
  uint32_t c = 2166136261U;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&b.weekIndex);
  for (size_t i = 0; i < sizeof(uint32_t) * 3; i++) c = (c ^ p[i]) * 16777619U;
  return c;
}

void store(int i) {
  Bucket b = buckets[i];
  uint32_t sum = crc(b);
  Preferences prefs;
  prefs.begin("practice", false);
  char key[8];
  snprintf(key, sizeof(key), "p%d", i);
  uint8_t data[sizeof(Bucket) + 4] = {};
  memcpy(data, &b, sizeof(b));
  memcpy(data + sizeof(b), &sum, 4);
  prefs.putBytes(key, data, sizeof(data));
  prefs.end();
}

void load(int i) {
  Preferences prefs;
  prefs.begin("practice", true);
  char key[8];
  snprintf(key, sizeof(key), "p%d", i);
  uint8_t data[sizeof(Bucket) + 4] = {};
  const size_t n = prefs.getBytes(key, data, sizeof(data));
  prefs.end();
  Bucket b;
  uint32_t sum = 0;
  if (n == sizeof(data)) {
    memcpy(&b, data, sizeof(b));
    memcpy(&sum, data + sizeof(b), 4);
    if (b.weekIndex > 40000U || b.allTime > 4000000U || crc(b) != sum) b = Bucket{};
  }
  buckets[i] = b;
}

// Credit the played floor minutes to persona p (week + all time) + persist.
void credit(int p, uint32_t minutes) {
  if (minutes == 0 || p < 0 || p >= kPersonaCount) return;
  buckets[p].weekMinutes += minutes;
  buckets[p].allTime += minutes;
  store(p);
}

void closeSession() {
  const uint32_t mins = playedMs / 60000;
  if (mins > creditedMin) credit(persona, mins - creditedMin);
  lastSessionMin = mins;
  sessionOpen = false;
  playing = false;
  creditedMin = 0;
}

void wipeAll() {
  for (int i = 0; i < kPersonaCount; i++) {
    buckets[i] = Bucket{};
    store(i);
  }
  sessionOpen = false;
  playing = false;
  playedMs = 0;
  creditedMin = 0;
  lastSessionMin = 0;
}

}  // namespace

namespace practice {

void begin() {
  rtc::begin();
  lastTickMs = millis();  // first tick right away — opens the clock gate
  for (int i = 0; i < kPersonaCount; i++) load(i);
  Preferences prefs;
  prefs.begin("practice", true);
  const int sel = prefs.getUChar("sel", 0) - 1;  // 0 = nobody
  prefs.end();
  persona = (sel >= 0 && sel < kPersonaCount) ? sel : -1;
}

int selectedPersona() { return persona; }
bool clockValid() { return clockOk; }

void setPersona(int i) {
  if (persona == i || i < 0 || i >= kPersonaCount) return;
  if (sessionOpen) closeSession();  // credit what the leaver played
  persona = i;
  lastSessionMin = 0;  // the next player hasn't played yet
  sessionOpen = false;
  playedMs = 0;
  creditedMin = 0;
  Preferences prefs;
  prefs.begin("practice", false);
  prefs.putUChar("sel", static_cast<uint8_t>(i + 1));
  prefs.end();
}

uint32_t sessionMinutes() {
  return sessionOpen ? playedMs / 60000 : lastSessionMin;
}

uint32_t week(int p) { return (p >= 0 && p < kPersonaCount) ? buckets[p].weekMinutes : 0; }
uint32_t allTime(int p) { return (p >= 0 && p < kPersonaCount) ? buckets[p].allTime : 0; }

// Runs every main-loop pass (~5 ms): accumulates play time, credits the
// displayed value, closes sessions after long breaks.
void pulse() {
  const uint32_t now = millis();
  const uint32_t dt = now - lastPulseMs;
  lastPulseMs = now;
  if (dt == 0 || dt > 60000) return;  // boot edge / clock jump safety

  const uint32_t last = rtc::valid() ? ble::lastPlayTick() : 0;
  playing = persona >= 0 && rtc::valid() && last && now - last < kActiveMs;

  if (sessionOpen &&  last && now - last > kBreakMs) closeSession();  // long break

  if (!sessionOpen && playing) {
    sessionOpen = true;
    playedMs = 0;
    creditedMin = 0;
  }
  if (sessionOpen &&  playing && dt < kActiveMs) playedMs += dt;  // pause: frozen

  // buckets always match exactly what the screen shows (no display-lead)
  if (sessionOpen) {
    const uint32_t mins = playedMs / 60000;
    if (mins > creditedMin) {
      credit(persona, mins - creditedMin);
      creditedMin = mins;
    }
  }
}

// 1 s housekeeping: clock validity/rollovers, persistence.
void tick() {
  const uint32_t now = millis();
  if (now - lastTickMs < 1000) return;
  int y, mo, dd, hh, mi, ss;
  const bool wasValid = clockOk;
  clockOk = rtc::read(y, mo, dd, hh, mi, ss);
  if (!clockOk) {
    if (wasValid) Serial.println("clock invalid — practice recording gated");
    return;  // VL set / backend gone: nothing accrues
  }
  const uint32_t dayIndex = rtc::dayIndex();
  const uint32_t weekNow = rtc::weekIndex();

  if (!wasValid) {
    // clock became valid (e.g. just clockset): sessions restart fresh
    lastDayIndex = dayIndex;
    lastWeekSeen = weekNow;
    Serial.println("clock valid — practice timer started");
    return;
  }
  if (dayIndex < lastDayIndex) {
    // clock moved backwards (wrong-ahead correction): fresh start, per plan
    Serial.println("clock moved backwards — practice data discarded");
    wipeAll();
    lastDayIndex = dayIndex;
    lastWeekSeen = weekNow;
    return;
  }
  lastDayIndex = dayIndex;

  // Monday rollover: week buckets reset lazily (all time persists);
  // the open session just continues into the new week (accepted deviation).
  if (lastWeekSeen && weekNow != lastWeekSeen && weekNow > lastWeekSeen) {
    for (int i = 0; i < kPersonaCount; i++)
      if (buckets[i].weekIndex == lastWeekSeen && buckets[i].weekMinutes) {
        buckets[i].weekIndex = weekNow;
        buckets[i].weekMinutes = 0;
        store(i);
      }
  }
  lastWeekSeen = weekNow;

  // mid-session store cadence
  if (sessionOpen &&  playedMs && now - (lastFlush ? lastFlush : 0) > kStoreMs) {
    const uint32_t mins = playedMs / 60000;
    if (mins > creditedMin) {
      credit(persona, mins - creditedMin);
      creditedMin = mins;
    }
    lastFlush = now;
  }
}

void powerOffFlush() {
  if (sessionOpen) closeSession();
  if (persona >= 0) store(persona);
}

void wipe() { wipeAll(); }

void dumpState() {
  Serial.printf("practice: persona=%d, clockOk=%s, sessionOpen=%s, playing=%s, "
                "playedMs=%lu, accruedMin=%lu, lastPlayMsAgo=%lu, week=%lu, allTime=%lu\n",
                persona, clockOk ? "yes" : "no", sessionOpen ? "yes" : "no",
                playing ? "yes" : "no", (unsigned long)playedMs,
                (unsigned long)(playedMs / 60000),
                (unsigned long)(ble::lastPlayTick() ? millis() - ble::lastPlayTick() : 0),
                (unsigned long)buckets[persona > -1 ? persona : 0].weekMinutes,
                (unsigned long)buckets[persona > -1 ? persona : 0].allTime);
}

}  // namespace practice
