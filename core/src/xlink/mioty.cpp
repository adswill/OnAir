// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// mioty / TS-UNB uplink (see mioty.h). Clause numbers: ETSI TS 103 357 V1.1.1 (2018-06).
#include "dect2/xlink/mioty.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace dect2::xlink::mioty {

using dect2::math::kPi;

namespace {
// Tables 6-49 .. 6-54: radio-burst carrier and time sets of the uplink TSMA pattern groups
const uint8_t kCar1[8][24] = {
    {5, 21, 13, 6, 22, 14, 1, 17, 9, 0, 16, 8, 7, 23, 15, 4, 20, 12, 3, 19, 11, 2, 18, 10},
    {4, 20, 12, 1, 17, 9, 0, 16, 8, 6, 22, 14, 7, 23, 15, 2, 18, 10, 5, 21, 13, 3, 19, 11},
    {4, 20, 12, 3, 19, 11, 6, 22, 14, 7, 23, 15, 0, 16, 8, 5, 21, 13, 2, 18, 10, 1, 17, 9},
    {6, 22, 14, 2, 18, 10, 7, 23, 15, 0, 16, 8, 1, 17, 9, 4, 20, 12, 5, 21, 13, 3, 19, 11},
    {7, 23, 15, 4, 20, 12, 3, 19, 11, 2, 18, 10, 6, 22, 14, 0, 16, 8, 1, 17, 9, 5, 21, 13},
    {3, 19, 11, 6, 22, 14, 2, 18, 10, 0, 16, 8, 7, 23, 15, 1, 17, 9, 4, 20, 12, 5, 21, 13},
    {3, 19, 11, 1, 17, 9, 5, 21, 13, 7, 23, 15, 0, 16, 8, 2, 18, 10, 6, 22, 14, 4, 20, 12},
    {0, 16, 8, 6, 22, 14, 3, 19, 11, 2, 18, 10, 4, 20, 12, 7, 23, 15, 5, 21, 13, 1, 17, 9},
};
const uint8_t kCar2[8][24] = {
    {4, 20, 12, 0, 16, 8, 3, 19, 11, 5, 21, 13, 1, 17, 9, 7, 23, 15, 2, 18, 10, 6, 22, 14},
    {3, 19, 11, 7, 23, 15, 2, 18, 10, 5, 21, 13, 4, 20, 12, 0, 16, 8, 1, 17, 9, 6, 22, 14},
    {6, 22, 14, 0, 16, 8, 1, 17, 9, 4, 20, 12, 3, 19, 11, 5, 21, 13, 2, 18, 10, 7, 23, 15},
    {3, 19, 11, 1, 17, 9, 4, 20, 12, 5, 21, 13, 2, 18, 10, 7, 23, 15, 6, 22, 14, 0, 16, 8},
    {5, 21, 13, 2, 18, 10, 0, 16, 8, 6, 22, 14, 7, 23, 15, 1, 17, 9, 4, 20, 12, 3, 19, 11},
    {1, 17, 9, 3, 19, 11, 4, 20, 12, 6, 22, 14, 7, 23, 15, 5, 21, 13, 2, 18, 10, 0, 16, 8},
    {5, 21, 13, 1, 17, 9, 2, 18, 10, 4, 20, 12, 3, 19, 11, 0, 16, 8, 6, 22, 14, 7, 23, 15},
    {3, 19, 11, 6, 22, 14, 5, 21, 13, 1, 17, 9, 7, 23, 15, 2, 18, 10, 0, 16, 8, 4, 20, 12},
};
const uint16_t kTim1[8][24] = {   // [0] unused: burst 0 has no spacing
    {0, 330, 387, 388, 330, 387, 354, 330, 387, 356, 330, 387, 432, 330, 387, 352, 330, 387, 467, 330, 387, 620, 330, 387},
    {0, 330, 387, 435, 330, 387, 409, 330, 387, 398, 330, 387, 370, 330, 387, 361, 330, 387, 472, 330, 387, 522, 330, 387},
    {0, 330, 387, 356, 330, 387, 439, 330, 387, 413, 330, 387, 352, 330, 387, 485, 330, 387, 397, 330, 387, 444, 330, 387},
    {0, 330, 387, 352, 330, 387, 382, 330, 387, 381, 330, 387, 365, 330, 387, 595, 330, 387, 604, 330, 387, 352, 330, 387},
    {0, 330, 387, 380, 330, 387, 634, 330, 387, 360, 330, 387, 393, 330, 387, 352, 330, 387, 373, 330, 387, 490, 330, 387},
    {0, 330, 387, 364, 330, 387, 375, 330, 387, 474, 330, 387, 355, 330, 387, 478, 330, 387, 464, 330, 387, 513, 330, 387},
    {0, 330, 387, 472, 330, 387, 546, 330, 387, 501, 330, 387, 356, 330, 387, 359, 330, 387, 359, 330, 387, 364, 330, 387},
    {0, 330, 387, 391, 330, 387, 468, 330, 387, 512, 330, 387, 543, 330, 387, 354, 330, 387, 391, 330, 387, 368, 330, 387},
};
const uint16_t kTim2[8][24] = {   // [0] unused: burst 0 has no spacing
    {0, 373, 319, 545, 373, 319, 443, 373, 319, 349, 373, 319, 454, 373, 319, 578, 373, 319, 436, 373, 319, 398, 373, 319},
    {0, 373, 319, 371, 373, 319, 410, 373, 319, 363, 373, 319, 354, 373, 319, 379, 373, 319, 657, 373, 319, 376, 373, 319},
    {0, 373, 319, 414, 373, 319, 502, 373, 319, 433, 373, 319, 540, 373, 319, 428, 373, 319, 467, 373, 319, 409, 373, 319},
    {0, 373, 319, 396, 373, 319, 516, 373, 319, 631, 373, 319, 471, 373, 319, 457, 373, 319, 416, 373, 319, 354, 373, 319},
    {0, 373, 319, 655, 373, 319, 416, 373, 319, 367, 373, 319, 400, 373, 319, 415, 373, 319, 342, 373, 319, 560, 373, 319},
    {0, 373, 319, 370, 373, 319, 451, 373, 319, 465, 373, 319, 593, 373, 319, 545, 373, 319, 380, 373, 319, 365, 373, 319},
    {0, 373, 319, 393, 373, 319, 374, 373, 319, 344, 373, 319, 353, 373, 319, 620, 373, 319, 503, 373, 319, 546, 373, 319},
    {0, 373, 319, 367, 373, 319, 346, 373, 319, 584, 373, 319, 579, 373, 319, 519, 373, 319, 351, 373, 319, 486, 373, 319},
};
const uint8_t kCar3[24] = {1, 5, 4, 3, 2, 17, 21, 20, 19, 18, 9, 13, 12, 11, 10, 6, 0, 7, 22, 16, 23, 14, 8, 15};
const uint16_t kTim3[24] = {0, 66, 66, 66, 66, 66, 66, 66, 66, 66, 123, 66, 66, 66, 66, 60, 66, 66, 198, 66, 66, 255, 66, 66};

const uint8_t kPilotCore[12] = {0, 1, 1, 1, 0, 1, 0, 0, 0, 0, 1, 0};     // clause 6.4.2.1.1
const uint8_t kPilotExt[12] = {0, 1, 0, 0, 1, 1, 1, 1, 1, 0, 1, 0};
// Table 6-43: symbol m of the o-th bit (ascending index) of a sub-packet
const int kMapEven[24] = {11, 24, 10, 25, 9, 26, 8, 27, 7, 28, 6, 29, 5, 30, 4, 31, 3, 32, 2, 33, 1, 34, 0, 35};
const int kMapOdd[24] = {24, 11, 25, 10, 26, 9, 27, 8, 28, 7, 29, 6, 30, 5, 31, 4, 32, 3, 33, 2, 34, 1, 35, 0};
// clause 6.4.6.3: 0155, 0123, 0137 octal; as masks on a register whose bit 0 is the newest input (bit-reversed, like the 802.11 code in wifi_ofdm.cpp)
const unsigned kTaps[3] = {0x5B, 0x65, 0x7D};
constexpr int kBurstSyms = 36;
constexpr int kTmplLen = 13 * kSps;            // the pilot's 12 symbols and half a pulse on each side

// symbol n of a burst carrying bit b: -(2b - 1) (-j)^n (Figure 6-13, absolute phase, repeating every 4 symbols)
cf32 symbolOf(int bit, int n) {
    static const cf32 r[4] = {cf32(1, 0), cf32(0, -1), cf32(-1, 0), cf32(0, 1)};
    return r[n & 3] * (bit ? -1.f : 1.f);
}
// half-sine pulse of MSK, centred, |t| < T samples
inline float pulse(double t) { return std::fabs(t) < kSps ? (float)std::cos(kPi * t / (2.0 * kSps)) : 0.f; }

void whiten(std::vector<uint8_t>& bits) {                 // PN9 of IEEE 802.15.4: x^9 + x^5 + 1, all ones at the start
    unsigned l = 0x1FF;
    for (uint8_t& b : bits) {
        b ^= (uint8_t)(l & 1);
        const unsigned nb = (l ^ (l >> 5)) & 1;
        l = (l >> 1) | (nb << 8);
    }
}

std::vector<uint8_t> encode(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> o;
    unsigned s = 0;
    for (uint8_t b : in) {
        const unsigned reg = (b & 1u) | (s << 1);
        for (unsigned t : kTaps) o.push_back((uint8_t)__builtin_parity(reg & t));
        s = reg & 0x3F;
    }
    return o;
}

// soft Viterbi, rate 1/3; soft > 0 means a one, 0 an erasure. terminated: the path ends in state 0
void viterbi(const float* soft, int n, uint8_t* out, bool terminated) {
    constexpr float kNeg = -1e30f;
    std::vector<uint64_t> dec((size_t)n);
    float m[64], nm[64];
    for (float& v : m) v = kNeg;
    m[0] = 0;
    for (int i = 0; i < n; i++) {
        uint64_t d = 0;
        for (int ns = 0; ns < 64; ns++) {
            const unsigned b = (unsigned)ns & 1;
            float best = kNeg; int bt = 0;
            for (int t = 0; t < 2; t++) {
                const int ps = (ns >> 1) | (t << 5);
                if (m[ps] <= kNeg) continue;
                const unsigned reg = b | ((unsigned)ps << 1);
                float v = m[ps];
                for (int k = 0; k < 3; k++) v += __builtin_parity(reg & kTaps[k]) ? soft[3 * i + k] : -soft[3 * i + k];
                if (v > best) { best = v; bt = t; }
            }
            nm[ns] = best;
            d |= (uint64_t)bt << ns;
        }
        dec[(size_t)i] = d;
        std::memcpy(m, nm, sizeof m);
    }
    int s = 0;
    if (!terminated) { float b = kNeg; for (int k = 0; k < 64; k++) if (m[k] > b) { b = m[k]; s = k; } }
    for (int i = n - 1; i >= 0; i--) {
        out[i] = (uint8_t)(s & 1);
        s = (s >> 1) | (int)(dec[(size_t)i] >> s & 1) << 5;
    }
}

void pushBits(std::vector<uint8_t>& v, unsigned x, int n) { for (int i = n - 1; i >= 0; i--) v.push_back((uint8_t)(x >> i & 1)); }

// The uncoded PHY payload of Table 6-33 (before whitening)
std::vector<uint8_t> phyPayload(const std::vector<uint8_t>& mpdu, int mmode, uint8_t* hcrc, uint8_t* pcrc) {
    std::vector<uint8_t> crcIn;
    for (uint8_t b : mpdu) pushBits(crcIn, b, 8);
    pushBits(crcIn, (unsigned)mmode, 2);
    const uint8_t pc = crc8(crcIn);
    std::vector<uint8_t> h;
    pushBits(h, pc, 8);
    pushBits(h, (unsigned)mpdu.size(), 8);
    const uint8_t hc = crc8(h);
    std::vector<uint8_t> o;
    pushBits(o, hc, 8);
    pushBits(o, pc, 8);
    pushBits(o, (unsigned)mpdu.size(), 8);
    const size_t padded = std::max<size_t>(20, mpdu.size());
    for (size_t i = 0; i < padded; i++) pushBits(o, i < mpdu.size() ? mpdu[i] : 0, 8);
    pushBits(o, (unsigned)mmode, 2);
    if (hcrc) *hcrc = hc;
    if (pcrc) *pcrc = pc;
    return o;
}

// Where coded bit j (before the cyclic shift) goes: sub-packet and symbol (clause 6.4.4.6)
struct Place { int s, m; };
std::vector<Place> placement(int N) {
    const int se = (N - 576) / 24, G = 12 + se;
    std::vector<int> sub((size_t)N);
    for (int k = 0; k < N; k++) {
        if (k < 288) sub[(size_t)k] = k % 24;
        else {
            const int g = (k - 288) / G, ig = (k - 288) - g * G;
            sub[(size_t)k] = ig < 12 ? ig * 2 + (g & 1) : ig + 12;
        }
    }
    std::vector<int> cnt((size_t)(24 + se), 0);
    std::vector<Place> pk((size_t)N);
    for (int k = 0; k < N; k++) {
        const int s = sub[(size_t)k], o = cnt[(size_t)s]++;
        pk[(size_t)k] = {s, (s & 1) ? kMapOdd[o] : kMapEven[o]};
    }
    std::vector<Place> pj((size_t)N);
    for (int j = 0; j < N; j++) pj[(size_t)j] = pk[(size_t)((j + 48) % N)];   // the last 48 coded bits are sent first
    return pj;
}

uint16_t lfsrNext(uint16_t r) { const bool l = r & 1; r >>= 1; if (l) r ^= 0xB4F3; return r; }   // Galois, right shift (assumed)

struct Burst { double t; int carrier; bool ext; };
// burst pilot centres in symbols from burst 0 and carriers, core then extension (clause 6.4.7.1.6)
std::vector<Burst> schedule(int pattern, int se, uint8_t hcrc, uint8_t pcrc) {
    std::vector<Burst> v;
    double t = 0;
    for (int s = 0; s < 24; s++) { if (s) t += patternTime(pattern, s); v.push_back({t, patternCarrier(pattern, s), false}); }
    uint16_t r = (uint16_t)(0x8000 | (hcrc & 0x7F) << 8 | pcrc);
    for (int e = 0; e < se; e++) {
        r = lfsrNext(r);
        t += patternGroupSpacing(pattern) + (r % 128);
        v.push_back({t, (r / 256) % 25, true});
    }
    return v;
}

int cumMax() {
    static const int v = [] { int m = 0; for (int p = 0; p < 17; p++) { int t = 0; for (int s = 1; s < 24; s++) t += patternTime(p, s); m = std::max(m, t); } return m; }();
    return v;
}
} // namespace

