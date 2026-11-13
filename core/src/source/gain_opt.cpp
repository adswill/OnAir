// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/source/gain_opt.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

bool rxSnrDb(const RxTelemetry& rx, double& snr, const char** what) {
    auto give = [&](double v, const char* w) {
        if (!std::isfinite(v)) return false;
        snr = v;
        if (what) *what = w;
        return true;
    };
    switch (rx.standard) {
    case 2: return rx.atsc.fieldSync && give(rx.atsc.snrDb, "SNR");   // ATSC 1.0: the equaliser's SNR once the field sync is found
    case 3: return rx.dab.sync && give(rx.dab.snrDb, "SNR");          // DAB: from the DQPSK clusters
    case 6: return rx.fm.carrier && rx.fm.state >= 1 && give(rx.fm.snrDb, "audio SNR");   // FM: noise above the multiplex
    default: return rx.dataValid && give(rx.dataSnrDb, "SNR");        // DVB-T2/T, ISDB-T and every mode from DVB-S on publish their SNR / MER / Es/N0 here
    }
}

bool spectrumSnrDb(const std::vector<float>& dbfs, double rateHz, double chanBwHz, double& snrDb, double centreOffsetHz) {
    const size_t n = dbfs.size();
    if (n < 64 || !(rateHz > 0) || !(chanBwHz > 0)) return false;
    const double binHz = rateHz / (double)n;
    std::vector<float> outside;   // linear power
    double inSum = 0;
    size_t inN = 0;
    for (size_t i = 0; i < n; i++) {
        const double f = ((double)i - (double)n / 2.0) * binHz;   // DC-centred
        if (std::fabs(f) < 2.5 * binHz) continue;                 // the radio's own DC spike is not signal
        const double d = std::fabs(f - centreOffsetHz);
        const float p = std::pow(10.f, dbfs[i] / 10.f);
        if (!std::isfinite(p)) continue;
        if (d <= 0.4 * chanBwHz) { inSum += p; inN++; }            // inside the flat part of the channel
        else if (d >= 0.75 * chanBwHz) outside.push_back(p);       // beside it, past the skirts
    }
    if (inN < 4 || outside.size() < 16) return false;
    // the noise floor: the quietest quarter of what lies beside the channel (other channels and spurs only raise the rest). The mean of an averaged FFT
    // bin sits about 0.55 dB above its 25th percentile (32 averages), which is added back.
    const size_t k = outside.size() / 4;
    std::nth_element(outside.begin(), outside.begin() + (long)k, outside.end());
    const double noise = (double)outside[k] * std::pow(10.0, 0.055);
    if (!(noise > 0)) return false;
    const double sig = inSum / (double)inN;
    snrDb = 10.0 * std::log10(std::max(sig / noise - 1.0, 1e-3));   // the in-channel power holds the noise as well
    return true;
}

std::vector<int> GainOptimiser::coarseGains(int minDb, int maxDb, int maxPoints) {
    std::vector<int> g;
    if (maxDb <= minDb) { g.push_back(minDb); return g; }
    const int n = std::max(2, std::min(maxPoints, maxDb - minDb + 1));
    for (int i = 0; i < n; i++) {
        const int v = minDb + (int)std::lround((double)i * (maxDb - minDb) / (n - 1));
        if (g.empty() || g.back() != v) g.push_back(v);
    }
    return g;
}

void GainOptimiser::start(double now, int minDb, int maxDb, int currentDb) {
    pts_.clear();
    if (maxDb < minDb) std::swap(minDb, maxDb);
    minDb_ = minDb; maxDb_ = maxDb; startDb_ = currentDb; bestDb_ = currentDb;
    for (int g : coarseGains(minDb, maxDb, std::max(2, cfg_.maxPoints))) { Point p; p.gainDb = g; pts_.push_back(p); }
    step_ = pts_.size() > 1 ? (int)std::lround((double)(maxDb - minDb) / (double)(pts_.size() - 1)) : 1;
    idx_ = -1; active_ = true; refined_ = false; result_ = Result::None; t0_ = now;
}

int GainOptimiser::stepsDone() const {
    int n = 0;
    for (const Point& p : pts_) n += p.done ? 1 : 0;
    return n;
}

void GainOptimiser::beginPoint(double now, int) {
    t0_ = now; snrSum_ = altSum_ = rmsSum_ = clipMax_ = 0; nSnr_ = nAlt_ = nAll_ = 0; over_ = false;
}

void GainOptimiser::closePoint() {
    Point& p = pts_[(size_t)idx_];
    p.n = nSnr_; p.nAll = nAll_;
    p.snrDb = nSnr_ ? snrSum_ / nSnr_ : -100;
    p.n2 = nAlt_; p.altDb = nAlt_ ? altSum_ / nAlt_ : -100;
    p.rms = nAll_ ? (float)(rmsSum_ / nAll_) : -120.f;
    p.clip = (float)clipMax_;
    p.overload = over_;
    p.done = true;
}

