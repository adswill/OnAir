// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Narrow Bandwidth Television and mechanical television: see atv_nbtv.h.
#include "dect2/atv/atv_nbtv.h"
#include "dect2/atv/atv_std.h"
#include "atv_dsp.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/common/diag.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace dect2 {

using namespace atvdsp;

std::string NbtvFormat::name() const {
    char b[96];
    const bool std32 = lines == 32 && std::fabs(frameHz - 12.5) < 1e-6 && vertical;
    snprintf(b, sizeof b, "%s %d lines %g/s", std32 ? "NBTV" : "mechanical", lines, frameHz);
    return b;
}

NbtvFormat nbtvStandard() { return NbtvFormat(); }

NbtvFormat nbtvMechanical(int lines, double frameHz) {
    NbtvFormat f;
    f.lines = std::max(24, std::min(240, lines));
    f.frameHz = frameHz > 0 ? frameHz : 12.5;
    return f;
}

float nbtvTestPicture(int pattern, double x, double y) {
    static const float kBarY[8] = {1.f, 0.6651f, 0.5305f, 0.4405f, 0.3095f, 0.2195f, 0.0849f, 0.f};   // the EBU 100/0/75/0 bars, 0.299 R + 0.587 G + 0.114 B
    x = std::min(0.999999, std::max(0.0, x)); y = std::min(0.999999, std::max(0.0, y));
    auto bars = [&]() { return kBarY[(int)(x * 8)]; };
    if (pattern == 1) return bars();
    if (pattern == 2) return (float)y;
    if (y < 0.45) return bars();
    if (y < 0.55) return 0.5f;
    return (float)((y - 0.55) / 0.45);
}

// ------------------------------------------------------------------------------------------------ generator

AtvNbtvGen::AtvNbtvGen(const NbtvFormat& f, double rate, int pattern) : f_(f), rate_(rate), pattern_(pattern) {}

float AtvNbtvGen::at(double t) const {
    const NbtvFormat& F = f_;
    const double lp = t * F.lineHz();
    const double L = std::floor(lp);
    const double u = lp - L;
    const int64_t Li = (int64_t)L;
    const int li = (int)(((Li % F.lines) + F.lines) % F.lines);
    const double syncLen = F.syncFrac * (li == 0 ? F.frameSyncMul : 1.0);
    if (u < syncLen) return 0.f;
    const double a0 = F.activeStartFrac(), a1 = 1.0 - F.endFrac;
    // black after the sync pulse (after the long one of the first line too: its picture starts that much later)
    if (u < std::max(a0, syncLen + F.porchFrac) || u >= a1) return (float)F.blackLevel;
    const double s = (u - a0) / F.activeFrac(), acr = (li + 0.5) / F.lines;
    const float Y = F.vertical ? nbtvTestPicture(pattern_, acr, s) : nbtvTestPicture(pattern_, s, acr);
    return (float)(F.blackLevel + (1.0 - F.blackLevel) * Y);
}

void AtvNbtvGen::generate(float* out, size_t n) {
    // each sample is the mean of four points across its interval: the sharp edges of the picture do not alias as much
    const double dt = 1.0 / rate_;
    for (size_t i = 0; i < n; i++, n_++) {
        const double t = (double)n_ * dt;
        out[i] = 0.25f * (at(t - 0.375 * dt) + at(t - 0.125 * dt) + at(t + 0.125 * dt) + at(t + 0.375 * dt));
    }
}

struct AtvNbtvRfGen::Impl {
    NbtvRfConfig c;
    AtvNbtvGen gen;
    NoiseSource noise;
    double ph = 0;
    std::vector<float> v;
    explicit Impl(const NbtvRfConfig& cc) : c(cc), gen(cc.fmt, cc.rate, cc.pattern), noise(cc.seed) {}
};

AtvNbtvRfGen::AtvNbtvRfGen(const NbtvRfConfig& c) : p_(std::make_unique<Impl>(c)) {}
AtvNbtvRfGen::~AtvNbtvRfGen() = default;
const NbtvRfConfig& AtvNbtvRfGen::config() const { return p_->c; }