int patternCarrier(int p, int s) { return p < 8 ? kCar1[p][s] : p < 16 ? kCar2[p - 8][s] : kCar3[s]; }
int patternTime(int p, int s) { return p < 8 ? kTim1[p][s] : p < 16 ? kTim2[p - 8][s] : kTim3[s]; }
int patternGroupSpacing(int p) { return p < 16 ? 337 : 66; }
const char* patternName(int p) {
    static const char* n[17] = {"UPG1/1", "UPG1/2", "UPG1/3", "UPG1/4", "UPG1/5", "UPG1/6", "UPG1/7", "UPG1/8", "UPG2/1", "UPG2/2", "UPG2/3", "UPG2/4", "UPG2/5", "UPG2/6", "UPG2/7", "UPG2/8", "UPG3"};
    return (p >= 0 && p < 17) ? n[p] : "?";
}

uint8_t crc8(const std::vector<uint8_t>& bits) {
    unsigned c = 0xFF;
    for (uint8_t b : bits) { const unsigned fb = ((c >> 7) ^ b) & 1; c = (c << 1) & 0xFF; if (fb) c ^= 0x9B; }
    return (uint8_t)c;
}

std::string describeMpdu(const std::vector<uint8_t>& m) {
    if (m.size() < 1 + 2 + 3 + 4) return "too short for the fixed MAC mode";
    const uint8_t h = m[0];       // bit 0 taken as the least significant bit (assumed)
    const bool mpf = h >> 1 & 1, full = h >> 5 & 1, attach = h >> 6 & 1;
    size_t i = 1;
    char b[200];
    std::string addr;
    const size_t na = full ? 8 : 2;
    if (m.size() < 1 + na + 3 + 4) return "too short for the fixed MAC mode";
    for (size_t k = 0; k < na; k++) { snprintf(b, sizeof b, "%02X", m[i + k]); addr += b; }
    i += na;
    const unsigned cnt = (unsigned)m[i] << 16 | (unsigned)m[i + 1] << 8 | m[i + 2];
    i += 3;
    if (mpf && i < m.size()) i++;
    const long pay = (long)m.size() - (long)i - 4;
    snprintf(b, sizeof b, "%s %s, counter %u, %ld payload bytes %s%s%s%s", full ? "EUI-64" : "short address", addr.c_str(), cnt, std::max(0L, pay),
             attach ? "(attach, unencrypted)" : "(encrypted)", h >> 2 & 1 ? ", control" : "", h >> 3 & 1 ? ", response wanted" : "", h >> 7 & 1 ? ", ACK" : "");
    return b;
}

