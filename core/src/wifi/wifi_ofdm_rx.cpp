// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi OFDM receiver for non-HT 6 Mbit/s frames at 20 Msps (see wifi_ofdm.h; IEEE 802.11-2020 clause 17).
//
// Stage 1 (every sample): sliding lag-16 autocorrelation of the short training; a plateau of |C|^2 / P^2 above 0.5 triggers.
// Stage 2 (a few hundred samples later): coarse carrier offset from the plateau, cross-correlation with the long training symbol to find its
//   start, fine offset from the two long symbols (64 apart), channel estimate, SIGNAL decode.
// Stage 3 (when the whole frame is in the buffer): DATA symbols, soft Viterbi, descramble, FCS.
#include "dect2/wifi/wifi_ofdm.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/wifi/wifi_proto.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2::wifi::ofdm {

namespace {
using dect2::math::kPi;
constexpr int kW = 80;                  // autocorrelation window, samples
constexpr int kLag = 16;
constexpr float kThresh = 0.5f;
constexpr int kMinRun = 24;             // samples the metric must stay above the threshold
constexpr int kSyncWait = 400;          // samples after the trigger before the long training is searched
constexpr int kSearch = 560;            // samples searched for the long training
constexpr int kKeep = 1200;             // history kept behind the newest sample
const int kPilotK[4] = {-21, -7, 7, 21};
const int kPilotBase[4] = {1, 1, 1, -1};
inline int bin(int k) { return (k + kFft) % kFft; }
inline bool isPilot(int k) { return k == -21 || k == -7 || k == 7 || k == 21; }

const std::vector<int>& dataK() {
    static const std::vector<int> v = [] { std::vector<int> r; for (int k = -26; k <= 26; k++) if (k && !isPilot(k)) r.push_back(k); return r; }();
    return v;
}
} // namespace

struct OfdmRx::Impl {
    FrameCb cb;
    Stats st;
    std::vector<cf32> b;                // samples base ... base + b.size() - 1
    int64_t base = 0, n = 0;            // n: samples received in all
    std::complex<double> C{0, 0};
    double P = 0;
    int run = 0;                        // consecutive samples above the threshold
    double bestM = 0;
    std::complex<double> bestC{0, 0};
    int64_t trig = -1;                  // index where the plateau was recognised
    int64_t skipUntil = 0;              // no trigger before this sample
    double cfoCoarse = 0;
    Fft fft{kFft};

    // a decoded packet waiting for its last sample
    bool pending = false;
    int64_t pd = 0, pend = 0;
    double pcfo = 0;
    int plen = 0, pnsym = 0;
    std::vector<cf32> H;                // channel estimate per carrier index k + 26 (0 for DC)
    float havg = 1;

    const cf32& at(int64_t i) const { return b[(size_t)(i - base)]; }

    void reset() { b.clear(); base = n = 0; C = 0; P = 0; run = 0; bestM = 0; trig = -1; skipUntil = 0; pending = false; }

    // derotated sample i (carrier offset f, phase 0 at sample ref)
    cf32 rot(int64_t i, double f, int64_t ref) const {
        const double a = -2 * kPi * f * (double)(i - ref) / kRate;
        const std::complex<double> v = std::complex<double>(at(i).real(), at(i).imag()) * std::polar(1.0, a);
        return cf32((float)v.real(), (float)v.imag());
    }

    // FFT of 64 derotated samples starting at s: carriers k = -26 .. 26 into out[k + 26]
    void carriers(int64_t s, double f, int64_t ref, cf32* out) {
        cf32 t[kFft];
        for (int i = 0; i < kFft; i++) t[i] = rot(s + i, f, ref);
        fft.forward(t);
        for (int k = -26; k <= 26; k++) out[k + 26] = t[bin(k)];
    }

