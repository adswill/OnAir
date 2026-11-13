// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DECT NR+ physical control channel receiver (see nrplus.h). Clause numbers: ETSI TS 103 636-3 V1.5.1 unless TS 103 636-4 is named.
#include "dect2/xlink/nrplus.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace dect2::xlink::nrplus {

using dect2::math::kPi;

namespace {
// clause 5.2.2: STF sequence for beta = 1 (times e^{j pi/4}) and the cover sequence of clause 6.3.7
const int8_t kStfSeq[14] = {1, -1, 1, 1, -1, 1, 1, -1, 1, 1, 1, -1, -1, -1};
const int8_t kCover[7] = {1, -1, 1, 1, -1, -1, -1};
// clause 5.2.3: DRS base sequence for beta = 1
const int8_t kDrsSeq[56] = {1, 1, 1, 1, -1, 1, 1, -1, -1, 1, 1, 1, 1, -1, 1, -1, 1, 1, -1, 1, -1, 1, -1, 1, 1, 1, 1, 1, -1, 1,
                            -1, -1, 1, 1, -1, -1, -1, -1, 1, -1, -1, -1, -1, -1, 1, 1, 1, -1, 1, 1, -1, -1, 1, -1, -1, -1};
// clause 6.1.5.2
const int kColPerm[32] = {0, 16, 8, 24, 4, 20, 12, 28, 2, 18, 10, 26, 6, 22, 14, 30, 1, 17, 9, 25, 5, 21, 13, 29, 3, 19, 11, 27, 7, 23, 15, 31};
constexpr int kNull = 255;
constexpr int kPccRe = 98, kPccBits = 196;

int occ(int i) { return i < 28 ? i - 28 : i - 27; }          // occupied subcarrier i = 0 .. 55: -28 .. -1, 1 .. 28
inline int bin(int k) { return (k + kFft) % kFft; }

std::vector<int> stfCarriers() { std::vector<int> v; for (int i = 0; i < 7; i++) v.push_back(occ(4 * i)); for (int i = 7; i < 14; i++) v.push_back(occ(28 + 3 + (i - 7) * 4)); return v; }
bool isDrsSymbol(int l) { return l >= 1 && (l - 1) % 5 == 0; }
std::vector<int> drsCarriers(int l) { const int n = (l - 1) / 5; std::vector<int> v; for (int i = 0; i < 14; i++) v.push_back(occ(4 * i + ((n % 2) * 2) % 4)); return v; }

std::vector<cf32> ifftUnit(std::vector<cf32> X, int used) {
    static Fft f(kFft);
    f.inverse(X.data());
    const float s = 1.f / std::sqrt((float)used);
    for (auto& v : X) v *= s;
    return X;
}

const std::vector<cf32>& stfBase() {                          // the 16-sample base sequence S
    static const std::vector<cf32> s = [] {
        std::vector<cf32> X(kFft, cf32(0, 0));
        const auto k = stfCarriers();
        const cf32 r = std::polar(1.f, (float)kPi / 4);
        for (int i = 0; i < 14; i++) X[(size_t)bin(k[(size_t)i])] = r * (float)kStfSeq[i];
        auto t = ifftUnit(X, 14);
        return std::vector<cf32>(t.begin(), t.begin() + 16);
    }();
    return s;
}

std::vector<uint8_t> goldSeq(uint32_t init, int n) {             // clause 6.2
    std::vector<uint8_t> x1((size_t)(n + 1600 + 31), 0), x2((size_t)(n + 1600 + 31), 0), g((size_t)n);
    x1[0] = 1;
    for (int i = 0; i < 31; i++) x2[(size_t)i] = (uint8_t)(init >> i & 1);
    for (int i = 0; i + 31 < (int)x1.size(); i++) {
        x1[(size_t)(i + 31)] = (uint8_t)((x1[(size_t)(i + 3)] + x1[(size_t)i]) & 1);
        x2[(size_t)(i + 31)] = (uint8_t)((x2[(size_t)(i + 3)] + x2[(size_t)(i + 2)] + x2[(size_t)(i + 1)] + x2[(size_t)i]) & 1);
    }
    for (int i = 0; i < n; i++) g[(size_t)i] = (uint8_t)((x1[(size_t)(i + 1600)] + x2[(size_t)(i + 1600)]) & 1);
    return g;
}

uint16_t crc16(const std::vector<uint8_t>& bits, size_t n) {   // D^16 + D^12 + D^5 + 1, clause 6.1.2
    unsigned c = 0;
    for (size_t i = 0; i < n; i++) { const unsigned fb = ((c >> 15) ^ bits[i]) & 1; c = (c << 1) & 0xFFFF; if (fb) c ^= 0x1021; }
    return (uint16_t)c;
}

void qpp(int K, int& f1, int& f2) { if (K == 56) { f1 = 19; f2 = 42; } else { f1 = 11; f2 = 24; } }   // table 6.1.4.2.3-1, K = 56 and 96
int qppIndex(int i, int K) { int f1, f2; qpp(K, f1, f2); return (int)(((long long)f1 * i + (long long)f2 * i * i) % K); }

// sub-block interleaver positions (clause 6.1.5.2): for output index k, the input index or kNull-marked dummy (-1)
std::vector<int> subblock(int D, bool third) {
    const int R = (D + 31) / 32, Kp = R * 32, nd = Kp - D;
    std::vector<int> y((size_t)Kp);
    for (int k = 0; k < Kp; k++) y[(size_t)k] = k < nd ? -1 : k - nd;
    std::vector<int> v((size_t)Kp);
    if (!third) { int o = 0; for (int c = 0; c < 32; c++) for (int r = 0; r < R; r++) v[(size_t)o++] = y[(size_t)(r * 32 + kColPerm[c])]; }
    else for (int k = 0; k < Kp; k++) v[(size_t)k] = y[(size_t)((kColPerm[k / R] + 32 * (k % R) + 1) % Kp)];
    return v;
}

// circular buffer selection for rv 0 (clause 6.1.5.3): for each output bit, (stream, index)
std::vector<std::pair<int, int>> rateMatchMap(int K, int E) {
    const int D = K + 4, R = (D + 31) / 32, Kp = R * 32, Kw = 3 * Kp;
    const auto v0 = subblock(D, false), v1 = subblock(D, false), v2 = subblock(D, true);
    std::vector<std::pair<int, int>> w((size_t)Kw);
    for (int k = 0; k < Kp; k++) { w[(size_t)k] = {0, v0[(size_t)k]}; w[(size_t)(Kp + 2 * k)] = {1, v1[(size_t)k]}; w[(size_t)(Kp + 2 * k + 1)] = {2, v2[(size_t)k]}; }
    std::vector<std::pair<int, int>> e;
    const int k0 = R * 2;
    for (int j = 0; (int)e.size() < E; j++) { const auto& p = w[(size_t)((k0 + j) % Kw)]; if (p.second >= 0) e.push_back(p); }
    return e;
}

// one 8-state constituent code step (clause 6.1.4.2.1): state bits r1 r2 r3 (r1 newest) packed as r1 | r2 << 1 | r3 << 2
inline void rsc(int s, int in, int& ns, int& parity) {
    const int r1 = s & 1, r2 = s >> 1 & 1, r3 = s >> 2 & 1;
    const int a = in ^ r2 ^ r3;
    parity = a ^ r1 ^ r3;
    ns = a | r1 << 1 | r2 << 2;
}

// max-log BCJR over K information steps and 3 tail steps; returns the a-posteriori LLRs of the K information bits (> 0 a one)
std::vector<float> bcjr(const std::vector<float>& sys, const std::vector<float>& par, const std::vector<float>& apr, const float* tailSys, const float* tailPar, int K) {
    const int T = K + 3;
    constexpr float kNeg = -1e30f;
    std::vector<float> A((size_t)(T + 1) * 8, kNeg), B((size_t)(T + 1) * 8, kNeg);
    A[0] = 0;
    auto gamma = [&](int t, int in, int p) {
        const float ls = t < K ? sys[(size_t)t] + apr[(size_t)t] : tailSys[t - K];
        const float lp = t < K ? par[(size_t)t] : tailPar[t - K];
        return 0.5f * ((in ? ls : -ls) + (p ? lp : -lp));
    };
    for (int t = 0; t < T; t++)
        for (int s = 0; s < 8; s++) {
            const float a = A[(size_t)t * 8 + (size_t)s];
            if (a <= kNeg) continue;
            for (int u = 0; u < 2; u++) {
                // in the tail the input is the feedback value, which makes the register shift in a zero
                int in = u;
                if (t >= K) { in = ((s >> 1) & 1) ^ ((s >> 2) & 1); if (u) break; }
                int ns, p; rsc(s, in, ns, p);
                float& d = A[(size_t)(t + 1) * 8 + (size_t)ns];
                d = std::max(d, a + gamma(t, in, p));
            }
        }
    B[(size_t)T * 8] = 0;
    for (int t = T - 1; t >= 0; t--)
        for (int s = 0; s < 8; s++) {
            float best = kNeg;
            for (int u = 0; u < 2; u++) {
                int in = u;
                if (t >= K) { in = ((s >> 1) & 1) ^ ((s >> 2) & 1); if (u) break; }
                int ns, p; rsc(s, in, ns, p);
                const float b = B[(size_t)(t + 1) * 8 + (size_t)ns];
                if (b > kNeg) best = std::max(best, b + gamma(t, in, p));
            }
            B[(size_t)t * 8 + (size_t)s] = best;
        }
    std::vector<float> L((size_t)K);
    for (int t = 0; t < K; t++) {
        float m1 = kNeg, m0 = kNeg;
        for (int s = 0; s < 8; s++) {
            const float a = A[(size_t)t * 8 + (size_t)s];
            if (a <= kNeg) continue;
            for (int in = 0; in < 2; in++) {
                int ns, p; rsc(s, in, ns, p);
                const float v = a + gamma(t, in, p) + B[(size_t)(t + 1) * 8 + (size_t)ns];
                if (in) m1 = std::max(m1, v); else m0 = std::max(m0, v);
            }
        }
        L[(size_t)t] = m1 - m0;
    }
    return L;
}

std::vector<uint8_t> turboDecode(const std::vector<float>& d0, const std::vector<float>& d1, const std::vector<float>& d2, int K) {
    std::vector<float> xs(d0.begin(), d0.begin() + K), z1(d1.begin(), d1.begin() + K), z2(d2.begin(), d2.begin() + K), xsI((size_t)K);
    for (int i = 0; i < K; i++) xsI[(size_t)i] = xs[(size_t)qppIndex(i, K)];
    const float ts1[3] = {d0[(size_t)K], d2[(size_t)K], d1[(size_t)(K + 1)]}, tp1[3] = {d1[(size_t)K], d0[(size_t)(K + 1)], d2[(size_t)(K + 1)]};
    const float ts2[3] = {d0[(size_t)(K + 2)], d2[(size_t)(K + 2)], d1[(size_t)(K + 3)]}, tp2[3] = {d1[(size_t)(K + 2)], d0[(size_t)(K + 3)], d2[(size_t)(K + 3)]};
    std::vector<float> la((size_t)K, 0.f), la2((size_t)K), le1((size_t)K);
    for (int it = 0; it < 8; it++) {
        const auto l1 = bcjr(xs, z1, la, ts1, tp1, K);
        for (int i = 0; i < K; i++) le1[(size_t)i] = l1[(size_t)i] - xs[(size_t)i] - la[(size_t)i];
        for (int i = 0; i < K; i++) la2[(size_t)i] = le1[(size_t)qppIndex(i, K)];
        const auto l2 = bcjr(xsI, z2, la2, ts2, tp2, K);
        for (int i = 0; i < K; i++) la[(size_t)qppIndex(i, K)] = l2[(size_t)i] - xsI[(size_t)i] - la2[(size_t)i];
    }
    std::vector<uint8_t> out((size_t)K);
    for (int i = 0; i < K; i++) out[(size_t)i] = xs[(size_t)i] + la[(size_t)i] + le1[(size_t)i] > 0;
    return out;
}

void pushBits(std::vector<uint8_t>& v, unsigned x, int n) { for (int i = n - 1; i >= 0; i--) v.push_back((uint8_t)(x >> i & 1)); }
unsigned takeBits(const std::vector<uint8_t>& v, int& at, int n) { unsigned x = 0; for (int i = 0; i < n; i++) x = x << 1 | v[(size_t)at++]; return x; }
} // namespace