void addFrame(std::vector<cf32>& buf, const std::vector<uint8_t>& mpdu, int pattern, double fs, double f0, int64_t start, int mmode) {
    uint8_t hc = 0, pc = 0;
    std::vector<uint8_t> u = phyPayload(mpdu, mmode, &hc, &pc);
    whiten(u);
    for (int i = 0; i < 6; i++) u.push_back(0);
    const std::vector<uint8_t> c = encode(u);
    const int N = (int)c.size(), se = (N - 576) / 24;
    const std::vector<Place> pl = placement(N);
    std::vector<std::vector<uint8_t>> bits((size_t)(24 + se), std::vector<uint8_t>(kBurstSyms, 0));
    for (int j = 0; j < N; j++) bits[(size_t)pl[(size_t)j].s][(size_t)pl[(size_t)j].m] = c[(size_t)j];
    for (size_t s = 0; s < bits.size(); s++) for (int k = 0; k < 12; k++) bits[s][(size_t)(12 + k)] = s < 24 ? kPilotCore[k] : kPilotExt[k];
    const std::vector<Burst> sc = schedule(pattern, se, hc, pc);
    const double T = fs / kSymRate;
    for (size_t s = 0; s < sc.size(); s++) {
        const double tc = (double)start + sc[s].t * T;
        const double f = f0 + sc[s].carrier * kCarrierStep;
        const int64_t a = (int64_t)std::floor(tc - 19 * T), z = (int64_t)std::ceil(tc + 19 * T);
        for (int64_t i = std::max<int64_t>(0, a); i < z && i < (int64_t)buf.size(); i++) {
            cf32 v(0, 0);
            for (int n = 0; n < kBurstSyms; n++) {
                const double tn = tc + (n + 0.5 - 18) * T;
                const double d = ((double)i - tn) / T;
                if (std::fabs(d) < 1) v += symbolOf(bits[s][(size_t)n], n) * (float)std::cos(kPi * d / 2);
            }
            const double ph = 2 * kPi * f * (double)i / fs;
            buf[(size_t)i] += v * cf32((float)std::cos(ph), (float)std::sin(ph));
        }
    }
}