    // Samples n .. n + k - 1, already in b. The products the running sums take in and give back are worked out for the whole block first (no
    // dependency between samples, so it vectorises); the sums themselves, the trigger and the later stages then go sample by sample.
    void feedBlock(int k) {
        const cf32* bp = b.data() - base;   // bp[i] = sample i
        dC.resize((size_t)k); dP.resize((size_t)k);
        const int64_t m0 = n;
        int j0 = 0;
        for (; j0 < k && m0 + j0 < kW + kLag; j0++) {   // the start of the stream
            const int64_t m = m0 + j0;
            const cf32 x = bp[m];
            float p = std::norm(x);
            cf32 c(0, 0);
            if (m >= kLag) c = bp[m - kLag] * std::conj(x);
            if (m >= kW) p -= std::norm(bp[m - kW]);
            dC[(size_t)j0] = c; dP[(size_t)j0] = p;
        }
        {   // everywhere else: plain arithmetic on the real and imaginary parts
            const float* xs = reinterpret_cast<const float*>(bp + m0);
            const float* as = reinterpret_cast<const float*>(bp + m0 - kLag);
            const float* us = reinterpret_cast<const float*>(bp + m0 - kW - kLag);
            const float* vs = reinterpret_cast<const float*>(bp + m0 - kW);
            float* dc = reinterpret_cast<float*>(dC.data());
            float* dp = dP.data();
            for (int j = j0; j < k; j++) {
                const float xr = xs[2 * j], xi = xs[2 * j + 1], ar = as[2 * j], ai = as[2 * j + 1];
                const float ur = us[2 * j], ui = us[2 * j + 1], vr = vs[2 * j], vi = vs[2 * j + 1];
                dc[2 * j] = (ar * xr + ai * xi) - (ur * vr + ui * vi);        // a conj(x) - u conj(v)
                dc[2 * j + 1] = (ai * xr - ar * xi) - (ui * vr - ur * vi);
                dp[j] = (xr * xr + xi * xi) - (vr * vr + vi * vi);
            }
        }
        // the state in locals (written back around the rare calls that read or change it, so the loop is not held up by stores to memory)
        double cr = C.real(), ci = C.imag(), pp = P;
        int64_t nn = n;
        for (int j = 0; j < k; j++) {
            const int64_t m = nn++;
            cr += (double)dC[(size_t)j].real(); ci += (double)dC[(size_t)j].imag();
            pp += (double)dP[(size_t)j];
            const double nc = cr * cr + ci * ci;
            if (m < kW + kLag + 2 || m < skipUntil || pending || trig >= 0) { if (trig < 0) run = 0; }
            else if (pp > 1e-12 && nc > kThresh * pp * pp) {
                const double met = nc / (pp * pp);
                if (met > bestM) { bestM = met; bestC = std::complex<double>(cr, ci); }
                if (++run >= kMinRun) {
                    // A narrowband signal (a Bluetooth packet, a carrier) repeats after 16 samples as well, but it also does after 8: the short
                    // training does not (its tones lie 1.25 MHz apart, so half of them turn by half a cycle in 8 samples and the sum cancels). Such a
                    // trigger would only cost a long-training search that fails.
                    std::complex<double> c8(0, 0);
                    for (int64_t i = m - kW + 9; i <= m; i++) c8 += std::complex<double>(at(i - 8)) * std::conj(std::complex<double>(at(i)));
                    if (std::abs(c8) > 0.5 * std::sqrt(nc)) { run = 0; bestM = 0; st.narrow++; }
                    else { trig = m; st.triggers++; cfoCoarse = -std::arg(bestC) * kRate / (2 * kPi * kLag); }
                }
            } else if (run) { run = 0; bestM = 0; }
            // act as soon as the trigger's wait or the packet's end is reached (a block may span several packets)
            if ((trig >= 0 && nn >= trig + kSyncWait) || (pending && nn >= pend + 4)) {
                C = std::complex<double>(cr, ci); P = pp; n = nn;
                run_();
            }
        }
        C = std::complex<double>(cr, ci); P = pp; n = nn;
    }
    std::vector<cf32> dC;
    std::vector<float> dP;

