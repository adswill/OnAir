// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "web_dsp.h"
#include "dect2/common/window.h"
#include "dect2/dsp/dsp_compat.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <tuple>

namespace onair_web {

static int log2i(int n) { int l = 0; while ((1 << l) < n) l++; return l; }
static int pow2AtLeast(double v) { int p = 1; while (p < v && p < (1 << 24)) p <<= 1; return p; }

// ---------------------------------------------------------------- SharedFft
bool SharedFft::configure(double rateHz) {
    if (!(rateHz >= 8000 && rateHz <= 200e6)) return false;
    rate_ = rateHz;
    n_ = std::clamp(pow2AtLeast(rateHz / 150.0), 4096, 262144);
    log2n_ = log2i(n_);
    hop_ = n_ - n_ / 4;
    buf_.assign((size_t)n_, cf32(0, 0));
    re_.assign((size_t)n_, 0.f); im_.assign((size_t)n_, 0.f);
    fill_ = n_ / 4;   // the first block starts with a quarter of zeros (the overlap of the block before)
    block_ = 0;
    return true;
}

void SharedFft::transform() {
    const float s = 1.f / (float)n_;
    for (int i = 0; i < n_; i++) { re_[(size_t)i] = buf_[(size_t)i].real() * s; im_[(size_t)i] = buf_[(size_t)i].imag() * s; }
    dect2::fftSplit(re_.data(), im_.data(), log2n_, false);
}

void SharedFft::shift() {
    const int v = n_ / 4;
    std::copy(buf_.end() - v, buf_.end(), buf_.begin());
    fill_ = v;
}

// ---------------------------------------------------------------- slices
bool planSlice(const SharedFft& f, double offsetHz, double outRate, SlicePlan& p, std::string& err) {
    if (f.size() <= 0) { err = "no capture"; return false; }
    if (!(outRate > 0)) { err = "bad rate"; return false; }
    const double bin = f.binHz();
    if (std::fabs(offsetHz) > f.rate() * 0.5) { err = "frequency outside the capture"; return false; }
    p.n = f.size(); p.hop = f.hop();
    p.m = std::max(8, pow2AtLeast(2.0 * outRate / bin));
    if (p.m > p.n) p.m = p.n;
    p.sliceRate = f.rate() * p.m / p.n;
    if (p.sliceRate < outRate) { err = "the capture is too narrow for this mode"; return false; }
    p.k0 = (int)std::lround(offsetHz / bin);
    p.residualHz = offsetHz - p.k0 * bin;
    return true;
}

static std::vector<float> lowpass(double cutoff, double rate, int taps, double beta);

// The slice's filter as M complex bins: a real Kaiser low-pass of V+1 taps at the capture's rate (V = N/4, the overlap, so the overlap-save is exact:
// no wrap-around at the block edges), cut-off between half the output rate and the point that would fold back after resampling; its N-point
// transform, of which the M bins around 0 are kept (the rest is below -70 dB). Cached: it depends only on N, the rate and the output rate.
static std::shared_ptr<const std::vector<cf32>> sliceResponse(int n, int m, double rate, double outRate, double sliceRate) {
    static std::mutex mu;
    static std::map<std::tuple<int, int, long long, long long>, std::shared_ptr<const std::vector<cf32>>> cache;
    const auto key = std::make_tuple(n, m, (long long)std::llround(rate), (long long)std::llround(outRate));
    {
        std::lock_guard<std::mutex> g(mu);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }
    auto h = std::make_shared<std::vector<cf32>>((size_t)m, cf32(1, 0));
    if (m < n) {
        const int v = n / 4;
        const double stop = std::min(0.72 * outRate, 0.5 * sliceRate);
        const double cut = 0.5 * (0.5 * outRate + stop);
        const std::vector<float> lp = lowpass(cut, rate, v + 1, 7.0);
        std::vector<float> re((size_t)n, 0.f), im((size_t)n, 0.f);
        for (int k = 0; k <= v; k++) re[(size_t)k] = lp[(size_t)k];
        dect2::fftSplit(re.data(), im.data(), log2i(n), false);
        for (int j = 0; j < m; j++) {
            const int rel = j < m / 2 ? j : j - m;
            const int src = (rel + n) % n;
            (*h)[(size_t)j] = cf32(re[(size_t)src], im[(size_t)src]);
        }
    }
    std::lock_guard<std::mutex> g(mu);
    if (cache.size() > 64) cache.clear();
    cache[key] = h;
    return h;
}

bool SliceIq::configure(const SharedFft& f, double offsetHz, double outRate, std::string& err) {
    if (!planSlice(f, offsetHz, outRate, p_, err)) return false;
    outRate_ = outRate;
    log2m_ = log2i(p_.m);
    keepFrom_ = p_.m / 4;   // the overlap (N/4 input samples) in slice samples
    h_ = sliceResponse(p_.n, p_.m, f.rate(), outRate, p_.sliceRate);
    yr_.assign((size_t)p_.m, 0.f); yi_.assign((size_t)p_.m, 0.f);
    if (!fine_.configure(p_.sliceRate, p_.residualHz, outRate)) { err = "resampler"; return false; }
    return true;
}

void SliceIq::process(const float* re, const float* im, uint64_t blockNo, std::vector<cf32>& out) {
    const int n = p_.n, m = p_.m;
    const int k0 = ((p_.k0 % n) + n) % n;
    for (int j = 0; j < m; j++) {
        const int rel = j < m / 2 ? j : j - m;
        const int src = (k0 + rel + n) % n;
        const cf32 h = (*h_)[(size_t)j];
        yr_[(size_t)j] = re[src] * h.real() - im[src] * h.imag();
        yi_[(size_t)j] = re[src] * h.imag() + im[src] * h.real();
    }
    dect2::fftSplit(yr_.data(), yi_.data(), log2m_, true);
    // shifting the spectrum by k0 bins mixes each block by exp(-j 2 pi k0 n / N) from the block's own start: put back the phase of the block's
    // absolute start (block * hop samples), exact in integers so it never drifts
    const uint64_t a = ((blockNo % (uint64_t)n) * (uint64_t)p_.hop) % (uint64_t)n;
    const uint64_t idx = (a * (uint64_t)k0) % (uint64_t)n;
    const double ang = -2.0 * M_PI * (double)idx / (double)n;
    const float cr = (float)std::cos(ang), ci = (float)std::sin(ang);
    tmp_.resize((size_t)(m - keepFrom_));
    for (int t = keepFrom_; t < m; t++) {
        const float xr = yr_[(size_t)t], xi = yi_[(size_t)t];
        tmp_[(size_t)(t - keepFrom_)] = cf32(xr * cr - xi * ci, xr * ci + xi * cr);
    }
    fine_.process(tmp_.data(), tmp_.size(), out);
}

// ---------------------------------------------------------------- VirtualRx
void defaultPassband(Demod d, double& lo, double& hi) {
    switch (d) {
    case Demod::Am: lo = -4500; hi = 4500; break;
    case Demod::Nfm: lo = -6250; hi = 6250; break;
    case Demod::Wfm: lo = -100000; hi = 100000; break;
    case Demod::Usb: lo = 100; hi = 2800; break;
    case Demod::Lsb: lo = -2800; hi = -100; break;
    case Demod::Cw: lo = -250; hi = 250; break;
    default: lo = -5000; hi = 5000; break;
    }
}
void passbandLimits(Demod d, double& lo, double& hi) {
    switch (d) {
    case Demod::Am: lo = -10000; hi = 10000; break;
    case Demod::Nfm: lo = -10000; hi = 10000; break;
    case Demod::Wfm: lo = -110000; hi = 110000; break;
    case Demod::Usb: lo = -1000; hi = 10000; break;
    case Demod::Lsb: lo = -10000; hi = 1000; break;
    case Demod::Cw: lo = -1500; hi = 1500; break;
    default: lo = -10000; hi = 10000; break;
    }
}

static std::vector<float> lowpass(double cutoff, double rate, int taps, double beta) {
    std::vector<float> h((size_t)taps);
    const double fc = cutoff / rate, i0b = dect2::window::besselI0(beta);
    const int c = taps / 2;
    double sum = 0;
    for (int k = 0; k < taps; k++) {
        const double x = k - c;
        const double r = x / (c ? c : 1);
        const double w = dect2::window::besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        const double s = x == 0 ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x);
        h[(size_t)k] = (float)(s * w);
        sum += s * w;
    }
    for (float& v : h) v = (float)(v / sum);
    return h;
}

