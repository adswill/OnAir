// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CODAR / HF ocean radar monitor (see codar_rx.h).
#include "dect2/codar/codar_rx.h"
#include "dect2/codar/codar_tel.h"
#include "dect2/cw/cw_morse.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/results/results_io.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr float kThrDb = 10.f;           // a peak must stand this far above its bin's floor
constexpr int kMaxPeaks = 8;
constexpr int kMaxTracks = 32;
constexpr double kStaleSec = 0.08;       // a confirmed track survives gaps (blanking, pulses) this long
constexpr int kTentFrames = 6;           // a track with fewer than 4 points survives this many frames
constexpr double kMaxBwHz = 500e3;
constexpr double kMinRateHz = 10e3, kMaxRateHz = 3e6;   // Hz per second
constexpr double kEnvStep = 0.005;       // the keying envelope: one point per 5 ms
constexpr size_t kEnvMax = 8000;         // 40 s
constexpr double kIdEvery = 5.0, kIdWindow = 30.0;

struct Pt { int64_t k; double t, f; float snr, lvl; };
struct Trk { std::vector<Pt> pts; int64_t lastK = 0; double slope = 0; };

struct St {
    CdStation s;
    double absHz = 0, offHz = 0;          // centre: absolute (radio centre + offset) and from the radio's centre
    std::deque<double> starts;            // start times of the last sweeps
    double lastEventSec = -1e9;
    double histSum = 0; int histN = 0; double histNext = 0;
    std::deque<float> env;                // band power over the floor, dB, one point per kEnvStep
    double envAcc = 0; int envN = 0; double envNext = 0;
    double nextId = 0;
    std::string idWord;
};

double lin(double dB) { return std::pow(10.0, dB / 10.0); }

float pct(std::vector<float> v, double q) {
    if (v.empty()) return 0;
    const size_t i = std::min(v.size() - 1, (size_t)(q * (double)v.size()));
    std::nth_element(v.begin(), v.begin() + (long)i, v.end());
    return v[i];
}
}

