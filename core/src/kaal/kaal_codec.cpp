// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL voice codec: port of kaal_codec.py (see the header). Every name below mirrors the Python.
#include "dect2/kaal/kaal_codec.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>

namespace dect2 {
namespace {

constexpr int FRAME = 320, BITS = 320, HDR_TYPE = 4, HDR_OFF = 5, T_SPEECH = 0, T_SILENCE = 1;
const int EDGES[17] = {0, 4, 8, 12, 16, 20, 26, 32, 40, 52, 66, 84, 108, 140, 184, 240, 320};
constexpr int NB = 16, EMAX = 63, ESC_E = 20, ESC_C = 14;
constexpr double ESTEP = 1.5;
constexpr double kPi = 3.141592653589793;

// ---------- MDCT (direct, float64) ----------
struct Tables {
    std::vector<double> win;     // 640
    std::vector<double> basis;   // [k][n], 320 x 640
    Tables() : win(2 * FRAME), basis((size_t)FRAME * 2 * FRAME) {
        const double sc = std::sqrt(2.0 / FRAME);
        for (int n = 0; n < 2 * FRAME; n++) win[n] = std::sin(kPi * (n + 0.5) / (2 * FRAME));
        for (int k = 0; k < FRAME; k++)
            for (int n = 0; n < 2 * FRAME; n++)
                basis[(size_t)k * 2 * FRAME + n] = sc * std::cos(kPi / FRAME * ((double)n + 0.5 + FRAME / 2) * ((double)k + 0.5));
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
void imdct(const double* X, double* y) {   // 320 coefs -> 640 windowed samples
    const Tables& T = tab();
    for (int n = 0; n < 2 * FRAME; n++) y[n] = 0;
    for (int k = 0; k < FRAME; k++) {
        const double xk = X[k];
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
struct Ctx {   // fresh per frame
    Model ez[3] = {Model(1500), Model(1500), Model(1500)};
    Model es[3];
    Model em[8] = {Model(1000), Model(1000), Model(1000), Model(1000), Model(1000), Model(1000), Model(1000), Model(1000)};
    Model cz[4][4];
    Model cm[2][6];
    Ctx() {
        for (int p = 0; p < 4; p++) for (int c = 0; c < 4; c++) cz[p][c] = Model(c == 0 ? 2600 : 1200);
        for (int a = 0; a < 2; a++) for (int i = 0; i < 6; i++) cm[a][i] = Model(1800);
    }
};

void codeEnergyEnc(Enc& io, Ctx& ctx, const int* e) {
    io.raw(e[0], 6);
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
    out[0] = (int)io.raw(6);
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

void codeCoefsEnc(Enc& io, Ctx& ctx, const int* q) {
    int a1 = 0, a2 = 0;
    for (int k = 0; k < FRAME; k++) {
        const int c = std::min(3, 2 * a1 + a2);
        const int pos = k < 40 ? 0 : (k < 100 ? 1 : (k < 180 ? 2 : 3));
        Model& mz = ctx.cz[pos][c];
        const int v = q[k];
        const int nz = v != 0;
        io.bit(mz, nz);
        if (nz) {
            io.raw(v < 0 ? 1 : 0, 1);
            const int m = std::abs(v) - 1;
            Model* cls = ctx.cm[(a1 + a2) > 2 ? 1 : 0];
            int i = 0;
            while (i < ESC_C) {
                const int stop = m == i;
                io.bit(cls[std::min(i, 5)], stop);
                if (stop) break;
                i++;
            }
            if (i == ESC_C) io.raw(std::min(m - ESC_C, 255), 8);
        }
        a2 = a1;
        a1 = std::min(3, std::abs(v));
    }
}

void codeCoefsDec(Dec& io, Ctx& ctx, int* out) {
    int a1 = 0, a2 = 0;
    for (int k = 0; k < FRAME; k++) {
        const int c = std::min(3, 2 * a1 + a2);
        const int pos = k < 40 ? 0 : (k < 100 ? 1 : (k < 180 ? 2 : 3));
        Model& mz = ctx.cz[pos][c];
        const int nz = io.bit(mz);
        int v = 0;
        if (nz) {
            const int neg = (int)io.raw(1);
            Model* cls = ctx.cm[(a1 + a2) > 2 ? 1 : 0];
            int i = 0;
            while (i < ESC_C) {
                if (io.bit(cls[std::min(i, 5)])) break;
                i++;
            }
            int m = i;
            if (i == ESC_C) m = ESC_C + (int)io.raw(8);
            v = neg ? -(m + 1) : (m + 1);
        }
        out[k] = v;
        a2 = a1;
        a1 = std::min(3, std::abs(v));
    }
}

// ---------- band helpers ----------
void rmsQ(const int* e, double* r) { for (int b = 0; b < NB; b++) r[b] = std::pow(10.0, (double)e[b] * ESTEP / 20.0); }
void stepsOf(const int* e, int off, double* st) {
    double r[NB];
    rmsQ(e, r);
    const double f = std::pow(2.0, off / 4.0 - 3.0);
    for (int b = 0; b < NB; b++) st[b] = f * std::pow(std::max(r[b], 1.0), 0.75);
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

void encodeFrame(const double* X, uint8_t* outBits) {
    int e[NB];
    bandEnergyIdx(X, e);
    auto attempt = [&](int off, Enc& io) {
        double st[NB];
        stepsOf(e, off, st);
        int q[FRAME];
        for (int b = 0; b < NB; b++)
            for (int k = EDGES[b]; k < EDGES[b + 1]; k++) {
                const int v = (int)std::nearbyint(X[k] / st[b]);
                q[k] = std::min(ESC_C + 255, std::max(-(ESC_C + 255), v));
            }
        Ctx ctx;
        codeEnergyEnc(io, ctx, e);
        codeCoefsEnc(io, ctx, q);
    };
    const int64_t budget = BITS - HDR_TYPE - HDR_OFF;
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
    if (bestOff < 0) {   // even the coarsest does not fit: energies only
        Enc io;
        Ctx ctx;
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
    for (int i = 0; i < BITS; i++) outBits[i] = i < (int)bits.size() ? (uint8_t)bits[i] : 0;
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

// Returns false for an implausible or reserved frame.
bool decodeFrame(const uint8_t* a, DecState& st, double* X) {
    int typ = 0;
    for (int i = 0; i < HDR_TYPE; i++) typ = (typ << 1) | a[i];
    if (typ != T_SPEECH && typ != T_SILENCE) return false;
    int off = 0;
    for (int i = 0; i < HDR_OFF; i++) off = (off << 1) | a[HDR_TYPE + i];
    Dec io(a + HDR_TYPE + HDR_OFF, BITS - HDR_TYPE - HDR_OFF);
    Ctx ctx;
    int e[NB];
    codeEnergyDec(io, ctx, e);
    double rms[NB], steps[NB];
    rmsQ(e, rms);
    int q[FRAME];
    double qa[FRAME];
    if (typ == T_SPEECH) {
        codeCoefsDec(io, ctx, q);
        for (int k = 0; k < FRAME; k++) qa[k] = q[k];
        stepsOf(e, off, steps);
    } else {
        for (int k = 0; k < FRAME; k++) qa[k] = 0;
        for (int b = 0; b < NB; b++) steps[b] = 1.0;
    }
    if (typ == T_SPEECH) {   // integrity: coefficients must fit the coded band energy
        for (int b = 0; b < NB; b++) {
            const int w = EDGES[b + 1] - EDGES[b];
            double s = 0;
            for (int k = EDGES[b]; k < EDGES[b + 1]; k++) { const double c = qa[k] * steps[b]; s += c * c; }
            if (s > 3.0 * rms[b] * rms[b] * w + 4.0 * steps[b] * steps[b]) return false;
        }
    }
    if ((int64_t)io.pos > (int64_t)(BITS - HDR_TYPE - HDR_OFF) + 32 + 2) return false;   // ran past the frame end
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
        if (nzc) {
            double amp = std::sqrt(rem / nzc) * 0.7;
            if (typ == T_SPEECH) amp = std::min(amp, 0.5 * steps[b]);
            for (int k = EDGES[b]; k < EDGES[b + 1]; k++)
                if (qa[k] == 0) X[k] = X[k] + nz[k] * amp * s3;
        }
    }
    return true;
}

}  // namespace

std::vector<uint8_t> KaalCodec::encode(const int16_t* pcm, size_t n) const {
    const size_t nIn = (n + FRAME - 1) / FRAME, nf = nIn + 1;
    std::vector<double> pad((nf + 1) * FRAME, 0.0);
    for (size_t i = 0; i < n; i++) pad[FRAME + i] = pcm[i];
    std::vector<uint8_t> out(nf * BITS);
    double X[FRAME];
    for (size_t t = 0; t < nf; t++) {
        mdct(&pad[t * FRAME], X);
        encodeFrame(X, &out[t * BITS]);
    }
    return out;
}

namespace {
// One decoder step: ptr == nullptr is a frame that is not valid by shape. Writes FRAME samples.
void stepFrame(const uint8_t* frame, DecState& st, int16_t* pcm) {
    double X[FRAME];
    bool ok = false;
    if (frame) {
        bool binary = true;
        for (int i = 0; i < BITS; i++) if (frame[i] > 1) { binary = false; break; }
        if (binary) ok = decodeFrame(frame, st, X);
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
}

std::vector<int16_t> decodeImpl(const std::vector<const uint8_t*>& frames) {
    DecState st;
    std::vector<int16_t> pcm(frames.size() * FRAME);
    for (size_t t = 0; t < frames.size(); t++) stepFrame(frames[t], st, &pcm[t * FRAME]);
    return pcm;
}
}  // namespace

struct KaalCodec::StreamDecoder::Impl { DecState st; };
KaalCodec::StreamDecoder::StreamDecoder() : p_(new Impl) {}
KaalCodec::StreamDecoder::~StreamDecoder() = default;
void KaalCodec::StreamDecoder::reset() { p_.reset(new Impl); }
void KaalCodec::StreamDecoder::push(const uint8_t* bits, int16_t* pcm) { stepFrame(bits, p_->st, pcm); }

std::vector<int16_t> KaalCodec::decode(const uint8_t* bits, size_t nFrames) const {
    std::vector<const uint8_t*> f(nFrames);
    for (size_t t = 0; t < nFrames; t++) f[t] = bits + t * BITS;
    return decodeImpl(f);
}

std::vector<int16_t> KaalCodec::decode(const std::vector<uint8_t>& bits) const {
    const size_t full = bits.size() / BITS, nf = (bits.size() + BITS - 1) / BITS;
    std::vector<const uint8_t*> f(nf, nullptr);
    for (size_t t = 0; t < full; t++) f[t] = bits.data() + t * BITS;
    return decodeImpl(f);
}

std::vector<int16_t> KaalCodec::decode(const std::vector<std::vector<uint8_t>>& frames) const {
    std::vector<const uint8_t*> f(frames.size(), nullptr);
    for (size_t t = 0; t < frames.size(); t++) if (frames[t].size() == (size_t)BITS) f[t] = frames[t].data();
    return decodeImpl(f);
}

}  // namespace dect2
