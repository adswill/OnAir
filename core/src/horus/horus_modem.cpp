// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Project Horus receiver internals (horus_modem.h): channel filter, tone estimator, 4FSK and RTTY demodulators, deframer, wide detector.
// Written for OnAir; the demodulator design is our own (non-coherent correlators, energy-peak symbol clock), not the codec2 modem.
#include "horus_modem.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <numeric>

namespace dect2::horus {
namespace {
constexpr double kPi = 3.14159265358979323846;

double besselI0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-12 * s) break; }
    return s;
}
// Kaiser windowed sinc, unity gain at DC; fc in cycles per sample
std::vector<float> kaiserLowpass(int taps, double fc, double beta) {
    std::vector<float> h((size_t)taps);
    const double m = (taps - 1) / 2.0, i0b = besselI0(beta);
    double sum = 0;
    for (int i = 0; i < taps; i++) {
        const double x = i - m, s = x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
        const double r = m > 0 ? x / m : 0, w = besselI0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0b;
        h[(size_t)i] = (float)(s * w); sum += s * w;
    }
    for (auto& v : h) v = (float)(v / sum);
    return h;
}
int largestDivisorUpTo(int n, int cap) { for (int d = std::min(n, cap); d > 1; d--) if (n % d == 0) return d; return 1; }
float db(double p) { return (float)(10 * std::log10(p + 1e-30)); }
} // namespace

// ---------------------------------------------------------------- FirDecim
void FirDecim::init(std::vector<float> taps, int decim) { h_ = std::move(taps); std::reverse(h_.begin(), h_.end()); d_ = std::max(1, decim); reset(); }
void FirDecim::reset() { buf_.assign(h_.size() > 0 ? h_.size() - 1 : 0, cf32(0, 0)); pos_ = 0; }
void FirDecim::process(const cf32* in, size_t n, std::vector<cf32>& out) {
    buf_.insert(buf_.end(), in, in + n);
    const size_t L = h_.size();
    while (pos_ + L <= buf_.size()) {
        float re = 0, im = 0;
        const cf32* x = buf_.data() + pos_;
        for (size_t k = 0; k < L; k++) { re += h_[k] * x[k].real(); im += h_[k] * x[k].imag(); }
        out.push_back(cf32(re, im));
        pos_ += (size_t)d_;
    }
    if (pos_ > 8192) { buf_.erase(buf_.begin(), buf_.begin() + (ptrdiff_t)pos_); pos_ = 0; }
}

// ---------------------------------------------------------------- Cic2
void Cic2::init(int d) { d_ = std::max(1, d); reset(); }
void Cic2::reset() { hist_.assign((size_t)d_, 0.0); integ_ = 0; acc_ = 0; pos_ = 0; cnt_ = 0; }
void Cic2::process(const cf32* in, size_t n, std::vector<cf32>& out) {
    if (d_ == 1) { out.insert(out.end(), in, in + n); return; }
    const double g = 1.0 / ((double)d_ * d_);
    for (size_t i = 0; i < n; i++) {
        integ_ += std::complex<double>(in[i].real(), in[i].imag());
        const std::complex<double> s1 = integ_ - hist_[(size_t)pos_];   // the sum of the last d inputs
        hist_[(size_t)pos_] = integ_;
        if (++pos_ == d_) pos_ = 0;
        acc_ += s1;
        if (++cnt_ == d_) { out.push_back(cf32((float)(acc_.real() * g), (float)(acc_.imag() * g))); acc_ = 0; cnt_ = 0; }
    }
}