std::vector<cf32> txFrame(const std::vector<uint8_t>& mpdu, int pattern, double fs, double f0, int64_t start, size_t total, int mmode) {
    std::vector<cf32> b(total, cf32(0, 0));
    addFrame(b, mpdu, pattern, fs, f0, start, mmode);
    return b;
}

// ---------------------------------------------------------------- receiver

struct Receiver::Impl {
    Cb cb;
    std::vector<cf32> h;           // history: samples base .. base + h.size() - 1
    int64_t base = 0, n = 0, next = kTmplLen;
    std::vector<cf32> tmpl;        // core pilot waveform, centred at index kTmplLen / 2
    std::vector<cf32> tmplExt;
    Fft fft{1024};
    struct Ev { int64_t t; double f; float p; bool used; };
    std::vector<Ev> cand, ev;
    uint64_t nPilots = 0;
    std::set<long long> tried;                 // hypotheses that failed to decode
    int64_t lastSearch = 0;

    Impl() {
        tmpl.resize(kTmplLen); tmplExt.resize(kTmplLen);
        for (int i = 0; i < kTmplLen; i++) {
            const double t = i - kTmplLen / 2.0;
            cf32 a(0, 0), e(0, 0);
            for (int k = 0; k < 12; k++) {
                const double tn = (12 + k + 0.5 - 18) * kSps;
                const float p = pulse(t - tn);
                a += symbolOf(kPilotCore[k], 12 + k) * p;
                e += symbolOf(kPilotExt[k], 12 + k) * p;
            }
            tmpl[(size_t)i] = a; tmplExt[(size_t)i] = e;
        }
    }