void AtvNbtvRfGen::generate(cf32* out, size_t n) {
    Impl& s = *p_;
    s.v.resize(n);
    s.gen.generate(s.v.data(), n);
    const double fs = s.c.rate, lvl = s.c.level;
    for (size_t i = 0; i < n; i++) {
        double amp = lvl, f = s.c.cfoHz;
        if (s.c.fm) f += s.c.devHz * (2.0 * s.v[i] - 1.0);
        else amp = lvl * (0.15 + 0.85 * s.v[i]);
        s.ph += 2 * kPi * f / fs;
        if (s.ph > kPi) s.ph -= 2 * kPi; else if (s.ph < -kPi) s.ph += 2 * kPi;
        out[i] = cf32((float)(amp * std::cos(s.ph)), (float)(amp * std::sin(s.ph)));
    }
    if (s.c.cnrDb < 100) {
        // noise against the carrier in twice the video bandwidth (the width of an AM channel)
        const double bw = 2 * s.c.fmt.videoBwHz();
        const float sigma = (float)(lvl * std::pow(10.0, -s.c.cnrDb / 20.0) * std::sqrt(fs / bw));
        s.noise.add(out, n, sigma);
    }
}

// ------------------------------------------------------------------------------------------------ decoder

struct AtvNbtvDecoder::Impl {
    AtvNbtvDecoder* owner = nullptr;
    double rate = 48000;
    NbtvFormat f;
    double P0 = 120, P = 120, syncW = 7;
    // the signal
    std::vector<float> ring, sm;
    size_t mask = 0;
    uint64_t nAbs = 0;
    int L = 1;                    // the slicer's running mean
    double runSum = 0;
    int polarity = 1;             // -1: the input is turned round (sync pulses at the top)
    int startPolarity = 1, polTries = 0;
    uint64_t lockAt = 0;
    int topAfter = 0, topLines = 0;
    uint64_t noLockSince = 0;
    // levels of the slicer signal
    double lo = 0, hi = 0;
    bool levelsInit = false;
    // pulses
    bool inPulse = false;
    double pT0 = 0;
    float prev = 0;
    // line loop
    bool locked = false;
    int good = 0, miss = 0;
    double lastT0 = -1e18, tNext = 0;
    float syncQ = 0;
    struct LineRec { double t; bool frame; bool pulse; };
    std::deque<LineRec> q;
    // picture
    int lineIdx = 0;
    int S = 1, W = 0, H = 0, along = 0;
    std::vector<float> pic;       // lines x along, luminance 0..1
    std::vector<uint8_t> have;    // the column was filled in this picture
    double tipE = 0, blackE = 0, noiseE = 0;
    bool levE = false;
    uint64_t lineCount = 0, frameCount = 0, frameSeq = 0;
    uint64_t lastFrameSyncAt = 0;
    std::vector<float> lineWave;
    int waveSkip = 0;

    void configure(double r, const NbtvFormat& fm) {
        rate = r; f = fm;
        P0 = rate / f.lineHz();
        syncW = f.syncFrac * P0;
        size_t sz = 1;
        while (sz < (size_t)(6 * P0 + 64)) sz <<= 1;
        ring.assign(sz, 0.f); sm.assign(sz, 0.f); mask = sz - 1;
        L = std::max(1, (int)std::lround(syncW / 5));
        along = f.alongLine();
        const int w0 = f.vertical ? f.lines : along, h0 = f.vertical ? along : f.lines;
        S = std::max(1, (int)std::ceil(384.0 / std::max(w0, h0)));
        W = w0 * S; H = h0 * S;
        reset();
    }