std::vector<uint8_t> turboEncode(const std::vector<uint8_t>& c) {
    const int K = (int)c.size();
    std::vector<uint8_t> x(c), z((size_t)K), zi((size_t)K), xt(3), zt(3), xit(3), zit(3);
    int s = 0;
    for (int k = 0; k < K; k++) { int ns, p; rsc(s, c[(size_t)k], ns, p); z[(size_t)k] = (uint8_t)p; s = ns; }
    for (int t = 0; t < 3; t++) { const int in = ((s >> 1) & 1) ^ ((s >> 2) & 1); int ns, p; rsc(s, in, ns, p); xt[(size_t)t] = (uint8_t)in; zt[(size_t)t] = (uint8_t)p; s = ns; }
    s = 0;
    for (int k = 0; k < K; k++) { int ns, p; rsc(s, c[(size_t)qppIndex(k, K)], ns, p); zi[(size_t)k] = (uint8_t)p; s = ns; }
    for (int t = 0; t < 3; t++) { const int in = ((s >> 1) & 1) ^ ((s >> 2) & 1); int ns, p; rsc(s, in, ns, p); xit[(size_t)t] = (uint8_t)in; zit[(size_t)t] = (uint8_t)p; s = ns; }
    std::vector<uint8_t> d0(x), d1(z), d2(zi);
    d0.insert(d0.end(), {xt[0], zt[1], xit[0], zit[1]});
    d1.insert(d1.end(), {zt[0], xt[2], zit[0], xit[2]});
    d2.insert(d2.end(), {xt[1], zt[2], xit[1], zit[2]});
    std::vector<uint8_t> o(d0);
    o.insert(o.end(), d1.begin(), d1.end());
    o.insert(o.end(), d2.begin(), d2.end());
    return o;
}

