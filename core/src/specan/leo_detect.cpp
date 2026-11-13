// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/specan/leo_detect.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

namespace {

struct Peak {
    double hz;
    float db;
};

// Narrow local maxima above the frame median, with the frequency refined by a parabola through three bins.
std::vector<Peak> findPeaks(const LeoFrame& fr, const LeoBand& band, const LeoConfig& cfg) {
    std::vector<Peak> out;
    const size_t n = fr.db.size();
    if (n < 3) return out;
    std::vector<float> v(fr.db);
    std::nth_element(v.begin(), v.begin() + n / 2, v.end());
    const float med = v[n / 2];
    const float thr = med + (float)cfg.riseDb;
    const float half = med + (float)cfg.riseDb / 2;
    for (size_t i = 1; i + 1 < n; i++) {
        const float b = fr.db[i];
        if (b < thr || b < fr.db[i - 1] || b <= fr.db[i + 1]) continue;
        size_t lo = i, hi = i;
        while (lo > 0 && fr.db[lo - 1] >= half) lo--;
        while (hi + 1 < n && fr.db[hi + 1] >= half) hi++;
        if (hi - lo + 1 > cfg.maxWidthBins) continue;
        const double a = fr.db[i - 1], c = fr.db[i + 1];
        const double den = a - 2.0 * b + c;
        double off = den < 0 ? 0.5 * (a - c) / den : 0.0;
        off = std::clamp(off, -0.5, 0.5);
        out.push_back({band.startHz + ((double)i + off) * band.binHz, b});
    }
    return out;
}

struct Track {
    double firstT = 0, firstHz = 0, lastT = 0, lastHz = 0;
    double minHz = 0, maxHz = 0;
    float peak = -1000.f;
    size_t hits = 0;
    size_t lastFrame = static_cast<size_t>(-1);
    double sx = 0, sy = 0, sxx = 0, sxy = 0;   // sums of (seconds, Hz) relative to the first hit

    double slope() const {                     // least-squares drift in Hz/s
        const double n = (double)hits, den = n * sxx - sx * sx;
        return (hits < 2 || den <= 0) ? 0.0 : (n * sxy - sx * sy) / den;
    }
};

void addHit(Track& tr, double tMs, double hz, float db, size_t fi) {
    if (tr.hits == 0) {
        tr.firstT = tMs;
        tr.firstHz = hz;
        tr.minHz = tr.maxHz = hz;
    }
    const double x = (tMs - tr.firstT) / 1000.0, y = hz - tr.firstHz;
    tr.sx += x;
    tr.sy += y;
    tr.sxx += x * x;
    tr.sxy += x * y;
    tr.hits++;
    tr.lastT = tMs;
    tr.lastHz = hz;
    tr.minHz = std::min(tr.minHz, hz);
    tr.maxHz = std::max(tr.maxHz, hz);
    tr.peak = std::max(tr.peak, db);
    tr.lastFrame = fi;
}

} // namespace

std::vector<LeoCarrier> detectDopplerCarriers(const std::vector<LeoFrame>& frames, const LeoBand& band, const LeoConfig& cfg) {
    std::vector<Track> tracks;
    for (size_t fi = 0; fi < frames.size(); fi++) {
        const LeoFrame& fr = frames[fi];
        const std::vector<Peak> peaks = findPeaks(fr, band, cfg);
        for (const Peak& pk : peaks) {
            // Attach the peak to the live track whose predicted frequency is closest and within reach.
            int best = -1;
            double bestErr = 0;
            for (size_t k = 0; k < tracks.size(); k++) {
                const Track& tr = tracks[k];
                if (tr.lastFrame == fi) continue;          // one peak per track per frame
                const double dtMs = fr.tMs - tr.lastT;
                if (dtMs > cfg.maxGapMs) continue;
                const double pred = tr.lastHz + tr.slope() * dtMs / 1000.0;
                const double tol = 2.0 * band.binHz + cfg.maxDriftHzPerS * dtMs / 1000.0;
                const double err = std::fabs(pk.hz - pred);
                if (err <= tol && (best < 0 || err < bestErr)) {
                    best = (int)k;
                    bestErr = err;
                }
            }
            if (best < 0) {
                tracks.emplace_back();
                addHit(tracks.back(), fr.tMs, pk.hz, pk.db, fi);
            } else {
                addHit(tracks[(size_t)best], fr.tMs, pk.hz, pk.db, fi);
            }
        }
    }

    std::vector<LeoCarrier> out;
    for (const Track& tr : tracks) {
        const double dur = tr.lastT - tr.firstT;
        if (tr.hits < cfg.minHits || dur < cfg.minMs) continue;
        if (tr.maxHz - tr.minHz < cfg.minSwingHz) continue;
        const double s = tr.slope();
        if (std::fabs(s) > cfg.maxDriftHzPerS) continue;
        LeoCarrier c;
        c.carrierHz = tr.firstHz;
        c.driftHzPerS = s;
        c.firstSeenMs = tr.firstT;
        c.lastSeenMs = tr.lastT;
        c.durationMs = dur;
        c.peakDb = tr.peak;
        c.hits = tr.hits;
        out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const LeoCarrier& a, const LeoCarrier& b) { return a.firstSeenMs < b.firstSeenMs; });
    return out;
}

} // namespace dect2