// ---------------------------------------------------------------- ChannelFilter
void ChannelFilter::init(double inRate) {
    inRate_ = inRate;
    // an output rate that divides the input exactly, 100 baud then has a whole number of samples per symbol
    int D = 0;
    for (double c : {10000.0, 9600.0, 12000.0, 8000.0, 16000.0}) {
        const double r = inRate / c;
        if (r >= 1 && std::fabs(r - std::round(r)) < 1e-9) { D = (int)std::lround(r); break; }
    }
    if (D == 0) D = std::max(1, (int)std::lround(inRate / 10000.0));
    outRate_ = inRate / D;
    const int D2 = largestDivisorUpTo(D, 10), D1 = D / D2;
    s1_.init(D1);   // nulls at multiples of the intermediate rate, where the aliases would come from
    const double fs1 = inRate / D1, pass = 0.40 * outRate_, stop = 0.52 * outRate_;
    int taps = (int)std::ceil(3.6 * fs1 / (stop - pass)) | 1;
    taps = std::clamp(taps, 31, 801) | 1;
    s2_.init(kaiserLowpass(taps, (pass + stop) / 2 / fs1, 6.0), D2);
    setMixer(mixHz_);
}
void ChannelFilter::reset() { s1_.reset(); s2_.reset(); ph_ = 0; }
void ChannelFilter::setMixer(double hz) { mixHz_ = hz; dph_ = inRate_ > 0 ? -2 * kPi * hz / inRate_ : 0; }
void ChannelFilter::process(const cf32* in, size_t n, std::vector<cf32>& out) {
    mixed_.resize(n);
    if (mixHz_ != 0) {
        std::complex<double> p = std::polar(1.0, ph_);
        const std::complex<double> w = std::polar(1.0, dph_);
        for (size_t i = 0; i < n; i++) {
            mixed_[i] = cf32((float)(in[i].real() * p.real() - in[i].imag() * p.imag()), (float)(in[i].real() * p.imag() + in[i].imag() * p.real()));
            p *= w;
            if ((i & 1023) == 1023) p /= std::abs(p);
        }
        ph_ = std::remainder(ph_ + dph_ * (double)n, 2 * kPi);
    } else std::copy(in, in + n, mixed_.begin());
    tmp_.clear();
    s1_.process(mixed_.data(), n, tmp_);
    s2_.process(tmp_.data(), tmp_.size(), out);
}

