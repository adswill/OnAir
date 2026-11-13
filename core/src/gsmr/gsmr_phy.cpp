// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GSM-R physical layer (gsmr_phy.h): channel coding, burst formats, GMSK modulation, FCCH search, equaliser.
#include "dect2/gsmr/gsmr_phy.h"
#include "dect2/cell/cell_dsp.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2::gsmr {

// 3GPP TS 45.002 5.2.3 (sync), 5.2.3 (normal burst training sequences) and 5.2.6 (dummy burst)
const uint8_t kSync[64] = {1, 0, 1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
                           0, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 1, 1};
const uint8_t kTsc[8][26] = {
    {0, 0, 1, 0, 0, 1, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1},
    {0, 0, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1},
    {0, 1, 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0},
    {0, 1, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1, 0},
    {0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 1, 1},
    {0, 1, 0, 0, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 1, 0},
    {1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 0, 1, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1},
    {1, 1, 1, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 0}};
const uint8_t kDummy[148] = {0, 0, 0, 1, 1, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1, 1,
                             1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0,
                             0, 1, 1, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 1,
                             0, 1, 1, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1,
                             1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 0, 0, 0};

// ---------------------------------------------------------------- channel coding

namespace {

// Remainder of d(D) * D^w mod g(D), bits fed first-first; the generator without its top term
uint64_t crcRem(const uint8_t* d, size_t n, uint64_t poly, int w) {
    const uint64_t mask = (w == 64) ? ~0ULL : ((1ULL << w) - 1), top = 1ULL << (w - 1);
    uint64_t r = 0;
    for (size_t i = 0; i < n; i++) {
        const bool fb = ((r & top) != 0) != (d[i] != 0);
        r = (r << 1) & mask;
        if (fb) r ^= poly;
    }
    return r;
}

constexpr uint64_t kFirePoly = 0x0004820009ULL;   // D^40 + D^26 + D^23 + D^17 + D^3 + 1
constexpr uint64_t kSchPoly = 0x175;               // D^10 + D^8 + D^6 + D^5 + D^4 + D^2 + 1

Bits parityBits(uint64_t rem, int w) {
    Bits p((size_t)w);
    for (int i = 0; i < w; i++) p[(size_t)i] = (uint8_t)(((rem >> (w - 1 - i)) & 1) ^ 1);   // transmitted inverted
    return p;
}

// One convolutional-code step: the two coded bits for input u in state s (bit i of s is the input i + 1 steps ago)
inline void convOut(int s, int u, int& c0, int& c1) {
    const int a = (s >> 2) & 1, b = (s >> 3) & 1;
    c0 = u ^ a ^ b;
    c1 = u ^ (s & 1) ^ a ^ b;
}

} // namespace

Bits fireParity(const uint8_t* d184) { return parityBits(crcRem(d184, 184, kFirePoly, 40), 40); }
Bits schParity(const uint8_t* d25) { return parityBits(crcRem(d25, 25, kSchPoly, 10), 10); }

Bits convEncode(const Bits& u) {
    Bits c;
    c.reserve(u.size() * 2);
    int s = 0;
    for (uint8_t b : u) {
        int c0, c1;
        convOut(s, b, c0, c1);
        c.push_back((uint8_t)c0); c.push_back((uint8_t)c1);
        s = ((s << 1) | b) & 15;
    }
    return c;
}

Bits convDecode(const float* llr, size_t nu) {
    constexpr float kNeg = -1e30f;
    std::vector<float> pm(16, kNeg), nx(16);
    pm[0] = 0;
    std::vector<uint8_t> from(nu * 16);   // the previous state's top bit decides: new state = (old << 1 | u) & 15, so old = (new >> 1) | (x << 3)
    for (size_t k = 0; k < nu; k++) {
        std::fill(nx.begin(), nx.end(), kNeg);
        const float l0 = llr[2 * k], l1 = llr[2 * k + 1];
        for (int s = 0; s < 16; s++) {
            if (pm[(size_t)s] <= kNeg) continue;
            for (int u = 0; u < 2; u++) {
                int c0, c1;
                convOut(s, u, c0, c1);
                const float m = pm[(size_t)s] + (c0 ? -l0 : l0) * 0.5f + (c1 ? -l1 : l1) * 0.5f;
                const int ns = ((s << 1) | u) & 15;
                if (m > nx[(size_t)ns]) { nx[(size_t)ns] = m; from[k * 16 + (size_t)ns] = (uint8_t)(s >> 3); }
            }
        }
        pm.swap(nx);
    }
    Bits u(nu);
    int s = 0;   // tail bits return the encoder to state 0
    for (size_t k = nu; k-- > 0;) {
        u[k] = (uint8_t)(s & 1);
        s = (s >> 1) | ((int)from[k * 16 + (size_t)s] << 3);
    }
    return u;
}

