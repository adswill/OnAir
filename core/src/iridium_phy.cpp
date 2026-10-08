// Iridium radio layer: burst detector and per-burst DQPSK demodulator (see iridium_phy.h for the sources of the constants).
#include "dect2/iridium_phy.h"
#include "dect2/dsp_compat.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace dect2 {
namespace iridium {

const uint8_t kUwDl[kUwLen] = {0, 2, 2, 2, 2, 0, 0, 0, 2, 0, 0, 2};
const uint8_t kUwUl[kUwLen] = {2, 2, 0, 0, 0, 2, 0, 0, 2, 0, 2, 2};
const char* const kUwDlBits = "001100000011000011110011";
const char* const kUwUlBits = "110011000011110011111100";

int nearestChannel(double hz) {
    return (int)std::floor((hz - kBaseHz) / kChannelHz);
}

std::vector<uint8_t> burstSymbols(const std::vector<uint8_t>& bits, bool downlink, int preambleSymbols) {
    std::vector<uint8_t> s;
    s.reserve(preambleSymbols + kUwLen + bits.size() / 2 + 1);
    for (int i = 0; i < preambleSymbols; i++) s.push_back(downlink ? 0 : ((preambleSymbols - i) % 2 ? 0 : 2));
    const uint8_t* uw = downlink ? kUwDl : kUwUl;
    for (int i = 0; i < kUwLen; i++) s.push_back(uw[i]);
    int cur = uw[kUwLen - 1];
    for (size_t i = 0; i < bits.size(); i += 2) {
        const int b0 = bits[i] & 1, b1 = i + 1 < bits.size() ? (bits[i + 1] & 1) : 0;
        cur = (cur + dqpskStep(b0, b1)) & 3;
        s.push_back((uint8_t)cur);
    }
    return s;
}

void symbolsToBits(const uint8_t* sym, size_t n, int ref, std::vector<uint8_t>& bits) {
    bits.clear();
    bits.reserve(2 * n);
    for (size_t i = 0; i < n; i++) {
        const int p = dqpskPair((sym[i] - ref) & 3);
        bits.push_back((uint8_t)(p >> 1));
        bits.push_back((uint8_t)(p & 1));
        ref = sym[i];
    }
}

int uwDistance(const uint8_t* sym, bool downlink) {
    const uint8_t* uw = downlink ? kUwDl : kUwUl;
    int d = 0;
    for (int i = 0; i < kUwLen; i++) {
        int e = std::abs((int)sym[i] - (int)uw[i]);
        if (e == 3) e = 1;
        d += e;
    }
    return d;
}

std::vector<float> rrcTaps(double sps, double alpha, int halfSpan) {
    const int half = (int)std::ceil(halfSpan * sps);
    std::vector<float> h(2 * half + 1);
    double e = 0;
    for (int i = -half; i <= half; i++) {
        const double t = i / sps;
        double v;
        if (std::fabs(t) < 1e-9) v = 1 - alpha + 4 * alpha / M_PI;
        else if (std::fabs(std::fabs(4 * alpha * t) - 1) < 1e-9)
            v = alpha / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * alpha)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * alpha)));
        else
            v = (std::sin(M_PI * t * (1 - alpha)) + 4 * alpha * t * std::cos(M_PI * t * (1 + alpha))) / (M_PI * t * (1 - (4 * alpha * t) * (4 * alpha * t)));
        h[i + half] = (float)v;
        e += v * v;
    }
    const double g = 1 / std::sqrt(e);
    for (auto& v : h) v = (float)(v * g);
    return h;
}

// ---------------------------------------------------------------- detector

