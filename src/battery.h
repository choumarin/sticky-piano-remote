// Battery readout from the TI BQ27220 fuel gauge.
#pragma once

#include <stddef.h>

namespace battery {

void begin();
bool update();  // re-read the gauge; true if the status text changed
int percent();  // -1 if unknown
bool charging();  // charger STAT pin says a charge is in progress (the gauge
                  // isn't consulted: its average current lags an unplug)

// Drain forensics for the off period: each timer check adds a sample (time +
// charge %), printed as a timeline on every boot — how we tell a real
// standby drain from a gauge artifact.
void noteCheckSample();
void printCheckSamples();

void statusText(char* buf, size_t n);  // e.g. "Battery 66%  ~10h40 left" / "... charging"

}  // namespace battery
