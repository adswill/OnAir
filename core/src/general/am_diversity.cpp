// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/general/am_diversity.h"
#include "dect2/audio/audioout.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

void FadeTracker::configure(double r) { rate_ = r; reset(); }
void FadeTracker::reset() { h_.clear(); depth_ = 0; period_ = 0; next_ = -1; }
bool FadeTracker::bestMoment() const {
    if (h_.size() < 8 || depth_ < 3) return false;
    return h_.back() >= *std::max_element(h_.begin(), h_.end()) - 0.2f * depth_;
}

void FadeTracker::push(float db) {
    h_.push_back(db);
    const size_t cap = (size_t)(60 * rate_);
    while (h_.size() > cap) h_.pop_front();
    const size_t n = h_.size();
    if (n < (size_t)(10 * rate_)) return;
    std::vector<float> v(h_.begin(), h_.end());
    double mean = 0; for (float x : v) mean += x;
    mean /= (double)n;
    std::vector<float> s(v); std::sort(s.begin(), s.end());
    depth_ = s[n * 9 / 10] - s[n / 10];
    if (depth_ < 3) { period_ = 0; next_ = -1; return; }
    // the autocorrelation of the level, between 1 s and half the history; the first strong peak after a dip through zero
    const int lo = std::max(2, (int)rate_), hi = (int)(n / 2);
    std::vector<double> ac((size_t)hi + 1, 0.0);
    double e = 0;
    for (size_t i = 0; i < n; i++) e += (v[i] - mean) * (v[i] - mean);
    for (int k = 1; k <= hi; k++) { double c = 0; for (size_t i = (size_t)k; i < n; i++) c += (v[i] - mean) * (v[i - (size_t)k] - mean); ac[(size_t)k] = e > 0 ? c / e : 0; }
    int best = 0; double bv = 0.3;
    bool below = false;
    for (int k = 1; k < hi; k++) {
        if (ac[(size_t)k] < 0) below = true;
        if (below && k >= lo && ac[(size_t)k] > bv && ac[(size_t)k] >= ac[(size_t)k - 1] && ac[(size_t)k] >= ac[(size_t)k + 1]) { best = k; bv = ac[(size_t)k]; }
    }
    period_ = best ? (float)(best / rate_) : 0;
    next_ = -1;
    if (best) {
        // the last dip: the minimum within the last period
        int idx = 0; float mn = 1e9f;
        for (int k = 0; k < best && k < (int)n; k++) { const float y = v[n - 1 - (size_t)k]; if (y < mn) { mn = y; idx = k; } }
        const double since = idx / rate_;
        next_ = (float)std::max(0.0, period_ - since);
    }
}

struct AmDiversity::Impl {
    int rate = 48000, decim = 6, N = 1 << 13;
    std::vector<float> ra, rb, da, db_;
    std::deque<float> qa, qb;
    int delay = 0;                  // of channel 1 against 0 (samples)
    int act = 0, want = 0, fade = 0, nsw = 0;
    float snr[2] = {0, 0};
    uint64_t sinceSwitch = 0, sinceCorr = 0;
    Fft fft{1 << 13};
    std::vector<cf32> fa, fb;

    void reset() { ra.clear(); rb.clear(); qa.clear(); qb.clear(); delay = 0; act = 0; want = 0; fade = 0; nsw = 0; snr[0] = snr[1] = 0; sinceSwitch = 0; sinceCorr = 0; }

    void corr() {
        const size_t need = (size_t)N * (size_t)decim;
        if (qa.size() < need || qb.size() < need) return;
        fa.assign((size_t)N, cf32(0, 0)); fb.assign((size_t)N, cf32(0, 0));
        // decimated by averaging: the speech band is enough
        for (int k = 0; k < N; k++) {
            float s0 = 0, s1 = 0;
            for (int j = 0; j < decim; j++) { s0 += qa[qa.size() - need + (size_t)k * (size_t)decim + (size_t)j]; s1 += qb[qb.size() - need + (size_t)k * (size_t)decim + (size_t)j]; }
            fa[(size_t)k] = cf32(s0, 0); fb[(size_t)k] = cf32(s1, 0);
        }
        fft.forward(fa.data()); fft.forward(fb.data());
        for (int k = 0; k < N; k++) fa[(size_t)k] = std::conj(fa[(size_t)k]) * fb[(size_t)k];
        fft.inverse(fa.data());
        // the peak within +-1 s (decimated rate 8 kHz): peak over the mean, or no change
        const int lim = std::min(N / 2 - 1, rate / decim);
        int bi = 0; float bv = 0; double sum = 0; int n = 0;
        for (int k = -lim; k <= lim; k++) {
            const float v = std::abs(fa[(size_t)((k + N) % N)]);
            sum += v; n++;
            if (v > bv) { bv = v; bi = k; }
        }
        if (bv > 5 * sum / n) delay = bi * decim;
    }

