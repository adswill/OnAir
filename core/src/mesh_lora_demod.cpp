// LoRa demodulator: preamble search on de-chirped FFTs, sync word and SFD alignment, fractional carrier and timing estimates from the
// preamble and the down-chirps, symbol demodulation with a timing loop for clock drift, and the coding chain of mesh_lora_code.cpp.
// The method follows gr-lora_sdr's frame_sync_impl.cc and fft_demod_impl.cc and the paper named in mesh_lora.h; the code is ours.
#include "dect2/mesh_lora.h"
#include "dect2/dsp_compat.h"
#include "dect2/gen_util.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <complex>

namespace dect2 {
namespace lora {

namespace {
constexpr double kPi = 3.14159265358979323846;
using cd = std::complex<double>;

double besselI0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 60; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
    return s;
}
double kaiserBeta(double a) { return a > 50 ? 0.1102 * (a - 8.7) : a > 21 ? 0.5842 * std::pow(a - 21, 0.4) + 0.07886 * (a - 21) : 0; }
} // namespace

// ------------------------------------------------------------------ channel buffer and channelizer

void ChanBuf::trim(int64_t keepFrom) {
    if (keepFrom <= base_) return;
    const int64_t drop = std::min<int64_t>(keepFrom - base_, (int64_t)x_.size());
    // move rarely: only when the dead part is large
    if (drop < (int64_t)(1 << 15) && drop * 4 < (int64_t)x_.size()) return;
    x_.erase(x_.begin(), x_.begin() + drop);
    base_ += drop;
}

namespace {
constexpr int kMixBlock = 4096;
}

namespace {
std::vector<float> kaiserLowpass(double fp, double fs, double rate, double att) {
    fs = std::min(fs, 0.499 * rate);
    const double fc = 0.5 * (fp + fs) / rate, df = std::max((fs - fp) / rate, 1e-4);
    int n = (int)std::ceil((att - 7.95) / (14.36 * df)) + 1;
    n = std::min(4001, std::max(9, n)) | 1;
    std::vector<float> h(n);
    const double beta = kaiserBeta(att), m = (n - 1) / 2.0;
    double sum = 0;
    std::vector<double> v(n);
    for (int i = 0; i < n; i++) {
        const double x = i - m;
        const double s = x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
        const double r = x / (m + 0.5);
        v[i] = s * besselI0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / besselI0(beta);
        sum += v[i];
    }
    for (int i = 0; i < n; i++) h[i] = (float)(v[i] / sum);
    return h;
}
} // namespace

Channelizer::Channelizer(double inRate, double offsetHz, double bwHz) : inRate_(inRate), offsetHz_(offsetHz), bwHz_(bwHz) {
    decim_ = std::max(1, (int)std::floor(inRate / (3.0 * bwHz) + 1e-9));
    outRate_ = inRate / decim_;
    // Two stages. The second, at the channel rate, passes the LoRa band plus room for a carrier error and stops 0.3 bw further
    // (noise beyond would fold in when the search samples at the chip rate). The first only keeps the decimation from folding
    // anything onto the second's band.
    const double margin = std::min(25e3, 0.4 * bwHz);
    const double fp = 0.5 * bwHz + margin, fs2 = fp + 0.3 * bwHz;
    taps2_ = kaiserLowpass(fp, fs2, outRate_, 50);
    taps_ = kaiserLowpass(fp, std::max(fs2, outRate_ - fs2), inRate, 60);
    reset();
}

void Channelizer::reset() {
    nIn_ = 0;
    re_.assign(taps_.size() - 1, 0.f);
    im_.assign(taps_.size() - 1, 0.f);
    re2_.assign(taps2_.size() - 1, 0.f);
    im2_.assign(taps2_.size() - 1, 0.f);
}

void Channelizer::process(const cf32* x, size_t n, ChanBuf& buf) {
    const double w = -2 * kPi * offsetHz_ / inRate_;
    size_t i = 0;
    while (i < n) {
        // the mixer's phase restarts exactly at every block of kMixBlock input samples, so chunking does not matter
        const int64_t blk = nIn_ / kMixBlock, off = nIn_ % kMixBlock;
        const size_t run = std::min(n - i, (size_t)(kMixBlock - off));
        const double ph0 = std::fmod(w * (double)(blk * kMixBlock), 2 * kPi);
        const size_t r0 = re_.size();
        re_.resize(r0 + run); im_.resize(r0 + run);
        // phasor recurrence in double inside the block, started from the exact phase of the block's sample `off`
        cd z = std::polar(1.0, ph0 + w * (double)off);
        const cd step = std::polar(1.0, w);
        for (size_t k = 0; k < run; k++) {
            const cf32 v = x[i + k];
            const float zr = (float)z.real(), zi = (float)z.imag();
            re_[r0 + k] = v.real() * zr - v.imag() * zi;
            im_[r0 + k] = v.real() * zi + v.imag() * zr;
            z *= step;
        }
        i += run;
        nIn_ += (int64_t)run;
    }
    const int p = (int)taps_.size();
    if ((int)re_.size() < p) return;
    const int nOut = ((int)re_.size() - p) / decim_ + 1;
    ore_.resize(nOut); oim_.resize(nOut);
    desamp(re_.data(), decim_, taps_.data(), ore_.data(), nOut, p);
    desamp(im_.data(), decim_, taps_.data(), oim_.data(), nOut, p);
    re2_.insert(re2_.end(), ore_.begin(), ore_.end());
    im2_.insert(im2_.end(), oim_.begin(), oim_.end());
    const int p2 = (int)taps2_.size();
    const int n2 = (int)re2_.size() - p2 + 1;
    if (n2 > 0) {
        ore_.resize(n2); oim_.resize(n2);
        desamp(re2_.data(), 1, taps2_.data(), ore_.data(), n2, p2);
        desamp(im2_.data(), 1, taps2_.data(), oim_.data(), n2, p2);
        tmp_.resize(n2);
        for (int k = 0; k < n2; k++) tmp_[k] = cf32(ore_[k], oim_[k]);
        buf.append(tmp_.data(), (size_t)n2);
        re2_.erase(re2_.begin(), re2_.begin() + n2);
        im2_.erase(im2_.begin(), im2_.begin() + n2);
    }
    const size_t used = (size_t)nOut * decim_;
    re_.erase(re_.begin(), re_.begin() + used);
    im_.erase(im_.begin(), im_.begin() + used);
}

// ------------------------------------------------------------------ demodulator

struct Demod::Impl {
    Params p;
    int sf, N;
    double fs, rho;              // channel rate, channel samples per chip
    std::vector<float> dnRe, dnIm;   // conj(up-chirp): multiplying an up-chirp by it leaves a tone
    std::vector<float> re, im, pw, yRe, yIm;   // yRe/yIm: the last de-chirped symbol before its FFT
    // accurate interpolator: windowed sinc with its -6 dB point at bw / 2, kP phases of kK taps
    static constexpr int kP = 64;
    int K = 0;
    std::vector<float> sincTab;
    std::vector<cf32> mixed;
    DemodStats st;