// ---------------------------------------------------------------- ToneEstimator
struct ToneEstimator::FftHolder { Fft f; explicit FftHolder(int n) : f(n) {} std::vector<cf32> x; };
void ToneEstimator::init(double fs) {
    fs_ = fs;
    n_ = 1;
    while (n_ < fs / 10.0) n_ <<= 1;   // about 10 Hz bins
    hop_ = n_ / 2;
    win_.resize((size_t)n_);
    for (int i = 0; i < n_; i++) win_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / n_));
    fft_ = std::make_shared<FftHolder>(n_);
    reset();
}
void ToneEstimator::reset() { buf_.clear(); avg_.assign((size_t)n_, 0.f); frames_ = 0; }
void ToneEstimator::push(const cf32* x, size_t n) {
    buf_.insert(buf_.end(), x, x + n);
    const float a = (float)std::min(1.0, hop_ / fs_ / 3.0);   // about 3 s memory
    while ((int)buf_.size() >= n_) {
        auto& v = fft_->x;
        v.resize((size_t)n_);
        for (int i = 0; i < n_; i++) v[(size_t)i] = buf_[(size_t)i] * win_[(size_t)i];
        fft_->f.forward(v.data());
        const float aa = frames_ < 4 ? 1.f / float(frames_ + 1) : a;
        for (int i = 0; i < n_; i++) {
            const int k = (i + n_ / 2) % n_;   // bin i of the shifted spectrum is FFT bin k
            avg_[(size_t)i] += aa * (std::norm(v[(size_t)k]) - avg_[(size_t)i]);
        }
        frames_++;
        buf_.erase(buf_.begin(), buf_.begin() + hop_);
    }
}
float ToneEstimator::floorDb() const {
    std::vector<float> p(avg_);
    if (p.empty()) return -300;
    std::nth_element(p.begin(), p.begin() + (ptrdiff_t)(p.size() / 2), p.end());
    return db(p[p.size() / 2]);
}
int ToneEstimator::peaks(int k, double minSepHz, double maxHz, double* outHz, float* outDb) const {
    const double bin = binHz();
    const int lo = std::max(1, (int)std::ceil(n_ / 2 - maxHz / bin)), hi = std::min(n_ - 2, (int)std::floor(n_ / 2 + maxHz / bin));
    // a 10 ms symbol spreads each tone over about 100 Hz: the spectrum summed over that width before the search (integration gain)
    const int h = std::max(1, (int)std::lround(45 / bin));
    std::vector<float> sm((size_t)n_, 0.f);
    {
        double run = 0;
        for (int i = 0; i < n_; i++) {
            run += avg_[(size_t)i];
            if (i - 2 * h - 1 >= 0) run -= avg_[(size_t)(i - 2 * h - 1)];
            if (i - h >= 0) sm[(size_t)(i - h)] = (float)(run / (2 * h + 1));
        }
    }
    std::vector<float> p(sm.begin() + lo, sm.begin() + hi + 1);
    if (p.size() < 8) return 0;
    std::nth_element(p.begin(), p.begin() + (ptrdiff_t)(p.size() / 2), p.end());
    const float floor = p[p.size() / 2], thr = floor * 1.5f;   // 1.8 dB over the smoothed floor (averaged over seconds and 90 Hz: noise rarely gets there)
    std::vector<int> cand;
    for (int i = lo; i <= hi; i++) if (sm[(size_t)i] > thr && sm[(size_t)i] > sm[(size_t)i - 1] && sm[(size_t)i] >= sm[(size_t)i + 1]) cand.push_back(i);
    std::sort(cand.begin(), cand.end(), [&](int a, int b) { return sm[(size_t)a] > sm[(size_t)b]; });
    std::vector<int> pick;
    const int sep = std::max(2, (int)std::lround(minSepHz / bin));
    for (int c : cand) {
        bool ok = true;
        for (int q : pick) if (std::abs(q - c) < sep) { ok = false; break; }
        if (ok) pick.push_back(c);
        if ((int)pick.size() == k) break;
    }
    std::sort(pick.begin(), pick.end());
    for (size_t i = 0; i < pick.size(); i++) {
        const int c = pick[i];
        const double a = sm[(size_t)c - 1], b = sm[(size_t)c], d = sm[(size_t)c + 1];
        const double den = a - 2 * b + d, off = den != 0 ? std::clamp(0.5 * (a - d) / den, -0.5, 0.5) : 0;
        outHz[i] = (c + off - n_ / 2) * bin;
        if (outDb) outDb[i] = db(b) - db(floor);
    }
    return (int)pick.size();
}

// ---------------------------------------------------------------- ToneBank
void ToneBank::init(int len, int tones) {
    L_ = std::max(1, len); K_ = std::max(1, tones);
    w_.assign((size_t)K_, 0.0);
    reset();
}
void ToneBank::reset() {
    raw_.assign((size_t)L_, cf32(0, 0)); mixed_.assign((size_t)L_ * K_, cf32(0, 0));
    sum_.assign((size_t)K_, 0.0); ph_.assign((size_t)K_, 0.0); pos_ = 0; sinceExact_ = 0;
}
void ToneBank::setFreqs(const double* f) {
    for (int k = 0; k < K_; k++) w_[(size_t)k] = 2 * kPi * f[k];
    // the stored samples mixed again with the new frequencies, oldest first, so the window stays consistent
    for (int k = 0; k < K_; k++) {
        std::complex<double> s = 0;
        double ph = 0;
        for (int i = 0; i < L_; i++) {
            const int at = (pos_ + i) % L_;
            const cf32 m = raw_[(size_t)at] * std::polar(1.f, (float)-ph);
            mixed_[(size_t)k * L_ + at] = m; s += std::complex<double>(m.real(), m.imag());
            ph = std::remainder(ph + w_[(size_t)k], 2 * kPi);
        }
        sum_[(size_t)k] = s; ph_[(size_t)k] = ph;
    }
}
void ToneBank::push(cf32 x, float* e) {
    raw_[(size_t)pos_] = x;
    for (int k = 0; k < K_; k++) {
        cf32& slot = mixed_[(size_t)k * L_ + pos_];
        const cf32 m = x * std::polar(1.f, (float)-ph_[(size_t)k]);
        sum_[(size_t)k] += std::complex<double>(m.real() - slot.real(), m.imag() - slot.imag());
        slot = m;
        ph_[(size_t)k] = std::remainder(ph_[(size_t)k] + w_[(size_t)k], 2 * kPi);
    }
    if (++pos_ == L_) pos_ = 0;
    if (++sinceExact_ >= 4 * L_) {   // the running sums again from scratch (rounding)
        sinceExact_ = 0;
        for (int k = 0; k < K_; k++) { std::complex<double> s = 0; for (int i = 0; i < L_; i++) { const cf32 v = mixed_[(size_t)k * L_ + i]; s += std::complex<double>(v.real(), v.imag()); } sum_[(size_t)k] = s; }
    }
    for (int k = 0; k < K_; k++) e[k] = (float)std::norm(sum_[(size_t)k]);
}