    void reset() {
        std::fill(ring.begin(), ring.end(), 0.f); std::fill(sm.begin(), sm.end(), 0.f);
        nAbs = 0; runSum = 0; levelsInit = false; inPulse = false; prev = 0;
        unlock();
        polarity = 1; startPolarity = 1; polTries = 0; noLockSince = 0; lastFrameSyncAt = 0;
        pic.assign((size_t)f.lines * along, 0.f); have.assign((size_t)f.lines, 0);
        lineIdx = 0; levE = false; tipE = blackE = noiseE = 0;
        lineWave.clear();
    }

    void unlock() {
        locked = false; good = 0; miss = 0; lastT0 = -1e18; syncQ = 0; q.clear(); P = P0;
    }

    void doLog(const std::string& s) { if (owner && owner->log) owner->log(s); }

    float at(uint64_t n) const { return ring[n & mask]; }

    void process(const float* x, size_t n) {
        const float pol = (float)polarity;
        const double kLo = 1.0 / (3 * P0), kHi = 1.0 / (3 * P0);
        for (size_t i = 0; i < n; i++) {
            const float v = pol * x[i];
            ring[nAbs & mask] = v;
            runSum += v - (nAbs >= (uint64_t)L ? ring[(nAbs - (uint64_t)L) & mask] : 0.f);
            const float s = (float)(runSum / L);
            sm[nAbs & mask] = s;
            nAbs++;
            // levels: the slicer follows the bottom (sync tips) and the top of the signal, each decaying towards the other over a few lines
            if (!levelsInit) { lo = hi = s; levelsInit = true; }
            if (s < lo) lo = s; else lo += (s - lo) * kLo;
            if (s > hi) hi = s; else hi -= (hi - s) * kHi;
            const double span = hi - lo;
            if (span <= 1e-9) { prev = s; continue; }
            const double th = lo + 0.15 * span, hy = 0.04 * span;
            // the smoothed signal is delayed by (L - 1) / 2 samples
            const double dly = 0.5 * (L - 1);
            if (!inPulse) {
                if (s < th - hy && prev >= th - hy) {
                    const double fr = prev != s ? (prev - (th - hy)) / (prev - s) : 0.0;
                    pT0 = (double)(nAbs - 2) + fr - dly;
                    inPulse = true;
                }
            } else if (s > th + hy) {
                const double fr = prev != s ? ((th + hy) - prev) / (s - prev) : 0.0;
                const double t1 = (double)(nAbs - 2) + fr - dly;
                inPulse = false;
                onPulse(pT0, t1 - pT0);
            }
            prev = s;
            if (locked) checkMiss();
            if (!q.empty() && q.front().t + P + 2 <= (double)nAbs) lines();
        }
        lines();
        // Locked, but no picture sync for four pictures: the "sync pulses" may be the picture (a signal turned over can have dark stretches
        // of about the width of a sync pulse once a line). The other polarity is tried, once each way; a signal without picture sync then
        // stays with the polarity it started with.
        if (locked && polTries < 2 && nAbs - lockAt > (uint64_t)(4.0 * rate / f.frameHz) && (lastFrameSyncAt < lockAt || nAbs - lastFrameSyncAt > (uint64_t)(4.0 * rate / f.frameHz))) {
            polTries++;
            if (polTries < 2 || polarity != startPolarity) flip("no picture sync");
        }
        // no lines for two seconds: try the signal the other way up
        if (!locked) {
            if (!noLockSince) noLockSince = nAbs;
            if (nAbs - noLockSince > (uint64_t)(2.0 * rate)) flip("no line sync");
        } else noLockSince = 0;
    }

    void flip(const char* why) {
        polarity = -polarity; noLockSince = nAbs; levelsInit = false; inPulse = false;
        for (auto& v : ring) v = -v;
        for (auto& v : sm) v = -v;
        runSum = -runSum;
        unlock();
        DIAG_DEBUG(462039, "The NBTV decoder found no sync with this polarity (%s), so it flips the signal and tries again.", why);
        doLog(std::string("NBTV: ") + why + ", trying the signal " + (polarity < 0 ? "turned over" : "the right way up"));
    }