    enum State { Search, Preamble, Data } state = Search;
    double origin = 0;           // channel sample of chip 0 of the search grid
    int64_t win = 0;             // next window of the grid
    std::vector<std::vector<float>> hist;   // power spectra of the last 3 windows (normalised to their mean)
    std::vector<float> preAcc;   // sum of the preamble's spectra
    int histN = 0;
    int kUp = 0;
    float preLevel = 0;          // the preamble's level in its bins (smoothed)
    int64_t preFirst = 0, preEnd = -1;
    // frame
    double posS = 0, perS = 0, chipS = 0;   // next symbol (channel samples), symbol and chip lengths
    double cfoBins = 0, drift = 0;
    double loopInt = 0;
    double posS0 = 0;              // the data start (a frame that fails its CRC is demodulated again from here)
    int retried = 0;
    double snrLin = 0, levelLin = 0;
    double frameStart = 0;
    uint8_t syncSeen = 0;
    FrameDecoder fd;

    Impl(const Params& pp, double fsChan) : p(pp), sf(pp.sf), N(1 << pp.sf), fs(fsChan), rho(fsChan / pp.bwHz) {
        dnRe.resize(N); dnIm.resize(N);
        for (int i = 0; i < N; i++) {
            const double ph = 2 * kPi * ((double)i * i / (2.0 * N) - 0.5 * i);
            dnRe[i] = (float)std::cos(ph); dnIm[i] = (float)-std::sin(ph);
        }
        re.resize(N); im.resize(N); pw.resize(N);
        hist.assign(3, std::vector<float>(N));
        preAcc.assign(N, 0.f);
        // interpolator: -6 dB at bw/2, transition +-0.1 bw, 50 dB
        const double fc = 0.5 / rho, df = 0.2 / rho;
        K = (int)std::ceil((50 - 7.95) / (14.36 * df)) + 1;
        K = std::max(8, (K + 1) & ~1);
        sincTab.assign((size_t)(kP + 1) * K, 0.f);
        const double beta = kaiserBeta(50), half = K / 2.0;
        for (int r = 0; r <= kP; r++) {
            const double f = (double)r / kP;
            double sum = 0;
            for (int k = 0; k < K; k++) {
                const double d = f + half - 1 - k;   // distance from the tap to the wanted point
                const double s = std::fabs(d) < 1e-12 ? 2 * fc : std::sin(2 * kPi * fc * d) / (kPi * d);
                const double rr = d / (half + 0.5);
                const double w = besselI0(beta * std::sqrt(std::max(0.0, 1 - rr * rr))) / besselI0(beta);
                sincTab[(size_t)r * K + k] = (float)(s * w);
                sum += s * w;
            }
            for (int k = 0; k < K; k++) sincTab[(size_t)r * K + k] = (float)(sincTab[(size_t)r * K + k] / sum);
        }
    }