// ---------------------------------------------------------------- Fsk4Demod
void Fsk4Demod::init(double fs, double baud) {
    fs_ = fs; baud_ = baud;
    L_ = std::max(4, (int)std::lround(fs / baud));
    bank_.init(L_, 4);
    timing_.assign((size_t)L_, 0.f);
    reset();
}
void Fsk4Demod::reset() { bank_.reset(); std::fill(timing_.begin(), timing_.end(), 0.f); phase_ = 0; best_ = 0; sinceEmit_ = 0; have_ = false; }
void Fsk4Demod::setTones(const double hz[4]) {
    double c[4];
    for (int i = 0; i < 4; i++) { hz_[i] = hz[i]; c[i] = hz[i] / fs_; }
    bank_.setFreqs(c);
    have_ = true;
}
void Fsk4Demod::process(const cf32* x, size_t n, const std::function<void(int, const float*)>& onSymbol) {
    float e[4];
    const float a = 1.f / 24.f;   // the clock remembers about 24 symbols
    for (size_t i = 0; i < n; i++) {
        bank_.push(x[i], e);
        if (!have_) continue;
        int m = 0;
        for (int k = 1; k < 4; k++) if (e[k] > e[m]) m = k;
        const float tot = e[0] + e[1] + e[2] + e[3];
        const float metric = tot > 0 ? (e[m] - (tot - e[m]) / 3.f) / tot : 0.f;   // how clearly one tone stands out at this sample phase
        float& tm = timing_[(size_t)phase_];
        tm += a * (metric - tm);
        if (tm > timing_[(size_t)best_]) best_ = phase_;
        sinceEmit_++;
        if (phase_ == best_ && sinceEmit_ > L_ / 2) { sinceEmit_ = 0; onSymbol(m, e); }
        if (++phase_ == L_) phase_ = 0;
    }
}

// ---------------------------------------------------------------- Deframer
void Deframer::reset() { reg_.fill(0); syms_.clear(); sig_.clear(); base_ = 0; nbits_ = 0; pend_.clear(); }

// variant bit 0: tones in reverse order (a mirrored spectrum); bit 1: the two bits of a symbol swapped
int Deframer::bitOf(int v, int64_t bit) const {
    const int64_t si = bit / 2 - base_;
    if (si < 0 || si >= (int64_t)syms_.size()) return 0;
    int s = syms_[(size_t)si];
    if (v & 1) s = 3 - s;
    const int first = (v & 2) ? (s & 1) : (s >> 1), second = (v & 2) ? (s >> 1) : (s & 1);
    return (bit & 1) ? second : first;
}

