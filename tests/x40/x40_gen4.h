// Test signals for the fourth block of 4.0 signal modes (families 779 and up). Generators exist only in the tests; none is reachable from the transmit side of the application.
#pragma once
#include "x40_gen3.h"
#include <functional>

namespace x40gen {

// ---- keyed tone on an AM carrier (ILS marker beacons) ----
// elements: (on seconds, off seconds) repeated; tone in Hz; depth of the tone in percent of the carrier; lo: the carrier's offset from the tuned frequency; fade: slow fading depth (0..1) at fadeHz
struct KeyedAm {
    std::vector<std::pair<double, double>> elems; double toneHz = 400, depth = 0.95, lo = 0, noise = 0, fadeDepth = 0, fadeHz = 0.3, startSec = 1.0, stopSec = 1e9, tone2Hz = 0, tone2Depth = 0; double fs = 12000; unsigned seed = 1;
    Rng rng{1}; double ph = 0, pt = 0, pt2 = 0; std::vector<double> edges; double cycle = 0;
    void prep() { cycle = 0; for (auto& e : elems) cycle += e.first + e.second; rng = Rng(seed); }
    bool keyed(double t) const {
        if (t < startSec || t >= stopSec || elems.empty()) return false;
        double u = std::fmod(t - startSec, cycle);
        for (auto& e : elems) { if (u < e.first) return true; u -= e.first; if (u < e.second) return false; u -= e.second; }
        return false;
    }
    std::vector<cf32> operator()(double t0, size_t n) {
        if (cycle == 0) prep();
        std::vector<cf32> x(n); const float sg = (float)std::sqrt(noise / 2);
        for (size_t i = 0; i < n; i++) {
            const double t = t0 + i / fs;
            pt += 2 * kPi * toneHz / fs; if (pt > 2 * kPi) pt -= 2 * kPi;
            pt2 += 2 * kPi * tone2Hz / fs; if (pt2 > 2 * kPi) pt2 -= 2 * kPi;
            double env = 1.0;
            if (keyed(t)) env += depth * std::sin(pt);
            if (tone2Hz > 0 && keyed(t)) env += tone2Depth * std::sin(pt2);
            env *= 1.0 + fadeDepth * std::sin(2 * kPi * fadeHz * t);
            ph += 2 * kPi * lo / fs; if (ph > 2 * kPi) ph -= 2 * kPi; else if (ph < -2 * kPi) ph += 2 * kPi;
            x[i] = (float)env * cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32(sg * rng.g(), sg * rng.g());
        }
        return x;
    }
};
// the three markers of Annex 10 (the element lengths are the generator's assumption: a dash is three dots)
inline std::vector<std::pair<double, double>> outerMarker() { return {{0.30, 0.20}}; }                    // two dashes per second
inline std::vector<std::pair<double, double>> middleMarker() { return {{0.10, 0.10}, {0.30, 0.13}}; }      // 95 dot-dash pairs per minute (period 0.63 s)
inline std::vector<std::pair<double, double>> innerMarker() { return {{0.08, 0.0867}}; }                    // six dots per second


// ---- asynchronous FSK modem tones (Bell 103, V.21, V.23, Bell 202) in a single-sideband channel ----
struct ModemTones { const char* name; double mark, space, baud; };
inline const ModemTones& modemStd(int i) {
    static const ModemTones k[] = {{"Bell 103 originate", 1270, 1070, 300}, {"Bell 103 answer", 2225, 2025, 300}, {"V.21 channel 1", 980, 1180, 300}, {"V.21 channel 2", 1650, 1850, 300}, {"V.23 mode 2", 1300, 2100, 1200}, {"Bell 202", 1200, 2200, 1200}};
    return k[i];
}
// a burst: idle mark, the bytes (start, 8 data LSB first, stopBits marks), idle mark. dial: error of the tuned frequency in Hz (all tones move); lower: the lower sideband (negative frequencies); gapBits: extra idle marks between bytes
inline std::vector<cf32> modemBurst(int std_, const std::vector<uint8_t>& bytes, double fs, double leadSec, double tailSec, double idleSec, double dial = 0, bool lower = false, double noise = 0, unsigned seed = 1, double clockPpm = 0, int stopBits = 1, int gapBits = 0, double amp = 0.5, double baudScale = 1.0) {
    const ModemTones& m = modemStd(std_);
    std::vector<int> bits;
    for (uint8_t b : bytes) { bits.push_back(0); for (int k = 0; k < 8; k++) bits.push_back((b >> k) & 1); for (int k = 0; k < stopBits + gapBits; k++) bits.push_back(1); }
    const double T = fs / (m.baud * baudScale) * (1 + clockPpm * 1e-6);
    const size_t lead = (size_t)(leadSec * fs), idle = (size_t)(idleSec * fs), tail = (size_t)(tailSec * fs);
    const size_t body = (size_t)(bits.size() * T) + 2;
    std::vector<cf32> x(lead + idle + body + idle + tail, cf32(0, 0));
    double ph = 0; const double sg = lower ? -1.0 : 1.0;
    for (size_t i = lead; i < lead + idle + body + idle; i++) {
        int bit = 1; const double pos = (double)(i - lead - idle);
        if (pos >= 0) { const size_t k = (size_t)(pos / T); bit = k < bits.size() ? bits[k] : 1; }
        const double f = sg * ((bit ? m.mark : m.space) + dial);
        ph += 2 * kPi * f / fs; if (ph > 2 * kPi) ph -= 2 * kPi; else if (ph < -2 * kPi) ph += 2 * kPi;
        x[i] = (float)amp * cf32((float)std::cos(ph), (float)std::sin(ph));
    }
    if (noise > 0) addNoise(x, noise, seed);
    return x;
}

}
