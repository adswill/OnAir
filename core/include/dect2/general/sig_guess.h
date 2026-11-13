// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// General receiver: "what is this signal?". From the power spectrum around one carrier (found by the carrier finder or clicked by the user) it measures
//   occupied bandwidth (the part holding 99 % of the power over the noise floor), where the strongest line sits inside it, how symmetric the power is about the
//   centre, how flat the top is, and how many separate humps there are,
// and turns that into a ranked list of guesses with the settings to use (mode, bandwidth). Heuristic: the confidence says how well the shape fits, never more than 0.9.
// Classes: carrier / CW, AM broadcast or voice, SSB (USB or LSB: the sideband is a convention, see guess()), narrow FM, wide FM, two-tone or FSK (two humps), flat OFDM / wideband digital.
// Input: power in linear units or dB per bin (as `db`), DC in the middle bin like the app's spectrum; binHz. The noise floor is the 20th percentile of the span.
// Tests: tests/general40/test_sig_guess.cpp (made from real modulated IQ through an FFT, with frequency offset and noise).
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace dect2::sigguess {

struct Features {
    double obwHz = 0;        // 99 % power bandwidth
    double centreOffHz = 0;  // the middle of the occupied band against the middle of the span
    double peakOffHz = 0;    // the strongest line against the middle of the OBW
    double peakOverDb = 0;   // that line over the median level inside the OBW
    double symmetry = 1;     // 1 = equal power either side of the OBW centre, 0 = all on one side
    double flatnessDb = 0;   // spread (p90 - p10) of the levels inside the OBW: small = flat top
    int humps = 1;           // separate maxima more than 6 dB over the dip between them
    double snrDb = 0;        // power in the OBW over the noise in the same width
    bool ok = false;
};

struct Guess {
    std::string what;        // "AM", "USB", "LSB", "NFM", "WFM", "CW / carrier", "FSK / two tones", "OFDM / wideband digital"
    std::string mode;        // the receiver mode to pick (OnAir mode name)
    double bandwidthHz = 0;  // the bandwidth to set
    double offsetHz = 0;     // where to tune, against the middle of the span (for SSB the carrier edge of the band)
    double confidence = 0;   // 0 .. 0.9
    std::string why;
};

inline Features measure(const std::vector<float>& db, double binHz) {
    Features f;
    const int n = (int)db.size();
    if (n < 64 || !(binHz > 0)) return f;
    std::vector<double> p((size_t)n);
    for (int i = 0; i < n; i++) p[(size_t)i] = std::pow(10.0, (double)db[(size_t)i] / 10.0);
    std::vector<double> sorted = p;
    std::sort(sorted.begin(), sorted.end());
    const double noise = sorted[(size_t)n / 5];
    double tot = 0;
    std::vector<double> e((size_t)n);
    for (int i = 0; i < n; i++) { e[(size_t)i] = std::max(0.0, p[(size_t)i] - noise); tot += e[(size_t)i]; }
    if (!(tot > noise * 8)) return f;   // nothing above the noise
    // 99 %: drop 0.5 % from each side
    double acc = 0;
    int lo = 0, hi = n - 1;
    for (int i = 0; i < n; i++) { acc += e[(size_t)i]; if (acc >= 0.005 * tot) { lo = i; break; } }
    acc = 0;
    for (int i = n - 1; i >= 0; i--) { acc += e[(size_t)i]; if (acc >= 0.005 * tot) { hi = i; break; } }
    if (hi <= lo) return f;
    f.obwHz = (double)(hi - lo + 1) * binHz;
    f.centreOffHz = ((lo + hi) / 2.0 - n / 2.0) * binHz;
    const double mid = (lo + hi) / 2.0;
    int pk = lo;
    double inP = 0, left = 0, right = 0;
    std::vector<double> in;
    for (int i = lo; i <= hi; i++) {
        if (e[(size_t)i] > e[(size_t)pk]) pk = i;
        inP += e[(size_t)i];
        if (i < mid) left += e[(size_t)i]; else if (i > mid) right += e[(size_t)i];
        in.push_back(10.0 * std::log10(std::max(p[(size_t)i], 1e-30)));
    }
    f.peakOffHz = ((double)pk - mid) * binHz;
    std::vector<double> ins = in;
    std::sort(ins.begin(), ins.end());
    const double medDb = ins[ins.size() / 2];
    f.peakOverDb = 10.0 * std::log10(std::max(p[(size_t)pk], 1e-30)) - medDb;
    f.symmetry = (left + right) > 0 ? std::min(left, right) / std::max(left, right) : 1;
    {   // with a clear carrier line the power is compared either side of that line (an AM carrier is not at the middle of a band that noise tails have widened)
        double l2 = 0, r2 = 0;
        const int half = (hi - lo + 1) / 2;
        for (int i = std::max(0, pk - half); i < pk - 3; i++) l2 += e[(size_t)i];   // the main lobe of the line itself is left out
        for (int i = pk + 4; i <= std::min(n - 1, pk + half); i++) r2 += e[(size_t)i];
        const double ps = (l2 + r2) > 0 ? std::min(l2, r2) / std::max(l2, r2) : 1;
        if (10.0 * std::log10(std::max(p[(size_t)pk], 1e-30)) - medDb > 10.0) f.symmetry = ps;
    }
    f.flatnessDb = ins[(size_t)((double)ins.size() * 0.9)] - ins[(size_t)((double)ins.size() * 0.1)];
    // humps: smooth over 3 bins, find maxima 6 dB above the lowest point between it and the next maximum
    const int w = std::max(1, (int)std::lround(std::max(f.obwHz / 40.0, binHz) / binHz));
    std::vector<double> sm(in.size());
    for (size_t i = 0; i < in.size(); i++) { double s = 0; int c = 0; for (long j = (long)i - w; j <= (long)i + w; j++) if (j >= 0 && j < (long)in.size()) { s += std::pow(10.0, in[(size_t)j] / 10.0); c++; } sm[i] = 10.0 * std::log10(s / c); }
    int humps = 0;
    double curMax = -1e9, dip = 1e9;
    bool rising = true;
    for (size_t i = 0; i < sm.size(); i++) {
        if (rising) { if (sm[i] > curMax) curMax = sm[i]; else if (curMax - sm[i] > 6.0) { rising = false; dip = sm[i]; humps++; } }
        else { if (sm[i] < dip) dip = sm[i]; else if (sm[i] - dip > 6.0) { rising = true; curMax = sm[i]; dip = 1e9; } }
    }
    if (rising && curMax > -1e9 && humps == 0) humps = 1;
    f.humps = std::max(1, humps);
    const double noiseInBw = noise * f.obwHz / binHz;
    f.snrDb = 10.0 * std::log10(std::max(inP, 1e-30) / std::max(noiseInBw, 1e-30));
    f.ok = f.snrDb >= 6.0;   // under 6 dB over the noise nothing is a signal (the 20th-percentile floor of pure noise reads 10 dB low: its spread is not a band)
    return f;
}

inline std::vector<Guess> guess(const Features& f) {
    std::vector<Guess> out;
    if (!f.ok) return out;
    auto add = [&](const char* what, const char* mode, double bw, double conf, const std::string& why, double off) { out.push_back({what, mode, bw, off, std::min(0.9, std::max(0.05, conf)), why}); };
    char b[160];
    const double B = f.obwHz;
    // carrier / CW
    if (B < 1000) add("CW / carrier", "CW", std::max(300.0, B * 1.5), 0.6 + (B < 600 ? 0.25 : 0), "very narrow, a single line", f.centreOffHz);
    // AM: a strong central line with symmetric sidebands
    if (B >= 3000 && B <= 20000 && f.symmetry > 0.4 && f.flatnessDb < 10 && f.peakOverDb > 10) {
        std::snprintf(b, sizeof b, "carrier line %.0f dB over the sidebands, sidebands equal (%.0f%%)", f.peakOverDb, f.symmetry * 100);
        add("AM", "AM", B * 1.1, 0.55 + std::min(0.3, (f.peakOverDb - 10) / 40.0), b, f.centreOffHz);
    }
    // SSB: voice-width band with no carrier line. USB or LSB cannot be told from the spectrum alone: by convention the band above the tuned frequency is USB; the suggested
    // tuning is the carrier edge of the band (the lower edge for USB, the upper edge for LSB).
    if (B >= 1200 && B <= 4500 && f.humps <= 2 && f.peakOverDb < 14 && f.flatnessDb > 3) {
        const bool usb = f.centreOffHz >= 0;
        std::snprintf(b, sizeof b, "voice-width (%.1f kHz), no carrier line; %s if the carrier belongs at the %s edge", B / 1000.0, usb ? "USB" : "LSB", usb ? "lower" : "upper");
        add("SSB", usb ? "USB" : "LSB", 3000, 0.45 + (f.peakOverDb < 9 ? 0.2 : 0), b, f.centreOffHz + (usb ? -B / 2 : B / 2));
    }
    // two humps
    if (f.humps >= 2 && B < 30000) {
        std::snprintf(b, sizeof b, "%d separate humps over %.1f kHz", f.humps, B / 1000.0);
        add("FSK / two tones", "FSK", B * 1.3, 0.6, b, f.centreOffHz);
    }
    // narrow FM
    if (B >= 6000 && B <= 25000 && f.humps <= 1 && (f.peakOverDb < 12 || f.flatnessDb >= 10)) {
        std::snprintf(b, sizeof b, "%.1f kHz wide, no carrier line", B / 1000.0);
        add("NFM", "NFM", std::max(12500.0, B * 1.1), 0.5 + (f.symmetry > 0.7 ? 0.2 : 0), b, f.centreOffHz);
    }
    // wide FM
    if (B >= 120000 && B <= 300000) { std::snprintf(b, sizeof b, "%.0f kHz wide", B / 1000.0); add("WFM", "FM", 200000, 0.65, b, f.centreOffHz); }
    // OFDM / flat wideband
    if (B >= 4000 && f.flatnessDb < 7 && f.symmetry > 0.6 && f.peakOverDb < 8 && f.humps <= 1) {
        std::snprintf(b, sizeof b, "flat top (%.0f dB spread) over %.0f kHz", f.flatnessDb, B / 1000.0);
        add("OFDM / wideband digital", "General", B * 1.1, 0.5 + (B > 100000 ? 0.2 : 0), b, f.centreOffHz);
    }
    std::sort(out.begin(), out.end(), [](const Guess& a, const Guess& c) { return a.confidence > c.confidence; });
    return out;
}

}  // namespace dect2::sigguess
