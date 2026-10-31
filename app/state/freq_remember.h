// #393: is a remembered per-mode frequency plausible? The 3.3.0 satellite carry saved a frequency in 1000-2100 MHz (ADS-B's 1090) for modes whose own
// default is far outside that range (airband, VDL2, AM), and the saved value then won over the mode's default on every later switch.
#pragma once
namespace freqrem {
inline bool ok(double minMhz, double maxMhz, double def, double fr) {
    if (!(fr >= minMhz && fr <= maxMhz)) return false;
    if (fr >= 1000 && fr <= 2100 && !(def >= 900 && def <= 2200)) return false;   // a mode whose default is up there keeps whatever the user tuned
    return true;
}
}
