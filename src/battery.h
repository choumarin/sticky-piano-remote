// Battery readout from the TI BQ27220 fuel gauge.
#pragma once

#include <stddef.h>

namespace battery {

void begin();
bool update();  // re-read the gauge; true if the status text changed
int percent();  // -1 if unknown
void statusText(char* buf, size_t n);  // e.g. "Battery 66%  ~10h40 left" / "... charging"

}  // namespace battery