void BurstDetector::configure(double rate, double thresholdDb) {
    rate_ = rate;
    // about 10 kHz bins: a 16 symbol preamble (640 us) spans several FFT frames even at 2 Msps
    fft_ = 64;
    while (fft_ < rate / 12000.0 && fft_ < 8192) fft_ *= 2;
    log2n_ = 0;
    while ((1 << log2n_) < fft_) log2n_++;
    win_.resize(fft_);
    for (int i = 0; i < fft_; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / fft_));
    re_.assign(fft_, 0); im_.assign(fft_, 0); pow_.assign(fft_, 0); prev_.assign(fft_, 0); sum_.assign(fft_, 0); noise_.assign(fft_, 0);
    buf_.assign(fft_, cf32(0, 0));
    setThreshold(thresholdDb);
    thrCont_ = 2.0f;
    const double bin = rate / fft_;
    wBins_ = std::max(1, (int)std::lround(30e3 / bin));    // peaks this close belong to the same burst
    cBins_ = std::max(1, (int)std::lround(15e3 / bin));    // the burst's own bins for "still there"
    // longest burst: long preamble + unique word + simplex maximum, plus a little
    maxFrames_ = (int)std::ceil((kPreambleLong + kUwLen + kMaxSymbolsSimplex + 8) / kSymbolRate * rate / fft_);
    reset();
}

void BurstDetector::setThreshold(double thresholdDb) {
    thr_ = (float)std::pow(10.0, thresholdDb / 10);
    // two frames summed (a 16 symbol preamble spans several): the threshold with the same false alarm rate as one frame at thr_
    // (exponential noise power per bin: P = exp(-T) for one frame, (1 + T) exp(-T) for the sum of two)
    double t2 = 2 * thr_;
    for (int i = 0; i < 20; i++) t2 = thr_ + std::log(1 + t2);
    thr2_ = (float)t2;
}

void BurstDetector::reset() {
    fill_ = 0; pos_ = 0; frames_ = 0;
    act_.clear();
    std::fill(noise_.begin(), noise_.end(), 0.f);
    std::fill(prev_.begin(), prev_.end(), 0.f);
}

double BurstDetector::noisePerHz() const {
    if (noise_.empty() || frames_ == 0) return 0;
    std::vector<float> v(noise_);
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    double wsum = 0;
    for (float w : win_) wsum += (double)w * w;
    return v[v.size() / 2] / (wsum * rate_);   // E|X|^2 = N0 * rate * sum(w^2) for white noise of density N0
}

void BurstDetector::feed(const cf32* x, size_t n, std::vector<DetectedBurst>& out) {
    if (fft_ == 0) return;
    out_ = &out;
    while (n > 0) {
        const size_t take = std::min(n, (size_t)fft_ - fill_);
        std::copy(x, x + take, buf_.begin() + fill_);
        fill_ += take; x += take; n -= take; pos_ += take;
        if (fill_ == (size_t)fft_) { frame(); fill_ = 0; }
    }
    out_ = nullptr;
}

void BurstDetector::finish(const Active& a, std::vector<DetectedBurst>& out) {
    DetectedBurst b;
    const uint64_t pre = 2 * (uint64_t)fft_ + (uint64_t)(2e-4 * rate_);
    const uint64_t post = (uint64_t)fft_ + (uint64_t)(8e-4 * rate_);
    const uint64_t s = a.startFrame * (uint64_t)fft_;
    b.start = s > pre ? s - pre : 0;
    b.end = (a.lastFrame + 1) * (uint64_t)fft_ + post;
    b.freqHz = a.freqHz;
    b.peakDb = a.peakDb;
    out.push_back(b);
}

