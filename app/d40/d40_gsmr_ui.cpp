// GSM-R: the channel numbers of a cell's SI1 / SI2 lists as downlink and uplink frequencies. docs/WIRING-4.0.md
#include "d40_ui.h"
#include "dect2/d40/radio_e40.h"
#include <cstdio>

std::string d40GsmrFrequencies(const std::vector<int>& arfcns) {
    std::string s;
    for (size_t i = 0; i < arfcns.size() && i < 32; i++) {
        double d = 0, u = 0;
        char b[64];
        if (!dect2::d40::gsmrArfcnToHz(arfcns[i], &d, &u)) snprintf(b, sizeof b, "%s%d: ?", s.empty() ? "" : ", ", arfcns[i]);
        else snprintf(b, sizeof b, "%s%d: %.1f / %.1f%s", s.empty() ? "" : ", ", arfcns[i], d / 1e6, u / 1e6, dect2::d40::isGsmrBand(arfcns[i]) ? " (R)" : "");
        s += b;
    }
    if (arfcns.size() > 32) s += ", ...";
    return s;
}
