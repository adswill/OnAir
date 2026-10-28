// The custom capture bandwidth and sample rate of every mode (3.4; #274 started it for VDL2, TETRA and TETRAPOL).
// The default is each mode's own standard bandwidth and rate; a Custom tick under Advanced lets the user set any bandwidth (and any sample
// rate) by hand. Nothing is clamped: a width gives the analysis passband, the radio's sample rate that carries it and the radio's analogue
// filter, and the driver has the last word (the app shows its own message when it refuses). The plan is pure
// (tests/state/test_rx_width.cpp); the saved width and the control are in rx_width.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

struct RxWidthPlan { double bandwidthMhz = 0, sampleRate = 0, basebandHz = 0; };

// widthMhz <= 0: the mode's own values, passed in. Otherwise the width as typed; the rate is the width over 0.9 (the flat part of a
// radio's decimation filter), rounded up to 0.25 Msps and never below what the mode's decoder needs (minRate: a floor of the decoder, not a limit of the control).
inline RxWidthPlan rxWidthPlan(double widthMhz, double autoBwMhz, double autoRate, double autoBaseband, double minRate) {
    RxWidthPlan p{autoBwMhz, autoRate, autoBaseband};
    if (!(widthMhz > 0) || !std::isfinite(widthMhz)) return p;
    p.bandwidthMhz = widthMhz;
    p.sampleRate = std::max(minRate, std::ceil(widthMhz / 0.9 / 0.25) * 0.25 * 1e6);
    p.basebandHz = 0.9 * p.sampleRate;
    return p;
}

// "Custom" tick of a per-mode bandwidth control (3.2): the mode uses its standard fixed bandwidth unless the user ticks Custom. flagPref is
// the saved tick (-1 = never saved: a user from before the tick), savedWidth the saved custom width (0 = Auto in the old control). A user who
// had saved a width keeps it, so the tick starts on for them and nothing changes; everybody else starts on the standard bandwidth.
inline bool bwCustomTicked(int flagPref, double savedWidth) { return flagPref >= 0 ? flagPref != 0 : savedWidth > 0; }
// the width the mode receives with: the custom one when ticked, 0 (the mode's own standard value) otherwise
inline double bwEffective(bool custom, double savedWidth) { return custom && savedWidth > 0 ? savedWidth : 0; }

// What the bandwidth control writes, as a width in MHz from text typed in kHz / MHz: "250k", "1.5", "12 MHz" (a bare number is MHz). 0 when it is not a width.
inline double parseWidthMhz(const char* s) {
    if (!s) return 0;
    char* e = nullptr;
    const double v = std::strtod(s, &e);
    if (e == s || !(v > 0) || !std::isfinite(v)) return 0;
    while (*e == ' ') e++;
    if (*e == 'k' || *e == 'K') return v / 1e3;
    if (*e == 'h' || *e == 'H') return v / 1e6;   // Hz
    return v;
}

// ---- what is saved (per mode; the sample rate also per radio). Templates over the settings store so the app (plat::prefs()) and the tests (a map) share them.
inline std::string rxWidthKey(const std::string& modeId) { return "rxbw." + modeId; }
inline std::string rxRateKey(const std::string& modeId, const std::string& radioKey) { return "rate." + modeId + "." + radioKey; }
template <class P> void rxWidthSave(P& p, const std::string& modeId, bool custom, double widthMhz) {
    p.setI((rxWidthKey(modeId) + ".custom").c_str(), custom ? 1 : 0);
    p.setD(rxWidthKey(modeId).c_str(), widthMhz);
}
// the width the mode receives with: the saved custom one when ticked, else 0 (the mode's standard); nothing saved: 0, so a user who saved nothing sees no change
template <class P> double rxWidthLoad(P& p, const std::string& modeId) {
    double w = p.getD(rxWidthKey(modeId).c_str(), 0);
    if (!(w > 0) || !std::isfinite(w)) w = 0;
    return bwEffective(bwCustomTicked((int)p.getI((rxWidthKey(modeId) + ".custom").c_str(), -1), w), w);
}