    const cf32& at(int64_t i) const { return h[(size_t)(i - base)]; }
    bool have(int64_t a, int64_t z) const { return a >= base && z <= n; }

    void detect(int64_t w0) {
        cf32 x[1024];
        for (int i = 0; i < 1024; i++) x[i] = i < kTmplLen ? at(w0 + i) * std::conj(tmpl[(size_t)i]) : cf32(0, 0);
        fft.forward(x);
        float pw[1024];
        double mean = 0;
        for (int k = 0; k < 1024; k++) { pw[k] = std::norm(x[k]); mean += pw[k]; }
        mean /= 1024;
        float mx = 0;
        for (int k = 0; k < 1024; k++) mx = std::max(mx, pw[k]);
        const float thr = std::max((float)(18 * mean), 0.1f * mx);   // sidelobes of a strong burst stay out
        const int64_t tc = w0 + kTmplLen / 2;
        for (int k = 0; k < 1024; k++) {
            if (pw[k] < thr) continue;
            bool peak = true;
            for (int d = -12; d <= 12 && peak; d++) if (d && pw[(k + d + 1024) & 1023] > pw[k]) peak = false;
            if (!peak) continue;
            const double f = (k < 512 ? k : k - 1024) * kRate / 1024;
            bool merged = false;
            for (Ev& c : cand) if (std::llabs(c.t - tc) <= 14 * kSps && std::fabs(c.f - f) < 300) { if (pw[k] > c.p) { c.t = tc; c.f = f; c.p = pw[k]; } merged = true; break; }
            if (!merged) cand.push_back({tc, f, pw[k], false});
        }
        // finalize the candidates that cannot grow any more
        for (size_t i = 0; i < cand.size();) {
            if (cand[i].t < tc - 16 * kSps) { ev.push_back(cand[i]); nPilots++; cand.erase(cand.begin() + (ptrdiff_t)i); } else i++;
        }
    }

