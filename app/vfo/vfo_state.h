// Multi-VFO channel rows of the app: what the list shows and the preferences keep ("vfoRows"). Pure data and text, no ImGui (docs/MULTI-VFO.md).
#pragma once
#include "dect2/engine/vfo.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

struct VfoRow {
    int id = 0;
    double freqMhz = 0;
    int mode = 0;            // dect2::VfoMode
    double bwKhz = 0;        // 0: the mode's default
    double sqDb = -150;      // dBFS; <= -150 open
    float vol = 1, pan = 0;
    bool mute = false, on = true;
};

// "id,freq,mode,bw,sq,vol,pan,mute,on;..." (one row per channel)
inline std::string vfoRowsToString(const std::vector<VfoRow>& rows) {
    std::string s;
    for (const VfoRow& r : rows) {
        char b[200];
        snprintf(b, sizeof b, "%d,%.6f,%d,%.3f,%.1f,%.3f,%.3f,%d,%d;", r.id, r.freqMhz, r.mode, r.bwKhz, r.sqDb, (double)r.vol, (double)r.pan, r.mute ? 1 : 0, r.on ? 1 : 0);
        s += b;
    }
    return s;
}

inline std::vector<VfoRow> vfoRowsFromString(const std::string& s) {
    std::vector<VfoRow> out;
    size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find(';', p);
        if (e == std::string::npos) e = s.size();
        VfoRow r; int m = 0, mu = 0, on = 1; double vol = 1, pan = 0;
        if (sscanf(s.substr(p, e - p).c_str(), "%d,%lf,%d,%lf,%lf,%lf,%lf,%d,%d", &r.id, &r.freqMhz, &m, &r.bwKhz, &r.sqDb, &vol, &pan, &mu, &on) == 9 &&
            r.freqMhz > 0 && m >= 0 && m < (int)dect2::VfoMode::Count && r.id > 0) {
            r.mode = m; r.vol = (float)vol; r.pan = (float)pan; r.mute = mu != 0; r.on = on != 0;
            bool dup = false; for (const VfoRow& o : out) dup |= o.id == r.id;
            if (!dup) out.push_back(r);
        }
        p = e + 1;
    }
    return out;
}

// the mode a new channel starts in for a frequency, and the main mode's engine standard (24 pagers, 25 APRS / packet)
inline dect2::VfoMode vfoDefaultMode(double mhz, int mainStd) {
    if (mainStd == 24) return dect2::VfoMode::Pager;
    if (mainStd == 25) return dect2::VfoMode::Packet;
    if (mhz < 30) return mhz < 10 ? dect2::VfoMode::Lsb : dect2::VfoMode::Usb;
    if (mhz >= 87.5 && mhz <= 108) return dect2::VfoMode::Wfm;
    if (mhz >= 108 && mhz <= 137) return dect2::VfoMode::Am;
    return dect2::VfoMode::Nfm;
}

// the half width of a row on the spectrum, MHz
inline double vfoHalfWidthMhz(const VfoRow& r) {
    const double bw = r.bwKhz > 0 ? r.bwKhz * 1e3 : dect2::vfoDefaultBandwidthHz((dect2::VfoMode)r.mode);
    return bw / 2e6;
}