bool GainOptimiser::update(double now, const Sample& s, int& gainDb) {
    if (!active_ || pts_.empty()) return false;
    if (idx_ < 0) { idx_ = 0; beginPoint(now, pts_[0].gainDb); gainDb = pts_[0].gainDb; return true; }
    const double el = now - t0_;
    if (el > cfg_.settleSec) {
        if (s.snrValid && std::isfinite(s.snrDb)) { snrSum_ += s.snrDb; nSnr_++; }
        if (s.altValid && std::isfinite(s.altDb)) { altSum_ += s.altDb; nAlt_++; }
        rmsSum_ += s.rms; nAll_++;
        clipMax_ = std::max<double>(clipMax_, s.clip);
        if (classifyAdc(s.rms, s.peak, s.clip) == AdcStatus::Overload) over_ = true;
    }
    if (el < cfg_.settleSec + cfg_.measureSec) return false;
    closePoint();
    if (++idx_ >= (int)pts_.size() && !refined_) {
        // a second look around the winner, half a step to each side (the coarse sweep is a few dB apart)
        refined_ = true;
        Result how = Result::None;
        const int bi = choose(pts_, cfg_, &how);
        const int h = step_ / 2;
        if (bi >= 0 && how != Result::AllClipped && h >= 1) {
            const int g0 = pts_[(size_t)bi].gainDb;
            for (int g : {g0 - h, g0 + h}) {
                if (g < minDb_ || g > maxDb_) continue;
                bool have = false;
                for (const Point& p : pts_) have = have || p.gainDb == g;
                if (!have) { Point p; p.gainDb = g; pts_.push_back(p); }
            }
        }
    }
    if (idx_ >= (int)pts_.size()) {
        const int bi = choose(pts_, cfg_, &result_);
        bestDb_ = bi >= 0 ? pts_[(size_t)bi].gainDb : startDb_;
        std::sort(pts_.begin(), pts_.end(), [](const Point& a, const Point& b) { return a.gainDb < b.gainDb; });
        active_ = false;
        gainDb = bestDb_;
        return true;
    }
    beginPoint(now, pts_[(size_t)idx_].gainDb);
    gainDb = pts_[(size_t)idx_].gainDb;
    return true;
}

bool GainOptimiser::cancel(int& gainDb) {
    if (!active_) return false;
    active_ = false;
    result_ = Result::None;
    bestDb_ = startDb_;
    gainDb = startDb_;
    return true;
}

int GainOptimiser::choose(const std::vector<Point>& pts, const Config& cfg, Result* how) {
    if (how) *how = Result::None;
    int lowest = -1, bestSnr = -1;
    std::vector<int> safe;
    for (size_t i = 0; i < pts.size(); i++) {
        const Point& p = pts[i];
        if (!p.done) continue;
        if (lowest < 0 || p.gainDb < pts[(size_t)lowest].gainDb) lowest = (int)i;
        if (!p.clipped(cfg.clipLimit)) safe.push_back((int)i);
    }
    if (lowest < 0) return -1;
    if (safe.empty()) { if (how) *how = Result::AllClipped; return lowest; }   // nothing is safe: the least gain is the least bad
    // the mode's own number if any safe step had it, else the spectrum's; never a mix of the two
    for (int pass = 0; pass < 2; pass++) {
        auto has = [&](const Point& p) { return pass == 0 ? p.hasSnr() : p.hasAlt(); };
        auto val = [&](const Point& p) { return pass == 0 ? p.snrDb : p.altDb; };
        for (int i : safe)
            if (has(pts[(size_t)i]) && (bestSnr < 0 || val(pts[(size_t)i]) > val(pts[(size_t)bestSnr]))) bestSnr = i;
        if (bestSnr < 0) continue;
        // all steps equally bad (no signal, or a lock that never held) tie within the tolerance and the "lowest gain" rule would pick the bottom of
        // the range: 0 dB with nothing received. Such a number says nothing; go on to the next kind, and in the end to the level.
        if (val(pts[(size_t)bestSnr]) < cfg.minUsableSnrDb) { bestSnr = -1; continue; }
        const double lim = val(pts[(size_t)bestSnr]) - cfg.snrTolDb;
        int win = bestSnr;
        for (int i : safe)
            if (has(pts[(size_t)i]) && val(pts[(size_t)i]) >= lim && pts[(size_t)i].gainDb < pts[(size_t)win].gainDb) win = i;
        if (how) *how = pass == 0 ? Result::Snr : Result::Spectrum;
        return win;
    }
    int win = safe[0];   // no step had a quality number: the level nearest the target, the lower gain on a tie
    for (int i : safe) {
        const double a = std::fabs(pts[(size_t)i].rms - cfg.targetDbfs), b = std::fabs(pts[(size_t)win].rms - cfg.targetDbfs);
        if (a < b - 1e-9 || (std::fabs(a - b) <= 1e-9 && pts[(size_t)i].gainDb < pts[(size_t)win].gainDb)) win = i;
    }
    if (how) *how = Result::Level;
    return win;
}

} // namespace dect2
