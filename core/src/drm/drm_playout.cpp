// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DRM playout: queue, concealment, clock drift (see drm_playout.h).
#include "dect2/drm/drm_playout.h"
#include "dect2/common/mathconst.h"
#include "dect2/common/window.h"
#include <algorithm>
#include <cmath>

namespace dect2 { namespace drm {

namespace {
constexpr int kHold = 240;             // frames kept back before the resampler: the join into a concealment is smoothed there (5 ms)
constexpr int kHistMax = 8192;         // history the concealment draws from
constexpr int kFull = 960;             // concealment at full level (20 ms) ...
constexpr int kFade = 9600;            // ... then faded out over 200 ms
constexpr int kRecover = 960;          // crossfade back into real sound (20 ms)
constexpr int kTaps = 16, kPhases = 256;   // resampler: 16 taps, 256 phases (linear between them)
enum : uint8_t { kReal = 0, kLost = 1, kPreroll = 2 };
}

struct Playout::Impl {
    double sf = 0.4;
    bool started = false;
    // the queue: stereo frames and what each one is
    std::vector<float> q;
    std::vector<uint8_t> qf;
    size_t qr = 0;
    uint64_t dry = 0;                  // frames the queue could not give since the last super frame
    double lastSig = -1, acc = 0;
    // the concealment
    std::vector<float> hs;             // what the concealment stage put out; the last kHold frames are not released yet
    size_t released = 0;               // frames of hs already handed to the resampler
    bool conc = false, recovering = false;
    int P = 0, pos = 0, rpos = 0;
    uint64_t n = 0;
    std::vector<float> B;              // one pitch period, looped
    // the resampler
    std::vector<float> rb;             // its input (stereo)
    double rt = kTaps / 2;             // input position of the next output frame
    double ratio = 1, lvl = -1;
    std::vector<float> tab;            // kPhases + 1 phases of kTaps taps
    // statistics
    uint64_t concealed = 0, muted = 0, real = 0, events = 0, trimmed = 0;

    Impl() {
        tab.assign((size_t)(kPhases + 1) * kTaps, 0.f);
        const double cut = 0.92, beta = 7.0;
        for (int ph = 0; ph <= kPhases; ph++) {
            const double f = (double)ph / kPhases;
            double sum = 0;
            for (int k = 0; k < kTaps; k++) {
                const double x = (double)(k - kTaps / 2 + 1) - f;   // taps at floor(t) - 7 .. floor(t) + 8
                const double u = x / (kTaps / 2);
                const double w = std::fabs(u) < 1 ? dect2::window::besselI0(beta * std::sqrt(1 - u * u), 50, 0.0) / dect2::window::besselI0(beta, 50, 0.0) : 0;
                const double a = dect2::math::kPi * cut * x;
                const double s = std::fabs(a) < 1e-12 ? 1.0 : std::sin(a) / a;
                tab[(size_t)(ph * kTaps + k)] = (float)(cut * s * w);
                sum += cut * s * w;
            }
            for (int k = 0; k < kTaps; k++) tab[(size_t)(ph * kTaps + k)] /= (float)sum;
        }
    }

    size_t qsize() const { return qf.size() - qr; }
    int sfFrames() const { return (int)std::lround(sf * kRate); }
    int marginFrames() const { return (int)std::lround(kMarginSecs * kRate); }

    void clear() {
        started = false;
        q.clear(); qf.clear(); qr = 0; dry = 0; lastSig = -1; acc = 0;
        hs.clear(); released = 0; conc = recovering = false; P = 0; pos = rpos = 0; n = 0; B.clear();
        rb.assign((size_t)kTaps * 2, 0.f); rt = kTaps / 2; ratio = 1; lvl = -1;
    }

    void push(float l, float r, uint8_t f) { q.push_back(l); q.push_back(r); qf.push_back(f); }

    // drop `ex` frames from the front of the queue, with a crossfade of 240 frames over the cut
    void trim(size_t ex) {
        const size_t L = 240;
        if (qsize() < ex + 2 * L) return;
        for (size_t i = 0; i < L; i++) {
            const float w = (float)(i + 0.5) / L;
            const size_t a = qr + i, b = qr + ex + i;
            q[2 * b] = q[2 * a] * (1 - w) + q[2 * b] * w;
            q[2 * b + 1] = q[2 * a + 1] * (1 - w) + q[2 * b + 1] * w;
            if (qf[a] != kReal) qf[b] = qf[a];
        }
        qr += ex;
        trimmed += ex;
    }

