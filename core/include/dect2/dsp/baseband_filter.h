// User-selectable baseband (channel) filter shared by the modes: bandwidth, window shape, transition width and centre offset, a design
// routine that gives the FIR taps, the response for the live plot, a text form for the preferences, and the per-mode presets.
// A configuration with enabled == false means "the mode's own filter, exactly as before"; nothing changes until the user opens it.
// The Butterworth shape is a linear-phase FIR sampled from the Butterworth magnitude (order 4), so every shape runs through the same FIR stage.
#pragma once
#include "../common/window.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

enum class BbShape { Blackman = 0, Kaiser = 1, BlackmanHarris = 2, Hann = 3, Butterworth = 4 };

struct BasebandFilterCfg {
    bool enabled = false;
    double bwHz = 0;          // full pass-band width, Hz (0 = the mode's own)
    BbShape shape = BbShape::Blackman;
    double transHz = 0;       // transition width, Hz (0 = the mode's own)
    double centreHz = 0;      // pass-band centre relative to the mode's nominal centre, Hz
    bool operator==(const BasebandFilterCfg& o) const {
        return enabled == o.enabled && bwHz == o.bwHz && shape == o.shape && transHz == o.transHz && centreHz == o.centreHz;
    }
    bool operator!=(const BasebandFilterCfg& o) const { return !(*this == o); }
};

struct BbDesign {
    std::vector<float> h;               // real low-pass taps (the pass-band centred on 0), sum 1
    bool shifted = false;               // centre offset != 0: use hc
    std::vector<std::complex<float>> hc; // the taps rotated to the centre offset
};