struct CodarReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rateSet = 0, offsetSet = 0;
    std::atomic<bool> resetReq{true}, clearReq{false}, loadReq{false};
    std::atomic<double> centerHz{0};
    CodarTelemetry pub, loadedTel;

    // feed thread
    double rate = 0, binHz = 0;
    int N = 0, hop = 0, jlo = 0, jhi = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> win;
    std::vector<cf32> q, spec;
    std::vector<double> floorP, p;
    std::vector<float> e;
    int64_t k = 0, nIn = 0;
    double fr = 0;
    int warm = 0;
    std::vector<Trk> trks;
    std::vector<St> st;
    std::vector<CdEvent> events;
    std::vector<CdSweep> marks;
    int nextId = 1;
    uint64_t sweepsOk = 0, tracksBad = 0, wide = 0;
    float bestSnr = -100; double bestSnrAt = -1e9;
    double power = 0; int64_t nPower = 0;
    double nextReport = 0;
    std::vector<uint8_t> wf, rowMax;
    double rowEnd = 0;
    double lastSweepSec = -1e9;
    double t0s = 0;                       // signal time of the detector's first sample

    double sigSec() const { return rate > 0 ? (double)nIn / rate : 0; }

    void initFront() {
        N = 512;
        while (N < rate / 1000.0 * 0.75 && N < 8192) N *= 2;
        hop = N / 2;
        binHz = rate / N;
        fr = rate / hop;
        jlo = (int)(N / 2 - 0.45 * N); jhi = (int)(N / 2 + 0.45 * N);
        fft = std::make_unique<Fft>(N);
        win.resize((size_t)N);
        for (int i = 0; i < N; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / N));
        spec.assign((size_t)N, cf32());
        floorP.assign((size_t)N, 0.0);
        p.assign((size_t)N, 0.0);
        e.assign((size_t)N, 0.f);
    }

    void resetDetector() {
        q.clear(); k = 0; warm = 0; trks.clear();
        std::fill(floorP.begin(), floorP.end(), 0.0);
        rowMax.assign((size_t)kCdWfCols, 0);
        wf.assign((size_t)kCdWfCols * kCdWfRows, 0);
        rowEnd = sigSec() + 0.025;
        t0s = sigSec();
        for (auto& s : st) { s.starts.clear(); s.env.clear(); s.envAcc = 0; s.envN = 0; s.envNext = 0; s.nextId = 0; }
    }

    void clearAll() {
        st.clear(); events.clear(); marks.clear(); nextId = 1; sweepsOk = tracksBad = wide = 0;
        bestSnr = -100;
        resetDetector();
    }

    void takeRequests() {
        if (loadReq.exchange(false)) {
            std::lock_guard<std::mutex> lk(mu);
            st.clear();
            for (const CdStation& s : loadedTel.stations) {
                St x; x.s = s; x.s.firstSec = x.s.lastSec = sigSec();
                x.offHz = s.offsetKhz * 1e3; x.absHz = s.centreMhz > 0 ? s.centreMhz * 1e6 : x.offHz;
                st.push_back(std::move(x));
                nextId = std::max(nextId, s.id + 1);
            }
            events = loadedTel.events;
        }
    }

    // ---- one FFT frame
    void frame(const cf32* seg) {
        for (int i = 0; i < N; i++) spec[(size_t)i] = cf32(seg[i].real() * win[(size_t)i], seg[i].imag() * win[(size_t)i]);
        fft->forward(spec.data());
        for (int j = 0; j < N; j++) {   // shifted: index j is (j - N/2) bins from the centre
            const cf32& v = spec[(size_t)((j + N / 2) % N)];
            p[(size_t)j] = (double)v.real() * v.real() + (double)v.imag() * v.imag();
        }
        const double t = ((double)k * hop + 0.5 * N) / rate + t0();
        if (warm < 64) {
            for (int j = jlo; j <= jhi; j++) floorP[(size_t)j] += p[(size_t)j] / 64.0;
            warm++; k++;
            return;
        }
        int hot = 0;
        for (int j = jlo; j <= jhi; j++) {
            e[(size_t)j] = (float)(p[(size_t)j] / std::max(1e-30, floorP[(size_t)j]));
            if (e[(size_t)j] >= 6.3f) hot++;
        }
        const bool burst = hot > (jhi - jlo) / 10;
        if (burst) wide++;
        waterfall(t);
        if (!burst) {
            // peaks
            struct Pk { int j; float e; double f; };
            Pk pk[kMaxPeaks]; int np = 0;
            const float thr = (float)lin(kThrDb);
            for (int j = jlo + 2; j <= jhi - 2; j++) {
                const float v = e[(size_t)j];
                if (v < thr || v < e[(size_t)j - 1] || v < e[(size_t)j + 1] || v < e[(size_t)j - 2] || v < e[(size_t)j + 2]) continue;
                bool nearBy = false;
                for (int i = 0; i < np; i++) if (std::abs(pk[i].j - j) < 6) { nearBy = true; break; }   // the sidelobes of a stronger peak
                if (nearBy) { for (int i = 0; i < np; i++) if (std::abs(pk[i].j - j) < 6 && v > pk[i].e) { pk[i] = {j, v, 0}; } continue; }
                if (np < kMaxPeaks) pk[np++] = {j, v, 0};
                else { int w = 0; for (int i = 1; i < np; i++) if (pk[i].e < pk[w].e) w = i; if (v > pk[w].e) pk[w] = {j, v, 0}; }
            }
            std::sort(pk, pk + np, [](const Pk& a, const Pk& b) { return a.e > b.e; });
            for (int i = 0; i < np; i++) {
                const int j = pk[i].j;
                const double a = std::log(p[(size_t)j - 1] + 1e-30), b = std::log(p[(size_t)j] + 1e-30), c = std::log(p[(size_t)j + 1] + 1e-30);
                const double den = a - 2 * b + c;
                const double d = den < -1e-9 ? std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den)) : 0;
                pk[i].f = (j + d - N / 2) * binHz;
            }
            link(pk, np, t);
        }
        // the floor follows the noise (a bin the chirp passes through counts at most 3 times the floor)
        if (!burst) {
            const double al = 1.0 / std::max(50.0, fr);
            for (int j = jlo; j <= jhi; j++) {
                const double v = std::min(p[(size_t)j], 3.0 * floorP[(size_t)j]);
                floorP[(size_t)j] += al * (v - floorP[(size_t)j]);
            }
        }
        envelope(t);
        k++;
    }
    double t0() const { return t0s; }

    template <class PK> void link(PK* pk, int np, double t) {
        std::vector<char> used((size_t)np, 0);
        // existing tracks take the nearest peak inside their tolerance
        for (Trk& tr : trks) {
            const int64_t dk = k - tr.lastK;
            double pred = tr.pts.back().f + tr.slope * (double)dk;
            const double tol = binHz * (3 + (tr.pts.size() < 4 ? 0.7 * (double)dk : 0.0)) + 0.25 * std::fabs(tr.slope * (double)dk);
            int best = -1; double bd = tol;
            for (int i = 0; i < np; i++) if (!used[(size_t)i]) { const double d = std::fabs(pk[i].f - pred); if (d < bd) { bd = d; best = i; } }
            if (best < 0) continue;
            used[(size_t)best] = 1;
            const int j = pk[best].j;
            tr.pts.push_back({k, t, pk[best].f, (float)(10 * std::log10(pk[best].e)), (float)(10 * std::log10(p[(size_t)j] + 1e-30) - 20 * std::log10(N / 2.0))});
            tr.lastK = k;
            const size_t n = tr.pts.size();
            if (n >= 4) {   // slope over the last points, Hz per frame
                const size_t m = std::min<size_t>(n, 10);
                double sk = 0, sf = 0, skk = 0, skf = 0;
                for (size_t i = n - m; i < n; i++) { const double x = (double)(tr.pts[i].k - tr.pts[n - m].k), y = tr.pts[i].f; sk += x; sf += y; skk += x * x; skf += x * y; }
                const double den = (double)m * skk - sk * sk;
                if (den > 0) tr.slope = ((double)m * skf - sk * sf) / den;
            }
        }
        for (int i = 0; i < np; i++) {
            if (used[(size_t)i] || (int)trks.size() >= kMaxTracks) continue;
            Trk tr;
            const int j = pk[i].j;
            tr.pts.push_back({k, t, pk[i].f, (float)(10 * std::log10(pk[i].e)), (float)(10 * std::log10(p[(size_t)j] + 1e-30) - 20 * std::log10(N / 2.0))});
            tr.lastK = k;
            trks.push_back(std::move(tr));
        }
        // ended tracks
        for (size_t i = 0; i < trks.size();) {
            Trk& tr = trks[i];
            const double idle = (double)(k - tr.lastK) / fr;
            const bool conf = tr.pts.size() >= 4;
            if ((conf && idle > kStaleSec) || (!conf && k - tr.lastK > kTentFrames)) {
                if (conf) finish(tr);
                trks.erase(trks.begin() + (long)i);
            } else i++;
        }
    }

    // ---- a track ended: is it a sweep
    void finish(const Trk& tr) {
        const size_t n = tr.pts.size();
        const double tA = tr.pts.front().t, tB = tr.pts.back().t, dur = tB - tA;
        if (n < 12 || dur < 0.03) return;
        double st_ = 0, sf = 0, stt = 0, stf = 0;
        for (const Pt& x : tr.pts) { const double u = x.t - tA; st_ += u; sf += x.f; stt += u * u; stf += u * x.f; }
        const double den = (double)n * stt - st_ * st_;
        if (den <= 0) return;
        const double r = ((double)n * stf - st_ * sf) / den, a = (sf - r * st_) / (double)n;
        double ss = 0;
        for (const Pt& x : tr.pts) { const double d = x.f - (a + r * (x.t - tA)); ss += d * d; }
        const double rms = std::sqrt(ss / (double)n);
        const double bw = std::fabs(r) * dur;
        const bool okRate = std::fabs(r) >= kMinRateHz && std::fabs(r) <= kMaxRateHz;
        if (!okRate || bw < 4e3 || bw > kMaxBwHz || rms > 2.5 * binHz + 0.03 * bw || (double)n < 0.1 * dur * fr) { tracksBad++; return; }
        const double f0 = a, f1 = a + r * dur, centre = 0.5 * (f0 + f1);
        float snr = 0, lvl = -200;
        for (const Pt& x : tr.pts) { snr += x.snr; lvl = std::max(lvl, x.lvl); }
        snr /= (float)n;
        sweepsOk++;
        accept(tA, dur, centre, bw, r, snr, lvl, f0, f1);
    }

    void accept(double tA, double dur, double centre, double bw, double r, float snr, float lvl, double f0, double f1) {
        const double cH = centerHz.load();
        const double absHz = cH + centre;
        const double rate_ = std::fabs(r);
        St* hit = nullptr;
        bool fragment = false;
        for (St& s : st) {
            const double dc = std::fabs((cH > 0 ? absHz : centre) - (cH > 0 ? s.absHz : s.offHz));
            if (dc <= std::max(4e3, 0.15 * s.s.bwKhz * 1e3) && std::fabs(bw - s.s.bwKhz * 1e3) <= std::max(3e3, 0.25 * s.s.bwKhz * 1e3) && (s.s.dir > 0) == (r > 0)) { hit = &s; break; }
        }
        if (!hit)   // a piece of a sweep of a known radar (the ends faded): same rate, inside its width
            for (St& s : st) {
                const double lo = (cH > 0 ? s.absHz : s.offHz) - 0.5 * s.s.bwKhz * 1e3 - 3e3, hi = lo + s.s.bwKhz * 1e3 + 6e3;
                const double a = cH > 0 ? absHz : centre;
                if (a - 0.5 * bw >= lo && a + 0.5 * bw <= hi && std::fabs(rate_ - s.s.rateKhzS * 1e3) <= 0.2 * s.s.rateKhzS * 1e3 && (s.s.dir > 0) == (r > 0)) { hit = &s; fragment = true; break; }
            }
        if (!hit) {
            if ((int)st.size() >= (int)kCdStationMax) return;
            St x;
            x.s.id = nextId++;
            x.s.firstSec = tA; x.s.firstWall = (int64_t)std::time(nullptr);
            x.s.sweeps = 0;
            x.s.dir = r > 0 ? 1 : -1;
            x.s.bwKhz = bw / 1e3; x.s.rateKhzS = rate_ / 1e3;
            x.absHz = absHz; x.offHz = centre;
            st.push_back(std::move(x));
            hit = &st.back();
        }
        St& s = *hit;
        const double sec = tA + dur;
        s.s.lastSec = sec; s.s.lastWall = (int64_t)std::time(nullptr);
        s.s.snrDb = snr; s.s.snrMax = std::max(s.s.snrMax, snr); s.s.levelDb = lvl;
        if (snr > bestSnr || sec - bestSnrAt > 3) { bestSnr = snr; bestSnrAt = sec; }
        lastSweepSec = sec;
        CdSweep m; m.sec = tA; m.f0Khz = (float)(f0 / 1e3); m.f1Khz = (float)(f1 / 1e3); m.station = s.s.id;
        marks.push_back(m);
        if (marks.size() > kCdSweepMarks) marks.erase(marks.begin());
        if (fragment) return;
        const double w = s.s.sweeps < 3 ? 0.5 : 0.15;   // the numbers settle
        s.s.sweeps++;
        s.offHz += w * (centre - s.offHz); s.absHz += w * (absHz - s.absHz);
        s.s.bwKhz += w * (bw / 1e3 - s.s.bwKhz); s.s.rateKhzS += w * (rate_ / 1e3 - s.s.rateKhzS);
        s.s.offsetKhz = s.offHz / 1e3;
        s.s.centreMhz = cH > 0 ? s.absHz / 1e6 : 0;
        s.s.band = cH > 0 ? (codar::bandOf(s.s.centreMhz).empty() ? "outside the radar bands" : codar::bandOf(s.s.centreMhz)) : "";
        s.starts.push_back(tA);
        if (s.starts.size() > 10) s.starts.pop_front();
        if (s.starts.size() >= 4) {
            std::vector<double> d;
            for (size_t i = 1; i < s.starts.size(); i++) { const double x = s.starts[i] - s.starts[i - 1]; if (x > 0.05 && x < 10) d.push_back(x); }
            if (d.size() >= 3) {
                std::vector<double> c = d; std::sort(c.begin(), c.end());
                const double med = c[c.size() / 2];
                double sum = 0; int cnt = 0;
                for (double x : d) if (std::fabs(x - med) < 0.1 * med) { sum += x; cnt++; }
                if (cnt >= 3) s.s.periodSec = sum / cnt;
            }
        }
        s.s.hint = codar::hintFor(s.s.centreMhz, s.s.bwKhz, s.s.periodSec);
        s.histSum += snr; s.histN++;
        if (sec >= s.histNext) {
            s.s.snrHist.push_back((float)(s.histSum / s.histN));
            if (s.s.snrHist.size() > kCdSnrHist) s.s.snrHist.erase(s.s.snrHist.begin());
            s.histSum = 0; s.histN = 0; s.histNext = sec + 1.0;
        }
        if (s.s.sweeps >= 2 && sec - s.lastEventSec > 60) {
            s.lastEventSec = sec;
            CdEvent ev; ev.wall = s.s.lastWall; ev.station = s.s.id; ev.band = s.s.band; ev.centreMhz = s.s.centreMhz; ev.snrDb = snr;
            events.insert(events.begin(), ev);
            if (events.size() > kCdEventMax) events.pop_back();
            if (log) { char b[200]; snprintf(b, sizeof b, "CODAR: radar %d heard, %.3f MHz, %.0f kHz sweeps, %s", s.s.id, s.s.centreMhz, s.s.bwKhz, s.s.dir > 0 ? "rising" : "falling"); log(b); }
        }
        {   // a piece of this radar's sweep that was taken for a radar before the whole sweep was seen
            const int keep = s.s.id;
            const double lo = s.offHz - 0.5 * s.s.bwKhz * 1e3 - 3e3, hi = s.offHz + 0.5 * s.s.bwKhz * 1e3 + 3e3;
            const double rate0 = s.s.rateKhzS, dir0 = s.s.dir;
            const bool abs_ = cH > 0;
            const double shift = abs_ ? s.absHz - s.offHz : 0;
            st.erase(std::remove_if(st.begin(), st.end(), [&](const St& o) {
                if (o.s.id == keep || o.s.sweeps > 3 || o.s.dir != dir0 || std::fabs(o.s.rateKhzS - rate0) > 0.3 * rate0) return false;
                const double oc = abs_ ? o.absHz - shift : o.offHz;
                return oc - 0.5 * o.s.bwKhz * 1e3 >= lo && oc + 0.5 * o.s.bwKhz * 1e3 <= hi;
            }), st.end());
        }
    }

    // ---- the keyed energy of each radar's sweep width
    void envelope(double t) {
        int done = 0;
        for (St& s : st) {
            if (s.s.sweeps < 2 || (sigSec() - s.s.lastSec > 600 && s.env.empty())) continue;
            if (++done > 16) break;
            const double c = (codarCentreFor(s)) ;
            const int a = std::max(jlo, (int)std::floor((c - 0.5 * s.s.bwKhz * 1e3 - 2e3) / binHz + N / 2)), b = std::min(jhi, (int)std::ceil((c + 0.5 * s.s.bwKhz * 1e3 + 2e3) / binHz + N / 2));
            if (b <= a) continue;
            double S = 0, F = 0;
            for (int j = a; j <= b; j++) { S += p[(size_t)j]; F += floorP[(size_t)j]; }
            s.envAcc += S / std::max(1e-30, F); s.envN++;
            if (t >= s.envNext) {
                if (s.envN) {
                    s.env.push_back((float)(10 * std::log10(std::max(1e-6, s.envAcc / s.envN))));
                    if (s.env.size() > kEnvMax) s.env.pop_front();
                }
                s.envAcc = 0; s.envN = 0; s.envNext = t + kEnvStep;
                if (s.env.size() > 2400 && t >= s.nextId) { s.nextId = t + kIdEvery; tryId(s); }
            }
        }
    }
    double codarCentreFor(const St& s) const {   // offset from the radio's centre
        const double cH = centerHz.load();
        return cH > 0 ? s.absHz - cH : s.offHz;
    }

    void tryId(St& s) {
        const size_t n = std::min(s.env.size(), (size_t)(kIdWindow / kEnvStep));
        std::vector<float> v(s.env.end() - (long)n, s.env.end());
        const float lo = pct(v, 0.10), hi = pct(v, 0.90);
        if (hi - lo < 2.5f) return;
        const float on = lo + 0.6f * (hi - lo), off = lo + 0.4f * (hi - lo);
        struct Run { bool on; double ms; };
        std::vector<Run> runs;
        bool key = v[0] > on;
        for (float x : v) {
            if (!key && x > on) key = true; else if (key && x < off) key = false;
            if (runs.empty() || runs.back().on != key) runs.push_back({key, kEnvStep * 1000}); else runs.back().ms += kEnvStep * 1000;
        }
        // specks shorter than 3 points join their neighbours
        for (size_t i = 1; i + 1 < runs.size();) {
            if (runs[i].ms < 3 * kEnvStep * 1000) { runs[i - 1].ms += runs[i].ms + runs[i + 1].ms; runs.erase(runs.begin() + (long)i, runs.begin() + (long)i + 2); }
            else i++;
        }
        // the runs between long silences (0.9 s and more) are decoded one by one: a sweep train is not part of the keying beside it
        std::vector<size_t> cuts;   // start index of each segment
        cuts.push_back(1);
        for (size_t i = 1; i + 1 < runs.size(); i++) if (!runs[i].on && runs[i].ms >= 900) cuts.push_back(i + 1);
        cuts.push_back(runs.size() - 1);
        std::string text;
        double wpm = 0;
        for (size_t c = 0; c + 1 < cuts.size(); c++) {
            if (cuts[c + 1] - cuts[c] < 12) continue;
            cw::Timing tm;
            tm.reset();
            for (size_t i = cuts[c]; i < cuts[c + 1]; i++) { if (runs[i].on) tm.mark(runs[i].ms); else if (runs[i].ms < 900) tm.space(runs[i].ms); }
            tm.idle(2000);
            const std::string tx = tm.take();
            if (!tm.locked() || tm.dotMs() < 30 || tm.dotMs() > 300) continue;
            text += tx + " ";
            wpm = tm.wpm();
        }
        size_t pos = 0;
        while (pos < text.size()) {
            while (pos < text.size() && text[pos] == ' ') pos++;
            size_t e2 = pos;
            while (e2 < text.size() && text[e2] != ' ') e2++;
            const std::string w = text.substr(pos, e2 - pos);
            pos = e2;
            if (w.size() < 4 || w.size() > 10) continue;
            bool ok = true; std::string dist;
            for (char c : w) { if (!std::isalnum((unsigned char)c)) ok = false; if (dist.find(c) == std::string::npos) dist += c; }
            if (!ok || dist.size() < 3) continue;
            if (s.idWord == w) s.s.idSeen++; else { s.idWord = w; s.s.idSeen = 1; }
            if (s.s.idSeen >= 2) {
                if (s.s.callsign != w && log) log("CODAR: radar " + std::to_string(s.s.id) + " ID " + w + " (unverified)");
                s.s.callsign = w; s.s.idWpm = wpm; s.s.idWall = (int64_t)std::time(nullptr);
            }
            break;
        }
    }

    // ---- the waterfall: one row per 25 ms, the maximum of each column
    void waterfall(double t) {
        const int span = jhi - jlo + 1;
        for (int j = jlo; j <= jhi; j++) {
            const int c = (j - jlo) * kCdWfCols / span;
            const float db = 10.f * std::log10(std::max(1e-3f, e[(size_t)j]));
            const uint8_t v = (uint8_t)std::max(0.f, std::min(255.f, db * (255.f / 30.f)));
            if (v > rowMax[(size_t)c]) rowMax[(size_t)c] = v;
        }
        if (t >= rowEnd) {
            std::memmove(wf.data(), wf.data() + kCdWfCols, (size_t)kCdWfCols * (kCdWfRows - 1));
            std::memcpy(wf.data() + (size_t)kCdWfCols * (kCdWfRows - 1), rowMax.data(), (size_t)kCdWfCols);
            std::fill(rowMax.begin(), rowMax.end(), (uint8_t)0);
            rowEnd += 0.025;
            if (rowEnd < t) rowEnd = t + 0.025;
        }
    }

    void process(const cf32* x, size_t n) {
        q.insert(q.end(), x, x + n);
        size_t off = 0;
        while (q.size() - off >= (size_t)N) { frame(q.data() + off); off += (size_t)hop; }
        if (off) q.erase(q.begin(), q.begin() + (long)off);
    }

    void report() {
        std::lock_guard<std::mutex> lk(mu);
        CodarTelemetry& t = pub;
        t.seq++;
        t.timeSec = sigSec(); t.inputRate = rate;
        const double cH = centerHz.load();
        t.centerMhz = cH / 1e6;
        t.spanKhz = 0.9 * rate / 1e3;
        t.bandNow = cH > 0 ? codar::bandOf(cH / 1e6) : std::string();
        t.stations.clear();
        for (const St& s : st) if (s.s.sweeps >= 2) t.stations.push_back(s.s);
        std::sort(t.stations.begin(), t.stations.end(), [](const CdStation& a, const CdStation& b) { return a.lastWall != b.lastWall ? a.lastWall > b.lastWall : a.lastSec > b.lastSec; });
        t.events = events;
        t.sweepMarks = marks;
        t.wf = wf;
        t.wfRowSec = 0.025; t.wfEndSec = rowEnd - 0.025;
        t.blocksOk = sweepsOk; t.blocksBad = tracksBad; t.wideBursts = wide;
        t.state = sigSec() - lastSweepSec < 5 ? 1 : 0;
        t.dataValid = t.state == 1;
        t.snrDb = sigSec() - bestSnrAt < 3 ? bestSnr : 0;
        t.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
    }
};