    // matched-filter output of symbol n of a burst centred at tc (fractional samples), carrier f (Hz)
    cf32 sym(double tc, double f, int nIdx) const {
        // half-sine weights for a symbol centre at a fractional position, in 1/32 sample steps
        static const std::vector<float> tab = [] {
            std::vector<float> t(32 * (2 * kSps + 1));
            for (int q = 0; q < 32; q++) for (int k = 0; k <= 2 * kSps; k++) t[(size_t)(q * (2 * kSps + 1) + k)] = pulse(k - kSps - q / 32.0);
            return t;
        }();
        const double tn = tc + (nIdx + 0.5 - 18) * kSps;
        const double fl = std::floor(tn);
        const int q = std::min(31, (int)((tn - fl) * 32 + 0.5));
        const int64_t a = (int64_t)fl - kSps;
        const float* wts = tab.data() + (size_t)q * (2 * kSps + 1);
        const double w = -2 * kPi * f / kRate;
        std::complex<double> ph = std::polar(1.0, w * (double)a), step = std::polar(1.0, w);
        std::complex<double> acc(0, 0);
        const cf32* x = h.data() + (a - base);
        for (int k = 0; k <= 2 * kSps; k++) { acc += std::complex<double>(x[k]) * ph * (double)wts[k]; ph *= step; }
        return cf32((float)acc.real(), (float)acc.imag());
    }

    double pilotFit(double tc, double f, const uint8_t* pilot, cf32* hOut) const {
        std::complex<double> acc(0, 0);
        for (int k = 0; k < 12; k++) acc += std::complex<double>(sym(tc, f, 12 + k)) * std::conj(std::complex<double>(symbolOf(pilot[k], 12 + k)));
        if (hOut) *hOut = cf32((float)acc.real() / 12, (float)acc.imag() / 12);
        return std::norm(acc);
    }

    // refine a burst around (tc, f); returns false when its samples are not available. soft: 36 values (> 0 a one), weighted by the fit
    bool burst(double& tc, double& f, const uint8_t* pilot, float* soft, double& sig, double& noise) {
        if (!have((int64_t)(tc - 21 * kSps), (int64_t)(tc + 21 * kSps))) return false;
        double bt = tc, bf = f, bm = -1;
        for (int dt = -24; dt <= 24; dt += 6) for (int df = -150; df <= 150; df += 30) { const double m = pilotFit(tc + dt, f + df, pilot, nullptr); if (m > bm) { bm = m; bt = tc + dt; bf = f + df; } }
        const double t1 = bt, f1 = bf;
        for (int dt = -5; dt <= 5; dt += 1) for (int df = -25; df <= 25; df += 5) { const double m = pilotFit(t1 + dt, f1 + df, pilot, nullptr); if (m > bm) { bm = m; bt = t1 + dt; bf = f1 + df; } }
        tc = bt; f = bf;
        // phase slope over the pilot: first half against second half
        cf32 hh;
        pilotFit(tc, f, pilot, &hh);
        std::complex<double> a1(0, 0), a2(0, 0);
        for (int k = 0; k < 6; k++) a1 += std::complex<double>(sym(tc, f, 12 + k)) * std::conj(std::complex<double>(symbolOf(pilot[k], 12 + k)));
        for (int k = 6; k < 12; k++) a2 += std::complex<double>(sym(tc, f, 12 + k)) * std::conj(std::complex<double>(symbolOf(pilot[k], 12 + k)));
        const double slope = std::arg(a2 * std::conj(a1)) / 6;          // radians per symbol
        f += slope * kSymRate / (2 * kPi);
        pilotFit(tc, f, pilot, &hh);
        double e = 0;
        for (int k = 0; k < 12; k++) e += std::norm(sym(tc, f, 12 + k) - hh * symbolOf(pilot[k], 12 + k));
        sig = std::norm(hh);
        noise = e / 12 + 1e-12;
        for (int n2 = 0; n2 < kBurstSyms; n2++) {
            const cf32 r = sym(tc, f, n2) * std::conj(hh) * std::conj(symbolOf(0, n2));
            soft[n2] = -r.real() / (float)noise;            // symbolOf(0) is +(-j)^n; a one is its negative
        }
        return true;
    }