    // a pulse below the slicer level, w samples long
    void onPulse(double t0, double w) {
        const bool lineSync = w > 0.45 * syncW && w < 1.9 * syncW;
        const bool frameSync = w > 2.1 * syncW && w < 1.6 * f.frameSyncMul * syncW;
        if (!lineSync && !frameSync) return;
        if (!locked) {
            const double d = t0 - lastT0;
            if (std::fabs(d - P0) < 0.04 * P0) good++; else if (std::fabs(d - 2 * P0) > 0.08 * P0) good = 0;
            lastT0 = t0;
            if (good >= 6) {
                locked = true; P = P0; tNext = t0 + P; miss = 0; syncQ = 0.8f; lockAt = nAbs; topAfter = topLines = 0;
                char b[120];
                snprintf(b, sizeof b, "NBTV: line sync found, %d lines, %.2f lines a second", f.lines, f.lineHz());
                doLog(b);
            }
            return;
        }
        // the pulse of the line due now, or of the next one if the one due was missed a moment ago
        double e = t0 - tNext;
        if (std::fabs(e) > 0.1 * P) return;
        const double tl = tNext + 0.4 * e;
        P += 0.03 * e;
        P = std::min(std::max(P, 0.97 * P0), 1.03 * P0);
        q.push_back({tl, frameSync, true});
        tNext = tl + P;
        miss = 0;
        syncQ += (1.f - syncQ) * 0.05f;
        if (frameSync) { lastFrameSyncAt = nAbs; polTries = 2; }      // picture sync: the polarity is right
    }

    void checkMiss() {
        const double now = (double)nAbs - 1 - 0.5 * (L - 1);
        if (now <= tNext + 0.12 * P || inPulse) return;
        q.push_back({tNext, false, false});
        tNext += P;
        syncQ += (0.f - syncQ) * 0.05f;
        if (++miss > 2 * f.lines + 8 || syncQ < 0.2f) { DIAG_DEBUG(733779, "The NBTV line sync was lost after %d missed lines, so the decoder searches for sync pulses again.", miss); doLog("NBTV: line sync lost"); unlock(); }
    }

    double meanAt(double a, double b) const {
        const uint64_t ia = (uint64_t)std::max(0.0, std::ceil(a)), ib = (uint64_t)std::max(0.0, std::floor(b));
        if (ib < ia) return at(ia);
        double s = 0;
        for (uint64_t k = ia; k <= ib; k++) s += at(k);
        return s / (double)(ib - ia + 1);
    }

    void lines() {
        while (!q.empty() && q.front().t + P + 2 <= (double)nAbs) {
            const LineRec r = q.front();
            q.pop_front();
            if (r.t < (double)nAbs - (double)ring.size() + 4) continue;   // too old for the ring
            oneLine(r);
        }
    }

