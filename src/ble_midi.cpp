#include "ble_midi.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#include "config.h"
#include "power.h"

namespace {

NimBLEClient* client = nullptr;
NimBLERemoteCharacteristic* midiChar = nullptr;
SemaphoreHandle_t txLock = nullptr;

volatile bool linkUp = false;
volatile bool changed = false;
volatile ble::State st = ble::State::Idle;

ble::Device devs[ble::kMaxDevices];
volatile int devCount = 0;
volatile bool scanned = false;

// Persistent settings (NVS namespace "sticky")
Preferences prefs;
volatile bool btOn = true;
char targetAddr[18] = "";
uint8_t targetType = BLE_ADDR_PUBLIC;
char targetName[24] = "";
volatile bool autoConnect = true;

// UI → BLE task requests
volatile bool reqScan = false, reqDisconnect = false, reqConnect = false;
volatile int reqConnectIdx = -1;

void setState(ble::State s) {
  if (st != s) {
    st = s;
    changed = true;
  }
}

// ---- Receive: BLE-MIDI parser + voice sync ---------------------------------
// The piano sends CC0 / CC32 / Program Change on the voice channel whenever a
// voice is picked on its panel. Dual voices (panel 17-19) additionally put the
// layer on the next channel: a ch1 PC followed within 300 ms by a ch2 PC = dual.

volatile int syncedVoice = -1;
volatile int syncedLayer = -1;
volatile uint32_t voicePcAtMs = 0;
volatile bool layerPending = false;
uint8_t rxMsb = cfg::kBankMsb, rxLsb = 0;

int findVoice(uint8_t msb, uint8_t lsb, uint8_t pc) {
  if (msb != cfg::kBankMsb) return -1;
  for (int i = 0; i < cfg::kVoiceCount; i++)
    if (cfg::kVoices[i].bankLsb == lsb && cfg::kVoices[i].program == pc) return i;
  return -1;
}

void onMidiMessage(uint8_t status, uint8_t d1, uint8_t d2) {
  power::noteActivity();  // keys, pedals, panel changes: someone is at the piano
  const uint8_t type = status & 0xF0, ch = status & 0x0F;
  if (ch == cfg::kLayerChannel && type == 0xC0) {
    if (millis() - voicePcAtMs < 300) syncedLayer = d1;
    return;
  }
  if (ch != cfg::kVoiceChannel) return;
  if (type == 0xB0 && d1 == 0x00) rxMsb = d2;
  else if (type == 0xB0 && d1 == 0x20) rxLsb = d2;
  else if (type == 0xC0) {
    voicePcAtMs = millis();
    syncedLayer = -1;
    layerPending = true;
    int v = findVoice(rxMsb, rxLsb, d1);
    Serial.printf("piano voice: MSB %u LSB %u PC %u -> %s\n", rxMsb, rxLsb, d1,
                  v >= 0 ? cfg::kVoices[v].name : "unknown");
    if (v >= 0) syncedVoice = v;
  }
}

// BLE-MIDI 1.0: [header] then messages. At a message boundary a byte with the
// high bit set is a timestamp, followed by a status byte or (running status)
// directly by data bytes; running-status messages may omit the timestamp.
void onNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  static bool inSysex = false;
  static uint8_t running = 0;
  size_t i = 1;
  while (i < len) {
    if (inSysex) {  // SysEx is ignored
      if (data[i++] == 0xF7) inSysex = false;
      continue;
    }
    if (data[i] & 0x80) {  // timestamp
      if (++i >= len) break;
    }
    const uint8_t b = data[i];
    if (b & 0x80) {
      i++;
      if (b == 0xF0) inSysex = true;
      if (b >= 0xF0) continue;  // SysEx start / system messages (e.g. Active Sensing)
      running = b;
    }
    if (!running) {
      i++;
      continue;
    }
    const uint8_t type = running & 0xF0;
    const size_t need = (type == 0xC0 || type == 0xD0) ? 1 : 2;
    if (i + need > len) break;
    onMidiMessage(running, data[i], need == 2 ? data[i + 1] : 0);
    i += need;
  }
}

// ---- Connection management (runs in bleTask) -------------------------------

class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*, int reason) override {
    linkUp = false;
    midiChar = nullptr;
    if (st == ble::State::Connected) st = ble::State::Idle;
    changed = true;
    Serial.printf("BLE disconnected (reason %d)\n", reason);
  }
};

void saveTarget() {
  prefs.putString("bt_addr", targetAddr);
  prefs.putUChar("bt_type", targetType);
  prefs.putString("bt_name", targetName);
}