std::vector<uint8_t> pccEncode(const std::vector<uint8_t>& bits, int mask) {
    std::vector<uint8_t> b(bits);
    const uint16_t p = (uint16_t)(crc16(b, b.size()) ^ mask);
    pushBits(b, p, 16);
    const int K = (int)b.size();
    const auto d = turboEncode(b);
    const int D = K + 4;
    const auto m = rateMatchMap(K, kPccBits);
    std::vector<uint8_t> e;
    for (const auto& q : m) e.push_back(d[(size_t)(q.first * D + q.second)]);
    return e;
}

bool pccDecode(const std::vector<float>& soft, std::vector<uint8_t>& bits, int& mask) {
    for (int A : {40, 80}) {
        const int K = A + 16, D = K + 4;
        const auto m = rateMatchMap(K, kPccBits);
        std::vector<float> d[3];
        for (auto& v : d) v.assign((size_t)D, 0.f);
        for (size_t i = 0; i < m.size(); i++) d[m[i].first][(size_t)m[i].second] += soft[i];
        const auto c = turboDecode(d[0], d[1], d[2], K);
        const uint16_t calc = crc16(c, (size_t)A);
        int at = A;
        const uint16_t got = (uint16_t)takeBits(c, at, 16);
        for (int mk : {0, 0x5555, 0xAAAA}) {
            if ((uint16_t)(calc ^ mk) == got) { bits.assign(c.begin(), c.begin() + A); mask = mk; return true; }
        }
    }
    return false;
}