bool VirtualRx::configure(const SharedFft& f, const RxSettings& sIn, std::string& err) {
    RxSettings s = sIn;
    if (s.lowHz == 0 && s.highHz == 0) defaultPassband(s.demod, s.lowHz, s.highHz);
    double lmin, lmax;
    passbandLimits(s.demod, lmin, lmax);
    s.lowHz = std::clamp(s.lowHz, lmin, lmax);
    s.highHz = std::clamp(s.highHz, lmin, lmax);
    if (s.highHz - s.lowHz < 50) { err = "passband too narrow for this mode"; return false; }
    if (s.audioRate < 8000 || s.audioRate > 48000) { err = "bad audio rate"; return false; }
    chRate_ = demodRate(s.demod);
    if (!slice_.configure(f, s.offsetHz, chRate_, err)) return false;
    s_ = s;
    if (s.demod != Demod::Wfm) {
        fc_ = 0.5 * (s.lowHz + s.highHz);
        const double half = 0.5 * (s.highHz - s.lowHz);
        const double tw = std::clamp(0.1 * (s.highHz - s.lowHz), 150.0, 2000.0);
        int nt = (int)std::ceil(3.6 * chRate_ / tw) | 1;
        nt = std::min(nt, 511);
        taps_ = lowpass(half + 0.5 * tw, chRate_, nt, 6.0);
        hist_.assign(2 * taps_.size(), cf32(0, 0));
        histPos_ = 0;
        ncoStep_ = 2 * M_PI * fc_ / chRate_;
        ncoPh_ = 0;
        bfoStep_ = s.demod == Demod::Cw ? 2 * M_PI * 700.0 / chRate_ : 0;
        if (!audioRs_.configure(chRate_, s.audioRate, 8)) { err = "audio resampler"; return false; }
    } else {
        wfmTaps_ = lowpass(15500, chRate_, 95, 6.0);
        wfmHist_.assign(2 * wfmTaps_.size(), 0.f);
        wfmPhase_ = 0;
        histPos_ = 0;
        deA_ = s.deemphUs > 0 ? (float)(1.0 - std::exp(-1.0 / (48000.0 * s.deemphUs * 1e-6))) : 1.f;
        if (!audioRs_.configure(48000, s.audioRate, 8)) { err = "audio resampler"; return false; }
    }
    prev_ = cf32(1, 0);
    dcAcc_ = 0; deState_ = 0; agcEnv_ = 1e-3f; levelDb_ = -200; sqAbove_ = 0; sqClosed_ = s.squelchDb > -150;   // a set squelch starts closed
    return true;
}

