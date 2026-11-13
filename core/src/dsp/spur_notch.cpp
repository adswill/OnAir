// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/dsp/spur_notch.h"
#include "dect2/dsp/dsp_compat.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

namespace {
constexpr int kHalf = 24;   // local level: the median of the bins within +/-24, minus the line's own +/-3
constexpr double kPi = 3.14159265358979323846;
}

void SpurNotch::configure(double rateHz) {
    rate_ = rateHz;
    win_.assign(kFft, 0.f);
    for (int i = 0; i < kFft; i++) win_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / kFft));
    re_.assign(kFft, 0.f); im_.assign(kFft, 0.f); acc_.assign(kFft, 0.f); base_.assign(kFft, 0.f);
    period_ = (size_t)std::max<double>(kFft, rateHz * frameSec);
    memory_.clear();
    reset();
}

void SpurNotch::reset() {
    fill_ = 0; gap_ = 0; capturing_ = false; frames_ = 0;
    std::fill(acc_.begin(), acc_.end(), 0.f);
    tr_.clear(); list_.clear(); nOsc_ = 0;
    for (auto& o : osc_) o = Osc();
}

void SpurNotch::setChannel(double offsetHz, double bwHz, double rfCentreHz, double altOffsetHz) {
    chOff_ = offsetHz; chBw_ = bwHz; altOff_ = altOffsetHz;
    if (haveRf_ && std::fabs(rfCentreHz - rf_) > 2 * binHz() && rate_ > 0) {
        // retuned: the lines seen so far become memory (their baseband place and the centre they were seen under); nothing is cut until seen again
        for (const auto& t : tr_) if (t.seen >= framesToAct) { Tr m = t; m.act = false; memory_.push_back(m); }
        if (memory_.size() > 24) memory_.erase(memory_.begin(), memory_.end() - 24);
        tr_.clear(); list_.clear(); nOsc_ = 0; std::fill(acc_.begin(), acc_.end(), 0.f); frames_ = 0; fill_ = 0; gap_ = 0; capturing_ = false;
        for (auto& o : osc_) o.on = false;
    }
    rf_ = rfCentreHz; haveRf_ = true;
}

bool SpurNotch::protectedHz(double hz) const {
    if (chBw_ <= 0) return true;                                   // no channel known: only a proved spur may be cut
    const double half = chBw_ / 2 + 3 * binHz();
    if (std::fabs(hz - chOff_) <= half) return true;
    if (altOff_ != 0 && std::fabs(hz - altOff_) <= half) return true;
    return false;
}

int SpurNotch::activeCount() const { int n = 0; for (const auto& t : tr_) if (t.act) n++; return n; }

void SpurNotch::frame() {
    for (int i = 0; i < kFft; i++) { re_[(size_t)i] *= win_[(size_t)i]; im_[(size_t)i] *= win_[(size_t)i]; }
    int lg = 0; while ((1 << lg) < kFft) lg++;
    fftSplit(re_.data(), im_.data(), lg, false);
    const float al = frames_ == 0 ? 1.f : 0.2f;
    for (int k = 0; k < kFft; k++) {
        const float p = re_[(size_t)k] * re_[(size_t)k] + im_[(size_t)k] * im_[(size_t)k];
        acc_[(size_t)k] += al * (p - acc_[(size_t)k]);
    }
    frames_++;
    detect();
}