std::vector<uint8_t> headerBits(const Header& h) {
    std::vector<uint8_t> b;
    pushBits(b, (unsigned)h.format, 3); pushBits(b, (unsigned)h.lengthType, 1); pushBits(b, (unsigned)h.length, 4); pushBits(b, (unsigned)h.networkId, 8);
    pushBits(b, (unsigned)h.txId, 16); pushBits(b, (unsigned)h.power, 4);
    if (h.type == 1) { pushBits(b, 0, 1); pushBits(b, (unsigned)h.mcs, 3); return b; }
    pushBits(b, (unsigned)h.mcs, 4); pushBits(b, (unsigned)h.rxId, 16); pushBits(b, (unsigned)h.streams, 2); pushBits(b, (unsigned)h.rv, 2); pushBits(b, (unsigned)h.ndi, 1);
    pushBits(b, (unsigned)h.harq, 3); pushBits(b, (unsigned)h.fbFormat, 4); pushBits(b, (unsigned)h.fbInfo, 12);
    return b;
}

static Header parseHeader(const std::vector<uint8_t>& b) {
    Header h;
    int at = 0;
    h.type = b.size() == 80 ? 2 : 1;
    h.format = (int)takeBits(b, at, 3); h.lengthType = (int)takeBits(b, at, 1); h.length = (int)takeBits(b, at, 4); h.networkId = (int)takeBits(b, at, 8);
    h.txId = (int)takeBits(b, at, 16); h.power = (int)takeBits(b, at, 4);
    if (h.type == 1) { takeBits(b, at, 1); h.mcs = (int)takeBits(b, at, 3); return h; }
    h.mcs = (int)takeBits(b, at, 4); h.rxId = (int)takeBits(b, at, 16); h.streams = (int)takeBits(b, at, 2); h.rv = (int)takeBits(b, at, 2); h.ndi = (int)takeBits(b, at, 1);
    h.harq = (int)takeBits(b, at, 3); h.fbFormat = (int)takeBits(b, at, 4); h.fbInfo = (int)takeBits(b, at, 12);
    return h;
}

