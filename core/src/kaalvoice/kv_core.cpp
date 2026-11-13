// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL voice codec 4: profiles, bit layout, LSF quantiser and the 8 kHz analysis-by-synthesis core (see docs/KAAL-CODEC.md).
#include "kv_core.h"
#include <cmath>
#include <cstring>

namespace dect2 {
namespace kaalvoice {
namespace detail {

const int kHbEdge[kHbBands + 1] = {36, 41, 47, 54, 61, 69, 80};

// ---------------------------------------------------------------- profiles
void lsfAllocKlt(int total, int table, int* bits);

namespace {
const Tier kTiers[] = {
    // minBits20  L  lsf res lag0 lagD gp gc   high band {on, gain, shape, time}       postfilter num/den/ltp
    {60,  80, 20, 1, 7, 3, 3, 3, {true, 4, 0, 2}, 0.55, 0.72, 0.50},
    {80,  80, 22, 1, 7, 4, 3, 4, {true, 4, 0, 2}, 0.55, 0.72, 0.50},
    {110, 80, 24, 2, 8, 4, 3, 4, {true, 5, 1, 2}, 0.55, 0.72, 0.50},
    {140, 40, 26, 2, 8, 4, 3, 4, {true, 5, 1, 2}, 0.60, 0.75, 0.40},
    {200, 40, 28, 2, 8, 5, 4, 5, {true, 5, 1, 2}, 0.65, 0.75, 0.30},
    {280, 40, 32, 2, 8, 5, 4, 5, {true, 5, 3, 2}, 0.70, 0.75, 0.25},
};
constexpr int kNTiers = sizeof(kTiers) / sizeof(kTiers[0]);

int hbBits(const HbProf& h) {
    if (!h.on) return 0;
    static const int shape[4] = {3, 5, 2 * kHbBands, 3 * kHbBands};
    return h.gainBits + shape[h.shapeMode & 3] + 4 * h.timeBits;
}


Field fieldDef(const Prof& P, FK k, int i) {
    Field f{k, 0, 0, 0};
    auto split = [&](int bits, int a, int b) {
        f.bits = (uint8_t)bits;
        f.a = (uint8_t)(a < bits ? a : bits);
        const int rest = bits - f.a;
        f.b = (uint8_t)(b < rest ? b : rest);
    };
    switch (k) {
    case FK::Lsf: split(P.lsfBits[i], i < 3 ? 2 : (i < 6 ? 1 : 0), 99); break;   // KLT order: first components carry most
    case FK::Interp: split(P.interpBits, 99, 0); break;
    case FK::Lag0: split(P.lag0Bits, 5, 99); break;
    case FK::LagD: split(P.lagDBits, 1, 99); break;
    case FK::Gp: split(P.gpBits, 2, 99); break;
    case FK::Gc: split(P.gcBits, 3, 99); break;
    case FK::Fcb: split(P.fcbBits[i], 0, 0); break;
    case FK::HbG: split(P.hb.gainBits, 3, 99); break;
    case FK::HbS: {
        const int b = P.hb.shapeMode == 0 ? 3 : P.hb.shapeMode == 1 ? (i == 0 ? 3 : 2) : P.hb.shapeMode == 2 ? 2 : 3;
        split(b, 0, 99);
        break;
    }
    case FK::HbT: split(P.hb.timeBits, 0, 0); break;
    }
    return f;
}
}  // namespace

const Tier* defaultTiers(int* n) {
    if (n) *n = kNTiers;
    return kTiers;
}

bool makeProf(const Config& c, Prof& P, std::string* why, const Tier* ov) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    if (c.sampleRate != 8000 && c.sampleRate != 16000) return fail("sample rate must be 8000 or 16000");
    if (c.frameMs != 20 && c.frameMs != 40) return fail("frame length must be 20 or 40 ms");
    if ((int)c.profile < 0 || c.profile >= Profile::Count) return fail("unknown profile");
    const int Bc = c.payloadBits > 0 ? c.payloadBits : profileBitsPer20ms(c.profile) * c.frameMs / 20;
    const int B20 = Bc * 20 / c.frameMs;
    if (B20 < 64 || B20 > 400) return fail("payload bits per 20 ms must be in [64, 400]");
    if (B20 < 72 && c.frameMs != 40) return fail("below 72 bits per 20 ms the frame must be 40 ms");
    const int B = Bc - (c.inBandType ? 1 : 0);   // the type bit
    const Tier* t = &kTiers[0];
    for (int i = 0; i < kNTiers; i++)
        if (B20 >= kTiers[i].minBits20) t = &kTiers[i];
    if (ov) t = ov;
    P = Prof();
    P.fs = c.sampleRate;
    P.frameMs = c.frameMs;
    P.N8 = 8 * c.frameMs;
    P.L = t->L;
    P.S = P.N8 / P.L;
    P.halves = c.frameMs / 20;
    P.lsfTotal = t->lsfTotal;
    lsfAllocKlt(P.lsfTotal, c.frameMs == 40 ? 1 : 0, P.lsfBits);
    P.interpBits = c.frameMs == 40 ? 2 : 0;
    P.lagRes = t->lagRes;
    P.lag0Bits = t->lagRes == 2 ? 8 : 7;
    P.lagDBits = t->lagDBits;
    P.gpBits = t->gpBits;
    P.gcBits = t->gcBits;
    P.hb = t->hb;
    P.hb.on = c.sampleRate == 16000 && t->hb.on;
    P.pfNum = t->pfNum;
    P.pfDen = t->pfDen;
    P.pfLtp = t->pfLtp;
    P.lsfRho = c.frameMs == 40 ? 0.45 : 0.6;
    const int sh = P.S / P.halves;
    const int side = P.lsfTotal + P.interpBits + P.halves * (P.lag0Bits + (sh - 1) * P.lagDBits) + P.S * (P.gpBits + P.gcBits) +
                     P.halves * hbBits(P.hb);
    int rem = B - side;
    if (rem < P.S * 6) return fail("payload too small for this tier");
    int used = 0;
    for (int s = 0; s < P.S; s++) {
        const int share = (rem - used) / (P.S - s);
        P.fcbK[s] = pvqMaxK(P.L, share);
        P.fcbBits[s] = pvqBits(P.L, P.fcbK[s]);
        used += P.fcbBits[s];
    }
    for (bool grew = true; grew;) {   // spend what is left on more pulses, first subframes first
        grew = false;
        for (int s = 0; s < P.S; s++) {
            const int nb = pvqBits(P.L, P.fcbK[s] + 1);
            if (nb - P.fcbBits[s] <= rem - used) {
                used += nb - P.fcbBits[s];
                P.fcbK[s]++;
                P.fcbBits[s] = nb;
                grew = true;
            }
        }
    }
    P.totalBits = B;
    P.padBits = rem - used;
    FrameQ dummy;
    walkFrame(P, dummy, [&](FK k, int i, uint64_t&) { P.fields.push_back(fieldDef(P, k, i)); });
    int sum = 0;
    for (const Field& f : P.fields) {
        P.classBits[0] += f.a;
        P.classBits[1] += f.b;
        P.classBits[2] += f.bits - f.a - f.b;
        sum += f.bits;
    }
    if (sum + P.padBits != B) return fail("internal: layout does not add up");
    return true;
}

// ---------------------------------------------------------------- packing
void packFrame(const Prof& P, FrameQ& q, uint8_t* out) {
    std::vector<uint64_t> v;
    v.reserve(P.fields.size());
    walkFrame(P, q, [&](FK, int, uint64_t& x) { v.push_back(x); });
    const int nbytes = (P.totalBits + 7) / 8;
    std::memset(out, 0, (size_t)nbytes);
    int pos = 0;
    auto put = [&](uint64_t val, int nb) {
        for (int i = nb - 1; i >= 0; i--, pos++)
            if ((val >> i) & 1) out[pos >> 3] |= (uint8_t)(0x80 >> (pos & 7));
    };
    for (int cls = 0; cls < 3; cls++)
        for (size_t i = 0; i < P.fields.size(); i++) {
            const Field& f = P.fields[i];
            const int c = f.bits - f.a - f.b;
            if (cls == 0 && f.a) put(v[i] >> (f.bits - f.a), f.a);
            if (cls == 1 && f.b) put((v[i] >> c) & ((1ull << f.b) - 1), f.b);
            if (cls == 2 && c) put(v[i] & (c >= 64 ? ~0ull : ((1ull << c) - 1)), c);
        }
}

void unpackFrame(const Prof& P, const uint8_t* in, uint32_t bad, FrameQ& q, bool* fcbBad, bool* hbTimeBad) {
    std::vector<uint64_t> v(P.fields.size(), 0);
    std::vector<uint8_t> lost(P.fields.size(), 0);   // bit 0: some lower part lost
    int pos = 0;
    auto get = [&](int nb) {
        uint64_t x = 0;
        for (int i = 0; i < nb; i++, pos++) x = (x << 1) | ((in[pos >> 3] >> (7 - (pos & 7))) & 1);
        return x;
    };
    for (int cls = 0; cls < 3; cls++)
        for (size_t i = 0; i < P.fields.size(); i++) {
            const Field& f = P.fields[i];
            const int c = f.bits - f.a - f.b;
            const bool isBad = (bad >> cls) & 1;
            if (cls == 0 && f.a) v[i] |= get(f.a) << (f.bits - f.a);
            if (cls == 1 && f.b) {
                const uint64_t x = get(f.b);
                if (isBad) lost[i] |= 2;   // B part lost
                else v[i] |= x << c;
            }
            if (cls == 2 && c) {
                const uint64_t x = get(c);
                if (isBad) lost[i] |= 4;
                else v[i] |= x;
            }
        }
    for (int s = 0; s < kMaxSub; s++) fcbBad[s] = false;
    *hbTimeBad = false;
    size_t idx = 0;
    walkFrame(P, q, [&](FK k, int i, uint64_t& x) {
        const Field& f = P.fields[idx];
        uint64_t val = v[idx];
        const uint8_t l = lost[idx];
        idx++;
        if (l) {
            const int c = f.bits - f.a - f.b;
            const int lowBits = (l & 2) ? f.b + c : c;   // everything below the surviving part
            if (k == FK::Fcb) { fcbBad[i] = true; val = 0; }
            else if (k == FK::HbT) { *hbTimeBad = true; val = 0; }
            else if (k == FK::LagD && f.a == 0) val = 1ull << (f.bits - 1);   // delta 0
            else if (lowBits > 0) val = (val & ~((1ull << lowBits) - 1)) | (1ull << (lowBits - 1));   // middle of the interval
        }
        x = val;
    });
}

// ---------------------------------------------------------------- LSF quantiser
// Predictive KLT scalar quantiser: the residual r = lsf - (mean + rho (prev - mean)) is rotated onto the eigenvectors of its covariance
// and each coefficient, divided by its standard deviation, is quantised with a Gaussian Lloyd-Max quantiser of b_k bits (greedy bit
// allocation). The encoder then refines the indices by coordinate descent on a spectrally weighted LSF error. Mean, eigenvectors and
// deviations were measured on synthetic training speech (12 seeds x 4 voices x 6 s, tools/kaalvoice_bench; the repo's real speech
// sample and other seeds are the held-out check) for rho = 0.6 (20 ms frames) and rho = 0.45 (40 ms frames).
namespace {
const double kLsfMean[kP] = {0.2473, 0.4096, 0.5728, 0.8734, 1.2392, 1.4805, 1.8386, 2.1137, 2.4404, 2.6917};
const double kKltSd[2][kP] = {{0.27574, 0.14605, 0.10672, 0.07917, 0.06279, 0.05711, 0.04950, 0.04549, 0.04127, 0.02663}, {0.31067, 0.16616, 0.12257, 0.08749, 0.07090, 0.06288, 0.05322, 0.04787, 0.04165, 0.02683}};
const double kKlt[2][kP][kP] = {{
    {0.13390, 0.20172, 0.37818, 0.40390, 0.47937, 0.46986, 0.31011, 0.25124, 0.14788, 0.05915},
    {0.22644, 0.43461, 0.54672, 0.33005, -0.23279, -0.42421, -0.25199, -0.18742, -0.12130, -0.06716},
    {-0.03511, -0.02648, -0.15772, 0.11567, 0.59045, 0.10426, -0.37485, -0.50688, -0.31326, -0.32364},
    {-0.35852, -0.44733, -0.10506, 0.73589, -0.07428, -0.22510, 0.06039, -0.11625, 0.16801, 0.13119},
    {0.03292, -0.08796, -0.07847, 0.01289, 0.39348, -0.54484, -0.08912, 0.67518, -0.16766, -0.20312},
    {-0.06258, 0.10570, -0.04854, -0.01843, -0.00683, -0.23159, 0.81337, -0.24152, -0.32007, -0.32586},
    {0.34169, 0.22348, -0.33858, -0.01015, 0.31844, -0.32915, 0.13343, -0.22990, 0.14537, 0.64578},
    {0.22502, 0.29025, -0.41738, 0.16942, -0.09248, -0.01792, -0.01943, 0.00344, 0.60903, -0.53106},
    {-0.44017, -0.01498, 0.40020, -0.36828, 0.31032, -0.27234, 0.04059, -0.20338, 0.54221, -0.05256},
    {0.65946, -0.64521, 0.25425, -0.08988, 0.02705, -0.06647, 0.08266, -0.15127, 0.14305, -0.14412},
}, {
    {0.12877, 0.18323, 0.34726, 0.38635, 0.49883, 0.48918, 0.31676, 0.25743, 0.14878, 0.05592},
    {0.22038, 0.42763, 0.56471, 0.37327, -0.23995, -0.40958, -0.22884, -0.14725, -0.09523, -0.04349},
    {-0.03577, -0.01474, -0.14561, 0.14922, 0.57087, 0.08033, -0.39340, -0.52947, -0.30876, -0.30373},
    {-0.37387, -0.41888, -0.12402, 0.71212, -0.12531, -0.15494, 0.12750, -0.19323, 0.21608, 0.14921},
    {0.00057, -0.16111, -0.10200, 0.10815, 0.39064, -0.55636, -0.13115, 0.65841, -0.12899, -0.14990},
    {-0.06539, 0.13691, -0.04850, -0.03803, 0.03181, -0.26938, 0.79318, -0.20015, -0.33368, -0.34507},
    {0.25604, 0.06619, -0.14073, -0.06019, 0.28635, -0.26123, 0.13502, -0.20471, -0.16055, 0.81883},
    {0.27793, 0.36740, -0.43496, 0.04972, 0.11393, -0.20500, 0.03784, -0.12683, 0.69704, -0.19422},
    {0.48540, 0.06281, -0.48346, 0.39489, -0.32075, 0.25529, -0.02622, 0.18578, -0.40666, -0.04628},
    {-0.64118, 0.64797, -0.26161, 0.08277, -0.02307, 0.07040, -0.09798, 0.16822, -0.14754, 0.16904},
}};
constexpr double kLsfGap = 0.025;
constexpr int kMaxQBits = 6;
// Lloyd-Max distortion of a unit Gaussian for b bits
const double kLmDist[kMaxQBits + 1] = {1.0, 0.3634, 0.1175, 0.03454, 0.009497, 0.002499, 0.000642};

struct LloydMax {
    double lv[kMaxQBits + 1][64];
    LloydMax() {
        // Lloyd iteration on a fine grid of the Gaussian density (deterministic: only + - * / and kvExp)
        constexpr int G = 6001;
        static double x[G], p[G];
        for (int i = 0; i < G; i++) { x[i] = -6.0 + 12.0 * i / (G - 1); p[i] = kvExp(-0.5 * x[i] * x[i]); }
        for (int b = 1; b <= kMaxQBits; b++) {
            const int n = 1 << b;
            double* l = lv[b];
            for (int j = 0; j < n; j++) l[j] = -3.0 + 6.0 * (j + 0.5) / n;
            for (int it = 0; it < 200; it++) {
                double sw[64] = {}, sx[64] = {};
                int j = 0;
                for (int i = 0; i < G; i++) {
                    while (j < n - 1 && x[i] > 0.5 * (l[j] + l[j + 1])) j++;
                    sw[j] += p[i];
                    sx[j] += p[i] * x[i];
                }
                for (int k = 0; k < n; k++)
                    if (sw[k] > 0) l[k] = sx[k] / sw[k];
            }
        }
        lv[0][0] = 0.0;
    }
};
const LloydMax& lloyd() { static const LloydMax t; return t; }
int lmNearest(int b, double v) {
    if (b == 0) return 0;
    const double* l = lloyd().lv[b];
    const int n = 1 << b;
    int k = 0;
    while (k < n - 1 && v > 0.5 * (l[k] + l[k + 1])) k++;
    return k;
}
int tableOf(const Prof& P) { return P.halves == 2 ? 1 : 0; }

void reconstruct(const Prof& P, const int* idx, const double* prev, double* q) {
    const int t = tableOf(P);
    for (int i = 0; i < kP; i++) q[i] = kLsfMean[i] + P.lsfRho * (prev[i] - kLsfMean[i]);
    for (int k = 0; k < kP; k++) {
        const int b = P.lsfBits[k];
        if (b == 0) continue;
        const int n = 1 << b;
        const int j = idx[k] < 0 ? 0 : (idx[k] >= n ? n - 1 : idx[k]);
        const double c = lloyd().lv[b][j] * kKltSd[t][k];
        for (int i = 0; i < kP; i++) q[i] += c * kKlt[t][k][i];
    }
}
}  // namespace

const double* LsfQ::mean() { return kLsfMean; }

void lsfAllocKlt(int total, int table, int* bits) {
    double d[kP];
    for (int k = 0; k < kP; k++) { bits[k] = 0; d[k] = kKltSd[table][k] * kKltSd[table][k]; }
    for (int n = 0; n < total; n++) {
        int best = -1;
        double gain = -1;
        for (int k = 0; k < kP; k++) {
            if (bits[k] >= kMaxQBits) continue;
            const double v = kKltSd[table][k] * kKltSd[table][k];
            const double g = v * (kLmDist[bits[k]] - kLmDist[bits[k] + 1]);
            if (g > gain) { gain = g; best = k; }
        }
        if (best < 0) break;
        bits[best]++;
    }
    (void)d;
}

void LsfQ::quantise(const Prof& P, const double* lsf, const double* prev, int* idx, double* q) {
    const int t = tableOf(P);
    double r[kP];
    for (int i = 0; i < kP; i++) r[i] = lsf[i] - (kLsfMean[i] + P.lsfRho * (prev[i] - kLsfMean[i]));
    for (int k = 0; k < kP; k++) {
        double c = 0;
        for (int i = 0; i < kP; i++) c += r[i] * kKlt[t][k][i];
        idx[k] = lmNearest(P.lsfBits[k], c / kKltSd[t][k]);
    }
    // weights: close LSF pairs (formant peaks) count more
    double w[kP];
    for (int i = 0; i < kP; i++) {
        const double lo = i == 0 ? lsf[0] : lsf[i] - lsf[i - 1];
        const double hi = i == kP - 1 ? kPi - lsf[i] : lsf[i + 1] - lsf[i];
        w[i] = 1.0 / (lo > 0.01 ? lo : 0.01) + 1.0 / (hi > 0.01 ? hi : 0.01);
    }
    auto err = [&](const int* ix) {
        double qq[kP], e = 0;
        reconstruct(P, ix, prev, qq);
        for (int i = 0; i < kP; i++) e += w[i] * (qq[i] - lsf[i]) * (qq[i] - lsf[i]);
        return e;
    };
    double e = err(idx);
    for (int pass = 0; pass < 2; pass++) {
        bool moved = false;
        for (int k = 0; k < kP; k++) {
            const int n = 1 << P.lsfBits[k];
            if (P.lsfBits[k] == 0) continue;
            for (int dlt = -1; dlt <= 1; dlt += 2) {
                const int old = idx[k];
                if (old + dlt < 0 || old + dlt >= n) continue;
                idx[k] = old + dlt;
                const double e2 = err(idx);
                if (e2 < e) { e = e2; moved = true; }
                else idx[k] = old;
            }
        }
        if (!moved) break;
    }
    dequantise(P, idx, prev, q);
}

void LsfQ::dequantise(const Prof& P, const int* idx, const double* prev, double* q) {
    reconstruct(P, idx, prev, q);
    lsfStabilise(q, kLsfGap);
}

// ---------------------------------------------------------------- shared core tables
namespace {
struct Tables {
    float interp[kUp][2 * kNI];   // fractional delay filters, phase f/4 (f = 1..3 used; 0 unused)
    float lpcWin[280];
    Tables() {
        for (int f = 1; f < kUp; f++) {
            const double q = 1.0 - (double)f / kUp;
            double s = 0, t[2 * kNI];
            for (int j = 0; j < 2 * kNI; j++) {
                const int tt = j - kNI + 1;
                const double u = q - tt;
                const double sinc = std::fabs(u) < 1e-12 ? 1.0 : kvSin(kPi * u) / (kPi * u);
                const double w = 0.5 * (1.0 + kvCos(kPi * u / (kNI + 1)));
                t[j] = sinc * w;
                s += t[j];
            }
            for (int j = 0; j < 2 * kNI; j++) interp[f][j] = (float)(t[j] / s);
        }
        for (int j = 0; j < 2 * kNI; j++) interp[0][j] = j == kNI - 1 ? 1.0f : 0.0f;
        // asymmetric LPC window: Hamming rise over 240 samples, quarter-cosine fall over the 40-sample look-ahead
        const int W = 280, W1 = 240;
        for (int n = 0; n < W1; n++) lpcWin[n] = (float)(0.54 - 0.46 * kvCos(kPi * n / (W1 - 1)));
        for (int n = W1; n < W; n++) lpcWin[n] = (float)kvCos(0.5 * kPi * (n - W1 + 1) / (W - W1 + 1));
    }
};
const Tables& tables() { static const Tables t; return t; }

// Pitch-gain levels and the energy-correction levels of the fixed codebook gain (dB).
double gpLevel(int bits, int i) {
    static const double g3[8] = {0.0, 0.22, 0.42, 0.58, 0.72, 0.84, 0.96, 1.10};
    if (bits == 3) return g3[i & 7];
    const int n = 1 << bits;
    return 1.2 * i / (n - 1);
}
double gcLevel(int bits, int j) {
    const int n = 1 << bits;
    const double step = bits >= 5 ? 1.6 : bits == 4 ? 2.8 : 5.0;
    return (j - (n - 1) * 0.5) * step;
}
constexpr double kGcMean = 33.0;
constexpr double kGpTame = 1.0;                          // mean innovation energy (dB per sample) the MA prediction centres on
const double kGcPred[4] = {0.68, 0.58, 0.34, 0.19};       // MA prediction of the energy correction

int lagMinUnits(int res) { return kPitMin * res; }
int lagMaxUnits(int res) { return res == 2 ? 2 * kPitMax + 1 : kPitMax; }

// Writes the adaptive-codebook vector for lag `units` into exc[pos .. pos + L) (past excitation repeated for short lags).
void predLt(float* exc, int pos, int units, int res, int L) {
    int Ti, f;
    if (res == 2) { Ti = units >> 1; f = (units & 1) ? 2 : 0; }
    else { Ti = units; f = 0; }
    if (f == 0) {
        for (int n = 0; n < L; n++) exc[pos + n] = exc[pos + n - Ti];
        return;
    }
    const float* t = tables().interp[f];
    for (int n = 0; n < L; n++) {
        const float* e = exc + pos + n - Ti - 1 - kNI + 1;
        float s = 0.0f;
        for (int j = 0; j < 2 * kNI; j++) s += t[j] * e[j];
        exc[pos + n] = s;
    }
}

void conv(const float* h, const float* x, float* y, int L) {
    for (int n = 0; n < L; n++) {
        float s = 0.0f;
        for (int k = 0; k <= n; k++) s += h[k] * x[n - k];
        y[n] = s;
    }
}

double dot(const float* a, const float* b, int n) {
    double s = 0.0;
    for (int i = 0; i < n; i++) s += (double)a[i] * (double)b[i];
    return s;
}

void interpLsf(const double* a, const double* b, double w, double* out) {
    for (int i = 0; i < kP; i++) out[i] = (1.0 - w) * a[i] + w * b[i];
}

// Per-subframe interpolation: 20 ms halves interpolate from the previous to the next anchor (prev -> mid -> cur for 40 ms).
void subLsf(const Prof& P, const double* prev, const double* mid, const double* cur, int s, double* out) {
    if (P.halves == 1) { interpLsf(prev, cur, (double)(s + 1) / P.S, out); return; }
    const int sh = P.S / 2;
    if (s < sh) interpLsf(prev, mid, (double)(s + 1) / sh, out);
    else interpLsf(mid, cur, (double)(s - sh + 1) / sh, out);
}
const double kInterpAlpha[4] = {0.25, 0.45, 0.62, 0.80};

double sharpOf(double gpPrev) { return gpPrev < 0.0 ? 0.0 : (gpPrev > 0.8 ? 0.8 : gpPrev); }

// Fixed-codebook energy (dB per sample) of an integer pulse vector.
double pulseDb(const int* y, int L) {
    long s = 0;
    for (int i = 0; i < L; i++) s += (long)y[i] * y[i];
    return kvDb((double)s / L);
}

int lagDecode(const Prof& P, int field, bool absolute, int prevUnits) {
    const int lo = lagMinUnits(P.lagRes), hi = lagMaxUnits(P.lagRes);
    int u;
    if (absolute) u = lo + field;
    else u = prevUnits + field - (1 << (P.lagDBits - 1));
    return u < lo ? lo : (u > hi ? hi : u);
}
}  // namespace

void CoreState::reset() {
    for (int i = 0; i < kP; i++) lsfPrev[i] = kLsfMean[i];
    std::memset(exc, 0, sizeof exc);
    std::memset(syn, 0, sizeof syn);
    for (double& g : gcHist) g = 0.0;
    gpPrev = 0.0;
    lagPrev = lagMinUnits(2) * 2;
}

// ---------------------------------------------------------------- encoder
CoreEncoder::CoreEncoder(const Prof& P) : P_(P) { reset(); }

void CoreEncoder::reset() {
    st_.reset();
    st_.lagPrev = lagMinUnits(P_.lagRes) * 2;
    for (int i = 0; i < kP; i++) lsfUnqPrev_[i] = kLsfMean[i];
    std::memset(sp_, 0, sizeof sp_);
    std::memset(wsp_, 0, sizeof wsp_);
    std::memset(wFirMem_, 0, sizeof wFirMem_);
    std::memset(wIirMem_, 0, sizeof wIirMem_);
    std::memset(errIir_, 0, sizeof errIir_);
    voicing_ = 0;
    resDb_ = 0;
    olPrev_ = 60;
}

void CoreEncoder::restart(const double* lsf) {
    std::memset(st_.exc, 0, sizeof st_.exc);
    std::memset(st_.syn, 0, sizeof st_.syn);
    std::memset(errIir_, 0, sizeof errIir_);
    for (double& g : st_.gcHist) g = 0.0;
    st_.gpPrev = 0.0;
    for (int i = 0; i < kP; i++) st_.lsfPrev[i] = lsf[i];
}

namespace {
// Weighted-synthesis cascade 1/Aq -> A(z/g1) -> 1/A(z/g2). syn: past decoded samples, iir: past weighted outputs.
void cascade(const float* aq, const float* a1, const float* a2, const float* u, float* out, int L, float* syn, float* iir) {
    for (int n = 0; n < L; n++) {
        float s = u[n];
        for (int k = 1; k <= kP; k++) s -= aq[k] * syn[k - 1];
        float t = s;
        for (int k = 1; k <= kP; k++) t += a1[k] * syn[k - 1];
        for (int k = kP - 1; k > 0; k--) syn[k] = syn[k - 1];
        syn[0] = s;
        float w = t;
        for (int k = 1; k <= kP; k++) w -= a2[k] * iir[k - 1];
        for (int k = kP - 1; k > 0; k--) iir[k] = iir[k - 1];
        iir[0] = w;
        out[n] = w;
    }
}
constexpr double kG1 = 0.94, kG2 = 0.60;
}  // namespace

int CoreEncoder::openLoop(const float* w, int n) {
    // normalised correlation, three lag sections favouring the shortest lag that is nearly as good (no pitch doubling)
    double best[3] = {-1e30, -1e30, -1e30};
    int bl[3] = {kPitMin, 40, 80};
    const double e0 = dot(w, w, n) + 1.0;
    for (int T = kPitMin; T <= kPitMax; T++) {
        const double c = dot(w, w - T, n), e = dot(w - T, w - T, n) + 1.0;
        double v = c / std::sqrt(e);
        if (std::abs(T - olPrev_) <= 3) v *= 1.04;   // continuity
        const int sec = T < 40 ? 0 : (T < 80 ? 1 : 2);
        if (v > best[sec]) { best[sec] = v; bl[sec] = T; }
    }
    int T = bl[2];
    double v = best[2];
    if (best[1] >= 0.85 * v) { T = bl[1]; v = best[1]; }
    if (best[0] >= 0.85 * v) { T = bl[0]; v = best[0]; }
    const double c = dot(w, w - T, n), e = dot(w - T, w - T, n) + 1.0;
    const double r = c / std::sqrt(e * e0);
    olVoicing_ = r < 0 ? 0 : r;
    olPrev_ = T;
    return T;
}

void CoreEncoder::encode(const float* in8, FrameQ& q) {
    const Prof& P = P_;
    const int N = P.N8, L = P.L;
    // input buffer: [past kPast | frame N | look-ahead kLA]
    const int len = kPast + N + kLA;
    std::memmove(sp_, sp_ + N, sizeof(float) * (size_t)(len - N));
    std::memcpy(sp_ + len - N, in8, sizeof(float) * (size_t)N);
    const float* frame = sp_ + kPast;

    // LPC analysis at the end of the frame (and at the middle for 40 ms frames)
    double lsfU[kP], lsfMid[kP];
    auto analyse = [&](int start, double* lsf, float* aOut) {
        double r[kP + 1];
        autocorr(sp_ + start, tables().lpcWin, 280, kP, r);
        lagWindow(r, kP);
        float a[kP + 1];
        bool ok = levinson(r, kP, a) && lpcToLsf(a, lsf);
        if (!ok) for (int i = 0; i < kP; i++) lsf[i] = lsfUnqPrev_[i];
        lsfStabilise(lsf, 0.012);
        if (aOut) lsfToLpc(lsf, aOut);
    };
    float aEnd[kP + 1];
    analyse(len - 280, lsfU, aEnd);
    if (P.halves == 2) analyse(0, lsfMid, nullptr);
    {   // LP residual power of the frame (SID level)
        float mem[kP], tmp[kMaxN8];
        for (int i = 0; i < kP; i++) mem[i] = frame[-1 - i];
        firA(aEnd, frame, tmp, N, mem);
        resDb_ = kvDb(dot(tmp, tmp, N) / N);
    }

    // LSF quantisation
    double lsfQ[kP], midQ[kP] = {}, midU[kP] = {};
    LsfQ::quantise(P, lsfU, st_.lsfPrev, q.lsf, lsfQ);
    if (P.halves == 2) {
        int bi = 0;
        double be = 1e30;
        for (int i = 0; i < 4; i++) {
            double m[kP], e = 0;
            interpLsf(st_.lsfPrev, lsfQ, kInterpAlpha[i], m);
            for (int k = 0; k < kP; k++) e += (m[k] - lsfMid[k]) * (m[k] - lsfMid[k]);
            if (e < be) { be = e; bi = i; }
        }
        q.interp = bi;
        interpLsf(st_.lsfPrev, lsfQ, kInterpAlpha[bi], midQ);
        for (int k = 0; k < kP; k++) midU[k] = lsfMid[k];
    }

    // per-subframe filters
    float aq[kMaxSub][kP + 1], a1[kMaxSub][kP + 1], a2[kMaxSub][kP + 1];
    for (int s = 0; s < P.S; s++) {
        double l[kP], lu[kP];
        subLsf(P, st_.lsfPrev, midQ, lsfQ, s, l);
        subLsf(P, lsfUnqPrev_, midU, lsfU, s, lu);
        lsfToLpc(l, aq[s]);
        float au[kP + 1];
        lsfToLpc(lu, au);
        weightLpc(au, kG1, a1[s]);
        weightLpc(au, kG2, a2[s]);
    }

    // weighted speech of the frame, open-loop pitch per 10 ms segment
    constexpr int H = kPitMax + 1;
    float* wcur = wsp_ + H;
    for (int s = 0; s < P.S; s++) {
        float t[80];
        firA(a1[s], frame + s * L, t, L, wFirMem_);
        iirA(a2[s], t, wcur + s * L, L, wIirMem_);
    }
    int ol[kMaxN8 / 80];
    double vsum = 0;
    for (int g = 0; g < N / 80; g++) {
        ol[g] = openLoop(wcur + g * 80, 80);
        vsum += olVoicing_;
    }
    voicing_ = vsum / (N / 80);

    // subframes
    const int lo = lagMinUnits(P.lagRes), hi = lagMaxUnits(P.lagRes);
    const int sh = P.S / P.halves;
    float* exc = st_.exc;
    for (int s = 0; s < P.S; s++) {
        const int pos = kExcHist + s * L;
        // target and impulse response
        float zir[80], x[80], h[80], zero[80] = {}, imp[80] = {};
        {
            float sy[kP], ii[kP];
            std::memcpy(sy, st_.syn, sizeof sy);
            std::memcpy(ii, errIir_, sizeof ii);
            cascade(aq[s], a1[s], a2[s], zero, zir, L, sy, ii);
            float sy0[kP] = {}, ii0[kP] = {};
            imp[0] = 1.0f;
            cascade(aq[s], a1[s], a2[s], imp, h, L, sy0, ii0);
        }
        for (int n = 0; n < L; n++) x[n] = wcur[s * L + n] - zir[n];

        // adaptive codebook (closed loop)
        const bool absolute = (s % sh) == 0;
        int cand[64], nc = 0;
        if (absolute) {
            const int T0 = ol[(s * L) / 80];
            for (int T = T0 - 4; T <= T0 + 4; T++) {
                const int u = T * P.lagRes;
                if (u >= lo && u <= hi) cand[nc++] = u;
            }
        } else {
            const int half = 1 << (P.lagDBits - 1);
            for (int u = st_.lagPrev - half; u <= st_.lagPrev + half - 1; u++)
                if (u >= lo && u <= hi && (P.lagRes == 1 || (u & 1) == 0)) cand[nc++] = u;
        }
        if (nc == 0) cand[nc++] = absolute ? lo : (st_.lagPrev < lo ? lo : (st_.lagPrev > hi ? hi : st_.lagPrev));
        float tmp[kExcHist + 80], y[80];
        std::memcpy(tmp, exc + pos - kExcHist, sizeof(float) * kExcHist);
        auto score = [&](int u, double* xyO, double* yyO) {
            predLt(tmp, kExcHist, u, P.lagRes, L);
            conv(h, tmp + kExcHist, y, L);
            const double xy = dot(x, y, L), yy = dot(y, y, L) + 1e-3;
            if (xyO) { *xyO = xy; *yyO = yy; }
            return xy / std::sqrt(yy);
        };
        int bu = cand[0];
        double bs = -1e30;
        for (int i = 0; i < nc; i++) {
            const double v = score(cand[i], nullptr, nullptr);
            if (v > bs) { bs = v; bu = cand[i]; }
        }
        if (P.lagRes == 2) {   // half-sample neighbours
            const int base = bu;
            for (int d = -1; d <= 1; d += 2) {
                const int u = base + d;
                if (u < lo || u > hi) continue;
                if (!absolute) {
                    const int half = 1 << (P.lagDBits - 1);
                    if (u < st_.lagPrev - half || u > st_.lagPrev + half - 1) continue;
                }
                const double v = score(u, nullptr, nullptr);
                if (v > bs) { bs = v; bu = u; }
            }
        }
        SubQ& sq = q.sub[s];
        sq.lag = absolute ? bu - lo : bu - st_.lagPrev + (1 << (P.lagDBits - 1));
        predLt(exc, pos, bu, P.lagRes, L);
        const float* v = exc + pos;
        conv(h, v, y, L);
        const double xy = dot(x, y, L), yy = dot(y, y, L) + 1e-3;
        double gpu = xy / yy;
        gpu = gpu < 0.0 ? 0.0 : (gpu > 1.2 ? 1.2 : gpu);

        // fixed codebook: pyramid VQ pulses through the pitch-sharpened weighted synthesis filter
        const int Ti = P.lagRes == 2 ? bu >> 1 : bu;
        const double beta = sharpOf(st_.gpPrev);
        float h2[80];
        std::memcpy(h2, h, sizeof(float) * (size_t)L);
        if (Ti < L)
            for (int n = Ti; n < L; n++) h2[n] += (float)beta * h2[n - Ti];
        float x2[80];
        for (int n = 0; n < L; n++) x2[n] = x[n] - (float)gpu * y[n];
        double d[80];
        for (int i = 0; i < L; i++) {
            double acc = 0;
            for (int n = i; n < L; n++) acc += (double)x2[n] * h2[n - i];
            d[i] = acc;
        }
        for (int dl = 0; dl < L; dl++) {   // Phi(i, i + dl) by the downward recursion
            double acc = 0;
            for (int i = L - 1 - dl; i >= 0; i--) {
                acc += (double)h2[L - 1 - i - dl] * h2[L - 1 - i];
                phi_[(size_t)i * L + i + dl] = acc;
                phi_[(size_t)(i + dl) * L + i] = acc;
            }
        }
        int pulses[80];
        pvqSearch(d, phi_, L, P.fcbK[s], pulses);
        sq.fcb = P.fcbK[s] > 0 ? pvqIndex(pulses, L, P.fcbK[s]) : 0;
        float c[80], z[80];
        for (int n = 0; n < L; n++) c[n] = (float)pulses[n];
        if (Ti < L)
            for (int n = Ti; n < L; n++) c[n] += (float)beta * c[n - Ti];
        conv(h, c, z, L);

        // gains: joint search of the pitch gain and the predicted fixed-codebook gain correction
        const double ec = P.fcbK[s] > 0 ? pulseDb(pulses, L) : 0.0;
        double pred = kGcMean;
        for (int i = 0; i < 4; i++) pred += kGcPred[i] * st_.gcHist[i];
        const double base = P.fcbK[s] > 0 ? kvPow10((pred - ec) / 20.0) : 0.0;
        const double xz = dot(x, z, L), zz = dot(z, z, L), yz = dot(y, z, L);
        int bgi = 0, bgj = 0;
        double be = 1e300;
        const int ngp = 1 << P.gpBits, ngc = 1 << P.gcBits;
        double gcv[32];
        for (int j = 0; j < ngc; j++) gcv[j] = base * kvPow10(gcLevel(P.gcBits, j) / 20.0);
        for (int i = 0; i < ngp; i++) {
            const double gp = gpLevel(P.gpBits, i);
            if (gp > kGpTame && st_.gpPrev > kGpTame) continue;   // never two pitch gains above 1 in a row (decoder stability)
            for (int j = 0; j < ngc; j++) {
                const double gc = gcv[j];
                const double e = gp * gp * yy - 2.0 * gp * xy + gc * gc * zz - 2.0 * gc * xz + 2.0 * gp * gc * yz;
                if (e < be) { be = e; bgi = i; bgj = j; }
            }
        }
        sq.gp = bgi;
        sq.gc = bgj;
        const double gp = gpLevel(P.gpBits, bgi), gc = gcv[bgj];
        for (int i = 3; i > 0; i--) st_.gcHist[i] = st_.gcHist[i - 1];
        st_.gcHist[0] = gcLevel(P.gcBits, bgj);
        st_.gpPrev = gp;
        st_.lagPrev = bu;
        float u[80];
        for (int n = 0; n < L; n++) u[n] = (float)gp * exc[pos + n] + (float)gc * c[n];
        for (int n = 0; n < L; n++) {   // the same guard as the decoder (keeps both sides identical)
            if (!(u[n] < 1e5f)) u[n] = 1e5f;
            if (!(u[n] > -1e5f)) u[n] = -1e5f;
        }
        std::memcpy(exc + pos, u, sizeof(float) * (size_t)L);
        float w[80];
        cascade(aq[s], a1[s], a2[s], u, w, L, st_.syn, errIir_);
    }
    std::memmove(exc, exc + N, sizeof(float) * kExcHist);
    std::memmove(wsp_, wsp_ + N, sizeof(float) * H);
    for (int i = 0; i < kP; i++) { st_.lsfPrev[i] = lsfQ[i]; lsfUnqPrev_[i] = lsfU[i]; lsfOut_[i] = lsfU[i]; }
}

// ---------------------------------------------------------------- decoder
CoreDecoder::CoreDecoder(const Prof& P) : P_(P) { reset(); }

void CoreDecoder::reset() {
    st_.reset();
    st_.lagPrev = lagMinUnits(P_.lagRes) * 2;
    rng_.s = 0x4b563444u;
    lost_ = 0;
    gpLast_ = 0;
    voicing_ = 0;
    std::memset(pfRes_, 0, sizeof pfRes_);
    std::memset(pfFir_, 0, sizeof pfFir_);
    std::memset(pfIir_, 0, sizeof pfIir_);
    pfTilt_ = 0;
    pfAgc_ = 1.0;
    mute_ = 1.0;
    std::memset(sub_, 0, sizeof sub_);
    std::memset(excOut_, 0, sizeof excOut_);
}

void CoreDecoder::restart(const double* lsf) {
    std::memset(st_.exc, 0, sizeof st_.exc);
    std::memset(st_.syn, 0, sizeof st_.syn);
    for (double& g : st_.gcHist) g = 0.0;
    st_.gpPrev = 0.0;
    for (int i = 0; i < kP; i++) st_.lsfPrev[i] = lsf[i];
    lost_ = 0;
    mute_ = 1.0;
}

void CoreDecoder::decode(const FrameQ* q, const bool* fcbBad, bool pf, float* out) {
    const Prof& P = P_;
    const int N = P.N8, L = P.L, sh = P.S / P.halves;
    double lsfQ[kP], midQ[kP] = {};
    const bool lost = q == nullptr;
    if (lost) {
        lost_++;
        for (int i = 0; i < kP; i++) lsfQ[i] = 0.85 * st_.lsfPrev[i] + 0.15 * kLsfMean[i];
        lsfStabilise(lsfQ, kLsfGap);
        if (P.halves == 2) interpLsf(st_.lsfPrev, lsfQ, 0.5, midQ);
    } else {
        LsfQ::dequantise(P, q->lsf, st_.lsfPrev, lsfQ);
        if (P.halves == 2) interpLsf(st_.lsfPrev, lsfQ, kInterpAlpha[q->interp & 3], midQ);
    }
    // attenuation of concealed frames: by lost time (20 ms units)
    const int lostMs = lost_ * P.frameMs;
    double att = 1.0;
    if (lost) att = lostMs <= 20 ? 0.98 : lostMs <= 40 ? 0.9 : lostMs <= 60 ? 0.75 : lostMs <= 100 ? 0.5 : 0.25;
    float* exc = st_.exc;
    double vs = 0;
    for (int s = 0; s < P.S; s++) {
        const int pos = kExcHist + s * L;
        double l[kP];
        subLsf(P, st_.lsfPrev, midQ, lsfQ, s, l);
        float aq[kP + 1];
        lsfToLpc(l, aq);
        int units;
        double gp, gc;
        float c[80];
        int pulses[80];
        const int K = P.fcbK[s];
        const bool randomPulses = lost || (fcbBad && fcbBad[s]);
        if (lost) units = st_.lagPrev;
        else units = lagDecode(P, q->sub[s].lag, (s % sh) == 0, st_.lagPrev);
        predLt(exc, pos, units, P.lagRes, L);
        if (randomPulses) {
            for (int n = 0; n < L; n++) pulses[n] = 0;
            for (int k = 0; k < (K > 0 ? K : 1); k++) {
                const int p = (int)((rng_.next() >> 8) % (uint32_t)L);
                pulses[p] += (rng_.next() & 0x10000) ? 1 : -1;
            }
        } else if (K > 0) pvqVector(q->sub[s].fcb, L, K, pulses);
        else for (int n = 0; n < L; n++) pulses[n] = 0;
        const int Ti = P.lagRes == 2 ? units >> 1 : units;
        const double beta = sharpOf(st_.gpPrev);
        for (int n = 0; n < L; n++) c[n] = (float)pulses[n];
        if (Ti < L)
            for (int n = Ti; n < L; n++) c[n] += (float)beta * c[n - Ti];
        double ec = 0;
        long sq = 0;
        for (int n = 0; n < L; n++) sq += (long)pulses[n] * pulses[n];
        ec = sq ? kvDb((double)sq / L) : 0.0;
        double pred = kGcMean;
        for (int i = 0; i < 4; i++) pred += kGcPred[i] * st_.gcHist[i];
        double corr;
        if (lost) {
            gp = gpLast_ * att;
            if (gp > 0.95) gp = 0.95;
            double m = 0;
            for (double g : st_.gcHist) m += g;
            m *= 0.25;
            corr = (m < 0 ? m : 0.0) - 2.0 * lost_;
        } else {
            gp = gpLevel(P.gpBits, q->sub[s].gp);
            corr = gcLevel(P.gcBits, q->sub[s].gc);
            if (gp > kGpTame && st_.gpPrev > kGpTame) gp = kGpTame;   // the encoder never sends this: a damaged frame
            if (lost_ > 0 && gp > 1.0) gp = 1.0;   // first good frame after a loss: the past excitation differs
        }
        gc = sq ? kvPow10((pred + corr - ec) / 20.0) : 0.0;
        if (lost) gc *= att;
        for (int i = 3; i > 0; i--) st_.gcHist[i] = st_.gcHist[i - 1];
        st_.gcHist[0] = corr;
        st_.gpPrev = gp;
        st_.lagPrev = units;
        gpLast_ = gp;
        vs += gp;
        float u[80], sy[80];
        for (int n = 0; n < L; n++) u[n] = (float)gp * exc[pos + n] + (float)gc * c[n];
        for (int n = 0; n < L; n++) {   // guards against damaged frames running the filters away
            if (!(u[n] < 1e5f)) u[n] = 1e5f;
            if (!(u[n] > -1e5f)) u[n] = -1e5f;
        }
        std::memcpy(exc + pos, u, sizeof(float) * (size_t)L);
        std::memcpy(excOut_ + s * L, u, sizeof(float) * (size_t)L);
        {
            SubInfo& si = sub_[s];
            std::memcpy(si.aq, aq, sizeof aq);
            si.Ti = Ti;
            si.lag = (double)units / P.lagRes;
            si.gp = gp;
            si.gc = gc;
            double e2 = 0;
            for (int n = 0; n < L; n++) e2 += (double)c[n] * c[n];
            si.ecb = e2 / L;
            si.rnd = randomPulses;
        }
        iirA(aq, u, sy, L, st_.syn);
        for (int k = 0; k < kP; k++) {
            if (!(st_.syn[k] < 1e5f)) st_.syn[k] = 1e5f;
            if (!(st_.syn[k] > -1e5f)) st_.syn[k] = -1e5f;
        }
        if (pf) postfilter(aq, sy, out + s * L, L, Ti, gp);
        else std::memcpy(out + s * L, sy, sizeof(float) * (size_t)L);
    }
    voicing_ = vs / P.S;
    std::memmove(exc, exc + N, sizeof(float) * kExcHist);
    for (int i = 0; i < kP; i++) st_.lsfPrev[i] = lsfQ[i];
    // long losses fade out completely
    if (lost && lostMs > 120) {
        for (int n = 0; n < N; n++) {
            mute_ *= 0.995;
            out[n] *= (float)mute_;
        }
    } else if (!lost) {
        lost_ = 0;
        mute_ = 1.0;
    }
}

void CoreDecoder::postfilter(const float* aq, const float* in, float* out, int L, int lag, double gp) {
    const Prof& P = P_;
    float an[kP + 1], ad[kP + 1];
    weightLpc(aq, P.pfNum, an);
    weightLpc(aq, P.pfDen, ad);
    // residual through A(z/gn)
    constexpr int RH = kPitMax + 2;
    float* res = pfRes_ + RH;
    firA(an, in, res, L, pfFir_);
    // long-term (pitch) postfilter on the residual
    float r2[80];
    int bt = lag;
    double bc = -1, be = 1, rr = dot(res, res, L) + 1e-3;
    for (int T = lag - 1; T <= lag + 1; T++) {
        if (T < kPitMin || T > kPitMax) continue;
        double c = 0, e = 0;
        for (int n = 0; n < L; n++) { c += (double)res[n] * res[n - T]; e += (double)res[n - T] * res[n - T]; }
        if (c * c * be > bc * bc * e && c > 0) { bc = c; be = e + 1e-3; bt = T; }
    }
    double gl = 0.0;
    if (bc > 0) {
        const double rho = bc / std::sqrt(be * rr);
        if (rho > 0.45 && gp > 0.25) gl = P.pfLtp * (bc / be > 1.0 ? 1.0 : bc / be);
    }
    for (int n = 0; n < L; n++) r2[n] = (float)((res[n] + gl * res[n - bt]) / (1.0 + gl));
    // formant postfilter denominator, tilt compensation
    float y[80];
    iirA(ad, r2, y, L, pfIir_);
    double hr0 = 0, hr1 = 0;
    {
        float hi[22] = {}, ho[22], m1[kP] = {}, m2[kP] = {};
        for (int i = 0; i <= kP && i < 22; i++) hi[i] = an[i];
        float tmp[22];
        std::memcpy(tmp, hi, sizeof tmp);
        (void)m1;
        iirA(ad, tmp, ho, 22, m2);
        for (int i = 0; i < 22; i++) hr0 += (double)ho[i] * ho[i];
        for (int i = 0; i < 21; i++) hr1 += (double)ho[i] * ho[i + 1];
    }
    const double mu = hr1 > 0 ? 0.8 * hr1 / (hr0 + 1e-9) : 0.0;
    for (int n = 0; n < L; n++) {
        const float v = y[n];
        y[n] = v - (float)mu * pfTilt_;
        pfTilt_ = v;
    }
    // automatic gain control to the energy before the postfilter
    const double ein = dot(in, in, L), eout = dot(y, y, L);
    const double g = eout > 1e-3 ? std::sqrt(ein / eout) : 0.0;
    for (int n = 0; n < L; n++) {
        pfAgc_ = 0.9 * pfAgc_ + 0.1 * g;
        out[n] = (float)(y[n] * pfAgc_);
    }
    std::memmove(pfRes_, pfRes_ + L, sizeof(float) * RH);
}

}  // namespace detail
}  // namespace kaalvoice
}  // namespace dect2