    // samples needed to extract a window that starts at sample t0 with chip spacing step
    double lastSample(double t0, double step) const { return t0 + (N - 1) * step + K / 2 + 3; }
    double firstSample(double t0) const { return t0 - K / 2 - 2; }

    // cheap: cubic interpolation, no mixing (search)
    bool extractFast(const ChanBuf& b, double t0) {
        if (t0 - 2 < (double)b.begin() || t0 + (N - 1) * rho + 3 >= (double)b.end()) return false;
        for (int i = 0; i < N; i++) {
            const double t = t0 + i * rho;
            const int64_t n0 = (int64_t)std::floor(t);
            const float f = (float)(t - (double)n0);
            const cf32* x = b.at(n0 - 1);
            const float cm = -f * (f - 1) * (f - 2) / 6, c0 = (f + 1) * (f - 1) * (f - 2) / 2, c1 = -(f + 1) * f * (f - 2) / 2, c2 = (f + 1) * f * (f - 1) / 6;
            const cf32 v = cm * x[0] + c0 * x[1] + c1 * x[2] + c2 * x[3];
            re[i] = v.real(); im[i] = v.imag();
        }
        return true;
    }

    // accurate: mix out cfo (bins), low-pass to the LoRa band and resample at t0 + i * step
    bool extract(const ChanBuf& b, double t0, double step, double cfo) {
        const int64_t lo = (int64_t)std::floor(firstSample(t0)), hi = (int64_t)std::ceil(lastSample(t0, step));
        if (lo < b.begin() || hi >= b.end()) return false;
        const size_t len = (size_t)(hi - lo + 1);
        mixed.resize(len);
        const double w = -2 * kPi * cfo / (N * rho);          // radians per channel sample
        cd z = std::polar(1.0, std::fmod(w * (double)lo, 2 * kPi));
        const cd stp = std::polar(1.0, w);
        const cf32* x = b.at(lo);
        for (size_t k = 0; k < len; k++) { mixed[k] = x[k] * cf32((float)z.real(), (float)z.imag()); z *= stp; }
        for (int i = 0; i < N; i++) {
            const double t = t0 + i * step - (double)lo;
            int64_t n0 = (int64_t)std::floor(t);
            int r = (int)std::lround((t - (double)n0) * kP);
            const float* h = &sincTab[(size_t)r * K];
            const cf32* s = &mixed[(size_t)(n0 - K / 2 + 1)];
            float ar = 0, ai = 0;
            for (int k = 0; k < K; k++) { ar += h[k] * s[k].real(); ai += h[k] * s[k].imag(); }
            re[i] = ar; im[i] = ai;
        }
        return true;
    }

