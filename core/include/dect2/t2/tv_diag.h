// OFDM TV channel diagnosis from what the receivers already measure (DVB-T, DVB-T2, ISDB-T share the layout of the power-delay profile and the per-carrier SNR):
//  1. findEchoes   the paths in the power-delay profile as a table: delay in us, level in dB under the main path, extra path length in km, and a verdict per path:
//                  pre-echo (arrives before the main path: the receiver locked onto a weaker, later transmitter), inside the guard interval (harmless),
//                  beyond the guard interval (costs SNR: it is inter-symbol interference), or a far SFN transmitter. "Fix" text says what to do.
//  2. findCarrierDips  narrow dips of the per-carrier SNR: a narrowband interferer (LTE guard, ingress, a radio link), its frequency, depth and width, and whether the
//                  channel is instead tilted or notched by multipath (a broad dip).
// Pure functions: tests/tvdiag/test_tv_diag.cpp (synthetic profiles with impairments).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2::tvdiag {

enum class EchoKind { Main, PreEcho, InsideGuard, BeyondGuard };

struct Echo {
    double delayUs = 0;       // against the main path (negative = before it)
    double levelDb = 0;       // under the main path (negative)
    double extraKm = 0;       // extra path length, c * delay
    EchoKind kind = EchoKind::InsideGuard;
};

struct EchoReport {
    std::vector<Echo> echoes;   // strongest first, the main path excluded
    double mainDelayUs = 0;     // position of the main path in the profile axis
    double worstBeyondGuardDb = -200;   // level of the strongest echo beyond the guard (-200: none)
    double preEchoDb = -200;            // level of the strongest pre-echo (-200: none)
    std::string verdict;        // one line, plain words
    std::string fix;            // what to do about it, empty when nothing is wrong
};

// irDb: power-delay profile in dB relative to its peak, one value per sample from tauMin (samples); rateHz: the sample rate of the profile; guardSamples: the guard interval.
// Peaks: local maxima at least minDb under the strongest value and at least minSepSamples apart from a stronger one. minDb default -35 dB.
inline EchoReport findEchoes(const std::vector<float>& irDb, int tauMin, double rateHz, int guardSamples, double minDb = -35.0, int minSepSamples = 2) {
    EchoReport r;
    if (irDb.size() < 8 || !(rateHz > 0)) { r.verdict = "No channel estimate yet."; return r; }
    const double us = 1e6 / rateHz;
    size_t main = 0;
    for (size_t i = 1; i < irDb.size(); i++) if (irDb[i] > irDb[main]) main = i;
    const double peak = irDb[main];
    r.mainDelayUs = (double)(tauMin + (long)main) * us;
    // local maxima
    std::vector<size_t> pk;
    for (size_t i = 0; i < irDb.size(); i++) {
        if (i == main || irDb[i] - peak < minDb) continue;
        const float l = i > 0 ? irDb[i - 1] : -1e9f, rr = i + 1 < irDb.size() ? irDb[i + 1] : -1e9f;
        if (irDb[i] >= l && irDb[i] > rr) pk.push_back(i);
    }
    std::sort(pk.begin(), pk.end(), [&](size_t a, size_t b) { return irDb[a] > irDb[b]; });
    std::vector<size_t> kept;
    for (size_t p : pk) {
        bool near = false;
        for (size_t k : kept) if (std::labs((long)p - (long)k) < minSepSamples) { near = true; break; }
        if (main != p && std::labs((long)p - (long)main) < minSepSamples) near = true;   // the skirt of the main path
        if (!near) kept.push_back(p);
    }
    for (size_t p : kept) {
        Echo e;
        const long d = (long)p - (long)main;
        e.delayUs = (double)d * us;
        e.levelDb = (double)irDb[p] - peak;
        e.extraKm = std::fabs(e.delayUs) * 1e-6 * 299792.458;
        e.kind = d < 0 ? EchoKind::PreEcho : (d > guardSamples ? EchoKind::BeyondGuard : EchoKind::InsideGuard);
        if (e.kind == EchoKind::PreEcho) r.preEchoDb = std::max(r.preEchoDb, e.levelDb);
        if (e.kind == EchoKind::BeyondGuard) r.worstBeyondGuardDb = std::max(r.worstBeyondGuardDb, e.levelDb);
        r.echoes.push_back(e);
    }
    char b[260];
    if (r.echoes.empty()) { r.verdict = "One clean path, no echoes."; return r; }
    const Echo& s = r.echoes.front();
    if (r.preEchoDb > -200 && r.preEchoDb > -15) {
        std::snprintf(b, sizeof b, "Pre-echo: a path %.1f us ahead of the main one (%.0f dB). The receiver is locked on a weaker, later transmitter.", 0.0, r.preEchoDb);
        for (const Echo& e : r.echoes) if (e.kind == EchoKind::PreEcho && e.levelDb == r.preEchoDb) { std::snprintf(b, sizeof b, "Pre-echo: a path %.1f us ahead of the main one (%.0f dB). The receiver is locked on a weaker, later transmitter.", -e.delayUs, e.levelDb); break; }
        r.verdict = b;
        r.fix = "Turn or move the antenna toward the stronger transmitter; a pre-echo that strong is the one you should be pointing at.";
    } else if (r.worstBeyondGuardDb > -200 && r.worstBeyondGuardDb > -25) {
        std::snprintf(b, sizeof b, "Echo beyond the guard interval: %.0f dB at %.1f us (%.1f km extra). It acts as interference and caps the SNR near %.0f dB.", r.worstBeyondGuardDb, s.delayUs, s.extraKm, -r.worstBeyondGuardDb);
        for (const Echo& e : r.echoes) if (e.kind == EchoKind::BeyondGuard && e.levelDb == r.worstBeyondGuardDb) { std::snprintf(b, sizeof b, "Echo beyond the guard interval: %.0f dB at %.1f us (%.1f km extra). It acts as interference and caps the SNR near %.0f dB.", e.levelDb, e.delayUs, e.extraKm, -e.levelDb); break; }
        r.verdict = b;
        r.fix = "Use a directional antenna aimed away from the late path (a reflection or a far transmitter of the same network), or move it a few metres.";
    } else {
        std::snprintf(b, sizeof b, "%d echo%s inside the guard interval, strongest %.0f dB at %.1f us. Harmless: the receiver uses them.", (int)r.echoes.size(), r.echoes.size() == 1 ? "" : "es", s.levelDb, s.delayUs);
        r.verdict = b;
    }
    return r;
}