void BurstDetector::frame() {
    const int N = fft_;
    for (int i = 0; i < N; i++) { re_[i] = buf_[i].real() * win_[i]; im_[i] = buf_[i].imag() * win_[i]; }
    fftSplit(re_.data(), im_.data(), log2n_, false);
    for (int i = 0; i < N; i++) pow_[i] = re_[i] * re_[i] + im_[i] * im_[i];
    const uint64_t f = frames_++;
    if (f == 0) {   // start from the median over the band: bursts cover a few bins at most
        std::vector<float> v(pow_);
        std::nth_element(v.begin(), v.begin() + N / 2, v.end());
        const float m = std::max(v[N / 2], 1e-20f);
        std::fill(noise_.begin(), noise_.end(), m);
        prev_ = pow_;
        return;
    }
    const double bin = rate_ / N;
    // peaks over the threshold
    std::vector<DetectedBurst>& out = *out_;
    for (int k = 0; k < N; k++) sum_[k] = pow_[k] + prev_[k];
    for (int k = 0; k < N; k++) {
        // one strong frame, or two weaker ones in a row
        const float* q = pow_.data();
        float p = pow_[k];
        if (p <= thr_ * noise_[k]) {
            // both frames clearly up (not the last frame of a burst that has just ended plus noise)
            if (sum_[k] <= thr2_ * noise_[k] || std::min(pow_[k], prev_[k]) < 4.f * noise_[k]) continue;
            q = sum_.data();
            p = sum_[k];
        }
        const float pl = q[(k + N - 1) % N], pr = q[(k + 1) % N];
        if (p < pl || p < pr) continue;
        // parabolic interpolation on the log power (Hann window)
        const double a = std::log(pl + 1e-30), b = std::log(p + 1e-30), c = std::log(pr + 1e-30);
        const double den = a - 2 * b + c;
        double d = std::fabs(den) > 1e-12 ? 0.5 * (a - c) / den : 0;
        d = std::clamp(d, -0.5, 0.5);
        const int ks = k < N / 2 ? k : k - N;
        const double fh = (ks + d) * bin;
        const float db = 10.f * std::log10(p / (q == pow_.data() ? noise_[k] : 2 * noise_[k]));
        bool known = false;
        for (auto& ac : act_) {
            if (std::fabs(fh - ac.freqHz) < 30e3) {
                known = true;
                // the carrier of the preamble comes first: keep the strongest peak of the first frames
                if (f - ac.startFrame < 3 && db > ac.peakDb) { ac.freqHz = fh; ac.bin = k; }
                ac.peakDb = std::max(ac.peakDb, db);
                ac.lastFrame = f; ac.quiet = 0;
                break;
            }
        }
        if (!known) {
            // the spectral skirts of a very strong burst are not bursts of their own
            for (const auto& ac : act_)
                if (std::fabs(fh - ac.freqHz) < 110e3 && ac.peakDb > db + 25) { known = true; break; }
        }
        if (!known && act_.size() < 512) act_.push_back(Active{fh, k, f, f, db, 0});
    }
    // bursts without a new peak: still above the noise in their channel?
    for (size_t i = 0; i < act_.size();) {
        Active& ac = act_[i];
        bool done = false;
        if (ac.lastFrame != f) {
            double ps = 0, ns = 0;
            for (int j = -cBins_; j <= cBins_; j++) { const int k = (ac.bin + j + N) % N; ps += pow_[k]; ns += noise_[k]; }
            if (ps > thrCont_ * ns) { ac.lastFrame = f; ac.quiet = 0; }
            else if (++ac.quiet >= 2) done = true;
        }
        if ((int)(f - ac.startFrame) > maxFrames_) done = true;
        if (done) { finish(ac, out); act_[i] = act_.back(); act_.pop_back(); }
        else i++;
    }
    // noise floor: follow quiet bins quickly, creep up slowly under a signal (a steady spur becomes noise in a fraction of a second)
    for (int k = 0; k < N; k++) {
        const float p = pow_[k];
        float& nf = noise_[k];
        if (p < 4.f * nf) nf += (p - nf) * (1.f / 64.f);
        else nf *= 1.002f;
        if (nf < 1e-20f) nf = 1e-20f;
    }
    prev_.swap(pow_);
}

// ---------------------------------------------------------------- demodulator