    // de-chirp (up: remove an up-chirp; else remove a down-chirp) and FFT; fills pw, returns the mean power
    float spectrum(bool up, bool keep = false) {
        for (int i = 0; i < N; i++) {
            const float a = re[i], c = im[i], dr = dnRe[i], di = up ? dnIm[i] : -dnIm[i];
            re[i] = a * dr - c * di; im[i] = a * di + c * dr;
        }
        if (keep) { yRe = re; yIm = im; }
        fftSplit(re.data(), im.data(), sf, false);
        double s = 0;
        for (int k = 0; k < N; k++) { pw[k] = re[k] * re[k] + im[k] * im[k]; s += pw[k]; }
        return (float)(s / N + 1e-30);
    }
    int argmax() const { return (int)(std::max_element(pw.begin(), pw.end()) - pw.begin()); }
    int wrap(int k) const { return ((k % N) + N) % N; }
    float near3(const std::vector<float>& v, int k) const { return std::max(v[wrap(k)], std::max(v[wrap(k - 1)], v[wrap(k + 1)])); }
    // fractional peak position around bin k from the complex FFT (Candan's bias-corrected Jacobsen estimator)
    double frac(int k) const {
        const std::complex<double> a(re[wrap(k - 1)], im[wrap(k - 1)]), b(re[wrap(k)], im[wrap(k)]), c(re[wrap(k + 1)], im[wrap(k + 1)]);
        const std::complex<double> den = 2.0 * b - a - c;
        if (std::abs(den) < 1e-20) return 0;
        const double d = std::real((a - c) / den) * std::tan(kPi / N) / (kPi / N);
        return std::max(-0.5, std::min(0.5, d));
    }

    void toSearch(double from) {
        state = Search;
        origin = from; win = 0; histN = 0; preEnd = -1;
    }

    double winStart(int64_t w) const { return origin + (double)w * N * rho; }

