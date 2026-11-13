// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/drm/drm_find.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/common/diag.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace dect2 {

namespace {
constexpr double kBinHz = 50, kAvgSec = 0.6, kTestSec = 1.6, kKeepSec = 30;
struct Numerology { int tu, tg; };   // samples at 12 kHz
const Numerology kModes[4] = {{288, 32}, {256, 64}, {176, 64}, {112, 88}};
const Numerology kModeE = {216, 24};  // at 96 kHz
}

int drmGuardTest(const cf32* x, size_t n, bool modeE, float& corr, float& peakOverMean) {
    corr = 0; peakOverMean = 0;
    int best = -1;
    const int first = modeE ? 4 : 0, last = modeE ? 4 : 3;
    for (int m = first; m <= last; m++) {
        const Numerology nu = modeE ? kModeE : kModes[m];
        const int ts = nu.tu + nu.tg;
        if (n < (size_t)(nu.tu + 8 * ts)) continue;
        std::vector<cf32> acc((size_t)ts, cf32(0, 0));
        std::vector<float> pow((size_t)ts, 0.f);
        const size_t end = n - (size_t)nu.tu;
        for (size_t i = 0; i < end; i++) {
            const cf32 z = x[i] * std::conj(x[i + (size_t)nu.tu]);
            acc[i % (size_t)ts] += z;
            pow[i % (size_t)ts] += 0.5f * (std::norm(x[i]) + std::norm(x[i + (size_t)nu.tu]));
        }
        // the moving sum over a guard interval, on the ring of symbol phases
        cf32 s(0, 0); float e = 0;
        for (int i = 0; i < nu.tg; i++) { s += acc[(size_t)i]; e += pow[(size_t)i]; }
        std::vector<float> rho((size_t)ts);
        float peak = 0, sum = 0; int at = 0;
        for (int i = 0; i < ts; i++) {
            const float r = e > 0 ? std::abs(s) / e : 0.f;
            rho[(size_t)i] = r; sum += r;
            if (r > peak) { peak = r; at = i; }
            s += acc[(size_t)((i + nu.tg) % ts)] - acc[(size_t)i];
            e += pow[(size_t)((i + nu.tg) % ts)] - pow[(size_t)i];
        }
        // the floor away from the peak (a triangle two guard intervals wide around it)
        float other = 0; int cnt = 0;
        for (int i = 0; i < ts; i++) {
            int d = std::abs(i - at); d = std::min(d, ts - d);
            if (d > nu.tg) { other += rho[(size_t)i]; cnt++; }
        }
        const float mean = cnt ? other / cnt : sum / ts;
        const float pom = mean > 0 ? peak / mean : 0.f;
        if (pom > peakOverMean) { peakOverMean = pom; corr = peak; best = m; }
    }
    return best;
}

double drmReferenceFromBlock(double lo, double hi) {
    const double w = hi - lo;
    if (w < 7000) return lo - 100;            // carriers start 2 steps above the reference: the edge is just above it
    if (w < 14000) return 0.5 * (lo + hi);
    if (w < 40000) return lo + 4400;
    return 0.5 * (lo + hi);                   // mode E: centred
}

struct DrmFinder::Impl {
    struct Cand {
        double loHz = 0, hiHz = 0, ref = 0, snrDb = 0;
        bool modeE = false;
        Channelizer ch;
        std::vector<cf32> buf;
        size_t want = 0;
        int goodRuns = 0, badRuns = 0;
        double t0 = 0;
    };
    mutable std::mutex mu;
    double rate = 0, centre = 0, now = 0;
    int nfft = 0, nAvg = 0, avgDone = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> win;
    std::vector<cf32> frame, work;
    size_t fill = 0;
    std::vector<double> acc;
    std::vector<std::unique_ptr<Cand>> cands;
    struct Pending { double lo, hi, ref; int count; double snr, t; };
    std::vector<Pending> pending;
    std::vector<DrmSpanHit> confirmed;
    bool changed = false, inScan = false;