    // long-training search and SIGNAL decode; true: a frame is pending
    bool sync() {
        const std::vector<cf32>& L = ltfSymbol();
        const int64_t r0 = std::max(base, trig - 260);
        const int64_t rEnd = std::min(n - (kFft * 2 + 4), r0 + kSearch);
        if (rEnd <= r0) { st.noSync++; return false; }
        // derotate the search region once
        const size_t cnt = (size_t)(rEnd - r0) + 2 * kFft + 4;
        std::vector<cf32> y(cnt);
        for (size_t i = 0; i < cnt && r0 + (int64_t)i < n; i++) y[i] = rot(r0 + (int64_t)i, cfoCoarse, r0);
        std::vector<float> mm((size_t)(rEnd - r0));
        double mean = 0;
        for (size_t d = 0; d < mm.size(); d++) {
            std::complex<float> a(0, 0);
            for (int i = 0; i < kFft; i++) a += y[d + (size_t)i] * std::conj(L[(size_t)i]);
            mm[d] = std::abs(a);
            mean += mm[d];
        }
        mean /= (double)mm.size();
        size_t bd = 0; float bv = 0;
        for (size_t d = 0; d + kFft < mm.size(); d++) { const float v = mm[d] + mm[d + kFft]; if (v > bv) { bv = v; bd = d; } }
        if (bv < 6 * mean) { st.noSync++; return false; }   // no sharp long-training pair
        // fine offset from the two long symbols
        std::complex<double> cc(0, 0);
        for (int i = 0; i < kFft; i++) cc += std::complex<double>(y[bd + (size_t)i]) * std::conj(std::complex<double>(y[bd + (size_t)kFft + (size_t)i]));
        const double f = cfoCoarse - std::arg(cc) * kRate / (2 * kPi * kFft);
        const int64_t d0 = r0 + (int64_t)bd;
        if (d0 < base || d0 + 128 + kSym + 4 > n) { st.noSync++; return false; }
        pcfo = f; pd = d0;
        // channel estimate from the two long symbols
        cf32 A[53], B[53];
        carriers(d0, f, d0, A);
        carriers(d0 + kFft, f, d0, B);
        H.assign(53, cf32(0, 0));
        double hp = 0;
        for (int k = -26; k <= 26; k++) {
            if (!k) continue;
            const float l = (float)ltfSign(k);
            H[(size_t)(k + 26)] = (A[k + 26] + B[k + 26]) * 0.5f * l;   // L is +-1: dividing by it is multiplying
            hp += std::norm(H[(size_t)(k + 26)]);
        }
        havg = (float)(hp / 52);
        if (havg < 1e-12f) { st.noSync++; return false; }
        // SIGNAL
        std::vector<float> soft(48);
        float sigErr = 0;
        symbol(d0 + 128, 0, soft.data(), sigErr, nullptr);
        std::vector<float> de(48);
        for (int k = 0; k < 48; k++) de[(size_t)k] = soft[(size_t)(3 * (k % 16) + k / 16)];
        uint8_t bits[24];
        std::vector<float> s2(48);
        for (int i = 0; i < 24; i++) { s2[(size_t)(2 * i)] = de[(size_t)(2 * i)]; s2[(size_t)(2 * i + 1)] = de[(size_t)(2 * i + 1)]; }
        viterbiDecode(s2.data(), 24, bits);
        uint8_t par = 0;
        for (int i = 0; i < 18; i++) par ^= bits[i];
        int len = 0;
        for (int i = 0; i < 12; i++) len |= bits[5 + i] << i;
        int tail = 0;
        for (int i = 18; i < 24; i++) tail |= bits[i];
        if (par || bits[4] || tail) { st.signalBad++; return false; }
        const int rate = bits[0] | bits[1] << 1 | bits[2] << 2 | bits[3] << 3;
        pqpsk = allowQpsk && rate == kRate12;
        if (rate != kRate6 && !pqpsk) { st.otherRate++; st.signalOk++; skipUntil = d0 + 208 + 80; return false; }
        if (len < 14 || len > 4095) { st.signalBad++; return false; }
        st.signalOk++;
        plen = len;
        const int dbps = pqpsk ? 48 : 24;       // data bits per symbol; the receiver only reads the two lowest rates
        pnsym = (16 + 8 * len + 6 + dbps - 1) / dbps;
        pend = d0 + 208 + (int64_t)kSym * pnsym;
        return true;
    }

    static int ltfSign(int k) {
        static const int8_t L[53] = {1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 0,
                                     1, -1, -1, 1, 1, -1, 1, -1, 1, -1, -1, -1, -1, -1, 1, 1, -1, -1, 1, -1, 1, -1, 1, 1, 1, 1};
        return L[k + 26];
    }

    // One symbol at start s (the start of its 80 samples): equalise, track the pilots, 48 soft values (a one positive). err accumulates the
    // squared distance of the decided points, nErr counts them
    int nErr = 0;
    bool allowQpsk = false, pqpsk = false;
    void symbol(int64_t s, int index, float* soft, float& err, double* /*unused*/) {
        cf32 Y[53];
        carriers(s + kCp, pcfo, pd, Y);
        const int pol = pilotPolarity(index);
        std::complex<double> acc(0, 0);
        cf32 E[53];
        for (int k = -26; k <= 26; k++) { if (k) E[k + 26] = Y[k + 26] / H[(size_t)(k + 26)]; else E[26] = 0; }
        for (int i = 0; i < 4; i++) acc += std::complex<double>(E[kPilotK[i] + 26]) * (double)(kPilotBase[i] * pol);
        const double a = std::arg(acc);
        double num = 0, den = 0;
        for (int i = 0; i < 4; i++) {
            const std::complex<double> v = std::complex<double>(E[kPilotK[i] + 26]) * (double)(kPilotBase[i] * pol) * std::polar(1.0, -a);
            num += kPilotK[i] * std::arg(v); den += (double)kPilotK[i] * kPilotK[i];
        }
        const double slope = den > 0 ? num / den : 0;
        const auto& dk = dataK();
        for (int j = 0; j < 48; j++) {
            const int k = dk[(size_t)j];
            const std::complex<double> e = std::complex<double>(E[k + 26]) * std::polar(1.0, -(a + slope * k));
            const float w = std::norm(H[(size_t)(k + 26)]) / havg;
            if (pqpsk && index > 0) {        // SIGNAL is always BPSK
                soft[2 * j] = (float)e.real() * w; soft[2 * j + 1] = (float)e.imag() * w;
                const double dr = std::fabs(e.real()) * std::sqrt(2.0) - 1.0, di = std::fabs(e.imag()) * std::sqrt(2.0) - 1.0;
                err += (float)(dr * dr + di * di);
            } else {
                soft[j] = (float)e.real() * w;
                const double dr = std::fabs(e.real()) - 1.0;
                err += (float)(dr * dr + e.imag() * e.imag());
            }
            nErr++;
        }
    }