    void oneLine(const LineRec& r) {
        lineCount++;
        const double t = r.t;
        if (r.frame) {
            if (lineIdx > 0) publish();
            lineIdx = 0;
        }
        // levels: the sync tip and the black porch after it (the frame sync's long pulse: the porch is further on)
        const double sw = f.syncFrac * P * (r.frame ? f.frameSyncMul : 1.0);
        if (r.pulse) {
            const double tip = meanAt(t + 0.25 * sw, t + 0.8 * sw);
            double var = 0; int c = 0;
            for (uint64_t k = (uint64_t)std::ceil(t + 0.25 * sw); k <= (uint64_t)std::floor(t + 0.8 * sw); k++) { const double d = at(k) - tip; var += d * d; c++; }
            const double pa = t + sw + 0.3 * f.porchFrac * P, pb = t + sw + 0.75 * f.porchFrac * P;
            const double blk = meanAt(pa, pb);
            if (!levE) { tipE = tip; blackE = blk; levE = blk > tip; if (c > 1) noiseE = var / (c - 1); }
            else { tipE += (tip - tipE) * 0.15; blackE += (blk - blackE) * 0.15; if (c > 1) noiseE += (var / (c - 1) - noiseE) * 0.05; }
        }
        const double g = (blackE - tipE) / f.blackLevel;            // signal units for the full sync-to-white range
        if (levE && g > 1e-9 && lineIdx < f.lines) {
            // the active part: `along` cells, each the mean of the samples in it
            const double a0 = t + (sw > f.syncFrac * P * 1.01 ? sw + f.porchFrac * P : f.activeStartFrac() * P), a1 = t + (1.0 - f.endFrac) * P;
            const double aS = t + f.activeStartFrac() * P, cell = (a1 - aS) / along;
            float* col = &pic[(size_t)lineIdx * along];
            for (int k = 0; k < along; k++) {
                const double ca = aS + k * cell, cb = ca + cell;
                if (cb <= a0) { col[k] = -1.f; continue; }               // hidden by the long frame sync pulse: filled from the next line
                const double v = meanAt(std::max(ca, a0), cb);
                const double y = ((v - tipE) / g - f.blackLevel) / (1.0 - f.blackLevel);
                col[k] = (float)std::min(1.2, std::max(-0.1, y));
            }
            have[(size_t)lineIdx] = 1;
            // The polarity check: the signal turned over has its real sync pulses at the top, and the "pulses" found are dark stretches of the
            // picture; then the top of the signal follows every pulse closely (the real sync pulse of the next line). The right way up that needs
            // white at the start of every line.
            if (r.pulse && hi > lo) {
                double mx = -1e30;
                for (uint64_t k = (uint64_t)std::ceil(t + sw); k <= (uint64_t)std::floor(t + sw + 0.15 * P); k++) mx = std::max(mx, (double)at(k));
                topAfter += mx > hi - 0.06 * (hi - lo) ? 1 : 0;
                if (++topLines >= 2 * f.lines) {
                    const bool wrong = topAfter > 0.85 * topLines;
                    topAfter = topLines = 0;
                    if (wrong) { flip("the pulses found are not the sync"); return; }
                }
            }
            if (++waveSkip >= 37) {
                waveSkip = 0;
                lineWave.assign(256, 0.f);
                for (int k = 0; k < 256; k++) lineWave[(size_t)k] = (float)((meanAt(t + P * k / 256.0, t + P * (k + 1) / 256.0) - tipE) / g);
            }
        }
        lineIdx++;
        if (lineIdx >= f.lines) {
            // no frame sync in this signal (or it was missed): the picture wraps by itself
            if (nAbs - lastFrameSyncAt > (uint64_t)(2.5 * rate / f.frameHz)) { publish(); lineIdx = 0; }
            else if (lineIdx > f.lines + 2) { publish(); lineIdx = 0; }
        }
    }

    void publish() {
        frameCount++;
        if (!owner || !owner->frameReady) { std::fill(have.begin(), have.end(), 0); return; }
        auto fr = std::make_shared<AtvFrame>();
        fr->seq = ++frameSeq; fr->width = W; fr->height = H; fr->fieldRate = f.frameHz; fr->colour = false; fr->field = 0;
        fr->rgba.assign((size_t)W * H * 4, 0);
        for (int li = 0; li < f.lines; li++) {
            float* col = &pic[(size_t)li * along];
            // samples hidden by the frame sync pulse: the neighbouring line's
            for (int k = 0; k < along; k++) if (col[k] < -0.5f) col[k] = li + 1 < f.lines ? pic[(size_t)(li + 1) * along + k] : 0.f;
        }
        for (int yy = 0; yy < H; yy++)
            for (int xx = 0; xx < W; xx++) {
                // vertical: line = column, the position along it = row; linear along the line, the line itself is a strip
                const int li = f.vertical ? xx / S : yy / S;
                const double pa = ((f.vertical ? yy : xx) + 0.5) / S - 0.5;
                const int k0 = std::max(0, std::min(along - 1, (int)std::floor(pa))), k1 = std::min(along - 1, k0 + 1);
                const double fr2 = std::min(1.0, std::max(0.0, pa - k0));
                const float* col = &pic[(size_t)li * along];
                const double y = col[k0] + fr2 * (col[k1] - col[k0]);
                const uint8_t g8 = (uint8_t)std::lround(std::min(1.0, std::max(0.0, y)) * 255);
                uint8_t* p = &fr->rgba[((size_t)yy * W + xx) * 4];
                p[0] = p[1] = p[2] = g8; p[3] = 255;
            }
        std::fill(have.begin(), have.end(), 0);
        owner->frameReady(fr);
    }