std::string describe(const Header& h) {
    char s[200];
    const int len = h.length + 1;
    if (h.type == 1) snprintf(s, sizeof s, "Type 1, length %d %s, MCS %d, power code %d", len, h.lengthType ? "slots" : "subslots", h.mcs, h.power);
    else snprintf(s, sizeof s, "Type 2 format %d, to %04X, length %d %s, MCS %d, HARQ %d rv %d, feedback format %d", h.format, (unsigned)h.rxId, len, h.lengthType ? "slots" : "subslots", h.mcs, h.harq, h.rv, h.fbFormat);
    std::string r = s;
    if (h.crcMask == 0x5555) r += ", closed-loop MIMO";
    if (h.crcMask == 0xAAAA) r += ", beamformed";
    return r;
}

std::vector<cf32> txPacket(const std::vector<uint8_t>& pccBits, int symbols, unsigned seed, int mask) {
    std::mt19937 g(seed);
    std::vector<cf32> out;
    const auto& S = stfBase();
    for (int k = 0; k < 7; k++) for (int i = 0; i < 16; i++) out.push_back(S[(size_t)i] * (float)kCover[k]);
    const auto coded = pccEncode(pccBits, mask);
    const auto sc = goldSeq(0x44454354u, kPccBits);
    std::vector<cf32> q;
    const float r2 = 1.f / std::sqrt(2.f);
    for (int i = 0; i < kPccRe; i++) {
        const int b0 = coded[(size_t)(2 * i)] ^ sc[(size_t)(2 * i)], b1 = coded[(size_t)(2 * i + 1)] ^ sc[(size_t)(2 * i + 1)];
        q.push_back(cf32((1 - 2 * b0) * r2, (1 - 2 * b1) * r2));
    }
    size_t qi = 0;
    for (int l = 1; l <= symbols; l++) {
        std::vector<cf32> X(kFft, cf32(0, 0));
        std::vector<bool> used(kFft, false);
        if (isDrsSymbol(l)) { const auto dk = drsCarriers(l); for (int i = 0; i < 14; i++) { X[(size_t)bin(dk[(size_t)i])] = cf32((float)kDrsSeq[4 * i], 0); used[(size_t)bin(dk[(size_t)i])] = true; } }
        for (int i = 0; i < 56; i++) {
            const int k = occ(i);
            if (used[(size_t)bin(k)]) continue;
            if (l <= 2 && qi < q.size()) X[(size_t)bin(k)] = q[qi++];
            else X[(size_t)bin(k)] = cf32((g() & 1) ? r2 : -r2, (g() & 1) ? r2 : -r2);
        }
        const auto t = ifftUnit(X, 56);
        out.insert(out.end(), t.end() - kCp, t.end());
        out.insert(out.end(), t.begin(), t.end());
    }
    return out;
}