    void decodeData() {
        const int cbps = pqpsk ? 96 : 48;
        std::vector<float> coded((size_t)pnsym * cbps);
        float err = 0;
        nErr = 0;
        for (int s = 0; s < pnsym; s++) {
            float soft[96];
            symbol(pd + 208 + (int64_t)kSym * s, s + 1, soft, err, nullptr);
            for (int k = 0; k < cbps; k++) coded[(size_t)s * cbps + (size_t)k] = soft[cbps / 16 * (k % 16) + k / 16];     // Equation 17-17; the second permutation does nothing below 16-QAM
        }
        const int nb = 16 + 8 * plen + 6;
        std::vector<uint8_t> bits((size_t)nb);
        viterbiDecode(coded.data(), nb, bits.data());
        // the first seven SERVICE bits are zero before scrambling, so the first seven decoded bits are the scrambler's output: its state is those bits
        unsigned stt = 0;
        for (int i = 0; i < 7; i++) stt |= (unsigned)bits[(size_t)i] << (6 - i);
        std::vector<uint8_t> rest(bits.begin() + 7, bits.end());
        scramble(rest, (uint8_t)stt);
        std::vector<uint8_t> f((size_t)plen, 0);
        for (int i = 0; i < plen; i++) for (int k = 0; k < 8; k++) f[(size_t)i] |= (uint8_t)(rest[(size_t)(9 + 8 * i + k)] << k);
        const size_t nn = f.size() - 4;
        const uint32_t got = (uint32_t)f[nn] | (uint32_t)f[nn + 1] << 8 | (uint32_t)f[nn + 2] << 16 | (uint32_t)f[nn + 3] << 24;
        if (crc32(f.data(), nn) != got) { st.framesBad++; return; }
        st.framesOk++;
        Info in;
        in.cfoHz = pcfo; in.at = pd; in.length = plen;
        in.snrDb = nErr ? (float)(-10 * std::log10(err / (float)nErr + 1e-9)) : 0;
        double pw = 0;
        const int64_t a = std::max(base, pd - 320), z = std::min(n, pend);
        for (int64_t i = a; i < z; i++) pw += std::norm(at(i));
        in.rssiDb = (float)(10 * std::log10(pw / (double)std::max<int64_t>(1, z - a) + 1e-20));
        if (cb) cb(f.data(), nn, in);
    }

    void run_() {
        for (;;) {
            if (pending) {
                if (n < pend + 4) return;
                decodeData();
                pending = false; skipUntil = pend; trig = -1; run = 0; bestM = 0;
                continue;
            }
            if (trig >= 0 && n >= trig + kSyncWait) {
                const int64_t t = trig;
                if (sync()) pending = true;
                else (void)t;
                trig = -1; run = 0; bestM = 0;
                if (pending) continue;
            }
            return;
        }
    }

    void trim() {
        int64_t keep = n - kKeep;
        if (trig >= 0) keep = std::min(keep, trig - 300);
        if (pending) keep = std::min(keep, pd - 400);
        if (keep - base > (1 << 17)) { b.erase(b.begin(), b.begin() + (ptrdiff_t)(keep - base)); base = keep; }
    }
};

OfdmRx::OfdmRx() : p_(std::make_unique<Impl>()) {}
OfdmRx::~OfdmRx() = default;
void OfdmRx::setCallback(FrameCb cb) { p_->cb = std::move(cb); }
void OfdmRx::setAllowQpsk(bool on) { p_->allowQpsk = on; }
void OfdmRx::reset() { p_->reset(); }
const OfdmRx::Stats& OfdmRx::stats() const { return p_->st; }
void OfdmRx::feed(const cf32* x, size_t cnt) {
    Impl& m = *p_;
    for (size_t i = 0; i < cnt;) {
        const int k = (int)std::min<size_t>(cnt - i, 512);
        const size_t o = m.b.size();
        m.b.resize(o + (size_t)k);
        cf32* d = m.b.data() + o;
        for (int j = 0; j < k; j++) {
            const float re = x[i + (size_t)j].real(), im = x[i + (size_t)j].imag();
            d[j] = std::fabs(re) <= 3.0e38f && std::fabs(im) <= 3.0e38f ? x[i + (size_t)j] : cf32(0, 0);   // finite
        }
        m.feedBlock(k);
        i += (size_t)k;
    }
    m.trim();
}

} // namespace dect2::wifi::ofdm