bool Deframer::tryDecode(const Pend& p, int bytes, const Tables& t, double baud, Packet& out) {
    const int payload = bytes == frameBytes(kV1Bytes) - 2 ? kV1Bytes : kV2Bytes;
    std::vector<uint8_t> body((size_t)bytes);
    for (int lsb = 0; lsb < 2; lsb++) {
        std::fill(body.begin(), body.end(), 0);
        for (int i = 0; i < bytes * 8; i++) if (bitOf(p.variant, p.start + i)) body[(size_t)(i >> 3)] |= uint8_t(lsb ? (1 << (i & 7)) : (0x80 >> (i & 7)));
        uint8_t pl[kV2Bytes];
        int fx = 0;
        decodeFrame(body.data(), payload, pl, &fx);
        Packet k;
        if (parseBinary(pl, payload, t, k)) {
            k.corrected = fx;
            k.baud = (int)std::lround(baud);
            // SNR in 3 kHz from the tone energies over the frame: (max - others) / others is the SNR in the symbol bandwidth
            double s = 0, nn = 0; int cnt = 0;
            const int64_t s0 = p.start / 2 - base_, s1 = (p.start + bytes * 8) / 2 - base_;
            for (int64_t q = std::max<int64_t>(0, s0); q < s1 && q < (int64_t)sig_.size(); q++) { s += sig_[(size_t)q][0]; nn += sig_[(size_t)q][1]; cnt++; }
            if (cnt > 0 && nn > 0) k.snrDb = (float)(10 * std::log10(std::max(1e-6, (s - nn) / nn) * baud / 3000.0));
            out = std::move(k);
            return true;
        }
    }
    return false;
}

void Deframer::push(int sym, const float* e, const Tables& t, double baud, std::vector<Packet>& out) {
    syms_.push_back((uint8_t)sym);
    float mx = e[sym], others = 0;
    for (int k = 0; k < 4; k++) if (k != sym) others += e[k];
    sig_.push_back({mx, others / 3.f});
    const int64_t need = (int64_t)(frameBytes(kV2Bytes) - 2) * 8;
    const uint32_t sync = 0x1B2424;   // the last preamble byte and the unique word
    for (int b = 0; b < 2; b++) {
        const int64_t bit = (base_ + (int64_t)syms_.size() - 1) * 2 + b;
        for (int v = 0; v < 4; v++) {
            reg_[(size_t)v] = ((reg_[(size_t)v] << 1) | (uint32_t)bitOf(v, bit)) & 0xFFFFFF;
            uint32_t d = reg_[(size_t)v] ^ sync; int err = 0;
            for (; d; d &= d - 1) err++;
            if (err <= 3) {
                bool dup = false;
                for (const auto& p : pend_) if (p.start == bit + 1) dup = true;
                if (!dup) { pend_.push_back({v, bit + 1}); syncs_++; }
            }
        }
        nbits_ = bit + 1;
    }
    for (size_t i = 0; i < pend_.size();) {
        Pend& p = pend_[i];
        const int64_t have = nbits_ - p.start;
        Packet k;
        bool done = false, decoded = false;
        if (!p.triedV1 && have >= (int64_t)(frameBytes(kV1Bytes) - 2) * 8) {
            p.triedV1 = true;
            if (tryDecode(p, frameBytes(kV1Bytes) - 2, t, baud, k)) { out.push_back(std::move(k)); done = decoded = true; }
        }
        if (!done && have >= need) {
            if (tryDecode(p, frameBytes(kV2Bytes) - 2, t, baud, k)) { out.push_back(std::move(k)); decoded = true; }
            else fails_++;
            done = true;
        }
        if (done) {
            const int64_t start = p.start;
            pend_.erase(pend_.begin() + (ptrdiff_t)i);
            if (decoded) {   // the other syncs inside a decoded frame were its own bits
                pend_.erase(std::remove_if(pend_.begin(), pend_.end(), [&](const Pend& q) { return q.start > start - 24 && q.start < start + need; }), pend_.end());
                i = 0;
            }
        } else i++;
    }
    // keep about two frames of symbols
    while (syms_.size() > 1200) { syms_.pop_front(); sig_.pop_front(); base_++; }
    if (pend_.size() > 64) pend_.erase(pend_.begin(), pend_.begin() + 32);
}