    void cfg(double r, double c) {
        rate = r; centre = c; now = 0; avgDone = 0; fill = 0; cands.clear(); confirmed.clear(); pending.clear(); changed = true;
        nfft = 256; while (nfft < r / kBinHz && nfft < (1 << 20)) nfft <<= 1;
        nAvg = std::max(4, (int)std::lround(kAvgSec * r / nfft));
        fft.reset(new Fft(nfft));
        win.resize((size_t)nfft);
        for (int i = 0; i < nfft; i++) win[(size_t)i] = 0.5f - 0.5f * (float)std::cos(2 * M_PI * (i + 0.5) / nfft);
        frame.assign((size_t)nfft, cf32(0, 0)); work = frame; acc.assign((size_t)nfft, 0.0);
    }
    double snap(double rel) const {
        if (centre <= 0) return rel;
        return std::round(centre + rel) - centre;           // to the Hz: the receiver's own frequency search takes it from there
    }
    void spectrum() {
        // the noise floor per 30 % quantile of the whole span in 20 kHz sections, then runs above it
        const double hzPerBin = rate / nfft;
        const int n = nfft;
        std::vector<float> p((size_t)n);
        for (int i = 0; i < n; i++) p[(size_t)i] = (float)acc[(size_t)((i + n / 2) % n)];         // DC in the middle
        // smooth over 300 Hz
        const int sm = std::max(1, (int)std::lround(300.0 / hzPerBin));
        std::vector<float> s((size_t)n, 0.f); double run = 0;
        for (int i = 0; i < n; i++) { run += p[(size_t)i]; if (i >= sm) run -= p[(size_t)(i - sm)]; s[(size_t)std::max(0, i - sm / 2)] = (float)(run / std::min(i + 1, sm)); }
        const int sect = std::max(32, (int)std::lround(40000.0 / hzPerBin));
        std::vector<float> floorDb((size_t)n);
        std::vector<float> tmp;
        std::vector<float> all = s; std::nth_element(all.begin(), all.begin() + n / 3, all.end());
        const float global = all[(size_t)(n / 3)];
        for (int i = 0; i < n; i += sect) {
            tmp.assign(s.begin() + i, s.begin() + std::min(n, i + sect));
            std::nth_element(tmp.begin(), tmp.begin() + (long)tmp.size() / 4, tmp.end());
            const float f = std::min(global * 2.f, tmp[tmp.size() / 4]);    // never above twice the span's floor: a section full of signals has no floor of its own
            for (int j = i; j < std::min(n, i + sect); j++) floorDb[(size_t)j] = f;
        }
        const float on = 3.2f;       // 5 dB after the smoothing
        const int edge = (int)(n * 0.03);
        for (int i = edge; i < n - edge;) {
            if (!(s[(size_t)i] > on * floorDb[(size_t)i])) { i++; continue; }
            int j = i, lastOn = i;
            const int gap = std::max(1, (int)std::lround(700.0 / hzPerBin));
            while (j < n - edge && j - lastOn <= gap) { if (s[(size_t)j] > on * floorDb[(size_t)j]) lastOn = j; j++; }
            const double lo = (i - n / 2) * hzPerBin, hi = (lastOn - n / 2) * hzPerBin, w = hi - lo;
            consider(p, i, lastOn, lo, hi, w, floorDb[(size_t)i], hzPerBin);
            i = j;
        }
        const size_t before = cands.size(); (void)before;
    }
    void consider(const std::vector<float>& p, int a, int b, double lo, double hi, double w, float flr, double hzPerBin) {
        const bool e = w > 75000 && w < 115000 && rate >= 192000;
        if (!e && w >= 23000 && w < 400000 && !inScan) {   // a block too wide for DRM30: neighbours touching each other (an AM station's sideband on a DRM channel). Windows of 10 kHz, each tried on its own
            inScan = true;
            const int win = (int)std::lround(10000.0 / hzPerBin), hop = (int)std::lround(5000.0 / hzPerBin);
            for (int i = a; i + win <= b && cands.size() < 32; i += hop) consider(p, i, i + win, lo + (i - a) * hzPerBin, lo + (i - a + win) * hzPerBin, 10000.0, flr, hzPerBin);
            inScan = false;
            return;
        }
        if (!e && !(w > 3500 && w < 23000)) return;
        // a carrier line: AM (or a bare carrier beside a block) is not DRM
        std::vector<float> v(p.begin() + a, p.begin() + b + 1);
        const float peak = *std::max_element(v.begin(), v.end());
        std::nth_element(v.begin(), v.begin() + (long)v.size() / 2, v.end());
        const float med = v[v.size() / 2];
        if (peak > 25.f * med) return;
        const double ref = snap(drmReferenceFromBlock(lo, hi));
        for (auto& c : cands) if (sameBlock(c->loHz, c->hiHz, lo, hi)) return;
        if (cands.size() >= 32) return;
        auto c = std::make_unique<Cand>();
        c->loHz = lo; c->hiHz = hi; c->ref = ref; c->modeE = e; c->snrDb = 10.0 * std::log10(std::max(1e-9f, med / flr)); c->t0 = now;
        const double out = e ? 96000.0 : 12000.0;
        if (!c->ch.configure(rate, out, 0.5 * (lo + hi), e ? 100000.0 : std::min(11000.0, w + 1500.0))) { DIAG_DEBUG_EVERY(331615, 10, "A DRM candidate at %.0f Hz offset could not get a channel filter for the test capture, so it is skipped.", 0.5 * (lo + hi)); return; }
        c->want = (size_t)(kTestSec * out);
        (void)hzPerBin;
        cands.push_back(std::move(c));
    }
    static bool sameBlock(double lo1, double hi1, double lo2, double hi2) {
        const double ov = std::min(hi1, hi2) - std::max(lo1, lo2);
        return ov > 0.4 * std::min(hi1 - lo1, hi2 - lo2);
    }
    void judge(Cand& c) {
        float corr = 0, pom = 0;
        const int m = drmGuardTest(c.buf.data(), c.buf.size(), c.modeE, corr, pom);
        if (m < 0 || corr < 0.12f || pom < 3.0f) return;
        for (auto& h : confirmed) if (sameBlock(h.widthHz > 0 ? h.loHz : h.hz, h.hiHz, c.loHz, c.hiHz)) { h.corr = corr; h.mode = m; h.lastSec = now; return; }
        // two confirmations of one block, however it was cut, before a receiver is started: the widest cut gives the reference (a fragment gives a wrong one)
        Pending* p = nullptr;
        for (auto& q : pending) if (sameBlock(q.lo, q.hi, c.loHz, c.hiHz)) p = &q;
        if (!p) { pending.push_back({c.loHz, c.hiHz, c.ref, 0, c.snrDb, now}); p = &pending.back(); }
        p->count++; p->t = now;
        if (c.hiHz - c.loHz > p->hi - p->lo) { p->lo = c.loHz; p->hi = c.hiHz; p->ref = c.ref; p->snr = c.snrDb; }
        if (p->count < 2) return;
        DrmSpanHit h; h.hz = p->ref; h.loHz = p->lo; h.hiHz = p->hi; h.widthHz = p->hi - p->lo; h.mode = m; h.corr = corr; h.snrDb = (float)p->snr; h.lastSec = now;
        confirmed.push_back(h); changed = true;
        pending.erase(pending.begin() + (p - pending.data()));
    }
    void feed(const cf32* x, size_t n) {
        if (rate <= 0) return;
        std::lock_guard<std::mutex> g(mu);
        size_t i = 0;
        while (i < n) {
            const size_t k = std::min(n - i, (size_t)nfft - fill);
            std::copy(x + i, x + i + k, frame.begin() + (long)fill);
            fill += k; i += k;
            if (fill == (size_t)nfft) {
                fill = 0;
                for (int b = 0; b < nfft; b++) work[(size_t)b] = frame[(size_t)b] * win[(size_t)b];
                fft->forward(work.data());
                for (int b = 0; b < nfft; b++) acc[(size_t)b] += std::norm(work[(size_t)b]);
                if (++avgDone >= nAvg) { spectrum(); std::fill(acc.begin(), acc.end(), 0.0); avgDone = 0; }
            }
        }
        now += (double)n / rate;
        for (auto it = cands.begin(); it != cands.end();) {
            Cand& c = **it;
            c.ch.process(x, n, c.buf);
            if (c.buf.size() >= c.want) { judge(c); it = cands.erase(it); } else ++it;
        }
        for (auto it = pending.begin(); it != pending.end();) { if (now - it->t > 10) it = pending.erase(it); else ++it; }
        for (auto it = confirmed.begin(); it != confirmed.end();) {
            if (now - it->lastSec > kKeepSec) { it = confirmed.erase(it); changed = true; } else ++it;
        }
    }
};

DrmFinder::DrmFinder() : p_(new Impl()) {}
DrmFinder::~DrmFinder() = default;
void DrmFinder::configure(double r, double c) { std::lock_guard<std::mutex> g(p_->mu); p_->cfg(r, c); }
void DrmFinder::setCentre(double c) { std::lock_guard<std::mutex> g(p_->mu); p_->centre = c; p_->confirmed.clear(); p_->pending.clear(); p_->cands.clear(); p_->changed = true; }
void DrmFinder::reset() { std::lock_guard<std::mutex> g(p_->mu); if (p_->rate > 0) p_->cfg(p_->rate, p_->centre); }
void DrmFinder::feed(const cf32* x, size_t n) { p_->feed(x, n); }
bool DrmFinder::takeChanged(std::vector<DrmSpanHit>& out) { std::lock_guard<std::mutex> g(p_->mu); if (!p_->changed) return false; p_->changed = false; out = p_->confirmed; return true; }
std::vector<DrmSpanHit> DrmFinder::hits() const { std::lock_guard<std::mutex> g(p_->mu); return p_->confirmed; }
int DrmFinder::candidates() const { std::lock_guard<std::mutex> g(p_->mu); return (int)p_->cands.size(); }

} // namespace dect2
