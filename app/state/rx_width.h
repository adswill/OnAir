// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

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
#include "dect2/engine/rate_choice.h"
#include "dect2/source/source.h"

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


// ---- the shared receive path (3.6): what applyBandwidth() and the tests both run, so every mode gets the custom bandwidth and rate the same way.
// `t` holds the mode's own values (bandwidth, rate, radio filter). widthMhz <= 0 leaves them untouched. Otherwise the bandwidth, the rate that
// carries it and the radio's analogue filter are replaced; minRate is the decoder's own floor (a floor of the arithmetic, never a limit of the control).
inline void rxApplyWidth(dect2::TuneSettings& t, double widthMhz, double minRate) {
    if (!(widthMhz > 0) || !std::isfinite(widthMhz)) return;
    const RxWidthPlan p = rxWidthPlan(widthMhz, t.bandwidthMhz, t.sampleRate, t.basebandFilterHz, minRate);
    if (!(t.channelBwMhz > 0)) t.channelBwMhz = t.bandwidthMhz;   // the signal keeps its own channel width: DVB-T2 / DVB-T / DTMB decode with it, the capture width is only the radio's
    t.bandwidthMhz = p.bandwidthMhz; t.sampleRate = p.sampleRate; t.basebandFilterHz = p.basebandHz;
    t.rateChosen = true;
}
// a rate the user chose for this mode (0 = the mode's own): any positive rate goes to the driver; a value outside the radio's list only warns
inline dect2::RateCheck rxApplyRate(dect2::TuneSettings& t, double chosenHz, const dect2::RateLimits& L, double minRate) {
    dect2::RateCheck c = dect2::checkManualRate(L, minRate, chosenHz);
    if (chosenHz > 0 && c.ok) { t.sampleRate = chosenHz; t.rateChosen = true; }
    return c;
}
// The one-line warning for a custom width: the rate that carries it against what this radio lists and what the decoder works with (empty: none).
// Never a refusal; the driver decides and the app shows its message if it does.
inline std::string rxWidthWarning(double rateHz, const dect2::RateLimits& L, double minRate) {
    if (!(rateHz > 0)) return "";
    return dect2::checkManualRate(L, minRate, rateHz).why;
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
