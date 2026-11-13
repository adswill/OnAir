// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL voice codec 4: the high band (3.6 .. 8 kHz) as a parametric noise envelope. 5 ms MDCT blocks; per 20 ms a level, a spectral
// shape over six bands and (higher profiles) a time envelope per block. The decoder fills the bins with noise of that envelope.
#include "kv_core.h"
#include <cmath>
#include <cstring>

namespace dect2 {
namespace kaalvoice {
namespace detail {

namespace {
struct MdctTab {
    float win[2 * kHbM];
    float cs[2 * kHbM][kHbM];
    MdctTab() {
        const int M = kHbM;
        for (int n = 0; n < 2 * M; n++) win[n] = (float)kvSin(kPi * (n + 0.5) / (2 * M));
        const double sc = std::sqrt(2.0 / M);
        for (int n = 0; n < 2 * M; n++)
            for (int k = 0; k < M; k++) cs[n][k] = (float)(sc * kvCos(kPi / M * (n + 0.5 + M * 0.5) * (k + 0.5)));
    }
};
const MdctTab& mtab() { static const MdctTab t; return t; }

double bandT(int j) { return (j - 2.5) / 2.5; }   // band position in [-1, 1]
double bandW(int j) { return kHbEdge[j + 1] - kHbEdge[j]; }
double gainStep(int bits) { return 96.0 / (1 << bits); }
const double kTimeLv[4] = {0.0, -4.0, -9.0, -16.0};

void shapeFromIdx(const HbProf& h, const int* sh, double* s) {
    switch (h.shapeMode) {
    case 0:
        for (int j = 0; j < kHbBands; j++) s[j] = (-21.0 + 6.0 * sh[0]) * bandT(j);
        break;
    case 1: {
        double m = 0;
        for (int j = 0; j < kHbBands; j++) m += bandT(j) * bandT(j);
        m /= kHbBands;
        for (int j = 0; j < kHbBands; j++) s[j] = (-21.0 + 6.0 * sh[0]) * bandT(j) + (-9.0 + 6.0 * sh[1]) * (bandT(j) * bandT(j) - m);
        break;
    }
    case 2:
        for (int j = 0; j < kHbBands; j++) s[j] = -13.5 + 6.0 * sh[j];
        break;
    default:
        for (int j = 0; j < kHbBands; j++) s[j] = -22.0 + 4.0 * sh[j];
        break;
    }
    // normalise: the width-weighted mean power of the shape is 0 dB, so the level carries the power
    double num = 0, den = 0;
    for (int j = 0; j < kHbBands; j++) { num += bandW(j) * kvFromDb(s[j]); den += bandW(j); }
    const double off = kvDb(num / den);
    for (int j = 0; j < kHbBands; j++) s[j] -= off;
}

int nearest(double v, double lo, double step, int n) {
    int k = (int)std::floor((v - lo) / step + 0.5);
    return k < 0 ? 0 : (k >= n ? n - 1 : k);
}
}  // namespace

void mdct80(const float* x, float* X) {
    const MdctTab& t = mtab();
    float xw[2 * kHbM];
    for (int n = 0; n < 2 * kHbM; n++) xw[n] = x[n] * t.win[n];
    for (int k = 0; k < kHbM; k++) {
        float s = 0;
        for (int n = 0; n < 2 * kHbM; n++) s += xw[n] * t.cs[n][k];
        X[k] = s;
    }
}

void imdct80(const float* X, float* y) {
    const MdctTab& t = mtab();
    for (int n = 0; n < 2 * kHbM; n++) {
        float s = 0;
        for (int k = 0; k < kHbM; k++) s += X[k] * t.cs[n][k];
        y[n] = s * t.win[n];
    }
}

// ---------------------------------------------------------------- encoder
HbEncoder::HbEncoder(const Prof& P) : P_(P) { reset(); }
void HbEncoder::reset() { std::memset(hist_, 0, sizeof hist_); lvl_ = 0; }

void HbEncoder::encode(const float* hb, FrameQ& q) {
    const HbProf& h = P_.hb;
    double lsum = 0, tsum = 0;
    for (int half = 0; half < P_.halves; half++) {
        double e[4][kHbBands], tot[4];
        for (int b = 0; b < 4; b++) {
            float buf[2 * kHbM], X[kHbM];
            std::memcpy(buf, hist_, sizeof hist_);
            std::memcpy(buf + kHbM, hb + (half * 4 + b) * kHbM, sizeof(float) * kHbM);
            std::memcpy(hist_, buf + kHbM, sizeof hist_);
            mdct80(buf, X);
            double t = 0, w = 0;
            for (int j = 0; j < kHbBands; j++) {
                double s = 0;
                for (int k = kHbEdge[j]; k < kHbEdge[j + 1]; k++) s += (double)X[k] * X[k];
                e[b][j] = s / bandW(j);
                t += s;
                w += bandW(j);
            }
            tot[b] = t / w;
        }
        const double mean = (tot[0] + tot[1] + tot[2] + tot[3]) * 0.25;
        const double G = kvDb(mean);
        lsum += G;
        {
            double num = 0, den = 0;
            for (int j = 0; j < kHbBands; j++) {
                const double sj = kvDb((e[0][j] + e[1][j] + e[2][j] + e[3][j]) * 0.25) - G;
                num += sj * bandT(j);
                den += bandT(j) * bandT(j);
            }
            tsum += num / den;
        }
        if (!h.on) continue;
        HbQ& o = q.hb[half];
        const int ng = 1 << h.gainBits;
        o.gain = nearest(G, 0.0, gainStep(h.gainBits), ng);
        if (G < 1.0) o.gain = 0;
        double s[kHbBands];
        for (int j = 0; j < kHbBands; j++) s[j] = kvDb((e[0][j] + e[1][j] + e[2][j] + e[3][j]) * 0.25) - G;
        if (h.shapeMode <= 1) {
            double num = 0, den = 0;
            for (int j = 0; j < kHbBands; j++) { num += s[j] * bandT(j); den += bandT(j) * bandT(j); }
            o.shape[0] = nearest(num / den, -21.0, 6.0, 8);
            if (h.shapeMode == 1) {
                double m = 0;
                for (int j = 0; j < kHbBands; j++) m += bandT(j) * bandT(j);
                m /= kHbBands;
                double n2 = 0, d2 = 0;
                for (int j = 0; j < kHbBands; j++) {
                    const double c = bandT(j) * bandT(j) - m;
                    n2 += (s[j] - (-21.0 + 6.0 * o.shape[0]) * bandT(j)) * c;
                    d2 += c * c;
                }
                o.shape[1] = nearest(n2 / d2, -9.0, 6.0, 4);
            }
        } else {
            for (int j = 0; j < kHbBands; j++)
                o.shape[j] = h.shapeMode == 2 ? nearest(s[j], -13.5, 6.0, 4) : nearest(s[j], -22.0, 4.0, 8);
        }
        if (h.timeBits) {
            double mx = -1e9, tb[4];
            for (int b = 0; b < 4; b++) { tb[b] = kvDb(tot[b]); if (tb[b] > mx) mx = tb[b]; }
            for (int b = 0; b < 4; b++) {
                const double d = tb[b] - mx;
                int bi = 0;
                for (int k = 1; k < 4; k++)
                    if (std::fabs(d - kTimeLv[k]) < std::fabs(d - kTimeLv[bi])) bi = k;
                o.time[b] = bi;
            }
        }
    }
    lvl_ = lsum / P_.halves;
    tilt_ = tsum / P_.halves;
}

// ---------------------------------------------------------------- decoder
HbDecoder::HbDecoder(const Prof& P) : P_(P) { reset(); }

void HbDecoder::reset() {
    rng_.s = 0x48424e5au;
    std::memset(ola_, 0, sizeof ola_);
    for (double& d : lastDb_) d = -100.0;
    have_ = false;
    lost_ = 0;
}

void HbDecoder::synthBlock(const double* bandDb, float* out) {
    float X[kHbM] = {}, y[2 * kHbM];
    for (int j = 0; j < kHbBands; j++) {
        if (bandDb[j] < -60.0) continue;
        const float a = (float)std::sqrt(kvFromDb(bandDb[j]) * 3.0);
        for (int k = kHbEdge[j]; k < kHbEdge[j + 1]; k++) X[k] = a * rng_.uni();
    }
    imdct80(X, y);
    for (int n = 0; n < kHbM; n++) out[n] = ola_[n] + y[n];
    std::memcpy(ola_, y + kHbM, sizeof ola_);
}

void HbDecoder::levels(const FrameQ* q, const bool* timeBad, double (*Lv)[kHbBands]) {
    const HbProf& h = P_.hb;
    for (int half = 0; half < P_.halves; half++) {
        double cur[kHbBands];
        double tl[4] = {0, 0, 0, 0};
        bool useTime = false;
        if (!q) {
            lost_++;
            for (int j = 0; j < kHbBands; j++) cur[j] = lastDb_[j] - 3.0;
        } else {
            lost_ = 0;
            const HbQ& o = q->hb[half];
            double s[kHbBands];
            shapeFromIdx(h, o.shape, s);
            const double G = o.gain == 0 ? -100.0 : o.gain * gainStep(h.gainBits);
            for (int j = 0; j < kHbBands; j++) cur[j] = G + s[j];
            if (h.timeBits && !(timeBad && *timeBad)) {
                useTime = true;
                double m = 0;
                for (int b = 0; b < 4; b++) { tl[b] = kTimeLv[o.time[b] & 3]; m += kvFromDb(tl[b]); }
                const double off = kvDb(m * 0.25);
                for (int b = 0; b < 4; b++) tl[b] -= off;
            }
        }
        if (!have_) { for (int j = 0; j < kHbBands; j++) lastDb_[j] = cur[j]; have_ = true; }
        for (int b = 0; b < 4; b++) {
            double* L = Lv[half * 4 + b];
            for (int j = 0; j < kHbBands; j++) {
                if (useTime) L[j] = cur[j] + tl[b];
                else L[j] = lastDb_[j] + (b + 1) * 0.25 * (cur[j] - lastDb_[j]);
                if (cur[j] < -60.0 && lastDb_[j] < -60.0) L[j] = -100.0;
            }
        }
        for (int j = 0; j < kHbBands; j++) lastDb_[j] = cur[j];
    }
}

void HbDecoder::synth(const double (*L)[kHbBands], int nb, float* out) {
    for (int b = 0; b < nb; b++) synthBlock(L[b], out + b * kHbM);
}

void HbDecoder::decode(const FrameQ* q, const bool* timeBad, float* out) {
    double L[8][kHbBands];
    levels(q, timeBad, L);
    synth(L, 4 * P_.halves, out);
}

void HbDecoder::comfortLevels(double levelDb, double tiltDb, double* L) {
    for (int j = 0; j < kHbBands; j++) L[j] = levelDb + tiltDb * bandT(j);
    for (int j = 0; j < kHbBands; j++) lastDb_[j] = L[j];
    have_ = true;
}

void HbDecoder::comfort(double levelDb, double tiltDb, float* out, int n16) {
    double L[kHbBands];
    for (int j = 0; j < kHbBands; j++) L[j] = levelDb + tiltDb * bandT(j);
    for (int b = 0; b * kHbM < n16; b++) synthBlock(L, out + b * kHbM);
    for (int j = 0; j < kHbBands; j++) lastDb_[j] = L[j];
    have_ = true;
}

}  // namespace detail
}  // namespace kaalvoice
}  // namespace dect2
