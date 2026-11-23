// Bluetooth LE MIDI: connection manager (Sticky = central) + MIDI send/receive.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ble {

enum class State : uint8_t { Off, Idle, Connecting, Connected, Scanning };

struct Device {
  char addr[18];
  uint8_t addrType;
  char name[24];
  int rssi;
  bool midi;  // advertises the BLE-MIDI service
};
inline constexpr int kMaxDevices = 5;

// Loads saved settings (NVS), initialises NimBLE and starts the BLE task, which
// does all scanning/connecting so callers never block.
void begin();

State state();
bool connected();
bool enabled();
void setEnabled(bool on);  // persisted
bool scanDone();           // at least one scan completed
int deviceCount();
const Device& device(int i);
bool isTarget(const Device& d);
void statusText(char* buf, size_t n);

void requestScan();
void requestConnect(int deviceIndex);  // also becomes the saved target
void requestDisconnect();              // stays disconnected until a device is picked

bool takeChanged();  // true once after any state change (for UI redraws)
void shutdown();     // disconnect cleanly before deep sleep (not persisted)

}  // namespace ble

namespace midi {

// Sends complete MIDI messages (no running status) as one BLE-MIDI packet.
void send(const uint8_t* bytes, size_t n);
void sendBankProgram(uint8_t channel, uint8_t msb, uint8_t lsb, uint8_t program);

// Voice sync, fed by what the piano transmits when its panel voice changes.
int takeSyncedVoice();              // index into cfg::kVoices, or -1 if nothing new
bool takeLayerUpdate(int& layer);   // dual-voice layer program (-1 = single) once decided

}  // namespace midi