Bits xcchEncode(const uint8_t* msg23) {
    Bits u;
    u.reserve(228);
    // layer 2 octets go on the air least significant bit first (44.006): d(0) is bit 1 of the first octet
    for (int i = 0; i < 184; i++) u.push_back((uint8_t)((msg23[i >> 3] >> (i & 7)) & 1));
    const Bits p = fireParity(u.data());
    u.insert(u.end(), p.begin(), p.end());
    u.insert(u.end(), 4, 0);
    return convEncode(u);
}

// 45.003 4.1.4: coded bit k goes to burst k mod 4, position 2 * ((49 k) mod 57) + ((k mod 8) div 4)
void xcchInterleave(const Bits& c, Bits out[4]) {
    for (int b = 0; b < 4; b++) out[b].assign(114, 0);
    for (int k = 0; k < 456; k++) out[k % 4][(size_t)(2 * ((49 * k) % 57) + ((k % 8) / 4))] = c[(size_t)k];
}

void xcchDeinterleave(const float* burst[4], float* llr456) {
    for (int k = 0; k < 456; k++) llr456[k] = burst[k % 4][2 * ((49 * k) % 57) + ((k % 8) / 4)];
}

bool xcchDecode(const float* llr456, uint8_t* msg23) {
    const Bits u = convDecode(llr456, 228);
    for (int i = 224; i < 228; i++) if (u[(size_t)i]) return false;
    const Bits p = fireParity(u.data());
    for (int i = 0; i < 40; i++) if (p[(size_t)i] != u[(size_t)(184 + i)]) return false;
    std::memset(msg23, 0, 23);
    for (int i = 0; i < 184; i++) if (u[(size_t)i]) msg23[i >> 3] |= (uint8_t)(1 << (i & 7));
    return true;
}

// 44.004 / 45.003 4.7: the 25 SCH information bits are the four octets  [BSIC << 2 | T1 >> 9]  [T1 >> 1]  [T1 & 1, T2, T3' >> 1]  [T3' & 1],
// sent least significant bit first like every octet of layer 1 / 2 (d(0) is bit 1 of the first octet)
Bits schEncode(int bsic, uint32_t fn) {
    const int t1 = (int)(fn / (26 * 51)), t2 = (int)(fn % 26), t3 = (int)(fn % 51), t3p = (t3 - 1) / 10;
    const uint8_t sb[4] = {(uint8_t)((bsic << 2) | (t1 >> 9)), (uint8_t)(t1 >> 1), (uint8_t)(((t1 & 1) << 7) | (t2 << 2) | (t3p >> 1)), (uint8_t)(t3p & 1)};
    Bits d;
    for (int i = 0; i < 25; i++) d.push_back((uint8_t)((sb[i >> 3] >> (i & 7)) & 1));
    const Bits p = schParity(d.data());
    d.insert(d.end(), p.begin(), p.end());
    d.insert(d.end(), 4, 0);
    return convEncode(d);
}

bool schDecode(const float* llr78, int& bsic, uint32_t& fn) {
    const Bits u = convDecode(llr78, 39);
    for (int i = 35; i < 39; i++) if (u[(size_t)i]) return false;
    const Bits p = schParity(u.data());
    for (int i = 0; i < 10; i++) if (p[(size_t)i] != u[(size_t)(25 + i)]) return false;
    uint8_t sb[4] = {};
    for (int i = 0; i < 25; i++) if (u[(size_t)i]) sb[i >> 3] |= (uint8_t)(1 << (i & 7));
    const int t1 = ((sb[0] & 3) << 9) | (sb[1] << 1) | (sb[2] >> 7), t2 = (sb[2] >> 2) & 31, t3p = ((sb[2] & 3) << 1) | (sb[3] & 1);
    if (t3p > 4 || t2 > 25 || t1 > 2047) return false;
    bsic = sb[0] >> 2;
    const int t3 = 10 * t3p + 1;
    fn = frameFromSch(t1, t2, t3);
    return true;
}