// ---------------------------------------------------------------- receiver

struct Receiver::Impl {
    Cb cb;
    std::vector<cf32> h;
    std::vector<cf32> y;           // 16-tap correlation with S, y[i] belongs to sample i
    int64_t base = 0, n = 0, hold = 0;
    double esS = 0;
    uint64_t nSync = 0;
    Fft fft{kFft};
    int64_t cand = -1; double candM = 0;

    Impl() { for (const auto& v : stfBase()) esS += std::norm(v); }
    const cf32& at(int64_t i) const { return h[(size_t)(i - base)]; }
    const cf32& yat(int64_t i) const { return y[(size_t)(i - base)]; }

    // metric of an STF starting at w
    double metric(int64_t w, std::complex<double>& D, double& rho) const {
        std::complex<double> r[7];
        double E = 0, P = 0;
        for (int k = 0; k < 7; k++) { r[k] = std::complex<double>(yat(w + 16 * k)) * (double)kCover[k]; E += std::norm(r[k]); }
        for (int i = 0; i < kStf; i++) P += std::norm(at(w + i));
        D = 0;
        for (int k = 1; k < 7; k++) D += r[k] * std::conj(r[k - 1]);
        rho = P > 0 ? E / (P * esS) : 0;
        return E > 0 ? std::abs(D) / (E * 6.0 / 7.0) : 0;
    }

    void process(int64_t w) {
        std::complex<double> D; double rho;
        metric(w, D, rho);
        double cfo = std::arg(D) * kRate / (2 * kPi * 16);
        auto fftSym = [&](int l, double f, cf32* Y) {
            const int64_t s = w + kStf + (int64_t)(l - 1) * kSym + kCp - 2;
            cf32 t[kFft];
            for (int i = 0; i < kFft; i++) {
                const double ph = -2 * kPi * f * (double)(s + i - w) / kRate;
                t[i] = at(s + i) * cf32((float)std::cos(ph), (float)std::sin(ph));
            }
            fft.forward(t);
            for (int k = 0; k < kFft; k++) Y[k] = t[k];
        };
        // fine offset from the cyclic prefixes of symbols 1 .. 4
        std::complex<double> cp(0, 0);
        for (int l = 1; l <= 4; l++) {
            const int64_t s = w + kStf + (int64_t)(l - 1) * kSym;
            for (int i = 0; i < kCp; i++) {
                const double ph = -2 * kPi * cfo * (double)(s + i - w) / kRate, ph2 = -2 * kPi * cfo * (double)(s + i + kFft - w) / kRate;
                cp += std::complex<double>(at(s + i)) * std::polar(1.0, ph) * std::conj(std::complex<double>(at(s + i + kFft)) * std::polar(1.0, ph2));
            }
        }
        cfo += -std::arg(cp) * kRate / (2 * kPi * kFft);
        cf32 Y1[kFft], Y2[kFft];
        fftSym(1, cfo, Y1);
        fftSym(2, cfo, Y2);
        // channel from the DRS of symbol 1, linear between the pilot carriers
        const auto dk = drsCarriers(1);
        std::vector<std::complex<double>> hp(14);
        for (int i = 0; i < 14; i++) hp[(size_t)i] = std::complex<double>(Y1[bin(dk[(size_t)i])]) / (double)kDrsSeq[4 * i];
        auto H = [&](int k) {
            if (k <= dk[0]) return hp[0];
            if (k >= dk[13]) return hp[13];
            for (int i = 0; i < 13; i++) if (k >= dk[(size_t)i] && k <= dk[(size_t)(i + 1)]) {
                const double a = (double)(k - dk[(size_t)i]) / (dk[(size_t)(i + 1)] - dk[(size_t)i]);
                return hp[(size_t)i] * (1 - a) + hp[(size_t)(i + 1)] * a;
            }
            return hp[0];
        };
        // noise from the empty guard carriers and DC
        double nz = 0; int nn = 0;
        for (int k : {-32, -31, -30, -29, 0, 29, 30, 31}) { nz += std::norm(Y1[bin(k)]) + std::norm(Y2[bin(k)]); nn += 2; }
        nz = nz / nn + 1e-12;
        double hpw = 0; for (auto& v : hp) hpw += std::norm(v); hpw /= 14;
        std::vector<float> soft;
        std::vector<bool> isD(kFft, false);
        for (int k : dk) isD[(size_t)bin(k)] = true;
        const auto sc = goldSeq(0x44454354u, kPccBits);
        auto add = [&](const cf32& Y, int k) {
            const std::complex<double> e = std::complex<double>(Y) * std::conj(H(k));
            const double sc2 = std::sqrt(2.0) * 2 / nz;
            soft.push_back((float)(-e.real() * sc2));
            soft.push_back((float)(-e.imag() * sc2));
        };
        for (int i = 0; i < 56; i++) { const int k = occ(i); if (!isD[(size_t)bin(k)]) add(Y1[bin(k)], k); }
        for (int i = 0; i < 56 && (int)soft.size() < kPccBits; i++) { const int k = occ(i); add(Y2[bin(k)], k); }
        for (int i = 0; i < kPccBits; i++) if (sc[(size_t)i]) soft[(size_t)i] = -soft[(size_t)i];
        std::vector<uint8_t> bits;
        int mask = 0;
        if (!pccDecode(soft, bits, mask)) return;
        Header hd = parseHeader(bits);
        hd.crcMask = mask;
        hd.cfoHz = cfo;
        hd.snrDb = (float)(10 * std::log10(hpw / nz + 1e-12));
        hd.at = w;
        if (cb) cb(hd);
        hold = w + kStf + 2 * kSym;
    }