    // decode one hypothesis; 1 decoded, 0 failed, -1 not all samples in yet
    int decode(int pattern, int64_t t0, double f0, int found) {
        std::vector<float> soft(24 * kBurstSyms);
        std::vector<double> tcs(24), fs(24);
        double sigSum = 0, noiseSum = 0;
        double dtAcc = 0, dfAcc = 0;
        for (int s = 0; s < 24; s++) {
            double t = 0; for (int k = 1; k <= s; k++) t += patternTime(pattern, k);
            double tc = (double)t0 + t * kSps + dtAcc, f = f0 + patternCarrier(pattern, s) * kCarrierStep + dfAcc;
            double sg, nz;
            if (!have((int64_t)(tc - 21 * kSps), (int64_t)(tc + 21 * kSps))) return -1;
            const double tp = tc, fp = f;
            burst(tc, f, kPilotCore, soft.data() + s * kBurstSyms, sg, nz);
            if (sg > 4 * nz) { dtAcc += 0.5 * (tc - tp); dfAcc += 0.5 * (f - fp); }   // follow slow drift from the strong bursts
            tcs[(size_t)s] = tc; fs[(size_t)s] = f;
            sigSum += sg; noiseSum += nz;
        }
        // PHY header first: the first 240 coded bits sit in the same core places whatever the extension length
        const std::vector<Place> p0 = placement(576);
        std::vector<float> c0(240);
        for (int j = 0; j < 240; j++) c0[(size_t)j] = soft[(size_t)(p0[(size_t)j].s * kBurstSyms + p0[(size_t)j].m)];
        uint8_t hb[80];
        viterbi(c0.data(), 80, hb, false);
        std::vector<uint8_t> hv(hb, hb + 24);
        whiten(hv);
        auto val = [](const std::vector<uint8_t>& v, int at, int nb) { unsigned x = 0; for (int i = 0; i < nb; i++) x = x << 1 | v[(size_t)(at + i)]; return x; };
        const uint8_t hc = (uint8_t)val(hv, 0, 8), pc = (uint8_t)val(hv, 8, 8);
        const int psi = (int)val(hv, 16, 8);
        std::vector<uint8_t> hchk(hv.begin() + 8, hv.begin() + 24);
        if (crc8(hchk) != hc || psi < 1 || psi > 245) return 0;
        const int se = std::max(0, psi - 20), N = 576 + 24 * se;
        soft.resize((size_t)(24 + se) * kBurstSyms, 0.f);
        if (se) {
            const std::vector<Burst> sc = schedule(pattern, se, hc, pc);
            const double base23 = sc[23].t;
            for (int e = 0; e < se; e++) {
                double tc = tcs[23] + (sc[(size_t)(24 + e)].t - base23) * kSps + dtAcc * 0;
                double f = fs[0] - patternCarrier(pattern, 0) * kCarrierStep + sc[(size_t)(24 + e)].carrier * kCarrierStep;
                double sg, nz;
                if (!have((int64_t)(tc - 21 * kSps), (int64_t)(tc + 21 * kSps))) return -1;
                burst(tc, f, kPilotExt, soft.data() + (24 + e) * kBurstSyms, sg, nz);
                sigSum += sg; noiseSum += nz;
            }
        }
        const std::vector<Place> pl = placement(N);
        std::vector<float> c((size_t)N);
        for (int j = 0; j < N; j++) c[(size_t)j] = soft[(size_t)(pl[(size_t)j].s * kBurstSyms + pl[(size_t)j].m)];
        const int L = N / 3;
        std::vector<uint8_t> u((size_t)L);
        viterbi(c.data(), L, u.data(), true);
        u.resize((size_t)(L - 6));
        whiten(u);
        Frame fr;
        fr.pattern = pattern; fr.psi = psi;
        fr.headerCrc = (uint8_t)val(u, 0, 8); fr.payloadCrc = (uint8_t)val(u, 8, 8);
        if (fr.headerCrc != hc || fr.payloadCrc != pc) return 0;
        for (int i = 0; i < psi; i++) fr.mpdu.push_back((uint8_t)val(u, 24 + 8 * i, 8));
        fr.mmode = (int)val(u, (int)u.size() - 2, 2);
        std::vector<uint8_t> pchk;
        for (uint8_t b : fr.mpdu) pushBits(pchk, b, 8);
        pushBits(pchk, (unsigned)fr.mmode, 2);
        if (crc8(pchk) != pc) return 0;
        fr.freqHz = fs[0] - patternCarrier(pattern, 0) * kCarrierStep;
        fr.snrDb = 10 * std::log10(sigSum / noiseSum + 1e-12);   // per symbol, after the matched filter
        fr.burstsFound = found;
        fr.at = (int64_t)tcs[0];
        if (cb) cb(fr);
        return 1;
    }