// ---------------------------------------------------------------- bursts

Bits normalBurst(const Bits& d, int tsc, int stealing) {
    Bits b(3, 0);
    b.insert(b.end(), d.begin(), d.begin() + 57);
    b.push_back((uint8_t)stealing);
    b.insert(b.end(), kTsc[tsc & 7], kTsc[tsc & 7] + 26);
    b.push_back((uint8_t)stealing);
    b.insert(b.end(), d.begin() + 57, d.begin() + 114);
    b.insert(b.end(), 3, 0);
    return b;
}

Bits syncBurst(const Bits& c) {
    Bits b(3, 0);
    b.insert(b.end(), c.begin(), c.begin() + 39);
    b.insert(b.end(), kSync, kSync + 64);
    b.insert(b.end(), c.begin() + 39, c.begin() + 78);
    b.insert(b.end(), 3, 0);
    return b;
}

Bits fcchBurst() { return Bits(148, 0); }

// ---------------------------------------------------------------- modulation

namespace {

// The phase pulse Q(t) of the Gaussian filtered rectangle (BT = 0.3), t in symbols, Q(-inf) = 0, Q(+inf) = 1
double qPulse(double t) {
    static const std::vector<double> tab = [] {
        const double sigma = std::sqrt(std::log(2.0)) / (2 * M_PI * 0.3);   // in symbols
        const int per = 256, half = 4;
        std::vector<double> q((size_t)(2 * half * per + 1));
        auto phi = [](double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); };
        double acc = 0, prev = 0;
        for (size_t i = 0; i < q.size(); i++) {
            const double tt = ((double)i / per) - half;
            const double g = phi((tt + 0.5) / sigma) - phi((tt - 0.5) / sigma);   // frequency pulse, unit area
            if (i) acc += 0.5 * (g + prev) / per;
            prev = g;
            q[i] = acc;
        }
        for (auto& v : q) v /= acc;
        return q;
    }();
    const int per = 256, half = 4;
    if (t <= -half) return 0;
    if (t >= half) return 1;
    const double p = (t + half) * per;
    const size_t i = (size_t)p;
    const double f = p - (double)i;
    return tab[i] + f * (tab[i + 1] - tab[i]);
}

} // namespace

std::vector<cf32> gmskBurst(const Bits& bits, int sps) {
    const int nb = (int)bits.size();
    std::vector<double> alpha((size_t)nb);
    uint8_t prev = 0;
    for (int i = 0; i < nb; i++) { alpha[(size_t)i] = 1.0 - 2.0 * (bits[(size_t)i] ^ prev); prev = bits[(size_t)i]; }
    std::vector<double> cum((size_t)nb + 1, 0.0);   // cum[i] = alpha[0] + ... + alpha[i - 1]
    for (int i = 0; i < nb; i++) cum[(size_t)i + 1] = cum[(size_t)i] + alpha[(size_t)i];
    const int total = (nb + 4) * sps;
    std::vector<cf32> out((size_t)total);
    for (int n = 0; n < total; n++) {
        const double t = (double)(n - 2 * sps) / sps;   // in symbols from the first symbol's start
        // symbol i is centred at i + 0.5; its Q is 0 more than 4 symbols before that and 1 more than 4 after
        const int lo = std::max(0, (int)std::floor(t - 4.5)), hi = std::min(nb - 1, (int)std::ceil(t + 3.5));
        double ph = cum[(size_t)std::min(nb, lo)];
        for (int i = lo; i <= hi; i++) ph += alpha[(size_t)i] * qPulse(t - (i + 0.5));
        ph *= M_PI / 2;
        // amplitude ramps
        double w = 1;
        const double ramp = 2.0 * sps;
        const double nn = n - 2.0 * sps;
        if (nn < 0) w = 0.5 * (1 - std::cos(M_PI * (nn + ramp) / ramp));
        else if (nn >= nb * (double)sps) w = 0.5 * (1 + std::cos(M_PI * (nn - nb * (double)sps) / ramp));
        if (nn < -ramp || nn >= nb * (double)sps + ramp) w = 0;
        out[(size_t)n] = cf32((float)(w * std::cos(ph)), (float)(w * std::sin(ph)));
    }
    return out;
}