void VirtualRx::process(const float* re, const float* im, uint64_t blockNo, std::vector<float>& audio) {
    iq_.clear();
    slice_.process(re, im, blockNo, iq_);
    if (iq_.empty()) return;
    demod_.clear();
    double pw = 0;
    const Demod d = s_.demod;
    const float fixedGain = (float)std::pow(10.0, s_.gainDb / 20.0);
    if (d == Demod::Wfm) {
        const float k = (float)(chRate_ / (2 * M_PI * 75000.0));
        const size_t T = wfmTaps_.size();
        for (const cf32& x : iq_) {
            pw += (double)std::norm(x);
            const cf32 p = x * std::conj(prev_);
            prev_ = x;
            const float v = std::atan2(p.imag(), p.real()) * k;
            wfmHist_[histPos_] = v; wfmHist_[histPos_ + T] = v;
            histPos_ = (histPos_ + 1) % T;
            if (++wfmPhase_ == 5) {   // 240 kHz -> 48 kHz
                wfmPhase_ = 0;
                float acc = 0;
                const float* hp = wfmHist_.data() + histPos_;
                for (size_t t = 0; t < T; t++) acc += wfmTaps_[t] * hp[t];
                deState_ += deA_ * (acc - deState_);
                demod_.push_back(cf32(deState_ * fixedGain, 0));
            }
        }
    } else {
        const size_t T = taps_.size();
        const float slowDecay = s_.agc == AgcMode::Fast ? 0.9995f : 0.99993f;
        for (const cf32& x : iq_) {
            // into the passband's centre, the real low-pass, and back
            const cf32 lo((float)std::cos(ncoPh_), (float)-std::sin(ncoPh_));
            const cf32 z = x * lo;
            hist_[histPos_] = z; hist_[histPos_ + T] = z;
            histPos_ = (histPos_ + 1) % T;
            float ar = 0, ai = 0;
            const cf32* hp = hist_.data() + histPos_;
            for (size_t t = 0; t < T; t++) { ar += taps_[t] * hp[t].real(); ai += taps_[t] * hp[t].imag(); }
            cf32 y(ar, ai);
            pw += (double)std::norm(y);
            y *= std::conj(lo);
            ncoPh_ += ncoStep_;
            if (ncoPh_ > M_PI) ncoPh_ -= 2 * M_PI; else if (ncoPh_ < -M_PI) ncoPh_ += 2 * M_PI;
            float v;
            if (d == Demod::Nfm) {
                const cf32 p = y * std::conj(prev_);
                prev_ = y;
                v = std::atan2(p.imag(), p.real()) * (float)(chRate_ / (2 * M_PI * 5000.0)) * 0.5f * fixedGain;
            } else {
                if (d == Demod::Am) v = std::abs(y);
                else if (d == Demod::Cw) {
                    v = (y * cf32((float)std::cos(bfoPh_), (float)std::sin(bfoPh_))).real();
                    bfoPh_ += bfoStep_;
                    if (bfoPh_ > M_PI) bfoPh_ -= 2 * M_PI;
                } else v = y.real();   // USB / LSB: the filter kept one side
                dcAcc_ += (v - dcAcc_) * 0.0005f;
                if (d == Demod::Am) v -= dcAcc_;
                if (s_.agc == AgcMode::Off) v *= fixedGain * 4.f;
                else {
                    const float a = std::fabs(v);
                    if (a > agcEnv_) agcEnv_ += (a - agcEnv_) * 0.05f; else agcEnv_ *= slowDecay;
                    agcEnv_ = std::max(agcEnv_, 1e-6f);
                    v *= std::min(0.3f / agcEnv_, 1000.f);
                }
            }
            demod_.push_back(cf32(v, 0));
        }
    }
    const float lvl = (float)(10.0 * std::log10(pw / (double)iq_.size() + 1e-20));
    levelDb_ = levelDb_ < -199 ? lvl : levelDb_ + 0.3f * (lvl - levelDb_);
    if (s_.squelchDb <= -150) sqClosed_ = false;
    else if (sqClosed_) { sqAbove_ = levelDb_ > s_.squelchDb + 1.5 ? sqAbove_ + 1 : 0; if (sqAbove_ >= 3) sqClosed_ = false; }   // 3 blocks above: not a start-up click
    else if (levelDb_ < s_.squelchDb - 1.5) { sqClosed_ = true; sqAbove_ = 0; }
    aout_.clear();
    audioRs_.process(demod_.data(), demod_.size(), aout_);
    const size_t base = audio.size();
    audio.resize(base + aout_.size());
    for (size_t i = 0; i < aout_.size(); i++) audio[base + i] = sqClosed_ ? 0.f : std::clamp(aout_[i].real(), -1.f, 1.f);
}