// ---------------------------------------------------------------- RttyDemod
void RttyDemod::init(double fs) {
    fs_ = fs;
    rates_.clear(); framers_.clear();
    for (int b : {50, 100, 300}) { Rate r; r.baud = b; r.bank.init(std::max(4, (int)std::lround(fs / b)), 2); rates_.push_back(std::move(r)); }
    for (int b : {50, 100, 300}) for (int nb : {7, 8}) for (int pol : {1, -1}) { Framer f; f.baud = b; f.nbits = nb; f.pol = pol; f.Lb = fs / b; framers_.push_back(f); }
    reset();
}
void RttyDemod::reset() {
    for (auto& r : rates_) { r.bank.reset(); r.d = 0; r.sAcc = 0; r.nAcc = 0; }
    for (auto& f : framers_) { f.state = 0; f.prev = 0; f.line.clear(); }
    n_ = 0; have_ = false;
}
void RttyDemod::setTones(double lo, double hi) {
    lo_ = lo; hi_ = hi; have_ = true;
    const double c[2] = {lo / fs_, hi / fs_};
    for (auto& r : rates_) r.bank.setFreqs(c);
}
void RttyDemod::onChar(Framer& f, int c, const Tables& t, std::vector<Packet>& out, const Rate& r) {
    if (c == '\n' || c == '\r' || f.line.size() > 400) {
        f.line.clear();
        return;
    }
    if (c < 32 || c > 126) { f.line.clear(); return; }
    f.line.push_back((char)c);
    const size_t n = f.line.size();
    if (n >= 10 && f.line[n - 5] == '*' && std::all_of(f.line.end() - 4, f.line.end(), [](char ch) { return std::isxdigit((unsigned char)ch) != 0; })) {
        const size_t d = f.line.rfind("$$");
        if (d == std::string::npos) return;
        Packet k;
        if (parseSentence(f.line.substr(d), t, k)) {
            k.baud = f.baud; k.rttyBits = f.nbits;
            k.toneSpacingHz = hi_ - lo_;
            if (r.nAcc > 0) k.snrDb = (float)(10 * std::log10(std::max(1e-6, (r.sAcc - r.nAcc) / r.nAcc) * f.baud / 3000.0));
            out.push_back(std::move(k));
            f.line.clear();
        }
    }
}
void RttyDemod::process(const cf32* x, size_t n, const Tables& t, std::vector<Packet>& out) {
    float e[2];
    for (size_t i = 0; i < n; i++, n_++) {
        for (auto& r : rates_) {
            r.bank.push(x[i], e);
            const float tot = e[0] + e[1];
            r.d = tot > 0 ? (e[1] - e[0]) / tot : 0.f;   // + : the higher tone
            const double a = 1.0 / (fs_ * 2.0);          // signal and noise energies, about 2 s
            r.sAcc += a * (std::max(e[0], e[1]) - r.sAcc);
            r.nAcc += a * (std::min(e[0], e[1]) - r.nAcc);
        }
        if (!have_) continue;
        for (auto& f : framers_) {
            const Rate& r = rates_[f.baud == 50 ? 0 : f.baud == 100 ? 1 : 2];
            const float s = f.pol * r.d;
            const double now = (double)n_;
            if (f.state == 0) {
                if (f.prev > 0 && s <= 0) {   // mark to space: a start bit; the window is half into it now
                    const double tc = now - 1 + f.prev / (f.prev - s);
                    f.ts = tc + 0.5 * f.Lb; f.k = 0; f.ch = 0; f.state = 1;
                }
            } else if (now >= f.ts) {
                const int bit = s > 0 ? 1 : 0;
                if (f.k == 0) { if (bit) f.state = 0; }
                else if (f.k <= f.nbits) f.ch |= bit << (f.k - 1);
                else {
                    f.state = 0;
                    if (bit) onChar(f, f.ch, t, out, r);
                }
                f.k++; f.ts += f.Lb;
            }
            f.prev = s;
        }
    }
}