// ---------------------------------------------------------------- reception

std::vector<cf32> channelize(const cf32* x, size_t n, double fs, double shiftHz, size_t start, size_t count) {
    std::vector<cf32> out;
    if (start >= n) return out;
    count = std::min(count, n - start);
    const int d1 = std::max(1, (int)std::floor(fs / 1.2e6));
    const double fs1 = fs / d1;
    const double w = 2 * M_PI * shiftHz / fs;
    std::vector<cf32> s1;   // after the first decimation
    if (d1 == 1) {
        s1.resize(count);
        for (size_t i0 = 0; i0 < count; i0 += 1024) {
            const double ph = -w * (double)(start + i0);
            std::complex<double> o(std::cos(ph), std::sin(ph));
            const std::complex<double> st(std::cos(-w), std::sin(-w));
            const size_t m = std::min<size_t>(1024, count - i0);
            for (size_t k = 0; k < m; k++) { s1[i0 + k] = x[start + i0 + k] * cf32((float)o.real(), (float)o.imag()); o *= st; }
        }
    } else {
        const int M = 16 * d1 + 1;
        const double fc = 0.5 * 0.55 / d1;
        std::vector<float> h((size_t)M);
        double sum = 0;
        for (int i = 0; i < M; i++) {
            const double t = i - (M - 1) / 2.0, a = 2 * fc * t;
            const double s = std::fabs(a) < 1e-9 ? 1.0 : std::sin(M_PI * a) / (M_PI * a);
            const double u = 2.0 * i / (M - 1) - 1;
            const double wd = 0.42 + 0.5 * std::cos(M_PI * u) + 0.08 * std::cos(2 * M_PI * u);
            h[(size_t)i] = (float)(s * wd);
            sum += h[(size_t)i];
        }
        for (auto& v : h) v = (float)(v / sum);
        const size_t nOut = count / (size_t)d1;
        s1.resize(nOut);
        constexpr size_t kChunk = 1 << 16;   // outputs per chunk
        std::vector<cf32> mix((kChunk + (size_t)M) * (size_t)d1);
        for (size_t o0 = 0; o0 < nOut; o0 += kChunk) {
            const size_t no = std::min(kChunk, nOut - o0);
            const size_t i0 = start + o0 * (size_t)d1;   // first input of the chunk (the filter looks M - 1 samples forward)
            const size_t len = std::min(no * (size_t)d1 + (size_t)M, n - i0);
            for (size_t b = 0; b < len; b += 1024) {
                const double ph = -w * (double)(i0 + b);
                std::complex<double> o(std::cos(ph), std::sin(ph));
                const std::complex<double> st(std::cos(-w), std::sin(-w));
                const size_t m = std::min<size_t>(1024, len - b);
                for (size_t k = 0; k < m; k++) { mix[b + k] = x[i0 + b + k] * cf32((float)o.real(), (float)o.imag()); o *= st; }
            }
            for (size_t k = 0; k < no; k++) {
                const cf32* p = &mix[k * (size_t)d1];
                const size_t avail = len - k * (size_t)d1;
                float re = 0, im = 0;
                const int mm = (int)std::min<size_t>((size_t)M, avail);
                for (int i = 0; i < mm; i++) { re += h[(size_t)i] * p[i].real(); im += h[(size_t)i] * p[i].imag(); }
                s1[o0 + k] = cf32(re, im);
            }
        }
    }
    const size_t nOut2 = (size_t)std::floor((double)s1.size() * kStreamRate / fs1) - 8;
    if (s1.size() < 64 || (long)nOut2 <= 0) return out;
    const cell::Resampler rs(std::min(0.5, 0.5 * 270e3 / fs1));
    out.resize(nOut2);
    rs.run(s1.data(), s1.size(), 0.0, fs1 / kStreamRate, out.data(), nOut2);
    return out;
}

