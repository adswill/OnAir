// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL voice codec v3: port of kaal_codec_v3.py (see the header). Every name below mirrors the Python.
#include "dect2/kaal/kaal_codec3.h"
#include "kaal_codec3_tables.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>

namespace dect2 {
namespace {

constexpr int FRAME = 320, HDR_PROF = 2, HDR_TYPE = 2, HDR_OFF = 5, HDR = HDR_PROF + HDR_TYPE + HDR_OFF, T_SPEECH = 0, T_SILENCE = 1;
const int EDGES[27] = {0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 46, 52, 59, 66, 75, 84, 96, 108, 124, 140, 160, 184, 210, 240, 280, 320};
constexpr int NB = 26, ESC_E = 20, ESC_C = 14, NRC = 8;
constexpr double ESTEP = 4.5;
constexpr int EMAX = 21, E0B = 5;   // int(63 * 1.5 / 4.5); 6 bits only when EMAX > 31
constexpr double kPi = 3.141592653589793;

struct Prof {
    int kbit, id, bits, bw;
    double ob, tilt, nfh;
};
// 12k: coarser step range, codes up to 7 kHz only; 24k and 32k: finer high band and a lower noise-fill level.
const Prof kProf[4] = {
    {16, 0, 320, 320, 3.0, 0.8, 0.9},
    {24, 1, 480, 320, 3.0, 0.4, 0.8},
    {32, 2, 640, 320, 3.0, 0.2, 0.8},
    {12, 3, 240, 280, 1.0, 0.4, 0.9},
};
const Prof* profOfKbit(int kbit) {
    for (const Prof& p : kProf) if (p.kbit == kbit) return &p;
    return nullptr;
}
// shared parameters of the Python P dict
constexpr double RC0 = 3.0, RCS = 1.0, G = 0.5, FL = 0.0, RND = 0.4, NF = 0.5, NFCAP = 0.5, NFV = 1.0, NFCH = 2.0, NFHF = 4500.0, INTERP = 1.0, SK = 3.0, SK2 = 4.0;
constexpr int N0 = 12;

struct Tables {
    std::vector<double> win;     // Vorbis power-sine window, 640
    std::vector<double> basis;   // [k][n], 320 x 640
    double fc[NB];
    int bandOf[FRAME];
    Tables() : win(2 * FRAME), basis((size_t)FRAME * 2 * FRAME) {
        const double sc = std::sqrt(2.0 / FRAME);
        for (int n = 0; n < 2 * FRAME; n++) {
            const double s = std::sin(kPi * (n + 0.5) / (2 * FRAME));
            win[n] = std::sin(kPi / 2 * s * s);
        }
        for (int k = 0; k < FRAME; k++)
            for (int n = 0; n < 2 * FRAME; n++)
                basis[(size_t)k * 2 * FRAME + n] = sc * std::cos(kPi / FRAME * ((double)n + 0.5 + FRAME / 2) * ((double)k + 0.5));
        for (int b = 0; b < NB; b++) {
            fc[b] = (EDGES[b] + EDGES[b + 1]) / 2.0 * 25.0;
            for (int k = EDGES[b]; k < EDGES[b + 1]; k++) bandOf[k] = b;
        }
    }
};
const Tables& tab() { static const Tables t; return t; }

void mdct(const double* block, double* X) {
    const Tables& T = tab();
    double w[2 * FRAME];
    for (int n = 0; n < 2 * FRAME; n++) w[n] = block[n] * T.win[n];
    for (int k = 0; k < FRAME; k++) {
        const double* b = &T.basis[(size_t)k * 2 * FRAME];
        double s = 0;
        for (int n = 0; n < 2 * FRAME; n++) s += w[n] * b[n];
        X[k] = s;
    }
}
void imdct(const double* X, double* y) {
    const Tables& T = tab();
    for (int n = 0; n < 2 * FRAME; n++) y[n] = 0;
    for (int k = 0; k < FRAME; k++) {
        const double xk = X[k];
        if (xk == 0) continue;
        const double* b = &T.basis[(size_t)k * 2 * FRAME];
        for (int n = 0; n < 2 * FRAME; n++) y[n] += b[n] * xk;
    }
    for (int n = 0; n < 2 * FRAME; n++) y[n] *= T.win[n];
}

// ---------- binary arithmetic coder (WNC, 32 bit, 12 bit probabilities) ----------
constexpr int64_t TOP = 0xFFFFFFFFLL, HALF = 0x80000000LL, Q1 = 0x40000000LL, Q3 = 0xC0000000LL;
constexpr int BYPASS = 2048;

struct Model {
    int p, n = 0;
    explicit Model(int p0 = 2048) : p(p0) {}
    Model(int p0, int init) : p(init ? init : p0), n(init ? N0 : 0) {}
    void update(int bit) {
        const int sh = n < 4 ? 2 : (n < 12 ? 3 : 4);
        n++;
        if (bit) p -= p >> sh; else p += (4096 - p) >> sh;
        p = std::min(4090, std::max(6, p));
    }
};

struct Enc {
    int64_t low = 0, high = TOP;
    int64_t pend = 0;
    std::vector<int> out;
    void emit(int b) {
        out.push_back(b);
        if (pend) { out.insert(out.end(), (size_t)pend, 1 - b); pend = 0; }
    }
    void put(int bit, int p0) {
        const int64_t rng = high - low + 1;
        const int64_t split = low + ((rng * p0) >> 12) - 1;
        if (bit == 0) high = split; else low = split + 1;
        for (;;) {
            if (high < HALF) emit(0);
            else if (low >= HALF) { emit(1); low -= HALF; high -= HALF; }
            else if (low >= Q1 && high < Q3) { pend++; low -= Q1; high -= Q1; }
            else break;
            low = (low << 1) & TOP;
            high = ((high << 1) & TOP) | 1;
        }
    }
    void bit(Model& m, int b) { put(b, m.p); m.update(b); }
    void raw(int64_t v, int nbits) { for (int i = nbits - 1; i >= 0; i--) put((int)((v >> i) & 1), BYPASS); }
    const std::vector<int>& finish() {
        pend++;
        emit(low < Q1 ? 0 : 1);
        return out;
    }
    int64_t nbitsIfFinished() const { return (int64_t)out.size() + pend + 2; }
};

struct Dec {
    const uint8_t* bits;
    size_t len, pos = 0;
    int64_t low = 0, high = TOP, val = 0;
    Dec(const uint8_t* b, size_t n) : bits(b), len(n) { for (int i = 0; i < 32; i++) val = (val << 1) | next(); }
    int next() { const int b = pos < len ? bits[pos] : 0; pos++; return b; }
    int get(int p0) {
        const int64_t rng = high - low + 1;
        const int64_t split = low + ((rng * p0) >> 12) - 1;
        int bit;
        if (val <= split) { bit = 0; high = split; } else { bit = 1; low = split + 1; }
        for (;;) {
            if (high < HALF) {}
            else if (low >= HALF) { low -= HALF; high -= HALF; val -= HALF; }
            else if (low >= Q1 && high < Q3) { low -= Q1; high -= Q1; val -= Q1; }
            else break;
            low = (low << 1) & TOP;
            high = ((high << 1) & TOP) | 1;
            val = ((val << 1) & TOP) | next();
        }
        return bit;
    }
    int bit(Model& m) { const int b = get(m.p); m.update(b); return b; }
    int64_t raw(int nbits) { int64_t v = 0; for (int i = 0; i < nbits; i++) v = (v << 1) | get(BYPASS); return v; }
};

// ---------- symbol coding ----------
struct Ctx {   // fresh per frame, start probabilities from the trained table of the profile
    Model ez[3], es[3], em[8], rz[NRC][4], rm[NRC][2][6];
    explicit Ctx(int pid) {
        const kaal3::InitTable& t = kaal3::kInit[pid];
        for (int i = 0; i < 3; i++) { ez[i] = Model(1500, t.ez[i]); es[i] = Model(2048, t.es[i]); }
        for (int i = 0; i < 8; i++) em[i] = Model(1000, t.em[i]);
        for (int r = 0; r < NRC; r++) {
            for (int c = 0; c < 4; c++) rz[r][c] = Model(c == 0 ? 3000 - 300 * r : 1500 - 150 * r, t.rz[r][c]);
            for (int j = 0; j < 2; j++) for (int i = 0; i < 6; i++) rm[r][j][i] = Model(1800 - 150 * r, t.rm[r][j][i]);
        }
    }
};

void codeEnergyEnc(Enc& io, Ctx& ctx, const int* e) {
    io.raw(e[0], E0B);
    int prev = e[0], pc = 0;
    for (int b = 1; b < NB; b++) {
        const int d = e[b] - prev;
        const int z = d != 0;
        io.bit(ctx.ez[pc], z);
        if (z) {
            io.bit(ctx.es[pc], d < 0 ? 1 : 0);
            const int m = std::abs(d) - 1;
            int i = 0;
            while (i < ESC_E) {
                const int stop = m == i;
                io.bit(ctx.em[std::min(i, 7)], stop);
                if (stop) break;
                i++;
            }
            if (i == ESC_E) io.raw(m - ESC_E, 6);
        }
        prev = std::min(EMAX, std::max(0, prev + d));
        pc = std::min(2, std::abs(d));
    }
}

void codeEnergyDec(Dec& io, Ctx& ctx, int* out) {
    out[0] = (int)io.raw(E0B);
    int pc = 0;
    for (int b = 1; b < NB; b++) {
        const int z = io.bit(ctx.ez[pc]);
        int d = 0;
        if (z) {
            const int neg = io.bit(ctx.es[pc]);
            int i = 0;
            while (i < ESC_E) {
                if (io.bit(ctx.em[std::min(i, 7)])) break;
                i++;
            }
            int m = i;
            if (i == ESC_E) m = ESC_E + (int)io.raw(6);
            d = neg ? -(m + 1) : (m + 1);
        }
        out[b] = std::min(EMAX, std::max(0, out[b - 1] + d));
        pc = std::min(2, std::abs(d));
    }
}

void codeCoefsEnc(Enc& io, Ctx& ctx, const int* q, const int* rc, int lim) {
    int a1 = 0, a2 = 0;
    for (int k = 0; k < FRAME; k++) {
        if (k >= lim) continue;
        const int c = std::min(3, 2 * a1 + a2);
        Model& mz = ctx.rz[rc[k]][c];
        const int v = q[k];
        const int nz = v != 0;
        io.bit(mz, nz);
        if (nz) {
            io.raw(v < 0 ? 1 : 0, 1);
            const int m = std::abs(v) - 1;
            Model* cls = ctx.rm[rc[k]][(a1 + a2) > 2 ? 1 : 0];
            int i = 0;
            while (i < ESC_C) {
                const int stop = m == i;
                io.bit(cls[std::min(i, 5)], stop);
                if (stop) break;
                i++;
            }
            if (i == ESC_C) io.raw(std::min(m - ESC_C, 4095), 12);
        }
        a2 = a1;
        a1 = std::min(3, std::abs(v));
    }
}

void codeCoefsDec(Dec& io, Ctx& ctx, const int* rc, int lim, int* out) {
    int a1 = 0, a2 = 0;
    for (int k = 0; k < FRAME; k++) out[k] = 0;
    for (int k = 0; k < FRAME; k++) {
        if (k >= lim) continue;
        const int c = std::min(3, 2 * a1 + a2);
        Model& mz = ctx.rz[rc[k]][c];
        const int nz = io.bit(mz);
        int v = 0;
        if (nz) {
            const int neg = (int)io.raw(1);
            Model* cls = ctx.rm[rc[k]][(a1 + a2) > 2 ? 1 : 0];
            int i = 0;
            while (i < ESC_C) {
                if (io.bit(cls[std::min(i, 5)])) break;
                i++;
            }
            int m = i;
            if (i == ESC_C) m = ESC_C + (int)io.raw(12);
            v = neg ? -(m + 1) : (m + 1);
        }
        out[k] = v;
        a2 = a1;
        a1 = std::min(3, std::abs(v));
    }
}

// ---------- band helpers ----------
void rmsQ(const int* e, double* r) { for (int b = 0; b < NB; b++) r[b] = std::pow(10.0, (double)e[b] * ESTEP / 20.0); }
void stepsOf(const Prof& P, const int* e, int off, double* st) {
    double r[NB];
    rmsQ(e, r);
    const Tables& T = tab();
    for (int b = 0; b < NB; b++) {
        const double wt = std::pow(2.0, P.tilt * std::max(0.0, T.fc[b] - 3500.0) / 1000.0);
        const double v = std::pow(2.0, off / 4.0 - P.ob) * std::pow(std::max(r[b], 1.0), G) * std::pow(100.0, 0.75 - G) * wt;
        st[b] = std::max(v, FL);
    }
}
void rclassOf(const int* e, const double* steps, int* rc) {   // per coefficient
    double r[NB];
    rmsQ(e, r);
    int cb[NB];
    for (int b = 0; b < NB; b++) {
        const double lr = std::log2(std::max(r[b], 1.0) / steps[b]);
        cb[b] = (int)std::min((double)(NRC - 1), std::max(0.0, std::floor((lr + RC0) * RCS)));
    }
    const Tables& T = tab();
    for (int k = 0; k < FRAME; k++) rc[k] = cb[T.bandOf[k]];
}
void bandEnergyIdx(const double* X, int* e) {
    for (int b = 0; b < NB; b++) {
        double s = 0;
        for (int k = EDGES[b]; k < EDGES[b + 1]; k++) s += X[k] * X[k];
        const double p = s / (EDGES[b + 1] - EDGES[b]) + 1e-9;
        const double db = 10.0 * std::log10(p);
        e[b] = (int)std::min((double)EMAX, std::max(0.0, std::nearbyint(db / ESTEP)));
    }
}
void bitsOf(int v, int n, std::vector<int>& o) { for (int i = n - 1; i >= 0; i--) o.push_back((v >> i) & 1); }

void encodeFrame(const Prof& P, const double* X, uint8_t* outBits) {
    int e[NB];
    bandEnergyIdx(X, e);
    auto attempt = [&](int off, Enc& io) {
        double st[NB];
        stepsOf(P, e, off, st);
        int q[FRAME];
        for (int b = 0; b < NB; b++)
            for (int k = EDGES[b]; k < EDGES[b + 1]; k++) {
                const double a = std::floor(std::fabs(X[k]) / st[b] + RND);
                const int v = (int)std::min(a, (double)(ESC_C + 4095));
                q[k] = X[k] < 0 ? -v : (X[k] > 0 ? v : 0);
            }
        // isolated +-1 above 3 kHz are birdies: zero them (np.roll wraps around)
        int q2[FRAME];
        for (int k = 0; k < FRAME; k++) {
            const int a = std::abs(q[k]), l = std::abs(q[(k + FRAME - 1) % FRAME]), r = std::abs(q[(k + 1) % FRAME]);
            const bool iso = a == 1 && l == 0 && r == 0 && k >= 120;
            q2[k] = iso ? 0 : q[k];
        }
        int rc[FRAME];
        rclassOf(e, st, rc);
        Ctx ctx(P.id);
        codeEnergyEnc(io, ctx, e);
        codeCoefsEnc(io, ctx, q2, rc, P.bw);
    };
    const int64_t budget = P.bits - HDR;
    int lo = 0, hi = 31, bestOff = -1;
    Enc best;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        Enc io;
        attempt(mid, io);
        if (io.nbitsIfFinished() <= budget) { bestOff = mid; best = std::move(io); hi = mid - 1; }
        else lo = mid + 1;
    }
    std::vector<int> bits;
    bitsOf(P.id, HDR_PROF, bits);
    if (bestOff < 0) {   // even the coarsest does not fit: energies only
        Enc io;
        Ctx ctx(P.id);
        codeEnergyEnc(io, ctx, e);
        bitsOf(T_SILENCE, HDR_TYPE, bits);
        bitsOf(0, HDR_OFF, bits);
        const auto& o = io.finish();
        bits.insert(bits.end(), o.begin(), o.end());
    } else {
        bitsOf(T_SPEECH, HDR_TYPE, bits);
        bitsOf(bestOff, HDR_OFF, bits);
        const auto& o = best.finish();
        bits.insert(bits.end(), o.begin(), o.end());
    }
    for (int i = 0; i < P.bits; i++) outBits[i] = i < (int)bits.size() ? (uint8_t)bits[i] : 0;
}

// ---------- decoder state ----------
struct DecState {
    double overlap[FRAME] = {};
    double lastX[FRAME] = {};
    int lossRun = 0;
    int64_t rng = 12345;
    void noise(double* out, int n) {   // LCG, uniform [-1, 1)
        int64_t r = rng;
        for (int i = 0; i < n; i++) {
            r = (r * 1103515245LL + 12345) & 0x7FFFFFFFLL;
            out[i] = (double)(r >> 8) / (double)(1 << 22) - 1.0;
        }
        rng = r;
    }
    void synth(const double* X, double* blk) {
        double y[2 * FRAME];
        imdct(X, y);
        for (int i = 0; i < FRAME; i++) { blk[i] = y[i] + overlap[i]; overlap[i] = y[FRAME + i]; }
    }
};

// Returns false for an implausible or reserved frame. a: P.bits bits (profile already checked).
bool decodeFrame(const Prof& P, const uint8_t* a, DecState& st, double* X) {
    int typ = 0;
    for (int i = 0; i < HDR_TYPE; i++) typ = (typ << 1) | a[HDR_PROF + i];
    if (typ != T_SPEECH && typ != T_SILENCE) return false;
    int off = 0;
    for (int i = 0; i < HDR_OFF; i++) off = (off << 1) | a[HDR_PROF + HDR_TYPE + i];
    const size_t blen = (size_t)(P.bits - HDR);
    Dec io(a + HDR, blen);
    Ctx ctx(P.id);
    int e[NB];
    codeEnergyDec(io, ctx, e);
    double rms[NB], steps[NB];
    rmsQ(e, rms);
    int q[FRAME];
    double qa[FRAME];
    if (typ == T_SPEECH) {
        stepsOf(P, e, off, steps);
        int rc[FRAME];
        rclassOf(e, steps, rc);
        codeCoefsDec(io, ctx, rc, P.bw, q);
        for (int k = 0; k < FRAME; k++) qa[k] = q[k];
    } else {
        for (int k = 0; k < FRAME; k++) qa[k] = 0;
        for (int b = 0; b < NB; b++) steps[b] = 1.0;
    }
    if (typ == T_SPEECH) {   // integrity: coefficients must fit the coded band energy
        for (int b = 0; b < NB; b++) {
            const int w = EDGES[b + 1] - EDGES[b];
            double s = 0;
            for (int k = EDGES[b]; k < EDGES[b + 1]; k++) { const double c = qa[k] * steps[b]; s += c * c; }
            if (s > SK * rms[b] * rms[b] * w + SK2 * steps[b] * steps[b]) return false;
        }
    }
    if ((int64_t)io.pos > (int64_t)blen + 32 + 2) return false;   // ran past the frame end
    const Tables& T = tab();
    double nz[FRAME];
    st.noise(nz, FRAME);
    const double s3 = std::sqrt(3.0);
    for (int b = 0; b < NB; b++) {
        const int w = EDGES[b + 1] - EDGES[b];
        double sum = 0;
        for (int k = EDGES[b]; k < EDGES[b + 1]; k++) { X[k] = qa[k] * steps[b]; sum += X[k] * X[k]; }
        const double tot = rms[b] * rms[b] * w;
        const double rem = std::max(0.0, tot - sum);
        int nzc = 0;
        for (int k = EDGES[b]; k < EDGES[b + 1]; k++) nzc += qa[k] == 0;
        if (!nzc) continue;
        const double fracNz = 1.0 - (double)nzc / (double)w;
        const double hf = std::min(1.0, std::max(0.0, (T.fc[b] - 3000.0) / (NFHF - 3000.0)));
        const double nfb = NF + (P.nfh - NF) * hf;
        const double capb = NFCAP + (NFCH - NFCAP) * hf;
        double amp = std::sqrt(rem / nzc) * nfb * (1.0 - NFV * fracNz * (T.fc[b] < 3000 ? 1.0 : 0.0));
        if (typ == T_SPEECH) amp = std::min(amp, capb * steps[b]);
        if (b > 0 && b < NB - 1 && T.fc[b] > 3000) {
            // tilt the noise inside the band following the neighbour band levels (energy preserving)
            const double lo_ = std::log(rms[b - 1] + 1e-3), hi_ = std::log(rms[b + 1] + 1e-3);
            double sl[64];
            double ms = 0;
            for (int i = 0; i < w; i++) {
                const double tt = w > 1 ? -0.5 + (double)i / (double)(w - 1) : 0.0;
                sl[i] = std::exp(tt * (hi_ - lo_) * INTERP);
                ms += sl[i] * sl[i];
            }
            const double nrm = std::sqrt(ms / w);
            for (int i = 0; i < w; i++) {
                const int k = EDGES[b] + i;
                if (qa[k] == 0) X[k] = X[k] + nz[k] * amp * s3 * (sl[i] / nrm);
            }
        } else {
            for (int k = EDGES[b]; k < EDGES[b + 1]; k++)
                if (qa[k] == 0) X[k] = X[k] + nz[k] * amp * s3;
        }
    }
    return true;
}

// One decoder step: bits == nullptr or a wrong length is a frame that is not valid by shape. Writes FRAME samples.
// kbitWanted 0 = any profile. Returns the profile decoded, 0 when concealed.
int stepFrame(const uint8_t* frame, size_t nbits, int kbitWanted, DecState& st, int16_t* pcm) {
    double X[FRAME];
    bool ok = false;
    int got = 0;
    if (frame && nbits >= (size_t)HDR_PROF) {
        const int pid = (frame[0] << 1) | frame[1];
        const Prof* P = nullptr;
        if (frame[0] <= 1 && frame[1] <= 1) for (const Prof& p : kProf) if (p.id == pid) P = &p;
        if (P && nbits == (size_t)P->bits && (kbitWanted == 0 || kbitWanted == P->kbit)) {
            bool binary = true;
            for (size_t i = 0; i < nbits; i++) if (frame[i] > 1) { binary = false; break; }
            if (binary) { ok = decodeFrame(*P, frame, st, X); if (ok) got = P->kbit; }
        }
    }
    if (!ok) {   // concealment: repeat the last good spectrum, decayed, with random signs
        st.lossRun++;
        double nz[FRAME];
        st.noise(nz, FRAME);
        const double decay = std::pow(0.6, st.lossRun);
        for (int k = 0; k < FRAME; k++) X[k] = st.lastX[k] * decay * (nz[k] < 0 ? -1.0 : 1.0);
    } else {
        st.lossRun = 0;
        std::copy(X, X + FRAME, st.lastX);
    }
    double blk[FRAME];
    st.synth(X, blk);
    for (int i = 0; i < FRAME; i++) pcm[i] = (int16_t)std::min(32767.0, std::max(-32768.0, std::nearbyint(blk[i])));
    return got;
}

}  // namespace