void loadSettings() {
  prefs.begin("sticky", false);
  btOn = prefs.getBool("bt_on", true);
  // isKey() first: avoids NVS "NOT_FOUND" log noise before anything was saved.
  String a = prefs.isKey("bt_addr") ? prefs.getString("bt_addr") : String(cfg::kDefaultTargetAddr);
  String n = prefs.isKey("bt_name") ? prefs.getString("bt_name")
             : a.length()          ? String(cfg::kDefaultTargetName)
                                   : String("");
  strlcpy(targetAddr, a.c_str(), sizeof(targetAddr));
  strlcpy(targetName, n.c_str(), sizeof(targetName));
  targetType = prefs.getUChar("bt_type", BLE_ADDR_PUBLIC);
}

bool connectTarget() {
  if (!targetAddr[0]) return false;
  setState(ble::State::Connecting);
  Serial.printf("connecting to %s (%s)\n", targetName, targetAddr);
  if (!client->connect(NimBLEAddress(targetAddr, targetType))) {
    Serial.printf("connect failed, err=%d\n", client->getLastError());
    setState(ble::State::Idle);
    return false;
  }
  NimBLERemoteService* svc = client->getService(cfg::kMidiServiceUuid);
  NimBLERemoteCharacteristic* ch = svc ? svc->getCharacteristic(cfg::kMidiCharUuid) : nullptr;
  if (!ch || !ch->canNotify() || !ch->subscribe(true, onNotify)) {
    Serial.println("not a BLE-MIDI device");
    client->disconnect();
    autoConnect = false;  // don't keep hammering a non-MIDI device
    setState(ble::State::Idle);
    return false;
  }
  midiChar = ch;
  linkUp = true;
  setState(ble::State::Connected);
  Serial.printf("connected to %s\n", targetName);
  return true;
}

// quiet = background search for a first device: no Scanning/Idle state flips
// (each would redraw the e-ink).
void scan(bool quiet = false) {
  if (!quiet) setState(ble::State::Scanning);
  NimBLEScan* s = NimBLEDevice::getScan();
  s->setActiveScan(true);
  s->clearResults();
  NimBLEScanResults results = s->getResults(4000, false);  // blocking (this task only)

  // Keep the best kMaxDevices: MIDI devices first, then strongest signal.
  ble::Device found[ble::kMaxDevices];
  int n = 0;
  auto better = [](const ble::Device& x, const ble::Device& y) {
    return x.midi != y.midi ? x.midi : x.rssi > y.rssi;
  };
  for (const NimBLEAdvertisedDevice* adv : results) {
    const bool isWidi = adv->haveName() && adv->getName() == cfg::kDefaultTargetName;
    const bool midi = isWidi || adv->isAdvertisingService(NimBLEUUID(cfg::kMidiServiceUuid));
    if (!midi && !adv->haveName()) continue;  // skip anonymous non-MIDI devices
    NimBLEAddress a = adv->getAddress();
    if (a.isNull()) {  // the WIDI sometimes advertises an all-zero address
      if (!isWidi || !cfg::kDefaultTargetAddr[0]) continue;
      a = NimBLEAddress(cfg::kDefaultTargetAddr, BLE_ADDR_PUBLIC);
    }
    ble::Device d{};
    strlcpy(d.addr, a.toString().c_str(), sizeof(d.addr));
    d.addrType = a.getType();
    strlcpy(d.name, adv->haveName() ? adv->getName().c_str() : "(MIDI device)", sizeof(d.name));
    d.rssi = adv->getRSSI();
    d.midi = midi;
    int pos = n;
    while (pos > 0 && better(d, found[pos - 1])) pos--;
    if (pos >= ble::kMaxDevices) continue;
    for (int k = (n < ble::kMaxDevices ? n : ble::kMaxDevices - 1); k > pos; k--) found[k] = found[k - 1];
    found[pos] = d;
    if (n < ble::kMaxDevices) n++;
  }
  memcpy(devs, found, sizeof(found));
  devCount = n;
  scanned = true;
  Serial.printf("scan: %d devices listed\n", n);
  if (quiet) return;
  setState(linkUp ? ble::State::Connected : ble::State::Idle);
  changed = true;
}