// ---------------------------------------------------------------- Channel
void Channel::init(double inRate, double mixerHz, bool dynamic) {
    filt_.init(inRate);
    filt_.setMixer(mixerHz);
    dynamic_ = dynamic;
    est_.init(filt_.outRate());
    fsk_.init(filt_.outRate(), 100);
    rtty_.init(filt_.outRate());
    reset();
}
void Channel::reset() {
    filt_.reset(); est_.reset(); fsk_.reset(); def_.reset(); rtty_.reset();
    t_ = 0; nextEst_ = 0.5; mode = 0; snrDb = -99; rttyTones[0] = rttyTones[1] = 0;
}
// SNR in 3 kHz from the averaged spectrum: the power above the floor between lo and hi against the floor. With a Hann window the
// signal sums to N A^2 sum(w^2) over the bins and the noise is sigma^2 sum(w^2) in each, so A^2 / sigma^2 = sum(P - floor) / (N floor),
// and sigma^2 = N0 fs. The spectrum averages the gaps between packets in, so this reads a little low on bursty signals.
float Channel::spectrumSnr(double lo, double hi) const {
    const auto& p = est_.power();
    const int n = est_.size();
    if (!est_.primed() || p.empty()) return -99;
    std::vector<float> s(p);
    std::nth_element(s.begin(), s.begin() + (ptrdiff_t)(s.size() / 2), s.end());
    const double floor = s[s.size() / 2];
    if (floor <= 0) return -99;
    const int a = std::max(0, (int)std::floor(n / 2 + lo / est_.binHz())), b = std::min(n - 1, (int)std::ceil(n / 2 + hi / est_.binHz()));
    double sum = 0;
    for (int i = a; i <= b; i++) sum += std::max(0.0, p[(size_t)i] - floor);
    const double r = sum / (n * floor) * filt_.outRate() / 3000.0;
    return r > 0 ? (float)(10 * std::log10(r)) : -99;
}
double Channel::toneSpacing() const {
    if (!fsk_.hasTones()) return 0;
    const double* f = fsk_.tones();
    return (f[3] - f[0]) / 3.0;
}
void Channel::updateTones() {
    const double maxHz = 0.40 * filt_.outRate();
    double f[4]; float d[4];
    const int n4 = est_.peaks(4, 80, maxHz, f, d);
    double meanGap = (f[3] - f[0]) / 3, worst = 0;
    for (int i = 0; n4 == 4 && i < 3; i++) worst = std::max(worst, std::fabs(f[i + 1] - f[i] - meanGap));
    if (n4 == 4 && worst < 0.25 * meanGap) {   // four tones evenly spaced (the RTTY pair with two noise peaks is not)
        // a fresh set only when it moved: retuning the correlators costs one symbol
        bool moved = !fsk_.hasTones();
        for (int i = 0; i < 4 && !moved; i++) if (std::fabs(f[i] - fsk_.tones()[i]) > 0.25 * est_.binHz() + 1.5) moved = true;
        if (moved) fsk_.setTones(f);
    }
    double g[2];
    if (est_.peaks(2, 80, maxHz, g, d) == 2 && g[1] - g[0] < 1300) {
        if (!rtty_.hasTones() || std::fabs(g[0] - rttyTones[0]) > 3 || std::fabs(g[1] - rttyTones[1]) > 3) { rtty_.setTones(g[0], g[1]); rttyTones[0] = g[0]; rttyTones[1] = g[1]; }
    }
}
void Channel::feed(const cf32* x, size_t n, const Tables& t, double dialHz, std::vector<Packet>& out) {
    y_.clear();
    filt_.process(x, n, y_);
    if (y_.empty()) return;
    est_.push(y_.data(), y_.size());
    t_ += (double)y_.size() / filt_.outRate();
    if (t_ >= nextEst_ && est_.primed()) { nextEst_ = t_ + 0.5; updateTones(); }
    const size_t first = out.size();
    fsk_.process(y_.data(), y_.size(), [&](int s, const float* e) { def_.push(s, e, t, 100, out); });
    for (size_t i = first; i < out.size(); i++) {
        const double* f = fsk_.tones();
        out[i].offsetHz = filt_.mixer() - dialHz + (f[0] + f[3]) / 2;
        out[i].toneSpacingHz = toneSpacing();
        const float s = spectrumSnr(f[0] - 150, f[3] + 150);
        if (s > -90) out[i].snrDb = s;
        mode = 1;
    }
    const size_t mid = out.size();
    rtty_.process(y_.data(), y_.size(), t, out);
    for (size_t i = mid; i < out.size(); i++) {
        out[i].offsetHz = filt_.mixer() - dialHz + (rttyTones[0] + rttyTones[1]) / 2;
        const float s = spectrumSnr(rttyTones[0] - 150, rttyTones[1] + 150);
        if (s > -90) out[i].snrDb = s;
        mode = 2;
    }
    if (out.size() > first) { lastDecodeSec = t_; packets += out.size() - first; snrDb = out.back().snrDb; }
}