struct CarrierDip {
    double freqMhz = 0;     // centre of the dip
    double depthDb = 0;     // under the median of the neighbourhood
    int widthPoints = 0;    // in points of the SNR array
    bool narrow = false;    // a narrowband interferer (up to maxNarrowPoints wide); false = a broad notch or tilt (multipath)
};

// snrDb: per-carrier SNR array (snrStep carriers between points); freq of point i = centreMhz + (i - (n-1)/2) * stepMhz. A dip: a run of points at least minDepthDb under the local
// median (a window of 121 points). Runs are reported strongest first.
inline std::vector<CarrierDip> findCarrierDips(const std::vector<float>& snrDb, double centreMhz, double stepMhz, double minDepthDb = 6.0, int maxNarrowPoints = 6) {
    std::vector<CarrierDip> out;
    const int n = (int)snrDb.size();
    if (n < 24) return out;
    std::vector<float> med((size_t)n);
    const int W = 60;
    std::vector<float> win;
    for (int i = 0; i < n; i++) {
        win.clear();
        for (int j = std::max(0, i - W); j <= std::min(n - 1, i + W); j++) win.push_back(snrDb[(size_t)j]);
        std::nth_element(win.begin(), win.begin() + (long)win.size() / 2, win.end());
        med[(size_t)i] = win[win.size() / 2];
    }
    int i = 0;
    while (i < n) {
        if (med[(size_t)i] - snrDb[(size_t)i] >= (float)minDepthDb) {
            int j = i;
            float deep = 0;
            int at = i;
            while (j < n && med[(size_t)j] - snrDb[(size_t)j] >= (float)minDepthDb * 0.5f) {
                const float d = med[(size_t)j] - snrDb[(size_t)j];
                if (d > deep) { deep = d; at = j; }
                j++;
            }
            CarrierDip c;
            c.depthDb = deep;
            c.widthPoints = j - i;
            c.freqMhz = centreMhz + ((double)(i + j - 1) / 2.0 - (double)(n - 1) / 2.0) * stepMhz;
            (void)at;
            c.narrow = c.widthPoints <= maxNarrowPoints;
            out.push_back(c);
            i = j;
        } else i++;
    }
    std::sort(out.begin(), out.end(), [](const CarrierDip& a, const CarrierDip& b) { return a.depthDb > b.depthDb; });
    return out;
}

}  // namespace dect2::tvdiag