std::vector<FcchHit> findFcch(const cf32* x, size_t n) {
    std::vector<FcchHit> hits;
    constexpr int W = 128 * kSps;
    if (n < (size_t)W * 3) return hits;
    // The tone is a quarter of the symbol rate above the carrier: a quarter turn per symbol. The phase step over one symbol (kSps samples)
    // is therefore constant for the FCCH, while for modulated bursts it is +-90 degrees at random and averages out.
    const double w0 = M_PI / 2;
    std::vector<cf32> d(n);
    for (size_t i = 0; i < (size_t)kSps; i++) d[i] = 0;
    for (size_t i = kSps; i < n; i++) d[i] = x[i] * std::conj(x[i - kSps]);
    std::complex<double> s(0, 0);
    double a = 0;
    for (int i = 1; i <= W; i++) { s += std::complex<double>(d[(size_t)i]); a += std::abs(d[(size_t)i]); }
    struct Run { long i0 = -1, last = -1; double peak = 0; long peakAt = 0; std::complex<double> sAtPeak; };
    Run run;
    std::vector<std::pair<long, std::complex<double>>> pl;   // indices at the plateau of the current run
    std::vector<float> cohRun;
    auto flush = [&]() {
        if (run.i0 < 0) return;
        if (run.last - run.i0 >= 60 && run.peak > 0.6) {
            // the plateau: window starts within 15 % of the peak
            long lo = -1, hi = -1;
            std::complex<double> acc(0, 0);
            for (size_t k = 0; k < cohRun.size(); k++)
                if (cohRun[k] >= 0.85 * run.peak) { if (lo < 0) lo = (long)k; hi = (long)k; }
            if (lo >= 0) {
                const long mid = run.i0 + (lo + hi) / 2;
                FcchHit h;
                h.center = (double)mid + W / 2.0;
                // the frequency from the middle 100 symbols of the tone only: the edges of the burst are not at the tone's frequency
                for (long i = (long)h.center - 200; i < (long)h.center + 200; i++) if (i >= (long)kSps && i < (long)n) acc += std::complex<double>(d[(size_t)i]);
                h.freqErrHz = std::remainder(std::arg(acc) - w0, 2 * M_PI) * kStreamRate / (2 * M_PI * kSps);
                h.coherence = (float)run.peak;
                hits.push_back(h);
            }
        }
        run = Run();
        pl.clear(); cohRun.clear();
    };
    const size_t last = n - 1;
    for (size_t i0 = 0; i0 + W + 1 <= last; i0++) {
        // window = d[i0 + 1 .. i0 + W]
        if (i0 > 0) {
            s += std::complex<double>(d[i0 + W]) - std::complex<double>(d[i0]);
            a += std::abs(d[i0 + W]) - std::abs(d[i0]);
        }
        const double coh = a > 1e-9 ? std::abs(s) / a : 0;
        const double dw = std::remainder(std::arg(s) - w0, 2 * M_PI);
        const bool good = coh > 0.55 && std::fabs(dw) < 1.5;
        if (good) {
            if (run.i0 < 0) run.i0 = (long)i0;
            run.last = (long)i0;
            if (coh > run.peak) run.peak = coh;
            pl.push_back({(long)i0, s});
            cohRun.push_back((float)coh);
        } else if (run.i0 >= 0) {
            flush();
        }
        if (i0 % 4096 == 0) { s = 0; a = 0; for (int i = 1; i <= W; i++) { s += std::complex<double>(d[i0 + (size_t)i]); a += std::abs(d[i0 + (size_t)i]); } }   // no drift from the running sum
    }
    flush();
    // a run broken by one noisy window shows twice: merge hits closer than a burst
    std::vector<FcchHit> m;
    for (const auto& h : hits) {
        if (!m.empty() && h.center - m.back().center < 4 * kSps * 100) { if (h.coherence > m.back().coherence) m.back() = h; }
        else m.push_back(h);
    }
    return m;
}