    void process(const float* a, const float* b, size_t n, float snrA, float snrB, std::vector<float>& out) {
        qa.insert(qa.end(), a, a + n); qb.insert(qb.end(), b, b + n);
        const double al = std::min(1.0, (double)n / (rate * 3.0));
        snr[0] += (snrA - snr[0]) * (float)al; snr[1] += (snrB - snr[1]) * (float)al;
        sinceCorr += n;
        if (sinceCorr >= (uint64_t)rate / 2) { sinceCorr = 0; corr(); }
        // read position: rate samples (1 s) behind the newest, on each channel with the alignment applied
        const size_t hold = (size_t)N * (size_t)decim + (size_t)rate / 20 + (size_t)std::abs(delay);
        const size_t cf = (size_t)(0.05 * rate);
        while (qa.size() > hold && qb.size() > hold + 0) {
            const int oa = delay > 0 ? 0 : 0;
            (void)oa;
            size_t ia = 0, ib = 0;
            if (delay > 0) ib = (size_t)delay; else ia = (size_t)(-delay);
            if (ia >= qa.size() || ib >= qb.size()) break;
            // switch decision: 3 dB better, at least 2 s since the last change
            if (sinceSwitch > (uint64_t)2 * rate && !fade) {
                const int best = snr[1] > snr[0] + 3 ? 1 : snr[0] > snr[1] + 3 ? 0 : act;
                if (best != act) { want = best; fade = (int)cf; nsw++; sinceSwitch = 0; }
            }
            const float x0 = qa[ia], x1 = qb[ib];
            float v;
            if (fade > 0) {
                const float t = 1.f - (float)fade / (float)cf;
                const float from = act == 0 ? x0 : x1, to = want == 0 ? x0 : x1;
                v = from * (1 - t) + to * t;
                if (--fade == 0) act = want;
            } else v = act == 0 ? x0 : x1;
            out.push_back(v);
            qa.pop_front(); qb.pop_front();
            sinceSwitch++;
        }
    }
};

AmDiversity::AmDiversity() : p_(std::make_unique<Impl>()) {}
AmDiversity::~AmDiversity() = default;
void AmDiversity::configure(int rate) { p_->rate = rate; p_->decim = std::max(1, rate / 8000); p_->reset(); }
void AmDiversity::reset() { p_->reset(); }
void AmDiversity::process(const float* a, const float* b, size_t n, float sa, float sb, std::vector<float>& out) { p_->process(a, b, n, sa, sb, out); }
int AmDiversity::active() const { return p_->act; }
int AmDiversity::delaySamples() const { return p_->delay; }
int AmDiversity::switches() const { return p_->nsw; }

AmDivPlayer::AmDivPlayer() = default;
AmDivPlayer::~AmDivPlayer() { stop(); }

void AmDivPlayer::start(int rate, bool silent) {
    std::lock_guard<std::mutex> lk(mu_);
    div_.configure(rate); a_.clear(); b_.clear(); outputs_ = 0; running_ = true;
    if (!silent) { audio_ = std::make_unique<AudioOut>(); audio_->start(rate); audio_->setVolume(vol_); audio_->setStartThreshold(rate / 5); }
}
void AmDivPlayer::stop() {
    std::lock_guard<std::mutex> lk(mu_);
    running_ = false; audio_.reset(); a_.clear(); b_.clear();
}
void AmDivPlayer::setVolume(float v) { std::lock_guard<std::mutex> lk(mu_); vol_ = v; if (audio_) audio_->setVolume(v); }
void AmDivPlayer::pushA(const float* x, size_t n) { std::lock_guard<std::mutex> lk(mu_); if (running_) { a_.insert(a_.end(), x, x + n); pump(); } }
void AmDivPlayer::pushB(const float* x, size_t n) { std::lock_guard<std::mutex> lk(mu_); if (running_) { b_.insert(b_.end(), x, x + n); pump(); } }
void AmDivPlayer::pump() {
    const size_t n = std::min(a_.size(), b_.size());
    if (n < 480) return;
    out_.clear();
    div_.process(a_.data(), b_.data(), n, snrA_.load(), snrB_.load(), out_);
    a_.erase(a_.begin(), a_.begin() + (std::ptrdiff_t)n);
    b_.erase(b_.begin(), b_.begin() + (std::ptrdiff_t)n);
    if (out_.empty()) return;
    outputs_ += out_.size();
    if (audio_) {
        std::vector<float> st(out_.size() * 2);
        for (size_t i = 0; i < out_.size(); i++) st[2 * i] = st[2 * i + 1] = out_[i];
        audio_->write(st.data(), (int)out_.size());
    }
}
int AmDivPlayer::active() const { return div_.active(); }
int AmDivPlayer::switches() const { return div_.switches(); }
int AmDivPlayer::delaySamples() const { return div_.delaySamples(); }

} // namespace dect2