    bool sfd(const ChanBuf& b);
    void dataSymbol(const ChanBuf& b, std::vector<RxFrame>& out);
    void run(const ChanBuf& b, std::vector<RxFrame>& out);
};

void Demod::Impl::run(const ChanBuf& b, std::vector<RxFrame>& out) {
    for (int guard = 0; guard < 100000; guard++) {
        if (state == Data) {
            if (std::ceil(lastSample(posS, chipS)) + 1 >= (double)b.end()) return;
            dataSymbol(b, out);
            continue;
        }
        if (state == Preamble && preEnd >= 0) {
            // the SFD search looks five windows past the end of the preamble
            if (winStart(preEnd + 6) + K + 4 >= (double)b.end()) return;
            if (!sfd(b)) { st.syncBad++; toSearch(winStart(preEnd)); }
            continue;
        }
        const double t0 = winStart(win);
        if (t0 + N * rho + 4 >= (double)b.end()) return;
        if (t0 - 2 < (double)b.begin()) { win++; continue; }    // history was dropped: skip
        extractFast(b, t0);
        const float mean = spectrum(true);
        std::vector<float>& h = hist[(size_t)(win % 3)];
        for (int k = 0; k < N; k++) h[k] = pw[k] / mean;
        histN++;
        if (state == Search) {
            if (histN >= 3) {
                // three windows in a row with their energy in the same pair of bins
                const auto& a = hist[0]; const auto& bb = hist[1]; const auto& c = hist[2];
                int best = 0; float bestQ = 0;
                for (int k = 0; k < N; k++) {
                    const int k1 = (k + 1) & (N - 1);
                    const float q = (a[k] + bb[k] + c[k] + a[k1] + bb[k1] + c[k1]) / 3.f;
                    if (q > bestQ) { bestQ = q; best = k; }
                }
                bool each = true;
                for (int j = 0; j < 3; j++) {
                    const auto& v = hist[(size_t)j];
                    each &= std::max(v[(size_t)best] + v[(size_t)((best + 1) & (N - 1))], std::max(v[(size_t)wrap(best - 1)] + v[(size_t)best], v[(size_t)((best + 1) & (N - 1))] + v[(size_t)((best + 2) & (N - 1))])) > 4.f;
                }
                if (bestQ > 7.5f && each) {
                    state = Preamble;
                    st.preambles++;
                    std::fill(preAcc.begin(), preAcc.end(), 0.f);
                    for (int j = 0; j < 3; j++) for (int k = 0; k < N; k++) preAcc[k] += hist[(size_t)j][k];
                    kUp = (int)(std::max_element(preAcc.begin(), preAcc.end()) - preAcc.begin());
                    preLevel = bestQ;
                    preFirst = win - 2;
                    preEnd = -1;
                }
            }
        } else {
            // still the preamble while the energy stays near kUp (followed bin by bin: a clock error moves it slowly), above a
            // quarter of the preamble's level so far (two noise bins alone pass a fixed low threshold too often)
            float v = 0;
            for (int d = -2; d <= 2; d++) {
                const int k = wrap(kUp + d);
                const float q = h[(size_t)k] + std::max(h[(size_t)wrap(k - 1)], h[(size_t)wrap(k + 1)]);
                v = std::max(v, q);
            }
            if (v > std::max(5.f, 0.25f * preLevel) && win - preFirst < 80) {
                // the bin of the recent windows (forgetting the old ones follows a drift; one window alone is too noisy)
                for (int k = 0; k < N; k++) preAcc[k] = 0.7f * preAcc[k] + h[k];
                kUp = (int)(std::max_element(preAcc.begin(), preAcc.end()) - preAcc.begin());
                preLevel = 0.8f * preLevel + 0.2f * v;
            } else if (win - preFirst >= 80) {
                toSearch(winStart(win));                // a steady carrier, not a frame
                continue;
            } else {
                preEnd = win;
            }
        }
        win++;
    }
}

// Finds the first down-chirp after the preamble, checks the sync word and measures the carrier and timing offsets.
bool Demod::Impl::sfd(const ChanBuf& b) {
    const int64_t e = preEnd;
    // the down-chirp's bin on the (unaligned) grid: every window that holds a part of the 2.25 down-chirps puts it in the same bin
    std::vector<float> dacc(N, 0.f);
    for (int64_t v = e; v <= e + 3; v++) {
        if (!extractFast(b, winStart(v))) return false;
        const float mean = spectrum(false);
        for (int k = 0; k < N; k++) dacc[k] += pw[k] / mean;
    }
    int kDn = 0; float bestDn = 0;
    for (int k = 0; k < N; k++) {
        const float q = dacc[k] + std::max(dacc[(size_t)wrap(k - 1)], dacc[(size_t)wrap(k + 1)]);
        if (q > bestDn) { bestDn = q; kDn = dacc[(size_t)wrap(k - 1)] > dacc[(size_t)wrap(k + 1)] && dacc[(size_t)wrap(k - 1)] > dacc[k] ? wrap(k - 1) : k; }
    }
    if (bestDn < 10.f + sf) return false;      // noise alone reaches about 14 at SF7, 20 at SF12
    // up-chirp bin = cfo - tau, down-chirp bin = cfo + tau (tau: where the symbols start, in chips, from the grid's windows);
    // both bins may be one off, so tau is tried two chips either side, and half a symbol away (the sum fixes cfo only modulo N / 2)
    const int diff = wrap(kDn - kUp);
    int taus[10]; int nt = 0;
    for (int d = -2; d <= 2; d++) { taus[nt++] = wrap(diff / 2 + d); taus[nt++] = wrap(diff / 2 + d + N / 2); }
    const int sw1 = syncSymbol(p.syncWord, 0, sf), sw2 = syncSymbol(p.syncWord, 1, sf);
    // Every candidate (tau, m) is scored first on the cheap windows (cached per tau: 9 up and 6 down windows serve all m),
    // the three best again on windows resampled with their carrier offset mixed out (full sensitivity).
    struct Cand { double score; int tau, m, cfoC; };
    std::vector<Cand> cands;
    std::vector<std::vector<float>> upW(9, std::vector<float>(N)), dnW(6, std::vector<float>(N));
    std::vector<int> upOk(9), dnOk(6);
    for (int ti = 0; ti < nt; ti++) {
        const int tau = taus[ti];
        const int cfoC = wrap(kUp + tau);
        // up windows start at (e + j) N + tau for j = -4..4, down windows for j = -1..4
        for (int j = -4; j <= 4; j++) {
            upOk[(size_t)(j + 4)] = extractFast(b, origin + (double)((e + j) * N + tau) * rho);
            if (!upOk[(size_t)(j + 4)]) continue;
            const float m = spectrum(true);
            for (int k = 0; k < N; k++) upW[(size_t)(j + 4)][(size_t)k] = pw[(size_t)k] / m;
        }
        for (int j = -1; j <= 4; j++) {
            dnOk[(size_t)(j + 1)] = extractFast(b, origin + (double)((e + j) * N + tau) * rho);
            if (!dnOk[(size_t)(j + 1)]) continue;
            const float m = spectrum(false);
            for (int k = 0; k < N; k++) dnW[(size_t)(j + 1)][(size_t)k] = pw[(size_t)k] / m;
        }
        for (int m = -1; m <= 3; m++) {
            // first down-chirp at (e + m) N + tau: up windows at m-3, m-2, m-1, down windows at m, m+1
            if (!upOk[(size_t)(m + 1)] || !upOk[(size_t)(m + 2)] || !upOk[(size_t)(m + 3)] || !dnOk[(size_t)(m + 1)] || !dnOk[(size_t)(m + 2)]) continue;
            const double sc = near3(upW[(size_t)(m + 1)], cfoC) + near3(upW[(size_t)(m + 2)], cfoC + sw1) + near3(upW[(size_t)(m + 3)], cfoC + sw2) +
                              near3(dnW[(size_t)(m + 1)], cfoC) + near3(dnW[(size_t)(m + 2)], cfoC);
            cands.push_back({sc, tau, m, cfoC});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.score > y.score; });
    if (cands.size() > 3) cands.resize(3);
    std::vector<float> w0(N), w1(N), w2(N), w3(N), w4(N);
    double cfoMix = 0;
    auto norm = [&](bool up, double t0, std::vector<float>& o) {
        if (!extract(b, t0, rho, cfoMix)) return false;
        const float m = spectrum(up);
        for (int k = 0; k < N; k++) o[k] = pw[k] / m;
        return true;
    };
    double bestScore = 0; int64_t bestS = 0; int bestCfo = 0, bestV1 = 0, bestV2 = 0;
    for (const auto& c : cands) {
        cfoMix = c.cfoC >= N / 2 ? c.cfoC - N : c.cfoC;     // after mixing, the expected bins are relative to it
        const int64_t s = (e + c.m) * N + c.tau;
        const double t = origin;
        if (!norm(true, t + (double)(s - 3 * N) * rho, w0) || !norm(true, t + (double)(s - 2 * N) * rho, w1) ||
            !norm(true, t + (double)(s - N) * rho, w2) || !norm(false, t + (double)s * rho, w3) || !norm(false, t + (double)(s + N) * rho, w4))
            continue;
        const double score = near3(w0, 0) + near3(w1, sw1) + near3(w2, sw2) + near3(w3, 0) + near3(w4, 0);
        if (score > bestScore) {
            bestScore = score; bestS = s; bestCfo = c.cfoC;
            bestV1 = (int)(std::max_element(w1.begin(), w1.end()) - w1.begin());
            bestV2 = (int)(std::max_element(w2.begin(), w2.end()) - w2.begin());
        }
    }
    if (bestScore < 15) return false;
    auto close = [&](int v, int want) { const int d = wrap(v - want); return d <= 2 || d >= N - 2; };
    if (!close(bestV1, sw1) || !close(bestV2, sw2)) return false;
    syncSeen = (uint8_t)(((bestV1 + 4) >> 3) << 4 | (((bestV2 + 4) >> 3) & 0xF));

    // fine estimates on accurately resampled windows, twice
    double s = (double)bestS;                          // chips from the origin
    double cfo = bestCfo >= N / 2 ? bestCfo - N : bestCfo;
    const int preWins = (int)(e - preFirst);
    const int J = std::max(1, std::min(12, preWins - 2));
    double slope = 0;
    for (int it = 0; it < 2; it++) {
        double epsUp = 0, wUp = 0, epsDn = 0, wDn = 0;
        cd acc(0, 0);
        std::vector<double> ej(J + 1, 0.0);
        std::vector<int> okj(J + 1, 0);
        std::vector<double> wj(J + 1, 0.0);
        std::vector<cd> xs((size_t)(J + 1) * 5);         // bins -2..2 of every preamble window
        double sig = 0, noi = 0; int ns = 0;
        for (int j = J; j >= 1; j--) {
            const double t0 = origin + (s - (2 + j) * N) * rho;
            if (!extract(b, t0, rho, cfo)) continue;
            const float mean = spectrum(true);
            int k = 0; float bp = 0;
            for (int d = -2; d <= 2; d++) if (pw[(size_t)wrap(d)] > bp) { bp = pw[(size_t)wrap(d)]; k = d; }
            const double f = k + frac(k);
            epsUp += f * bp; wUp += bp;
            ej[(size_t)j] = f; okj[(size_t)j] = 1; wj[(size_t)j] = bp;
            for (int d = -2; d <= 2; d++) xs[(size_t)j * 5 + (size_t)(d + 2)] = cd(re[(size_t)wrap(d)], im[(size_t)wrap(d)]);
            // noise from the bins far from the peak (the band edges of the filter spread a little energy around it)
            double nf = 0; int nn = 0;
            for (int d = N / 8; d <= N - N / 8; d++) { nf += pw[(size_t)wrap(k + d)]; nn++; }
            nf /= std::max(1, nn);
            sig += std::max(0.0, (double)mean * N - nf * N); noi += nf; ns++;
        }
        {
            // the phase of one bin advances by 2 pi times the carrier offset from window to window
            int kb = 0; double bp = -1;
            for (int d = 0; d < 5; d++) {
                double e = 0;
                for (int j = 1; j <= J; j++) e += std::norm(xs[(size_t)j * 5 + (size_t)d]);
                if (e > bp) { bp = e; kb = d; }
            }
            for (int j = J; j >= 2; j--)
                if (okj[(size_t)j] && okj[(size_t)(j - 1)]) acc += xs[(size_t)(j - 1) * 5 + (size_t)kb] * std::conj(xs[(size_t)j * 5 + (size_t)kb]);
        }
        for (int d = 0; d < 2; d++) {
            const double t0 = origin + (s + d * N) * rho;
            if (!extract(b, t0, rho, cfo)) return false;
            spectrum(false);
            int k = 0; float bp = 0;
            for (int dd = -2; dd <= 2; dd++) if (pw[(size_t)wrap(dd)] > bp) { bp = pw[(size_t)wrap(dd)]; k = dd; }
            epsDn += (k + frac(k)) * bp; wDn += bp;
        }
        if (wUp <= 0 || wDn <= 0) return false;
        epsDn /= wDn;
        // the up-chirps' peak against the window index (a clock error moves it by the drift per symbol), taken where the
        // down-chirps are (j = -2.5)
        {
            double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
            for (int j = 1; j <= J; j++) if (okj[(size_t)j]) { const double w = wj[(size_t)j]; sw += w; sx += w * j; sy += w * ej[(size_t)j]; sxx += w * j * j; sxy += w * j * ej[(size_t)j]; n++; }
            slope = 0;
            if (n >= 4) { const double den = sw * sxx - sx * sx; if (den > 0) slope = (sw * sxy - sx * sy) / den; }
            slope = std::max(-2.5e-4 * N, std::min(2.5e-4 * N, slope));   // up to 250 ppm
            epsUp = (sy - slope * sx) / sw + slope * -2.5;
        }
        double dres = 0.5 * (epsUp + epsDn);
        const double tres = 0.5 * (epsDn - epsUp) - 0.5 * slope;    // at the first down-chirp
        if (std::abs(acc) > 0) {
            const double ph = std::arg(acc) / (2 * kPi);
            double dd = ph - dres;
            dd -= std::round(dd);
            dres += dd;
        }
        cfo += dres;
        s += tres;
        if (ns > 0) {
            // SNR in the LoRa band from the preamble: peak bins against the mean (which is the noise per bin)
            const double sMean = sig / ns, nMean = noi / ns;
            snrLin = std::max(1e-4, sMean / (N * std::max(nMean, 1e-30)));
            levelLin = sMean / ((double)N * N);
        }
    }
    cfoBins = cfo;
    drift = slope;                                     // chips per symbol
    chipS = rho * (1 + drift / N);
    perS = N * chipS;
    // the data start 2.25 symbols after the first down-chirp (and the clock error of 2.25 symbols later)
    posS = origin + (s + 2.25 * (N + drift)) * rho;
    frameStart = origin + (s - (2.0 + p.preamble) * N) * rho;   // the first up-chirp of the preamble (of the configured length)
    loopInt = 0;
    posS0 = posS; retried = 0;
    fd.start(sf, p.ldro);
    state = Data;
    return true;
}

void Demod::Impl::dataSymbol(const ChanBuf& b, std::vector<RxFrame>& out) {
    if (!extract(b, posS, chipS, cfoBins)) { toSearch(posS); return; }
    const float mean = spectrum(true, true);
    const int k = argmax();
    const double q = pw[(size_t)k] / mean;
    fd.push((uint16_t)k);
    // Timing loop. The peak's fractional position does not show a timing error: the shift of the tone is undone, on average,
    // by the phase step where the chirp wraps. That step itself measures it: the de-chirped symbol's two parts (before and after
    // the wrap at chip N - k) differ in phase by pi (tau + residual carrier offset), tau = how late the symbol starts, in chips.
    double adj = 0;
    // wider loop for the first 40 symbols (the drift from a short preamble may be well off), then the narrow one
    constexpr double kI0 = 0.3, kP0 = 0.8;
    constexpr int kAcq = 40;
    const bool acq = fd.pushed() <= kAcq;
    const double kI = acq ? kI0 : 0.02, kP_ = acq ? kP0 : 0.3;
    const int fold = (N - k) % N;
    const double w = 4.0 * fold * (N - fold) / ((double)N * N);
    if (q > 8 && w > 0.25) {
        cd s1(0, 0), s2(0, 0), rot(1, 0);
        const cd stp = std::polar(1.0, -2 * kPi * k / N);
        for (int i = 0; i < N; i++) {
            const cd v = cd(yRe[(size_t)i], yIm[(size_t)i]) * rot;
            if (i < fold) s1 += v; else s2 += v;
            rot *= stp;
        }
        const double tau = std::arg(s2 * std::conj(s1)) / kPi;
        const double e = std::max(-0.5, std::min(0.5, tau)) * w;
        loopInt += kI * e;
        adj = kP_ * e + loopInt;
    } else adj = loopInt;
    posS += perS + adj * chipS;
    if (fd.pushed() == 8 && !fd.header().ok) { st.headerBad++; toSearch(posS); return; }
    if (fd.done()) {
        RxFrame f;
        int corr = 0;
        fd.finish(f.payload, f.crcOk, corr);
        if (fd.header().crc && !f.crcOk && retried < 2 && std::fabs(loopInt) > 0.02) {
            // A short preamble at a low SNR leaves the clock drift poorly known: the timing loop catches up, but the first symbols
            // may be lost. The loop's integral then holds the rest of the drift: demodulate the frame again from its start with it.
            retried++;
            const double dd = loopInt;
            drift += dd;
            chipS = rho * (1 + drift / N);
            perS = N * chipS;
            posS0 += 1.75 * dd * rho;
            posS = posS0;           // the start was placed 2.25 symbols past the down-chirp, less half the slope
            loopInt = 0;
            fd.start(sf, p.ldro);
            return;
        }
        f.hdr = fd.header();
        f.corrected = corr;
        f.cfoHz = cfoBins * p.bwHz / N;
        f.sfoPpm = (drift + loopInt) / N * 1e6;
        f.snrDb = 10 * std::log10(snrLin);
        f.levelDb = 10 * std::log10(std::max(levelLin, 1e-20));
        f.startSample = frameStart;
        f.endSample = posS;
        f.syncWord = syncSeen;
        st.frames++;
        if (f.hdr.crc && !f.crcOk) st.crcBad++;
        out.push_back(std::move(f));
        toSearch(posS);
    }
}

Demod::Demod(const Params& p, double fsChan, int) : p_(std::make_unique<Impl>(p, fsChan)) {}
Demod::~Demod() = default;
const Params& Demod::params() const { return p_->p; }
void Demod::reset(int64_t from) { p_->toSearch((double)from); p_->st = DemodStats(); }
void Demod::process(const ChanBuf& b, std::vector<RxFrame>& out) { p_->run(b, out); }
bool Demod::busy() const { return p_->state != Impl::Search; }
const DemodStats& Demod::stats() const { return p_->st; }
int64_t Demod::oldestNeeded() const {
    const Impl& m = *p_;
    if (m.state == Impl::Data) return (int64_t)std::floor(m.retried >= 2 ? m.posS : std::min(m.posS, m.posS0 - 4 * m.rho)) - m.K - 8;
    // the SFD search and the fine estimates look back up to 13 windows
    const double back = m.winStart(m.win - 14);
    return (int64_t)std::floor(std::max(back, m.origin - 8.0)) - m.K - 8;
}

} // namespace lora
} // namespace dect2
