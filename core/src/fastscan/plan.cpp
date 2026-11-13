// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/fastscan/plan.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace dect2 {
namespace fastscan {

const std::vector<RadioProfile>& radioProfiles() {
    static const std::vector<RadioProfile> v = [] {
        std::vector<RadioProfile> p;
        auto add = [&](const char* id, const char* name, double rate, double usable, double retune, double dwell, bool nat, double dc) {
            RadioProfile r; r.id = id; r.name = name; r.rateHz = rate; r.usableFrac = usable; r.retuneSec = retune; r.minDwellSec = dwell; r.nativeSweep = nat; r.dcFrac = dc;
            p.push_back(r);
        };
        //  id         name                        rate     usable retune  dwell   native dc
        add("rtlsdr",  "RTL-SDR (R820T/E4000)",    2.56e6,  0.75,  0.015,  0.002,  false, 0.02);
        add("airspy",  "Airspy R2 / Mini",         10e6,    0.80,  0.008,  0.001,  false, 0.0);
        add("hackrf",  "HackRF One / Pro",         20e6,    0.75,  0.010,  0.0005, true,  0.02);
        add("sdrplay", "SDRplay RSP",              8e6,     0.85,  0.010,  0.001,  false, 0.0);
        add("wide",    "BladeRF / LimeSDR / USRP", 20e6,    0.80,  0.010,  0.0005, false, 0.02);
        add("pluto",   "PlutoSDR (USB 2)",         4e6,     0.75,  0.006,  0.002,  false, 0.02);
        add("generic", "Other radio",              2.4e6,   0.70,  0.020,  0.002,  false, 0.02);
        return p;
    }();
    return v;
}

const RadioProfile& radioProfile(const std::string& id) {
    for (const auto& r : radioProfiles()) if (r.id == id) return r;
    return radioProfiles().front();
}

RadioProfile profileForBoard(const std::string& boardIn, double maxRate) {
    std::string b;
    for (char c : boardIn) b += (char)std::tolower((unsigned char)c);
    auto has = [&](const char* s) { return b.find(s) != std::string::npos; };
    std::string id = "generic";
    if (has("hackrf")) id = "hackrf";
    else if (has("rtl")) id = "rtlsdr";
    else if (has("airspyhf") || has("airspy hf")) id = "generic";
    else if (has("airspy")) id = "airspy";
    else if (has("sdrplay") || has("rsp")) id = "sdrplay";
    else if (has("pluto")) id = "pluto";
    else if (has("blade") || has("lime") || has("usrp") || has("b2")) id = "wide";
    RadioProfile r = radioProfile(id);
    if (maxRate > 0 && r.rateHz > maxRate) r.rateHz = maxRate;
    return r;
}

long FastPlan::binOf(double hz) const {
    const double f = std::floor((hz - startHz) / binHz + 0.5);
    return (f < 0 || f >= (double)totalBins) ? -1 : (long)f;
}

double ceilingMhzPerSec(const RadioProfile& r, double dwellSec) {
    const int l = (int)std::lround(std::log2(std::max(16.0, r.rateHz / 12500.0)));
    const double fftSec = (double)(1 << std::min(18, std::max(4, l))) / r.rateHz;
    const double hop = (r.nativeSweep ? r.nativeRetuneSec : r.retuneSec) + std::max(dwellSec, fftSec);
    return r.rateHz * r.usableFrac / 1e6 / hop;
}

FastPlan planFast(const FastPlanConfig& c, const RadioProfile& r) {
    FastPlan p;
    p.rateHz = r.rateHz;
    int l = (int)std::lround(std::log2(std::max(16.0, r.rateHz / std::max(1.0, c.binHz))));
    l = std::min(18, std::max(4, l));
    p.fftSize = 1 << l;
    p.binHz = r.rateHz / p.fftSize;
    const double usable = c.cropFrac >= 0 ? 1.0 - 2.0 * std::min(0.45, c.cropFrac) : r.usableFrac;
    p.binsPerHop = std::max<size_t>(2, (size_t)std::floor(p.fftSize * usable) & ~(size_t)1);
    p.settleSec = c.settleSec >= 0 ? c.settleSec : (r.nativeSweep ? r.nativeRetuneSec : r.retuneSec);
    p.dwellSec = std::max(c.dwellSec, r.minDwellSec);
    const double step = p.fftSize * 0.5;
    p.averages = std::max(1, (int)std::floor((p.dwellSec * r.rateHz - p.fftSize) / step) + 1);
    p.samplesPerHop = (size_t)(p.fftSize + (p.averages - 1) * step);
    std::vector<FreqRange> bands;
    for (const auto& b : c.bands) if (b.hiHz > b.loHz) bands.push_back(b);
    if (bands.empty()) return p;
    double lo = bands[0].loHz, hi = bands[0].hiHz;
    for (const auto& b : bands) { lo = std::min(lo, b.loHz); hi = std::max(hi, b.hiHz); }
    p.startHz = lo;
    p.totalBins = std::min<size_t>((size_t)std::floor((hi - lo) / p.binHz) + 1, (size_t)8 << 20);
    p.mask.assign(p.totalBins, 0);
    for (const auto& e : c.exclusions) {
        const long a = std::max(0L, (long)std::ceil((e.loHz - lo) / p.binHz)), b = std::min((long)p.totalBins - 1, (long)std::floor((e.hiHz - lo) / p.binHz));
        for (long i = a; i <= b; i++) p.mask[i] = 1;
    }
    for (const auto& b : bands) {
        const size_t b0 = (size_t)std::max(0.0, std::floor((b.loHz - lo) / p.binHz + 0.5));
        const size_t b1 = std::min(p.totalBins, (size_t)std::floor((b.hiHz - lo) / p.binHz) + 1);
        for (size_t first = b0; first < b1; first += p.binsPerHop) {
            Hop h;
            h.firstBin = first; h.nBins = std::min(p.binsPerHop, b1 - first);
            h.centerHz = lo + (first + p.binsPerHop / 2) * p.binHz;
            h.loHz = lo + first * p.binHz; h.hiHz = lo + (first + h.nBins - 1) * p.binHz;
            bool whole = false;
            for (const auto& e : c.exclusions) if (h.loHz >= e.loHz && h.hiHz <= e.hiHz) whole = true;
            if (whole) continue;
            for (const auto& q : c.priority) if (h.hiHz >= q.loHz && h.loHz <= q.hiHz) h.priority = true;
            p.hops.push_back(h);
        }
    }
    std::sort(p.hops.begin(), p.hops.end(), [](const Hop& a, const Hop& b) { return a.firstBin < b.firstBin; });
    std::vector<int> prio;
    for (size_t i = 0; i < p.hops.size(); i++) if (p.hops[i].priority) prio.push_back((int)i);
    size_t pi = 0;
    int sinceP = 0;
    for (size_t i = 0; i < p.hops.size(); i++) {
        p.order.push_back((int)i);
        if (!prio.empty() && c.priorityEvery > 0 && ++sinceP >= c.priorityEvery) { p.order.push_back(prio[pi++ % prio.size()]); sinceP = 0; }
    }
    const double per = p.settleSec + (double)p.samplesPerHop / r.rateHz;
    p.passSec = per * (double)p.order.size();
    for (const Hop& h : p.hops) {
        size_t good = 0;
        for (size_t i = 0; i < h.nBins; i++) good += !p.mask[h.firstBin + i];
        p.coveredHz += (double)good * p.binHz;
    }
    p.mhzPerSec = p.passSec > 0 ? p.coveredHz / 1e6 / p.passSec : 0;
    return p;
}

std::vector<ChannelPower> channelise(const float* db, const float* fl, size_t n, double first, double binHz, double origin, double width, double lo, double hi) {
    std::vector<ChannelPower> out;
    if (!(width > 0) || !n) return out;
    long cur = 0;
    bool have = false;
    double sum = 0;
    ChannelPower cp;
    auto flush = [&] {
        if (!have || cp.bins <= 0) return;
        cp.meanDb = (float)(10.0 * std::log10(std::max(1e-30, sum / cp.bins)));
        out.push_back(cp);
    };
    for (size_t i = 0; i < n; i++) {
        const double f = first + i * binHz;
        if (f < lo || f > hi) continue;
        const long ch = (long)std::floor((f - origin) / width + 0.5);
        if (!have || ch != cur) {
            flush();
            have = true; cur = ch; sum = 0; cp = ChannelPower(); cp.centerHz = origin + ch * width; cp.snrDb = -1e9f;
        }
        sum += std::pow(10.0, db[i] / 10.0);
        cp.bins++;
        cp.peakDb = std::max(cp.peakDb, db[i]);
        cp.snrDb = std::max(cp.snrDb, db[i] - fl[i]);
    }
    flush();
    return out;
}

std::string guessMode(double hz, double bwHz) {
    (void)bwHz;
    if (hz >= 118e6 && hz <= 137e6) return "am";
    if (hz >= 225e6 && hz <= 400e6) return "am";   // the military UHF air band (AM; the 328.6 - 335.4 MHz glide slopes are AM too)
    return "fm";
}

}  // namespace fastscan
}  // namespace dect2