namespace {
inline cf32 cubicAt(const std::vector<cf32>& v, double t) {
    const long i = (long)std::floor(t);
    const float u = (float)(t - i);
    auto g = [&](long k) { return k >= 0 && k < (long)v.size() ? v[k] : cf32(0, 0); };
    const cf32 y0 = g(i - 1), y1 = g(i), y2 = g(i + 1), y3 = g(i + 2);
    // Lagrange, 4 points
    const float c0 = -u * (u - 1) * (u - 2) / 6, c1 = (u + 1) * (u - 1) * (u - 2) / 2, c2 = -(u + 1) * u * (u - 2) / 2, c3 = (u + 1) * u * (u - 1) / 6;
    return y0 * c0 + y1 * c1 + y2 * c2 + y3 * c3;
}
// sum of x[i] h[i] over interleaved re/im (h doubled: h2[2i] = h2[2i+1] = h[i]), with 8 partial sums the compiler vectorises
inline cf32 firInterleaved(const cf32* x, const float* h2, int n2) {
    const float* xs = reinterpret_cast<const float*>(x);
    float a[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int t = 0;
    for (; t + 8 <= n2; t += 8)
        for (int u = 0; u < 8; u++) a[u] += xs[t + u] * h2[t + u];
    for (; t < n2; t++) a[t & 7] += xs[t] * h2[t];
    return cf32(a[0] + a[2] + a[4] + a[6], a[1] + a[3] + a[5] + a[7]);
}
inline int quadrant(cf32 y) {    // iridium_qpsk_demod_impl.cc demod_qpsk
    if (y.real() >= 0) return y.imag() >= 0 ? 0 : 3;
    return y.imag() >= 0 ? 1 : 2;
}
const cf32 kPoint[4] = {cf32(M_SQRT1_2, M_SQRT1_2), cf32(-M_SQRT1_2, M_SQRT1_2), cf32(-M_SQRT1_2, -M_SQRT1_2), cf32(M_SQRT1_2, -M_SQRT1_2)};
} // namespace

void BurstDemod::configure(double inputRate) {
    rate_ = inputRate;
    m1_ = std::max(1, (int)std::floor(inputRate / 500e3));
    fs1_ = inputRate / m1_;
    fs2_ = fs1_ / 2;
    sps_ = fs2_ / kSymbolRate;
    // decimate-by-2 low pass: keeps +-60 kHz (the burst plus the carrier error), Blackman window
    const int L = 31;
    hb_.resize(L);
    const double fc = 60e3 / fs1_;
    double s = 0;
    for (int i = 0; i < L; i++) {
        const double t = i - (L - 1) / 2.0;
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (L - 1)) + 0.08 * std::cos(4 * M_PI * i / (L - 1));
        const double v = (std::fabs(t) < 1e-9 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t)) * w;
        hb_[i] = (float)v; s += v;
    }
    for (auto& v : hb_) v = (float)(v / s);
    rrc_ = rrcTaps(sps_, kRrcAlpha, 5);
    auto dbl = [](const std::vector<float>& h) { std::vector<float> d(2 * h.size()); for (size_t i = 0; i < h.size(); i++) d[2 * i] = d[2 * i + 1] = h[i]; return d; };
    hb2_ = dbl(hb_);
    rrc2_ = dbl(rrc_);
    // CIC (3 stages) as one FIR: three boxcars of m1 convolved, unit gain at 0 Hz
    std::vector<double> a(m1_, 1.0), b;
    for (int st = 1; st < 3; st++) {
        b.assign(a.size() + m1_ - 1, 0.0);
        for (size_t i = 0; i < a.size(); i++) for (int k = 0; k < m1_; k++) b[i + k] += a[i];
        a.swap(b);
    }
    double sum = 0;
    for (double v : a) sum += v;
    cic_.resize(a.size());
    cic2_.resize(2 * a.size());
    for (size_t i = 0; i < a.size(); i++) cic_[i] = cic2_[2 * i] = cic2_[2 * i + 1] = (float)(a[i] / sum);
}