void SpurNotch::detect() {
    const int N = kFft;
    auto at = [&](int k) { return acc_[(size_t)(((k % N) + N) % N)]; };
    std::vector<float> loc;
    loc.reserve(2 * kHalf);
    std::vector<std::pair<double, float>> found;
    const double thr = std::pow(10.0, (thresholdDb - 4.0) / 10.0), thrSk = std::pow(10.0, thresholdDb / 20.0);
    for (int k = 0; k < N; k++) {
        const float p = acc_[(size_t)k];
        if (!(p > at(k - 1) && p >= at(k + 1))) continue;
        // narrow: the line falls to a smaller excess 4 bins away on both sides (Hann's main lobe is +/-2 bins)
        loc.clear();
        for (int d = -kHalf; d <= kHalf; d++) { if (std::abs(d) <= 3) continue; loc.push_back(at(k + d)); }
        std::nth_element(loc.begin(), loc.begin() + (long)loc.size() / 2, loc.end());
        const float base = std::max(loc[loc.size() / 2], 1e-20f);
        if (p < base * (float)thr) continue;
        if (at(k - 4) > base * (float)thrSk || at(k + 4) > base * (float)thrSk) continue;
        // a pure line: two bins away it is 10 dB down (Hann: a tone is under -20 dB there; a modulated carrier of a few hundred hertz, such as the 1200 bit/s channels
        // of Inmarsat Aero, is not and is never taken for a spur)
        if (at(k - 2) > 0.1f * p || at(k + 2) > 0.1f * p) continue;
        const double l0 = std::log(std::max(at(k - 1), 1e-20f)), l1 = std::log((double)p), l2 = std::log(std::max(at(k + 1), 1e-20f));
        const double den = l0 - 2 * l1 + l2;
        const double dl = std::fabs(den) > 1e-9 ? 0.5 * (l0 - l2) / den : 0.0;
        double bin = k + std::max(-0.5, std::min(0.5, dl));
        if (bin >= N / 2) bin -= N;
        const double hz = bin * rate_ / N;
        if (std::fabs(hz) < 3 * binHz()) continue;                  // DC: the DC removal's job
        found.push_back({hz, 10 * std::log10(p / base)});
    }
    for (auto& t : tr_) t.missed++;
    for (const auto& f : found) {
        Tr* hit = nullptr;
        for (auto& t : tr_) if (std::fabs(t.hz - f.first) < 1.5 * binHz()) { hit = &t; break; }
        if (hit) {
            hit->hz += 0.3 * (f.first - hit->hz); hit->dB = f.second; hit->seen++; hit->missed = 0;
        } else if (f.second >= thresholdDb && (int)tr_.size() < 4 * kMax) {   // a new line must reach the threshold; a tracked one is kept 4 dB under it
            Tr t; t.hz = f.first; t.dB = f.second; t.seen = 1; t.rf = rf_;
            tr_.push_back(t);
        }
    }
    tr_.erase(std::remove_if(tr_.begin(), tr_.end(), [&](const Tr& t) { return t.missed > 12 || (t.seen < framesToAct && t.missed > 3); }), tr_.end());
    // decide: a line is cut only when proved instrumental: the same baseband place was a line under another radio centre (a signal of the air moves with
    // the tuner, a spur of the radio does not). Never on persistence alone: modes that watch the whole capture (Inmarsat Aero's other P channels, a
    // TETRA / HFDL carrier set) have wanted lines anywhere (tests/engine/test_iq_modes.cpp caught one at +97 kHz); a signal inside the channel is
    // therefore never cut unless proved either
    for (auto& t : tr_) {
        if (t.seen < framesToAct) { t.act = false; continue; }
        if (!t.conf) {
            for (const auto& m : memory_)
                if (std::fabs(m.hz - t.hz) < 1.5 * binHz() && std::fabs(m.rf - rf_) > 3 * binHz()) { t.conf = true; break; }
        }
        t.act = t.conf;
    }
    // strongest first, at most kMax cut
    std::vector<Tr*> act;
    for (auto& t : tr_) if (t.act) act.push_back(&t);
    std::sort(act.begin(), act.end(), [](const Tr* a, const Tr* b) { return a->dB > b->dB; });
    for (size_t i = kMax; i < act.size(); i++) act[i]->act = false;
    act.resize(std::min<size_t>(act.size(), kMax));
    // map to the oscillators (keep a notch's state when its line is still the same one)
    Osc nw[kMax];
    int n = 0;
    for (auto* t : act) {
        const double w = 2 * kPi * t->hz / rate_;
        int reuse = -1;
        for (int i = 0; i < nOsc_; i++) if (osc_[i].on && std::fabs(osc_[i].w - w) < 2 * kPi * 2.0 * binHz() / rate_) { reuse = i; break; }
        if (reuse >= 0) { nw[n] = osc_[reuse]; nw[n].w = w; }
        else { nw[n] = Osc(); nw[n].w = w; nw[n].on = true; }
        n++;
    }
    for (int i = 0; i < kMax; i++) osc_[i] = i < n ? nw[i] : Osc();
    nOsc_ = n;
    a_ = (float)std::min(0.5, 2 * kPi * (0.5 * binHz()) / rate_);
    list_.clear();
    for (const auto& t : tr_) {
        Spur s; s.hz = t.hz; s.dB = t.dB; s.inChannel = protectedHz(t.hz); s.confirmed = t.conf; s.active = t.act;
        if (t.seen >= framesToAct) list_.push_back(s);
    }
}

void SpurNotch::process(cf32* x, size_t n) {
    if (rate_ <= 0 || !n) return;
    // analysis: a block of kFft samples about every 0.2 s (the samples are only read)
    for (size_t i = 0; i < n; i++) {
        if (capturing_) {
            re_[fill_] = x[i].real(); im_[fill_] = x[i].imag();
            if (++fill_ == (size_t)kFft) { capturing_ = false; fill_ = 0; gap_ = 0; frame(); }
        } else if (++gap_ >= period_) { capturing_ = true; fill_ = 0; }
    }
    // the cut: per line mix to DC, follow the mean, subtract it mixed back
    for (int k = 0; k < nOsc_; k++) {
        Osc& o = osc_[k];
        const std::complex<double> st = std::polar(1.0, o.w);
        std::complex<double> p = std::polar(1.0, o.ph);
        const cf32 stf((float)st.real(), (float)st.imag());
        cf32 pf((float)p.real(), (float)p.imag());
        cf32 m = o.m;
        for (size_t i = 0; i < n; i++) {
            m += a_ * (x[i] * std::conj(pf) - m);
            x[i] -= m * pf;
            pf *= stf;
            if ((i & 1023) == 1023) pf /= std::abs(pf);
        }
        pf /= std::abs(pf);
        o.m = m; o.ph = std::arg(std::complex<double>(pf.real(), pf.imag()));
    }
}

} // namespace dect2