namespace bbf {

inline const char* shapeName(BbShape s) {
    switch (s) {
        case BbShape::Blackman: return "Blackman";
        case BbShape::Kaiser: return "Kaiser";
        case BbShape::BlackmanHarris: return "Blackman-Harris";
        case BbShape::Hann: return "Hann";
        case BbShape::Butterworth: return "Butterworth";
    }
    return "?";
}

// taps per (sample rate / transition width) for the window
inline double tapFactor(BbShape s) {
    switch (s) {
        case BbShape::Blackman: return 4.0;
        case BbShape::Kaiser: return 3.6;
        case BbShape::BlackmanHarris: return 5.0;
        case BbShape::Hann: return 2.0;
        case BbShape::Butterworth: return 4.0;
    }
    return 4.0;
}

inline double windowAt(BbShape s, int i, int taps) {
    const double N = taps - 1;
    const double t = 2 * M_PI * i / N;
    switch (s) {
        case BbShape::Hann: return 0.5 - 0.5 * std::cos(t);
        case BbShape::BlackmanHarris: return 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        case BbShape::Kaiser: {
            const double beta = 8.0, r = 2.0 * i / N - 1.0;
            return window::besselI0(beta * std::sqrt(std::max(0.0, 1 - r * r)), 60) / window::besselI0(beta, 60);
        }
        default: return 0.42 - 0.5 * std::cos(t) + 0.08 * std::cos(2 * t);   // Blackman (the HFDL receiver's original window)
    }
}

// low-pass taps, cut-off at bw/2 for the sample rate fs, count from the transition width (clamped to minTaps..maxTaps, odd)
inline std::vector<float> lowpass(BbShape shape, double bwHz, double transHz, double fs, int minTaps = 31, int maxTaps = 301) {
    int taps = (int)std::ceil(tapFactor(shape) * fs / std::max(50.0, transHz)) | 1;
    taps = std::max(minTaps, std::min(maxTaps, taps));
    std::vector<float> h((size_t)taps);
    const double fc = std::max(10.0, bwHz / 2);
    double sum = 0;
    if (shape == BbShape::Butterworth) {
        // sample the order-4 Butterworth magnitude on a dense grid, inverse DFT to a zero-phase impulse response, Hann-trimmed
        const int G = 4096;
        std::vector<double> mag((size_t)G);
        for (int k = 0; k < G; k++) {
            const double f = (k <= G / 2 ? k : k - G) * fs / G;
            mag[(size_t)k] = 1.0 / std::sqrt(1.0 + std::pow(f / fc, 8));
        }
        for (int i = 0; i < taps; i++) {
            const int m = i - (taps - 1) / 2;
            double a = 0;
            for (int k = 0; k < G; k++) a += mag[(size_t)k] * std::cos(2 * M_PI * k * (double)m / G);
            h[(size_t)i] = (float)(a / G * (0.5 + 0.5 * std::cos(M_PI * m / ((taps + 1) / 2.0))));
            sum += h[(size_t)i];
        }
    } else {
        for (int i = 0; i < taps; i++) {
            const double m = i - (taps - 1) / 2.0;
            const double x = 2 * fc / fs * m;
            const double sinc = std::fabs(m) < 1e-9 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
            h[(size_t)i] = (float)(2 * fc / fs * sinc * windowAt(shape, i, taps));
            sum += h[(size_t)i];
        }
    }
    for (auto& v : h) v = (float)(v / sum);
    return h;
}

// the configuration applied to the mode's defaults (its bandwidth and transition) at the sample rate fs
inline BbDesign design(const BasebandFilterCfg& c, double defBwHz, double defTransHz, double fs, int minTaps = 31, int maxTaps = 301) {
    BbDesign d;
    const double bw = c.enabled && c.bwHz > 0 ? c.bwHz : defBwHz;
    const double tr = c.enabled && c.transHz > 0 ? c.transHz : defTransHz;
    const BbShape sh = c.enabled ? c.shape : BbShape::Blackman;
    d.h = lowpass(sh, std::min(bw, fs * 0.98), tr, fs, minTaps, maxTaps);
    const double off = c.enabled ? c.centreHz : 0;
    if (off != 0) {
        d.shifted = true;
        d.hc.resize(d.h.size());
        for (size_t i = 0; i < d.h.size(); i++) {
            const double m = (double)i - (d.h.size() - 1) / 2.0;
            { const double ph = 2 * M_PI * off / fs * m; d.hc[i] = std::complex<float>((float)(d.h[i] * std::cos(ph)), (float)(d.h[i] * std::sin(ph))); }
        }
    }
    return d;
}

// gain in dB of the design at frequency f (Hz, relative to the nominal centre) for the plot
inline double responseDb(const BbDesign& d, double fs, double f) {
    std::complex<double> a(0, 0);
    const size_t n = d.h.size();
    for (size_t i = 0; i < n; i++) {
        const double m = (double)i - (n - 1) / 2.0;
        const std::complex<double> t = d.shifted ? std::complex<double>(d.hc[i].real(), d.hc[i].imag()) : std::complex<double>(d.h[i], 0);
        a += t * std::polar(1.0, -2 * M_PI * f / fs * m);
    }
    return 20 * std::log10(std::abs(a) + 1e-9);
}

inline std::string toString(const BasebandFilterCfg& c) {
    char b[128];
    snprintf(b, sizeof b, "%d,%.1f,%d,%.1f,%.1f", c.enabled ? 1 : 0, c.bwHz, (int)c.shape, c.transHz, c.centreHz);
    return b;
}
inline BasebandFilterCfg fromString(const std::string& s) {
    BasebandFilterCfg c;
    int en = 0, sh = 0; double bw = 0, tr = 0, ce = 0;
    if (sscanf(s.c_str(), "%d,%lf,%d,%lf,%lf", &en, &bw, &sh, &tr, &ce) != 5) return c;
    c.enabled = en != 0;
    c.bwHz = std::max(0.0, std::min(1e6, bw));
    c.shape = (BbShape)std::max(0, std::min(4, sh));
    c.transHz = std::max(0.0, std::min(1e6, tr));
    c.centreHz = std::max(-1e6, std::min(1e6, ce));
    return c;
}

struct Preset { const char* mode; const char* name; BasebandFilterCfg cfg; };
// Presets per mode; the first of a mode is its default (the filter as shipped, so enabling it changes nothing). Modes not listed use the generic ones.
inline const std::vector<Preset>& presets() {
    static const std::vector<Preset> p = {
        {"hfdl", "Default (3.4 kHz Blackman)", {true, 3400, BbShape::Blackman, 2000, 0}},
        {"hfdl", "Narrow (2.8 kHz Kaiser)", {true, 2800, BbShape::Kaiser, 1200, 0}},
        {"hfdl", "Wide (4.2 kHz Blackman-Harris)", {true, 4200, BbShape::BlackmanHarris, 2400, 0}},
        {"hfdl", "Smooth (3.4 kHz Butterworth)", {true, 3400, BbShape::Butterworth, 2000, 0}},
        {"", "Default (Blackman)", {true, 0, BbShape::Blackman, 0, 0}},
        {"", "Sharp (Kaiser)", {true, 0, BbShape::Kaiser, 0, 0}},
    };
    return p;
}

// ---- the per-mode settings (key = the mode's short name: "airband", "acars", "vdl2", "ais", "pager", "packet", "hfdig", "cw", "general", "tetrapol") ----
// The app fills the registry from the preferences and when the user changes a filter; a mode reads it where it designs its channel filter
// (at start / configure), so a change applies the next time the mode starts. Nothing set: the mode's own filter.
struct Registry { std::mutex mu; std::map<std::string, BasebandFilterCfg> m; };
inline Registry& registry() { static Registry r; return r; }
inline BasebandFilterCfg get(const std::string& key) { Registry& r = registry(); std::lock_guard<std::mutex> lk(r.mu); auto it = r.m.find(key); return it == r.m.end() ? BasebandFilterCfg() : it->second; }
inline void set(const std::string& key, const BasebandFilterCfg& c) { Registry& r = registry(); std::lock_guard<std::mutex> lk(r.mu); r.m[key] = c; }

// The mode's own low-pass taps `def` (n taps at sample rate fs, cut-off about defBwHz / 2) or, when the user switched the key's filter on,
// taps of the chosen bandwidth / shape / transition (the mode's own values where the user left them at 0). The transition of the mode's own
// filter is estimated from its length. Works on real taps; the centre offset is for I/Q stages and is not used here.
template <class T> inline std::vector<T> overrideTaps(const std::string& key, std::vector<T> def, double fs, double defBwHz, int minTaps = 5, int maxTaps = 4001) {
    const BasebandFilterCfg c = get(key);
    if (!c.enabled || def.size() < 3 || fs <= 0) return def;
    const double defTrans = tapFactor(BbShape::Kaiser) * fs / (double)def.size();
    const std::vector<float> h = lowpass(c.shape, c.bwHz > 0 ? c.bwHz : defBwHz, c.transHz > 0 ? c.transHz : defTrans, fs, minTaps, maxTaps);
    return std::vector<T>(h.begin(), h.end());
}

// the preference key of a receiver family (app.h family numbers); empty: the family has no baseband filter setting
inline const char* keyForFamily(int family) {
    switch (family) {
        case 35: return "hfdl";
        case 16: return "acars";
        case 27: return "vdl2";
        case 26: return "airband";
        case 14: return "ais";
        case 23: return "pager";
        case 24: return "packet";
        case 25: case 36: return "hfdig";
        case 34: return "cw";
        case 55: return "general";
        case 80: return "tetrapol";
        default: return "";
    }
}

} // namespace bbf
} // namespace dect2