// Mix to freqHz and decimate by m1 with a 3-stage CIC written as one FIR (3 m1 - 2 taps, weights of three boxcars): no running
// sums that grow, plain multiply-adds the compiler vectorises. Output j stands for the window ending at input j m1 + m1 - 1.
void BurstDemod::prepare(const cf32* x, size_t n, double freqHz) {
    freqBase_ = freqHz;
    const int M = m1_, L = (int)cic_.size();
    const size_t n1 = n / M;
    // mix_ holds L - 1 zeros, then the mixed input: the first windows start before the segment
    mix_.assign(n + L, cf32(0, 0));
    cf32* mx = mix_.data() + (L - 1);
    {
        const double w = -2 * M_PI * freqHz / rate_;
        constexpr int B = 256;
        if ((int)tab_.size() != B || tabW_ != w) {
            tab_.resize(B);
            for (int k = 0; k < B; k++) tab_[k] = cf32((float)std::cos(w * k), (float)std::sin(w * k));
            tabW_ = w;
        }
        const std::complex<double> stepB = std::polar(1.0, w * B);
        std::complex<double> base(1, 0);
        for (size_t i0 = 0; i0 < n; i0 += B) {
            const float br = (float)base.real(), bi = (float)base.imag();
            const size_t e = std::min(n, i0 + B);
            for (size_t i = i0; i < e; i++) {
                const cf32 t = tab_[i - i0];
                const float pr = br * t.real() - bi * t.imag(), pi = br * t.imag() + bi * t.real();
                const float xr = x[i].real(), xi = x[i].imag();
                mx[i] = cf32(xr * pr - xi * pi, xr * pi + xi * pr);
            }
            base *= stepB;
        }
    }
    // the taps doubled for interleaved re/im, then 8 partial sums (even lanes re, odd lanes im)
    s1Base_.resize(n1);
    for (size_t j = 0; j < n1; j++) s1Base_[j] = firInterleaved(mx + (long)(j * M + M - 1) - (L - 1), cic2_.data(), 2 * L);
}

void BurstDemod::demod(const cf32* x, size_t n, double freqHz, int maxSymbols, DemodResult& out) {
    out = DemodResult();
    out.freqHz = freqHz;
    if (rate_ <= 0 || n < (size_t)(m1_ * 64)) return;
    prepare(x, n, freqHz);
    demodAt(x, n, freqHz, maxSymbols, out);
    // the usual case: a downlink burst whose carrier the detector found within a few kHz
    if (out.uwOk && std::fabs(out.freqHz - freqHz) < 4e3) return;
    if (n < (size_t)(m1_ * 64)) return;
    // Otherwise centre the filters on the carrier found, and try 12.5 kHz either side: an uplink preamble (phase alternating every
    // symbol) shows the detector two lines 12.5 kHz either side of its carrier, and the carrier estimate from symbol to symbol is
    // ambiguous by the symbol rate. The right centre gives the cleanest constellation.
    double cands[3];
    int nc = 0;
    if (out.found) cands[nc++] = out.freqHz;
    cands[nc++] = freqHz + kSymbolRate / 2;
    cands[nc++] = freqHz - kSymbolRate / 2;
    DemodResult best = out;
    bool have = out.uwOk && std::fabs(out.freqHz - freqHz) < 4e3;
    for (int i = 0; i < nc; i++) {
        DemodResult r;
        demodAt(x, n, cands[i], maxSymbols, r);
        if (r.found && std::fabs(r.freqHz - cands[i]) > 4e3) { const double c = r.freqHz; demodAt(x, n, c, maxSymbols, r); }
        if (r.uwOk && (!have || r.snrDb > best.snrDb)) { best = std::move(r); have = true; }
    }
    if (have) out = std::move(best);
}