namespace {

constexpr int kL = 5;   // channel taps (16 trellis states)

inline cf32 derot(cf32 v, int k) {   // multiply by (-j)^k
    switch (k & 3) {
    case 0: return v;
    case 1: return cf32(v.imag(), -v.real());
    case 2: return -v;
    default: return cf32(-v.imag(), v.real());
    }
}

// Solve the 5 x 5 real system m * out = rhs (Gauss, partial pivoting); false when singular
bool solve5(double m[kL][kL], double inv[kL][kL]) {
    double a[kL][2 * kL];
    for (int i = 0; i < kL; i++) for (int j = 0; j < kL; j++) { a[i][j] = m[i][j]; a[i][kL + j] = (i == j); }
    for (int c = 0; c < kL; c++) {
        int p = c;
        for (int r = c + 1; r < kL; r++) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-9) return false;
        if (p != c) for (int j = 0; j < 2 * kL; j++) std::swap(a[p][j], a[c][j]);
        const double dv = a[c][c];
        for (int j = 0; j < 2 * kL; j++) a[c][j] /= dv;
        for (int r = 0; r < kL; r++) if (r != c) { const double f = a[r][c]; if (f != 0) for (int j = 0; j < 2 * kL; j++) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < kL; i++) for (int j = 0; j < kL; j++) inv[i][j] = a[i][kL + j];
    return true;
}

} // namespace