// ---------------------------------------------------------------- WideDetector
struct WideDetector::FftHolder { Fft f; explicit FftHolder(int n) : f(n) {} std::vector<cf32> x; };
void WideDetector::init(double rate) {
    rate_ = rate;
    n_ = 1;
    while (n_ < rate / 60.0 && n_ < 65536) n_ <<= 1;
    every_ = (int)(rate * 0.25);   // one spectrum every quarter second
    win_.resize((size_t)n_);
    for (int i = 0; i < n_; i++) win_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / n_));
    fft_ = std::make_shared<FftHolder>(n_);
    reset();
}
void WideDetector::reset() { buf_.clear(); avg_.assign((size_t)n_, 0.f); frames_ = 0; since_ = 0; gather_ = 0; }
void WideDetector::push(const cf32* x, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (gather_ == 0) {   // waiting for the next spectrum
            const int64_t wait = every_ - since_;
            if ((int64_t)(n - i) < wait) { since_ += (int64_t)(n - i); return; }
            i += (size_t)wait; since_ = 0; gather_ = 1; buf_.clear();
        }
        const size_t take = std::min(n - i, (size_t)n_ - buf_.size());
        buf_.insert(buf_.end(), x + i, x + i + take);
        i += take; since_ += (int64_t)take;
        if ((int)buf_.size() == n_) {
            auto& v = fft_->x;
            v.resize((size_t)n_);
            for (int k = 0; k < n_; k++) v[(size_t)k] = buf_[(size_t)k] * win_[(size_t)k];
            fft_->f.forward(v.data());
            const float a = frames_ < 8 ? 1.f / float(frames_ + 1) : 0.125f;   // about 2 s
            for (int k = 0; k < n_; k++) avg_[(size_t)k] += a * (std::norm(v[(size_t)((k + n_ / 2) % n_)]) - avg_[(size_t)k]);
            frames_++; gather_ = 0;
        }
    }
}
std::vector<WideDetector::Hit> WideDetector::hits(double exHz, double exHalf, double maxAbsHz) const {
    std::vector<Hit> out;
    const double bin = rate_ / n_;
    const int edge = std::max((int)(n_ * 0.05), n_ / 2 - (int)(maxAbsHz / bin));
    std::vector<float> p(avg_.begin() + edge, avg_.end() - edge);
    if (p.size() < 16) return out;
    std::nth_element(p.begin(), p.begin() + (ptrdiff_t)(p.size() / 2), p.end());
    const float thr = p[p.size() / 2] * 6.3f;   // 8 dB over the median
    const int gap = std::max(1, (int)std::lround(600 / bin)), maxW = (int)std::lround(2500 / bin);
    int k = edge;
    while (k < n_ - edge) {
        if (avg_[(size_t)k] <= thr) { k++; continue; }
        int a = k, b = k, last = k;
        double sw = 0, swf = 0, mx = 0;
        while (b < n_ - edge && b - last <= gap) {
            if (avg_[(size_t)b] > thr) { last = b; sw += avg_[(size_t)b]; swf += avg_[(size_t)b] * b; mx = std::max(mx, (double)avg_[(size_t)b]); }
            b++;
        }
        k = last + 1;
        if (last - a > maxW) continue;   // wider than a balloon signal
        const double hz = (swf / sw - n_ / 2) * bin;
        if (std::fabs(hz - exHz) < exHalf) continue;
        out.push_back({hz, db(mx) - db(p[p.size() / 2])});
    }
    return out;
}

} // namespace dect2::horus