void BurstDemod::demodAt(const cf32* x, size_t n, double freqHz, int maxSymbols, DemodResult& out) {
    out = DemodResult();
    out.freqHz = freqHz;
    if (rate_ <= 0 || n < (size_t)(m1_ * 64)) return;
    // 1. the input mixed to freqBase and decimated (prepare), moved by what is left at the low rate; zeros either side for the filters
    const size_t n1 = s1Base_.size();
    const int L = (int)hb_.size(), hl = (L - 1) / 2;
    const int R = (int)rrc_.size(), rh = (R - 1) / 2;
    s1_.assign(n1 + 2 * hl + 2, cf32(0, 0));
    cf32* s1 = s1_.data() + hl;
    {
        const double w = -2 * M_PI * (freqHz - freqBase_) * m1_ / rate_;
        std::complex<double> ph(1, 0);
        const std::complex<double> step(std::cos(w), std::sin(w));
        for (size_t k = 0; k < n1; k++) {
            s1[k] = s1Base_[k] * cf32((float)ph.real(), (float)ph.imag());
            ph *= step;
            if ((k & 255) == 255) ph /= std::abs(ph);
        }
    }
    // 2. low pass and decimate by 2 (centred)
    const size_t n2 = n1 / 2;
    s2_.assign(n2 + 2 * rh + 2, cf32(0, 0));
    cf32* s2 = s2_.data() + rh;
    for (size_t k = 0; k < n2; k++) s2[k] = firInterleaved(s1 + 2 * k - hl, hb2_.data(), 2 * L);
    // 3. matched filter (centred)
    mf_.assign(n2, cf32(0, 0));
    for (size_t k = 0; k < n2; k++) mf_[k] = firInterleaved(s2 + k - rh, rrc2_.data(), 2 * R);
    // time of fs2 sample k in input samples: CIC output j stands for input j*m1 + (m1-1) - 1.5 (m1-1)
    const double cicDelay = -1.5 * (m1_ - 1);
    auto inputTime = [&](double k2) { return (2 * k2 * m1_ + (m1_ - 1) + cicDelay) / rate_; };

    // 4. unique word search: differential correlation over the last 16 preamble symbols and the unique word
    const int D = (int)std::lround(sps_);
    const int nk = kPreambleShort + kUwLen;
    d_.assign(n2, cf32(0, 0));
    for (size_t k = D; k < n2; k++) d_[k] = mf_[k] * std::conj(mf_[k - D]);
    int off[nk];
    for (int j = 0; j < nk; j++) off[j] = (int)std::lround(j * sps_);
    std::vector<uint8_t> refDl(nk), refUl(nk);
    for (int j = 0; j < kPreambleShort; j++) { refDl[j] = 0; refUl[j] = (kPreambleShort - j) % 2 ? 0 : 2; }
    for (int j = 0; j < kUwLen; j++) { refDl[kPreambleShort + j] = kUwDl[j]; refUl[kPreambleShort + j] = kUwUl[j]; }
    float sgnDl[nk], sgnUl[nk];
    for (int j = 1; j < nk; j++) {
        sgnDl[j] = ((refDl[j] - refDl[j - 1]) & 3) == 0 ? 1.f : -1.f;
        sgnUl[j] = ((refUl[j] - refUl[j - 1]) & 3) == 0 ? 1.f : -1.f;
    }
    // the unique word starts within the first long preamble + the detector's margin
    const long lastP = std::min((long)n2 - off[nk - 1] - 2 * D - 1, (long)((kPreambleLong + 24) * sps_ + 0.6e-3 * fs2_));
    if (lastP < 1) return;
    const long nd = std::min((long)n2, lastP + off[nk - 1] + 1);
    std::vector<float> dAbs(nd);
    for (long k = 0; k < nd; k++) dAbs[k] = std::sqrt(std::norm(d_[k]));
    std::vector<float> mDl(lastP + 1), mUl(lastP + 1);
    std::vector<cf32> cDl(lastP + 1), cUl(lastP + 1);
    long best = -1; bool bestDl = true; float bestM = 0;
    for (long p = 0; p <= lastP; p++) {
        cf32 a(0, 0), b(0, 0);
        float e = 0;
        for (int j = 1; j < nk; j++) {
            const cf32 v = d_[p + off[j]];
            a += v * sgnDl[j]; b += v * sgnUl[j];
            e += dAbs[p + off[j]];
        }
        e = std::max(e, 1e-30f);
        cDl[p] = a; cUl[p] = b;
        mDl[p] = std::abs(a) / e; mUl[p] = std::abs(b) / e;
        if (mDl[p] > bestM) { bestM = mDl[p]; best = p; bestDl = true; }
        if (mUl[p] > bestM) { bestM = mUl[p]; best = p; bestDl = false; }
    }
    if (best < 0 || bestM < 0.3f) return;
    out.found = true;
    out.downlink = bestDl;
    const std::vector<float>& m = bestDl ? mDl : mUl;
    double pf = (double)best;
    if (best > 0 && best < lastP) {
        const double a = m[best - 1], b = m[best], c = m[best + 1], den = a - 2 * b + c;
        if (std::fabs(den) > 1e-12) pf += std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
    }
    const cf32 cc = bestDl ? cDl[best] : cUl[best];
    // rotation per symbol (the lag D is sps rounded: scale to a whole symbol)
    double wSym = std::arg(cc) * sps_ / D;           // radians per symbol
    const std::vector<uint8_t>& ref = bestDl ? refDl : refUl;

    const std::vector<uint8_t>& ref0 = ref;
    // 5. fine timing: the known symbols correlate best at the right instant (carrier removed with the estimate above)
    {
        auto score = [&](double t0) {
            cf32 acc(0, 0);
            for (int k = 0; k < nk; k++) acc += cubicAt(mf_, t0 + k * sps_) * std::conj(kPoint[ref0[k]]) * std::polar(1.f, (float)(-wSym * k));
            return std::abs(acc);
        };
        // the differential search is coarse: look half a symbol either side
        const double st = sps_ / 16;
        double bestT = pf, bestS = -1;
        for (int i = -8; i <= 8; i++) {
            const double sc = score(pf + i * st);
            if (sc > bestS) { bestS = sc; bestT = pf + i * st; }
        }
        const double h = st / 2, a = score(bestT - h), c = score(bestT + h), den = a - 2 * bestS + c;
        if (std::fabs(den) > 1e-12) bestT += h * std::clamp(0.5 * (a - c) / den, -1.0, 1.0);
        pf = std::max(0.0, bestT);
    }
    // symbols of the known part, carrier and phase from them
    auto sampleAt = [&](int k) { return cubicAt(mf_, pf + k * sps_); };
    const int maxK = std::min(nk + maxSymbols, (int)((n2 - 3 - pf) / sps_));
    if (maxK < nk + 4) return;
    std::vector<cf32> y(maxK);
    for (int k = 0; k < maxK; k++) y[k] = sampleAt(k);
    {
        cf32 acc(0, 0);
        for (int k = 1; k < nk; k++) {
            const cf32 z1 = y[k] * std::conj(kPoint[ref[k]]), z0 = y[k - 1] * std::conj(kPoint[ref[k - 1]]);
            acc += z1 * std::conj(z0);
        }
        if (std::abs(acc) > 0) wSym = std::arg(acc);
    }
    cf32 pacc(0, 0);
    for (int k = 0; k < nk; k++) {
        const cf32 r = std::polar(1.f, (float)(-wSym * (k - nk / 2)));
        pacc += y[k] * r * std::conj(kPoint[ref[k]]);
    }
    double theta = std::arg(pacc) - wSym * (nk / 2);    // phase at k = 0
    double omega = wSym;
    // 6. remove the carrier found from the known symbols; what is left drifts slowly
    std::vector<uint8_t> sym(maxK);
    std::vector<float> mag(maxK);
    float amp = 0;
    for (int k = 0; k < maxK; k++) {
        y[k] *= std::polar(1.f, (float)-(theta + omega * k));
        mag[k] = std::abs(y[k]);
        if (k < nk) amp += mag[k];
    }
    amp /= nk;
    // 7. end of the burst: three weak symbols in a row. gr-iridium (demod_qpsk) calls a symbol weak below 1/8 of the strongest;
    // noise crosses that often at 20 dB, so weak here is below a third of the mean amplitude of the preamble and unique word.
    const float weak = amp / 3;
    float mx = 0;
    int nEnd = maxK;
    int low = 0;
    for (int k = kPreambleShort; k < maxK; k++) {
        mx = std::max(mx, mag[k]);
        if (mag[k] < weak) { if (++low == 3) { nEnd = k - 2; break; } }
        else low = 0;
    }
    if (nEnd == maxK) {   // the segment ended first: drop weak symbols at its end
        while (nEnd > nk && mag[nEnd - 1] < weak) nEnd--;
    }
    // 8. phase through the burst: fourth power (Viterbi and Viterbi) over +-W symbols, unwrapped in quarter turns. No decision
    // feedback, so no cycle slips that run on; a quarter-turn jump costs one symbol, as the differential code only looks at steps.
    {
        const int W = 10, n = nEnd;
        std::vector<std::complex<double>> acc(n + 1);
        acc[0] = 0;
        for (int k = 0; k < n; k++) {
            const std::complex<double> v(y[k].real(), y[k].imag());
            const std::complex<double> v2 = v * v;
            const double m2 = std::norm(v);
            acc[k + 1] = acc[k] + (m2 > 0 ? v2 * v2 / m2 : std::complex<double>(0, 0));
        }
        double prev = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int k = 0; k < n; k++) {
            const int a = std::max(0, k - W), b = std::min(n, k + W + 1);
            const std::complex<double> S = acc[b] - acc[a];
            // the points sit at 45 + 90 m degrees: their fourth power points to 180 degrees
            double ph = (std::arg(S) - M_PI) / 4;
            while (ph - prev > M_PI / 4) ph -= M_PI / 2;
            while (ph - prev < -M_PI / 4) ph += M_PI / 2;
            prev = ph;
            const cf32 r = y[k] * std::polar(1.f, (float)-ph);
            y[k] = r;
            sym[k] = (uint8_t)quadrant(r);
            sx += k; sy += ph; sxx += (double)k * k; sxy += k * ph;
        }
        const double den = n * sxx - sx * sx;
        if (n > 2 && den > 0) omega += (n * sxy - sx * sy) / den;
    }
    const int nData = std::max(0, nEnd - nk);
    out.nSymbols = nData;
    out.uwOk = uwDistance(&sym[kPreambleShort], bestDl) <= 2;
    symbolsToBits(&sym[nk], nData, sym[nk - 1], out.bits);
    // quality over the unique word and the data
    int ok = 0, cnt = 0;
    double ps = 0, es = 0;
    for (int k = kPreambleShort; k < nEnd; k++) {
        const int q = quadrant(y[k]);
        const float e = std::arg(y[k] * std::conj(kPoint[q]));
        if (std::fabs(e) <= 22.f * (float)M_PI / 180.f) ok++;
        cnt++;
        ps += std::norm(y[k]);
        es += std::norm(y[k] - kPoint[q] * amp);
    }
    if (cnt > 0) {
        out.confidence = 100.f * ok / cnt;
        out.snrDb = (float)(10 * std::log10(std::max(1e-12, (double)amp * amp) / std::max(1e-12, es / cnt)));
        out.levelDb = (float)(10 * std::log10(std::max(1e-20, ps / cnt)));
    }
    // the mean carrier over the burst
    out.freqHz = freqHz + omega * kSymbolRate / (2 * M_PI);
    out.uwTime = inputTime(pf + kPreambleShort * sps_);
}

} // namespace iridium
} // namespace dect2