CodarReceiver::CodarReceiver() : p_(new Impl) {
    p_->wf.assign((size_t)kCdWfCols * kCdWfRows, 0);
}
CodarReceiver::~CodarReceiver() = default;

void CodarReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void CodarReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetSet = hz; }
void CodarReceiver::setCenterHz(double hz) {
    if (std::fabs(p_->centerHz.load() - hz) > 1e3) p_->resetReq = true;   // another part of the band: the floor and the tracks are of the old one
    p_->centerHz = hz;
}
bool CodarReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= codarTuning().minSampleRate - 1; }
void CodarReceiver::reset() { p_->resetReq = true; }
void CodarReceiver::clearResults() {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->pub.stations.clear(); p_->pub.events.clear(); p_->pub.sweepMarks.clear();
        p_->pub.seq++;
    }
    p_->clearReq = true;
}

void CodarReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) {
        { std::lock_guard<std::mutex> lk(m.mu); if (m.rateSet != m.rate || !m.fft) { m.rate = m.rateSet; if (m.rate >= codarTuning().minSampleRate - 1) m.initFront(); m.nIn = 0; m.nextReport = 0; } }
        if (m.rate > 0 && m.fft) m.resetDetector();
    }
    if (m.clearReq.exchange(false)) m.clearAll();
    m.takeRequests();
    if (m.rate < codarTuning().minSampleRate - 1 || !m.fft || n == 0) {
        if (m.rate > 0) { m.nIn += (int64_t)n; if ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); } }
        return;
    }
    const size_t step = (size_t)(0.05 * m.rate);   // reports and power every 50 ms of samples at most
    size_t pos = 0;
    while (pos < n) {
        const size_t c = std::min(step, n - pos);
        for (size_t i = 0; i < c; i++) {
            float re = x[pos + i].real(), im = x[pos + i].imag();
            if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
            m.power += (double)re * re + (double)im * im;
        }
        m.nPower += (int64_t)c;
        m.process(x + pos, c);
        m.nIn += (int64_t)c;
        pos += c;
        if ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); }
    }
}

std::string CodarReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.stations, p_->pub.events);
}
bool CodarReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    CodarTelemetry l;
    if (!results::unpack(blob, t0, l.stations, l.events)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->loadedTel = l;
    p_->pub.stations = l.stations;
    p_->pub.events = l.events;
    p_->pub.seq++;
    p_->loadReq = true;
    return true;
}
bool CodarReceiver::telemetry(CodarTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void CodarReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning codarTuning() {
    ModeTuning t;
    t.stdMode = 74; t.id = "codar"; t.name = "CODAR / HF ocean radar";
    t.minMhz = 3; t.maxMhz = 50; t.defMhz = 13.5;
    t.sampleRate = 2000000;                  // 1.8 MHz of the band at once; 250 ksps already sees a 25 - 100 kHz sweep
    t.basebandHz = 0;
    t.bandwidthMhz = 2;
    t.minSampleRate = 240000;
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