// ---------------------------------------------------------------- Waterfall
bool Waterfall::configure(double rateHz, int fps) {
    if (!(rateHz >= 8000) || fps < 1) return false;
    rate_ = rateHz;
    n_ = std::clamp(pow2AtLeast(rateHz / 250.0), 2048, 32768);
    log2n_ = log2i(n_);
    every_ = std::max(n_, (int)(rateHz / std::min(fps, 30)));
    win_.resize((size_t)n_);
    double ws = 0;
    for (int i = 0; i < n_; i++) { win_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / n_)); ws += win_[(size_t)i]; }
    for (float& w : win_) w = (float)(w / ws);   // a full-scale tone gives 0 dBFS
    re_.assign((size_t)n_, 0.f); im_.assign((size_t)n_, 0.f);
    acc_.assign((size_t)n_, 0.f); db_.assign((size_t)n_, -200.f);
    buf_.assign((size_t)n_, cf32(0, 0));
    fill_ = 0; nAcc_ = 0; gap_ = 0; seq_ = 0;
    return true;
}

void Waterfall::push(const cf32* x, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (gap_ > 0) { const size_t s = std::min(n - i, (size_t)gap_); gap_ -= (long)s; i += s; continue; }
        const size_t take = std::min(n - i, (size_t)(n_ - fill_));
        std::copy(x + i, x + i + take, buf_.begin() + fill_);
        fill_ += (int)take; i += take;
        if (fill_ < n_) break;
        for (int k = 0; k < n_; k++) { re_[(size_t)k] = buf_[(size_t)k].real() * win_[(size_t)k]; im_[(size_t)k] = buf_[(size_t)k].imag() * win_[(size_t)k]; }
        dect2::fftSplit(re_.data(), im_.data(), log2n_, false);
        const int h = n_ / 2;
        for (int k = 0; k < n_; k++) {
            const int src = (k + h) % n_;   // DC in the middle
            const float p = re_[(size_t)src] * re_[(size_t)src] + im_[(size_t)src] * im_[(size_t)src];
            db_[(size_t)k] = 10.f * std::log10(p + 1e-20f);
        }
        seq_++;
        fill_ = 0;
        gap_ = every_ - n_;
    }
}