    void decoded(const float* x, size_t frames, const std::vector<std::pair<size_t, size_t>>& gaps) {
        if (!started) {
            started = true;
            for (int i = 0; i < marginFrames(); i++) push(0, 0, kPreroll);
        } else if (dry > 0) {
            // the queue ran empty: put this super frame where it belongs, so that the queue is at its nominal level again
            const long ins = (long)sfFrames() + marginFrames() - (long)(qsize() + frames);
            for (long i = 0; i < ins; i++) push(0, 0, kLost);
        }
        dry = 0;
        size_t g = 0;
        for (size_t i = 0; i < frames; i++) {
            while (g < gaps.size() && i >= gaps[g].first + gaps[g].second) g++;
            const bool lost = g < gaps.size() && i >= gaps[g].first;
            push(x[2 * i], x[2 * i + 1], lost ? kLost : kReal);
        }
        // Two super frames may come together (the multiplex frames are not aligned with the transmission frames: in DRM30 the second and the
        // third are complete at the end of the same frame, and then nothing comes for 800 ms); more than that is too much
        const size_t nominal = (size_t)(sfFrames() + marginFrames());
        if (qsize() > nominal + (size_t)sfFrames() + (size_t)(0.3 * kRate)) trim(qsize() - nominal - (size_t)sfFrames());
        if (qr > (size_t)kRate) { q.erase(q.begin(), q.begin() + (ptrdiff_t)(2 * qr)); qf.erase(qf.begin(), qf.begin() + (ptrdiff_t)qr); qr = 0; }
    }

    // the concealment starts: the pitch period of the last sound, looped
    void startConceal() {
        events++;
        conc = true; recovering = false; n = 0; pos = 0;
        const int have = (int)(hs.size() / 2);
        const int W = 480, maxP = 960, minP = 120;
        P = 0;
        if (have < W + minP + 120) return;            // nothing to repeat: silence
        auto m = [&](int i) { return 0.5f * (hs[2 * (size_t)i] + hs[2 * (size_t)i + 1]); };
        const int end = have;
        const int hiP = std::min(maxP, have - W - 120);
        double best = -1; int bp = 0;
        double e0 = 0;
        for (int i = end - W; i < end; i++) e0 += (double)m(i) * m(i);
        for (int p = minP; p <= hiP; p++) {
            double c = 0, e1 = 0;
            for (int i = end - W; i < end; i++) { const double v = m(i - p); c += (double)m(i) * v; e1 += v * v; }
            const double r = c / (std::sqrt(e0 * e1) + 1e-20);
            if (r > best) { best = r; bp = p; }
        }
        // a period that is not clearly there (noise, a chord): 20 ms; a short one is repeated as several periods, which buzzes less
        int p = best > 0.5 ? bp : std::min(960, hiP);
        while (p < 240 && 2 * p <= hiP) p *= 2;
        P = p;
        const int Lx = std::min(P / 4, 120);
        B.assign((size_t)P * 2, 0.f);
        for (int i = 0; i < P; i++) { B[2 * (size_t)i] = hs[2 * (size_t)(end - P + i)]; B[2 * (size_t)i + 1] = hs[2 * (size_t)(end - P + i) + 1]; }
        // the end of the period runs into what came before its start, so that the loop has no step
        for (int i = 0; i < Lx; i++) {
            const float w = (float)(i + 0.5f) / Lx;
            const size_t j = (size_t)(P - Lx + i), h = (size_t)(end - P - Lx + i);
            for (int c = 0; c < 2; c++) B[2 * j + c] = B[2 * j + c] * (1 - w) + hs[2 * h + c] * w;
        }
        // the last frames not yet released run into the same, so that the loop follows them without a step
        const int unrel = (int)((hs.size() / 2) - released);
        const int Lt = std::min(Lx, unrel);
        for (int i = 0; i < Lt; i++) {
            const float w = (float)(i + 0.5f) / Lt;
            const size_t idx = (size_t)(end - Lt + i), src = idx - (size_t)P;
            for (int c = 0; c < 2; c++) hs[2 * idx + c] = hs[2 * idx + c] * (1 - w) + hs[2 * src + c] * w;
        }
    }

    float gain() const {
        if (n < (uint64_t)kFull) return 1.f;
        if (n >= (uint64_t)(kFull + kFade)) return 0.f;
        return (float)(0.5 * (1 + std::cos(dect2::math::kPi * (double)(n - kFull) / kFade)));
    }
    void concNext(float& l, float& r) {
        if (P <= 0) { l = r = 0; n++; return; }
        const float g = gain();
        l = B[2 * (size_t)pos] * g; r = B[2 * (size_t)pos + 1] * g;
        pos = (pos + 1) % P;
        n++;
    }

    // one frame through the concealment stage into hs
    void stage(float x, float y, uint8_t f) {
        float l = x, r = y;
        if (f == kReal) {
            if (conc) {
                if (!recovering) { recovering = true; rpos = 0; }
                float cl, cr;
                concNext(cl, cr);
                const float w = (float)(rpos + 1) / kRecover;
                l = x * w + cl * (1 - w); r = y * w + cr * (1 - w);
                if (++rpos >= kRecover) conc = recovering = false;
            }
            real++;
        } else if (f == kLost) {
            if (!conc || recovering) startConceal();
            const bool silentNow = P <= 0 || gain() == 0.f;
            concNext(l, r);
            if (silentNow) muted++; else concealed++;
        } else {
            l = r = 0;
        }
        hs.push_back(l); hs.push_back(r);
    }