BurstRx demodBurst(const cf32* x, size_t n, double predicted, int searchSyms, const uint8_t* train, int trainLen, int trainPos) {
    BurstRx r;
    // least-squares channel estimate: y[trainPos + m] = sum_l h_l s[m - l] for m = kL - 1 .. trainLen - 1
    const int rows = trainLen - (kL - 1);
    std::vector<float> s((size_t)trainLen);
    for (int i = 0; i < trainLen; i++) s[(size_t)i] = train[i] ? -1.f : 1.f;
    double ata[kL][kL] = {}, inv[kL][kL];
    for (int m = kL - 1; m < trainLen; m++)
        for (int i = 0; i < kL; i++) for (int j = 0; j < kL; j++) ata[i][j] += (double)s[(size_t)(m - i)] * s[(size_t)(m - j)];
    if (!solve5(ata, inv)) return r;
    // pseudo-inverse P (kL x rows)
    std::vector<float> P((size_t)kL * (size_t)rows);
    for (int i = 0; i < kL; i++)
        for (int m = 0; m < rows; m++) {
            double v = 0;
            for (int j = 0; j < kL; j++) v += inv[i][j] * s[(size_t)(m + kL - 1 - j)];
            P[(size_t)i * (size_t)rows + (size_t)m] = (float)v;
        }
    const long R = (long)searchSyms * kSps, p0 = (long)std::llround(predicted);
    double bestE = -1;
    long bestP = 0;
    std::vector<cf32> y((size_t)rows);
    const long lastNeeded = (long)(kBurstSyms - 1) * kSps;
    for (long p = p0 - R; p <= p0 + R; p++) {
        if (p < 0 || (size_t)(p + lastNeeded) >= n) continue;
        double tot = 0;
        for (int m = 0; m < rows; m++) { const int k = trainPos + (kL - 1) + m; y[(size_t)m] = derot(x[p + (long)k * kSps], k); tot += std::norm(y[(size_t)m]); }
        double e = 0;
        for (int i = 0; i < kL; i++) {
            std::complex<float> h(0, 0);
            for (int m = 0; m < rows; m++) h += P[(size_t)i * (size_t)rows + (size_t)m] * y[(size_t)m];
            e += std::norm(h);
        }
        // energy explained by the taps against what is there; the power of the taps is what the equaliser can use
        if (e > bestE) { bestE = e; bestP = p; }
        (void)tot;
    }
    if (bestE < 0) return r;
    const long p = bestP;
    cf32 h[kL];
    double resid = 0;
    {
        for (int m = 0; m < rows; m++) { const int k = trainPos + (kL - 1) + m; y[(size_t)m] = derot(x[p + (long)k * kSps], k); }
        for (int i = 0; i < kL; i++) {
            cf32 a(0, 0);
            for (int m = 0; m < rows; m++) a += P[(size_t)i * (size_t)rows + (size_t)m] * y[(size_t)m];
            h[i] = a;
        }
        for (int m = 0; m < rows; m++) {
            cf32 pr(0, 0);
            for (int l = 0; l < kL; l++) pr += h[l] * s[(size_t)(m + kL - 1 - l)];
            resid += std::norm(y[(size_t)m] - pr);
        }
    }
    double hp = 0;
    for (int l = 0; l < kL; l++) hp += std::norm(h[l]);
    const double nv = std::max(resid / rows, hp * 1e-4);
    r.snrDb = (float)(10 * std::log10(hp / nv));
    r.pos = (double)p;
    r.ok = r.snrDb > 1.5f;
    // max-log BCJR over the burst (16 states: the last four bits); the tail bits at both ends are known zeros
    constexpr int N = kBurstSyms;
    std::vector<cf32> yy((size_t)N);
    for (int k = 0; k < N; k++) yy[(size_t)k] = derot(x[p + (long)k * kSps], k);
    // predicted value for each (state, input): taps over s[k], s[k-1] .. s[k-4]
    cf32 pred[32];
    for (int st = 0; st < 16; st++)
        for (int u = 0; u < 2; u++) {
            cf32 v = h[0] * (u ? -1.f : 1.f);
            for (int l = 1; l < kL; l++) v += h[l] * (((st >> (l - 1)) & 1) ? -1.f : 1.f);
            pred[st * 2 + u] = v;
        }
    const float inf = 1e30f;
    std::vector<float> alpha((size_t)(N + 1) * 16, -inf), beta((size_t)(N + 1) * 16, -inf), gam((size_t)N * 32);
    const float inv2 = (float)(1.0 / nv);
    for (int k = 0; k < N; k++) {
        const bool forced = k < 3 || k >= N - 3;
        for (int st = 0; st < 16; st++)
            for (int u = 0; u < 2; u++)
                gam[(size_t)k * 32 + (size_t)st * 2 + (size_t)u] = (forced && u) ? -inf : -std::norm(yy[(size_t)k] - pred[st * 2 + u]) * inv2;
    }
    alpha[0] = 0;
    for (int k = 0; k < N; k++)
        for (int st = 0; st < 16; st++) {
            const float a = alpha[(size_t)k * 16 + (size_t)st];
            if (a <= -inf) continue;
            for (int u = 0; u < 2; u++) {
                const float g = gam[(size_t)k * 32 + (size_t)st * 2 + (size_t)u];
                if (g <= -inf) continue;
                const int ns = ((st << 1) | u) & 15;
                float& t = alpha[(size_t)(k + 1) * 16 + (size_t)ns];
                if (a + g > t) t = a + g;
            }
        }
    for (int st = 0; st < 16; st++) beta[(size_t)N * 16 + (size_t)st] = 0;
    for (int k = N - 1; k >= 0; k--)
        for (int st = 0; st < 16; st++) {
            float best = -inf;
            for (int u = 0; u < 2; u++) {
                const float g = gam[(size_t)k * 32 + (size_t)st * 2 + (size_t)u];
                if (g <= -inf) continue;
                const float b = beta[(size_t)(k + 1) * 16 + (size_t)(((st << 1) | u) & 15)];
                if (b <= -inf) continue;
                best = std::max(best, g + b);
            }
            beta[(size_t)k * 16 + (size_t)st] = best;
        }
    for (int k = 0; k < N; k++) {
        float m0 = -inf, m1 = -inf;
        for (int st = 0; st < 16; st++) {
            const float a = alpha[(size_t)k * 16 + (size_t)st];
            if (a <= -inf) continue;
            for (int u = 0; u < 2; u++) {
                const float g = gam[(size_t)k * 32 + (size_t)st * 2 + (size_t)u];
                const float b = beta[(size_t)(k + 1) * 16 + (size_t)(((st << 1) | u) & 15)];
                if (g <= -inf || b <= -inf) continue;
                const float v = a + g + b;
                if (u == 0) m0 = std::max(m0, v); else m1 = std::max(m1, v);
            }
        }
        float l = (m0 <= -inf) ? -20.f : (m1 <= -inf ? 20.f : m0 - m1);
        r.llr[k] = std::max(-20.f, std::min(20.f, l));
    }
    return r;
}

} // namespace dect2::gsmr