void Waterfall::row(double startHz, double stopHz, int bins, std::vector<float>& out) const {
    out.assign((size_t)std::max(bins, 0), -200.f);
    if (n_ <= 0 || bins <= 0) return;
    const double half = rate_ * 0.5;
    startHz = std::clamp(startHz, -half, half);
    stopHz = std::clamp(stopHz, -half, half);
    if (stopHz <= startHz) return;
    const double binHz = rate_ / n_;
    const double per = (stopHz - startHz) / bins;
    for (int j = 0; j < bins; j++) {
        const double f0 = startHz + j * per, f1 = f0 + per;
        const double s0 = f0 / binHz + n_ / 2, s1 = f1 / binHz + n_ / 2;
        if (s1 - s0 >= 1.0) {
            int a = std::clamp((int)std::floor(s0), 0, n_ - 1), b = std::clamp((int)std::ceil(s1) - 1, 0, n_ - 1);
            float mx = -200.f;
            for (int k = a; k <= b; k++) mx = std::max(mx, db_[(size_t)k]);
            out[(size_t)j] = mx;
        } else {
            const double c = 0.5 * (s0 + s1) - 0.5;
            const int a = std::clamp((int)std::floor(c), 0, n_ - 1), b = std::min(a + 1, n_ - 1);
            const float t = (float)std::clamp(c - a, 0.0, 1.0);
            out[(size_t)j] = db_[(size_t)a] * (1 - t) + db_[(size_t)b] * t;
        }
    }
}

void quantizeRow(const std::vector<float>& db, RowScale& sc, std::vector<uint8_t>& out) {
    out.resize(db.size());
    if (db.empty()) return;
    std::vector<float> tmp(db);
    const size_t k = tmp.size() / 5;
    std::nth_element(tmp.begin(), tmp.begin() + (long)k, tmp.end());
    const float noise = std::max(tmp[k], -190.f);
    const float target = noise - 10.f;
    if (!sc.init) { sc.floorDb = target; sc.init = true; }
    else sc.floorDb += 0.1f * (target - sc.floorDb);
    sc.topDb = sc.floorDb + 100.f;
    const float span = sc.topDb - sc.floorDb;
    for (size_t i = 0; i < db.size(); i++) {
        const float v = (db[i] - sc.floorDb) / span * 255.f;
        out[i] = (uint8_t)std::clamp((int)std::lround(v), 0, 255);
    }
}

} // namespace onair_web