    // the frames of hs beyond the hold-back go to the resampler
    void release(std::vector<float>& out) {
        const size_t total = hs.size() / 2;
        if (total > released + kHold) {
            const size_t from = released, to = total - kHold;
            rb.insert(rb.end(), hs.begin() + (ptrdiff_t)(2 * from), hs.begin() + (ptrdiff_t)(2 * to));
            released = to;
        }
        if (total > (size_t)kHistMax + kHold + 4096) {
            const size_t cut = total - kHistMax - kHold;
            hs.erase(hs.begin(), hs.begin() + (ptrdiff_t)(2 * cut));
            released -= cut;
        }
        resample(out);
    }

    void resample(std::vector<float>& out) {
        const size_t nin = rb.size() / 2;
        const double step = 1.0 / ratio;
        if (ratio == 1.0 && rt == std::floor(rt)) {
            // exact: no filtering (no device to follow)
            const size_t i0 = (size_t)rt;
            if (nin > i0) out.insert(out.end(), rb.begin() + (ptrdiff_t)(2 * i0), rb.end());
            rt = (double)nin;
        } else {
            while (rt + kTaps / 2 < (double)nin) {
                const double fl = std::floor(rt);
                const double fr = (rt - fl) * kPhases;
                const int ph = (int)fr;
                const float a = (float)(fr - ph);
                const float* t0 = &tab[(size_t)ph * kTaps];
                const float* t1 = &tab[(size_t)(ph + 1) * kTaps];
                const size_t base = (size_t)fl - (kTaps / 2 - 1);
                float l = 0, r = 0;
                for (int k = 0; k < kTaps; k++) {
                    const float c = t0[k] + (t1[k] - t0[k]) * a;
                    l += rb[2 * (base + (size_t)k)] * c; r += rb[2 * (base + (size_t)k) + 1] * c;
                }
                out.push_back(l); out.push_back(r);
                rt += step;
            }
        }
        // keep what the filter still needs
        const size_t keepFrom = (size_t)std::max(0.0, std::floor(rt) - kTaps);
        if (keepFrom > 4096) { rb.erase(rb.begin(), rb.begin() + (ptrdiff_t)(2 * keepFrom)); rt -= (double)keepFrom; }
    }

    void advance(double sig, int devQ, std::vector<float>& out) {
        if (!started) { lastSig = sig; return; }
        if (lastSig < 0 || sig < lastSig) lastSig = sig;
        acc += (sig - lastSig) * kRate;
        lastSig = sig;
        long nf = (long)std::floor(acc);
        acc -= (double)nf;
        nf = std::min<long>(nf, 2 * kRate);
        if (devQ >= 0) {
            // the device's queue at its target: about 20 s to take out an error, at most 0.4 % faster or slower (inaudible)
            lvl = lvl < 0 ? devQ : lvl + (devQ - lvl) * 0.05;
            const double e = (lvl - kDeviceTargetFrames) / (double)kRate;
            ratio = 1.0 - std::max(-0.004, std::min(0.004, 0.05 * e));
        } else ratio = 1.0;
        for (long i = 0; i < nf; i++) {
            if (qr < qf.size()) { stage(q[2 * qr], q[2 * qr + 1], qf[qr]); qr++; }
            else { dry++; stage(0, 0, kLost); }
        }
        release(out);
    }
};

Playout::Playout() : p_(std::make_unique<Impl>()) { p_->clear(); }
Playout::~Playout() = default;
void Playout::reset() { p_->clear(); }
void Playout::setSuperFrameSecs(double s) { p_->sf = s > 0 ? s : 0.4; }
void Playout::decoded(const float* x, size_t frames, const std::vector<std::pair<size_t, size_t>>& gaps) { p_->decoded(x, frames, gaps); }
void Playout::advance(double sig, int devQ, std::vector<float>& out) { p_->advance(sig, devQ, out); }
bool Playout::started() const { return p_->started; }
double Playout::concealedSecs() const { return p_->concealed / (double)kRate; }
double Playout::mutedSecs() const { return p_->muted / (double)kRate; }
double Playout::realSecs() const { return p_->real / (double)kRate; }
uint64_t Playout::events() const { return p_->events; }
double Playout::trimmedSecs() const { return p_->trimmed / (double)kRate; }
double Playout::queueSecs() const { return p_->qsize() / (double)kRate; }
double Playout::ratio() const { return p_->ratio; }

}} // namespace dect2::drm