    NbtvStatus status() const {
        NbtvStatus s;
        s.lineLocked = locked && syncQ > 0.5f;
        s.frameLocked = s.lineLocked && lastFrameSyncAt > 0 && nAbs - lastFrameSyncAt < (uint64_t)(2.5 * rate / f.frameHz);
        s.lineHz = locked ? rate / P : 0;
        s.lineErrPpm = locked ? (rate / P / f.lineHz() - 1) * 1e6 : 0;
        s.syncQuality = syncQ;
        s.lines = lineCount; s.frames = frameCount;
        const double g = (blackE - tipE) / f.blackLevel;
        if (levE && g > 0) s.snrDb = noiseE > 0 ? (float)std::min(60.0, 20 * std::log10(g * (1 - f.blackLevel) / std::sqrt(noiseE))) : 60.f;
        s.lineWave = lineWave;
        return s;
    }
};

AtvNbtvDecoder::AtvNbtvDecoder() : p_(std::make_unique<Impl>()) { p_->owner = this; }
AtvNbtvDecoder::~AtvNbtvDecoder() = default;
void AtvNbtvDecoder::configure(double rate, const NbtvFormat& f) { p_->owner = this; p_->configure(rate, f); }
void AtvNbtvDecoder::reset() { p_->reset(); }
void AtvNbtvDecoder::process(const float* x, size_t n) { p_->process(x, n); }
NbtvStatus AtvNbtvDecoder::status() const { return p_->status(); }
const NbtvFormat& AtvNbtvDecoder::format() const { return p_->f; }
int AtvNbtvDecoder::scale() const { return p_->S; }

// ------------------------------------------------------------------------------------------------ the radio side

namespace {
// a streaming decimating FIR on I and Q
struct CDec {
    std::vector<float> h, hi, hq, xi, xq;
    int d = 1, cnt = 0;
    void design(std::vector<float> t, int dd) { h = std::move(t); d = std::max(1, dd); hi.assign(h.size() - 1, 0.f); hq = hi; cnt = 0; }
    void reset() { std::fill(hi.begin(), hi.end(), 0.f); std::fill(hq.begin(), hq.end(), 0.f); cnt = 0; }
    size_t run(const float* ii, const float* qq, size_t n, std::vector<float>& oi, std::vector<float>& oq) {
        const size_t nt = h.size(), base = nt - 1;
        xi.resize(base + n); xq.resize(base + n);
        memcpy(xi.data(), hi.data(), base * sizeof(float)); memcpy(xi.data() + base, ii, n * sizeof(float));
        memcpy(xq.data(), hq.data(), base * sizeof(float)); memcpy(xq.data() + base, qq, n * sizeof(float));
        size_t o = 0;
        const size_t j0 = (size_t)(d - 1 - cnt);
        oi.resize(n / (size_t)d + 2); oq.resize(oi.size());
        if (j0 < n) {
            o = (n - 1 - j0) / (size_t)d + 1;
            desamp(xi.data() + j0, d, h.data(), oi.data(), (int)o, (int)nt);
            desamp(xq.data() + j0, d, h.data(), oq.data(), (int)o, (int)nt);
        }
        cnt = (int)(((size_t)cnt + n) % (size_t)d);
        memcpy(hi.data(), xi.data() + n, base * sizeof(float)); memcpy(hq.data(), xq.data() + n, base * sizeof(float));
        return o;
    }
};
} // namespace