    void feedOne(cf32 x) {
        h.push_back(x);
        y.push_back(cf32(0, 0));
        const int64_t m = n++;
        if (m >= 15) {
            const int64_t i = m - 15;
            std::complex<double> a(0, 0);
            const auto& S = stfBase();
            for (int j = 0; j < 16; j++) a += std::complex<double>(at(i + j)) * std::conj(std::complex<double>(S[(size_t)j]));
            y[(size_t)(i - base)] = cf32((float)a.real(), (float)a.imag());
        }
        // a window can be judged when its last y is known
        const int64_t w = m - 15 - 6 * 16;
        if (w < base || w < hold) return;
        std::complex<double> D; double rho;
        const double M = metric(w, D, rho);
        if (M > 0.7 && rho > 0.45) {
            if (cand < 0 || M > candM) { if (cand < 0) nSync++; cand = w; candM = M; }
        }
        // decide when the candidate is 12 samples behind and the PCC symbols are in
        if (cand >= 0 && w > cand + 12 && n >= cand + kStf + 4 * kSym + 4) { const int64_t c = cand; cand = -1; candM = 0; if (c >= hold) process(c); }
    }
};

Receiver::Receiver() : p_(std::make_unique<Impl>()) {}
Receiver::~Receiver() = default;
void Receiver::setCallback(Cb cb) { p_->cb = std::move(cb); }
void Receiver::reset() { Impl& m = *p_; m.h.clear(); m.y.clear(); m.base = m.n = m.hold = 0; m.cand = -1; }
uint64_t Receiver::syncs() const { return p_->nSync; }
void Receiver::feed(const cf32* x, size_t cnt) {
    Impl& m = *p_;
    for (size_t i = 0; i < cnt; i++) {
        const float re = x[i].real(), im = x[i].imag();
        m.feedOne(std::isfinite(re) && std::isfinite(im) ? x[i] : cf32(0, 0));
    }
    const int64_t keep = 4096;
    if (m.n - m.base > 4 * keep) { int64_t cut = m.n - keep - m.base; if (m.cand >= 0) cut = std::min(cut, m.cand - m.base - 16); if (cut > 0) { m.h.erase(m.h.begin(), m.h.begin() + (ptrdiff_t)cut); m.y.erase(m.y.begin(), m.y.begin() + (ptrdiff_t)cut); m.base += cut; } }
}

} // namespace dect2::xlink::nrplus