bool KaalCodec3::validBitrate(int kbit) { return profOfKbit(kbit) != nullptr; }
size_t KaalCodec3::frameBits(int kbit) { const Prof* p = profOfKbit(kbit); return p ? (size_t)p->bits : 0; }
int KaalCodec3::bitrateOfHeader(const uint8_t* bits) {
    if (bits[0] > 1 || bits[1] > 1) return 0;
    const int pid = (bits[0] << 1) | bits[1];
    for (const Prof& p : kProf) if (p.id == pid) return p.kbit;
    return 0;
}

std::vector<uint8_t> KaalCodec3::encode(const int16_t* pcm, size_t n) const {
    const Prof& P = *profOfKbit(kbit_);
    const size_t nIn = (n + FRAME - 1) / FRAME, nf = nIn + 1;
    std::vector<double> pad((nf + 1) * FRAME, 0.0);
    for (size_t i = 0; i < n; i++) pad[FRAME + i] = pcm[i];
    std::vector<uint8_t> out(nf * (size_t)P.bits);
    double X[FRAME];
    for (size_t t = 0; t < nf; t++) {
        mdct(&pad[t * FRAME], X);
        encodeFrame(P, X, &out[t * (size_t)P.bits]);
    }
    return out;
}

struct KaalCodec3::StreamDecoder::Impl {
    DecState st;
    int kbit;
};
KaalCodec3::StreamDecoder::StreamDecoder(int kbit) : p_(new Impl) { p_->kbit = validBitrate(kbit) ? kbit : 0; }
KaalCodec3::StreamDecoder::~StreamDecoder() = default;
void KaalCodec3::StreamDecoder::reset() { const int k = p_->kbit; p_.reset(new Impl); p_->kbit = k; }
int KaalCodec3::StreamDecoder::push(const uint8_t* bits, size_t nbits, int16_t* pcm) { return stepFrame(bits, nbits, p_->kbit, p_->st, pcm); }

std::vector<int16_t> KaalCodec3::decode(const uint8_t* bits, size_t nFrames) const {
    const size_t fb = frameBits();
    DecState st;
    std::vector<int16_t> pcm(nFrames * FRAME);
    for (size_t t = 0; t < nFrames; t++) stepFrame(bits + t * fb, fb, kbit_, st, &pcm[t * FRAME]);
    return pcm;
}

std::vector<int16_t> KaalCodec3::decode(const std::vector<std::vector<uint8_t>>& frames) const {
    const size_t fb = frameBits();
    DecState st;
    std::vector<int16_t> pcm(frames.size() * FRAME);
    for (size_t t = 0; t < frames.size(); t++)
        stepFrame(frames[t].size() == fb ? frames[t].data() : nullptr, fb, kbit_, st, &pcm[t * FRAME]);
    return pcm;
}

}  // namespace dect2