struct AtvNbtvRadio::Impl {
    double fs = 0, fsA = 0, fs1 = 0, fa = 0, dev = 5000;
    bool fm = false;
    NbtvFormat f;
    CDec sA, sB, sC;
    std::vector<float> ii, qq, aI, aQ, bI, bQ, det, zero, cI, cQ;
    cf32 prevZ = cf32(1, 0);
    double lvlSum = 0; size_t lvlN = 0;
    float carrierDb = -120;
    AtvNbtvDecoder dec;

    void configure(double rate, const NbtvFormat& fm_, bool isFm, double devHz) {
        fs = rate; f = fm_; fm = isFm; dev = devHz > 0 ? devHz : 5000;
        const double vbw = f.videoBwHz();
        const double hw = (fm ? dev + vbw : vbw) + 60e3;                   // the channel, with room for the transmitter to be 50 kHz off
        const int dA = std::max(1, (int)std::floor(fs / std::max(1e6, 10 * hw)));
        fsA = fs / dA;
        sA.design(lowpass(hw, std::max(hw * 1.2, fsA - hw), fs, 55, 255), dA);
        const int dB = std::max(1, (int)std::floor(fsA / (2.5 * hw)));
        fs1 = fsA / dB;
        sB.design(lowpass(hw, std::max(hw * 1.15, fs1 - hw), fsA, 55, 511), dB);
        const int dC = std::max(1, (int)std::floor(fs1 / std::max(4 * vbw, 8000.0)));
        fa = fs1 / dC;
        sC.design(lowpass(vbw, std::min(1.5 * vbw, 0.5 * fa), fs1, 50, 511), dC);
        dec.configure(fa, f);
        reset();
    }
    void reset() { sA.reset(); sB.reset(); sC.reset(); prevZ = cf32(1, 0); lvlSum = 0; lvlN = 0; dec.reset(); }

    void process(const cf32* x, size_t n) {
        ii.resize(n); qq.resize(n);
        for (size_t k = 0; k < n; k++) { ii[k] = x[k].real(); qq[k] = x[k].imag(); }
        const size_t na = sA.run(ii.data(), qq.data(), n, aI, aQ);
        const size_t nb = sB.run(aI.data(), aQ.data(), na, bI, bQ);
        if (!nb) return;
        det.resize(nb);
        for (size_t k = 0; k < nb; k++) {
            const cf32 z(bI[k], bQ[k]);
            if (fm) {
                const cf32 pr = z * std::conj(prevZ);
                prevZ = z;
                det[k] = (float)(0.5 + 0.5 * std::atan2((double)pr.imag(), (double)pr.real()) * fs1 / (2 * kPi) / dev);
            } else det[k] = std::abs(z);
            lvlSum += std::norm(z); lvlN++;
        }
        if (lvlN > (size_t)(0.1 * fs1)) { carrierDb = (float)(10 * std::log10(std::max(lvlSum / lvlN, 1e-20))); lvlSum = 0; lvlN = 0; }
        zero.assign(nb, 0.f);
        const size_t nc = sC.run(det.data(), zero.data(), nb, cI, cQ);
        if (nc) dec.process(cI.data(), nc);
    }
};

AtvNbtvRadio::AtvNbtvRadio() : p_(std::make_unique<Impl>()) {}
AtvNbtvRadio::~AtvNbtvRadio() = default;
void AtvNbtvRadio::configure(double fs, const NbtvFormat& f, bool fm, double devHz) { p_->configure(fs, f, fm, devHz); }
void AtvNbtvRadio::reset() { p_->reset(); }
void AtvNbtvRadio::process(const cf32* x, size_t n) { p_->process(x, n); }
AtvNbtvDecoder& AtvNbtvRadio::decoder() { return p_->dec; }
double AtvNbtvRadio::audioRate() const { return p_->fa; }
float AtvNbtvRadio::carrierDbfs() const { return p_->carrierDb; }

} // namespace dect2