void bleTask(void*) {
  uint32_t lastTryMs = 0;
  for (;;) {
    if (!btOn) {
      if (client->isConnected()) client->disconnect();
      reqScan = false;
      reqConnect = false;
      reqDisconnect = false;
      setState(ble::State::Off);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (st == ble::State::Off) {  // just switched back on
      setState(ble::State::Idle);
      autoConnect = true;
      lastTryMs = 0;
    }
    if (reqDisconnect) {
      reqDisconnect = false;
      autoConnect = false;
      if (client->isConnected()) client->disconnect();
    }
    if (reqConnect) {
      reqConnect = false;
      const int i = reqConnectIdx;
      if (i >= 0 && i < devCount) {
        strlcpy(targetAddr, devs[i].addr, sizeof(targetAddr));
        targetType = devs[i].addrType;
        strlcpy(targetName, devs[i].name, sizeof(targetName));
        saveTarget();
        autoConnect = true;
        lastTryMs = 0;
        if (client->isConnected()) {
          client->disconnect();
          vTaskDelay(pdMS_TO_TICKS(300));
        }
        changed = true;
      }
    }
    if (reqScan) {
      reqScan = false;
      scan();
    }
    if (!linkUp && autoConnect && millis() - lastTryMs > (targetAddr[0] ? 3000u : 10000u)) {
      lastTryMs = millis();
      if (targetAddr[0]) {
        connectTarget();
      } else {
        // No device chosen yet: scan and adopt the first BLE-MIDI device
        // (MIDI devices sort first; a "WIDI Master" counts as MIDI).
        scan(true);
        if (devCount > 0 && devs[0].midi) {
          strlcpy(targetAddr, devs[0].addr, sizeof(targetAddr));
          targetType = devs[0].addrType;
          strlcpy(targetName, devs[0].name, sizeof(targetName));
          saveTarget();
          Serial.printf("found %s, saved as target\n", targetName);
          connectTarget();
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

}  // namespace

// ---- Public API ---------------------------------------------------------------

namespace ble {

void begin() {
  loadSettings();
  txLock = xSemaphoreCreateMutex();
  NimBLEDevice::init(cfg::kDeviceName);
  NimBLEDevice::setSecurityAuth(true, false, true);  // the WIDI requires bonding
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  client = NimBLEDevice::createClient();
  client->setClientCallbacks(new ClientCallbacks());
  client->setConnectTimeout(4000);
  xTaskCreate(bleTask, "ble", 6144, nullptr, 3, nullptr);
}

State state() { return st; }
bool connected() { return linkUp; }
bool enabled() { return btOn; }

void setEnabled(bool on) {
  btOn = on;
  prefs.putBool("bt_on", on);
  if (on) setState(State::Idle);
  changed = true;
}

bool scanDone() { return scanned; }
int deviceCount() { return devCount; }
const Device& device(int i) { return devs[i]; }
bool isTarget(const Device& d) { return strcasecmp(d.addr, targetAddr) == 0; }

void statusText(char* buf, size_t n) {
  switch (st) {
    case State::Off: snprintf(buf, n, "Bluetooth off"); break;
    case State::Scanning: snprintf(buf, n, "Scanning..."); break;
    case State::Connecting: snprintf(buf, n, "Connecting to %s...", targetName); break;
    case State::Connected: snprintf(buf, n, "Connected: %s", targetName); break;
    default:
      if (autoConnect && !targetAddr[0]) snprintf(buf, n, "Looking for a BLE-MIDI device...");
      else if (autoConnect) snprintf(buf, n, "Not connected (retrying %s)", targetName);
      else snprintf(buf, n, "Not connected");
  }
}

void requestScan() {
  if (btOn) reqScan = true;
}

void requestConnect(int deviceIndex) {
  reqConnectIdx = deviceIndex;
  reqConnect = true;
}

void requestDisconnect() { reqDisconnect = true; }

bool takeChanged() {
  if (!changed) return false;
  changed = false;
  return true;
}

void shutdown() {
  btOn = false;  // not persisted: the task disconnects
  for (int i = 0; i < 30 && client->isConnected(); i++) delay(20);
}

}  // namespace ble

namespace midi {

void send(const uint8_t* bytes, size_t n) {
  if (!linkUp || !midiChar || n > 16) return;
  // BLE-MIDI framing: header, then a timestamp byte before every status byte.
  // (The WIDI silently drops unframed data.)
  const uint32_t ms = millis();
  const uint8_t ts = 0x80 | (ms & 0x7F);
  uint8_t pkt[1 + 16 * 2];
  size_t k = 0;
  pkt[k++] = 0x80 | ((ms >> 7) & 0x3F);
  for (size_t i = 0; i < n; i++) {
    if (bytes[i] & 0x80) pkt[k++] = ts;
    pkt[k++] = bytes[i];
  }
  xSemaphoreTake(txLock, portMAX_DELAY);
  NimBLERemoteCharacteristic* c = midiChar;
  if (c && linkUp) c->writeValue(pkt, k, false);  // write without response
  xSemaphoreGive(txLock);
}

void sendBankProgram(uint8_t channel, uint8_t msb, uint8_t lsb, uint8_t program) {
  const uint8_t m[] = {
      (uint8_t)(0xB0 | channel), 0x00, msb,  // CC 0: Bank MSB
      (uint8_t)(0xB0 | channel), 0x20, lsb,  // CC 32: Bank LSB
      (uint8_t)(0xC0 | channel), program,    // Program Change
  };
  send(m, sizeof(m));
}

int takeSyncedVoice() {
  const int v = syncedVoice;
  if (v >= 0) syncedVoice = -1;
  return v;
}

bool takeLayerUpdate(int& layer) {
  if (!layerPending || millis() - voicePcAtMs < 300) return false;
  layerPending = false;
  layer = syncedLayer;
  return true;
}

}  // namespace midi