    void search(bool final) {
        std::sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
        const int64_t span = (int64_t)cumMax() * kSps;
        for (size_t ai = 0; ai < ev.size(); ai++) {
            Ev& a = ev[ai];
            if (a.used) continue;
            int bestScore = 0, bp = -1; int64_t bt0 = 0; double bf0 = 0;
            bool incomplete = false;
            for (int p = 0; p < 17; p++) {
                int cum[24]; cum[0] = 0;
                for (int s = 1; s < 24; s++) cum[s] = cum[s - 1] + patternTime(p, s);
                for (int s = 0; s < 24; s++) {
                    const int64_t t0 = a.t - (int64_t)cum[s] * kSps;
                    const double f0 = a.f - patternCarrier(p, s) * kCarrierStep;
                    if (t0 < base + 22 * kSps) continue;
                    if (t0 + (int64_t)cum[23] * kSps + 24 * kSps > n) { if (!final) incomplete = true; continue; }
                    int score = 0;
                    for (int k = 0; k < 24; k++) {
                        const int64_t tk = t0 + (int64_t)cum[k] * kSps;
                        const double fk = f0 + patternCarrier(p, k) * kCarrierStep;
                        auto it = std::lower_bound(ev.begin(), ev.end(), tk - 60, [](const Ev& e, int64_t v) { return e.t < v; });
                        for (; it != ev.end() && it->t <= tk + 60; ++it) if (!it->used && std::fabs(it->f - fk) < 200) { score++; break; }
                    }
                    if (score > bestScore) { bestScore = score; bp = p; bt0 = t0; bf0 = f0; }
                }
            }
            const long long key = ((long long)bp << 40) ^ ((long long)(bt0 / 24) << 12) ^ (long long)std::lround(bf0 / 100 + 2048);
            if (bestScore >= 5 && !tried.count(key)) {
                const int r = decode(bp, bt0, bf0, bestScore);
                if (r == 0) tried.insert(key);
                if (r < 0) continue;                                   // wait for the rest of the frame
                if (r == 1) {
                    int cum = 0;
                    for (int k = 0; k < 24; k++) {
                        if (k) cum += patternTime(bp, k);
                        const int64_t tk = bt0 + (int64_t)cum * kSps;
                        const double fk = bf0 + patternCarrier(bp, k) * kCarrierStep;
                        for (Ev& e : ev) if (std::llabs(e.t - tk) <= 60 && std::fabs(e.f - fk) < 200) e.used = true;
                    }
                    continue;
                }
            }
            if (incomplete && !final && a.t > n - 3 * span) continue;    // the frame may still be arriving
            a.used = true;
        }
        ev.erase(std::remove_if(ev.begin(), ev.end(), [&](const Ev& e) { return e.used && e.t < n - 3 * span; }), ev.end());
        ev.erase(std::remove_if(ev.begin(), ev.end(), [&](const Ev& e) { return e.t < base + 22 * kSps; }), ev.end());
    }
};

Receiver::Receiver() : p_(std::make_unique<Impl>()) {}
Receiver::~Receiver() = default;
void Receiver::setCallback(Cb cb) { p_->cb = std::move(cb); }
void Receiver::reset() { Impl& m = *p_; m.tried.clear(); m.h.clear(); m.base = m.n = 0; m.next = kTmplLen; m.cand.clear(); m.ev.clear(); m.lastSearch = 0; }
uint64_t Receiver::pilots() const { return p_->nPilots; }

void Receiver::feed(const cf32* x, size_t cnt) {
    Impl& m = *p_;
    for (size_t i = 0; i < cnt; i++) {
        const float re = x[i].real(), im = x[i].imag();
        m.h.push_back(std::isfinite(re) && std::isfinite(im) ? x[i] : cf32(0, 0));
        m.n++;
        if (m.n >= m.next) { m.detect(m.next - kTmplLen); m.next += kSps / 2; }
    }
    if (m.n - m.lastSearch > (int64_t)(kRate / 2)) { m.search(false); m.lastSearch = m.n; }
    // keep 20 s: enough for a core frame and an extension of about 30 bytes behind it
    const int64_t keep = (int64_t)(20 * kRate);
    if (m.n - m.base > keep + (int64_t)kRate) { const int64_t cut = m.n - keep - m.base; m.h.erase(m.h.begin(), m.h.begin() + (ptrdiff_t)cut); m.base += cut; }
}

void Receiver::flush() {
    Impl& m = *p_;
    for (auto& c : m.cand) { m.ev.push_back(c); m.nPilots++; }
    m.cand.clear();
    m.search(true);
}

} // namespace dect2::xlink::mioty
