// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/hfbeacon/hfbeacon_dsp.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/geo/maidenhead.h"
#include "dect2/results/results_io.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>

namespace dect2::hfb {

namespace {
constexpr int kM = 32, kHop = 16;           // stage 2: spectrum size and hop (in stage 1 output samples)
constexpr size_t kRingCap = 4096;           // frames of x kept per channel (about 53 s at 13 ms)
constexpr double kSnrDash = 1.0;            // mean excess (noise bins) over a dash window that counts as heard
constexpr double kDay = 86400.0;

// the mean of n unit exponentials over the expected value of their r-th smallest (1-based): E = H(n) - H(n - r)
double rankToMean(int n, int r) {
    double e = 0;
    for (int i = 0; i < r; i++) e += 1.0 / (n - i);
    return 1.0 / e;
}

// Stage 1 passband shape of a Hann window at a fraction delta of a bin from the bin centre, power relative to the centre
double hannShape(double d) {
    const double pd = 3.14159265358979 * d;
    const double r = (std::fabs(d) < 1e-9 ? 1.0 : std::sin(pd) / pd) / (1 - d * d);
    return r * r;
}
// Noise (aliased from the neighbouring bins by the window's width) is flat across the bin at 1.5 times its level at the centre, the tone is
// seen once through the shape: the factor that turns the excess over the flat noise into SNR in 100 Hz
double gainAt(double d) { return 1.5 / hannShape(d); }

struct SavedHist { std::string key; double lastHeard = 0; float snr = NAN; std::vector<HfbHistPt> pts; };

struct Run { double t0 = 0, dur = 0; bool on = false; float mean = 0; };

struct DashEv { double t0 = 0; float snr = NAN; };

struct Stat {
    double lastHeard = 0, lastEnergy = 0;
    float snr = NAN;
    int conf = 0, steps = 0;
    float stepSnr[4] = {NAN, NAN, NAN, NAN};
    std::deque<HfbHistPt> hist;
    // the minute being collected (single letters, dashes, carriers)
    double minute = -1; float minSnr = NAN; bool minHeard = false;
    std::deque<DashEv> dash;
    std::string note;
};

struct Meas {
    double fHz = 0, searchHz = 150;
    bool wide = false;
    int group = -1, j0 = 0, w = 0, kind = Carrier, band = -1;
    std::vector<int> bs;
    std::vector<float> xr;
    double nb = 0; int nbCount = 0;
    double onThr = 16, offThr = 6;   // in noise bins (excess)
    bool on = false; int streak = 0; double streakT = 0, onT0 = 0, lastEnd = 0, onSum = 0; int onCnt = 0;
    std::deque<Run> runs;
    uint64_t firstFi = 0;
    double gain = 1.5;               // SNR(100 Hz) = x * gain / kM * rate1 / 100 (see hfbeacon_dsp.h)
    double slotDone = -1;            // NCDXF: start (stream UTC) of the last slot evaluated
    double carrierT = 0;
    int jlock = -1;                  // a wide channel follows its peak while a run is on (the maximum over the whole search would hold the run open on noise)
    float x1 = 0, x2 = 0;            // the two frames before: x is smoothed over three for the on / off runs
};

struct Group { int k = 0; std::vector<cf32> y; int wr = 0, count = 0, since = 0; std::vector<int> meas; };
}

struct Analyzer::Impl {
    std::vector<Beacon> list;
    std::vector<Stat> stat;
    std::vector<Meas> meas;
    std::vector<Group> groups;
    double rate = 0, center = 0, tStart = 0, ppm = 0, clockOff = 0, dt = 0, r1 = 0;
    bool clockValid = false, haveObs = false;
    double obsLat = 0, obsLon = 0;
    int sel = -1, N = 0;
    uint64_t fi = 0, frames = 0, falseRuns = 0;
    double powerDb = -200;
    // stage 1
    std::vector<float> w1, w2;
    std::unique_ptr<Fft> fft1, fft2;
    std::vector<cf32> fbuf;
    int fpos = 0;
    bool configured = false;
    std::vector<DashEv> dummy;
    std::deque<std::pair<int, DashEv>> allDash;   // (beacon, event), the last two minutes

    // ------------------------------------------------------------------ structure
    void rebuild() {
        meas.clear(); groups.clear();
        if (!configured || rate <= 0) return;
        int n = 8;
        while (n * 2 <= rate / 1500.0 && n < 16384) n *= 2;
        N = n; r1 = rate / N; dt = kHop / r1;
        if (!fft1 || fft1->size() != N) fft1.reset(new Fft(N));
        w1.assign((size_t)N, 0);
        for (int i = 0; i < N; i++) w1[(size_t)i] = 0.5f * (1 - std::cos(2 * 3.14159265358979 * (i + 0.5) / N));
        fbuf.assign((size_t)N, cf32(0, 0)); fpos = 0;
        if (!fft2) fft2.reset(new Fft(kM));
        w2.assign(kM, 0);
        for (int i = 0; i < kM; i++) w2[(size_t)i] = 0.5f * (1 - std::cos(2 * 3.14159265358979 * (i + 0.5) / kM));
        const double B1 = r1;
        std::map<std::tuple<long, long, int, int>, int> byKey;
        std::map<int, int> byBin;
        for (int bi = 0; bi < (int)list.size(); bi++) {
            const Beacon& b = list[(size_t)bi];
            const double f = b.khz * 1e3 * (1 + ppm * 1e-6), d = f - center;
            const double search = searchOf(b);
            if (std::fabs(d) > 0.4 * rate - 3000 || std::fabs(d) < 3.2 * B1) continue;
            const bool wide = search > 300;
            const long fk = std::lround(f * 10);
            const auto key = std::make_tuple(fk, (long)search, (int)wide, b.kind == Ncdxf ? 1 : 0);
            auto it = byKey.find(key);
            int mi;
            if (it == byKey.end()) {
                Meas m;
                m.fHz = f; m.searchHz = search; m.wide = wide; m.kind = b.kind; m.band = b.kind == Ncdxf ? b.ncdxfBand : -1;
                const long k = std::lround(d / B1);
                const double e = d - k * B1;
                const double binHz = r1 / kM;
                m.j0 = ((int)std::lround(e / binHz) % kM + kM) % kM;
                m.w = wide ? std::min(kM / 2 - 2, (int)std::ceil(search / binHz)) : 0;
                m.xr.assign(kRingCap, 0.f); m.firstFi = fi;
                m.onThr = wide ? 14 : 9; m.offThr = m.onThr * 0.4;   // on the 3 frame mean of x (about 2 noise sigmas per frame mean over a false run's start)
                m.gain = gainAt(e / B1);
                int gk = (int)(((k % N) + N) % N);
                auto g = byBin.find(gk);
                if (g == byBin.end()) { Group gr; gr.k = gk; gr.y.assign(kM, cf32(0, 0)); groups.push_back(std::move(gr)); g = byBin.emplace(gk, (int)groups.size() - 1).first; }
                m.group = g->second;
                meas.push_back(std::move(m));
                mi = (int)meas.size() - 1;
                groups[(size_t)m.group].meas.push_back(mi);
                byKey[key] = mi;
            } else mi = it->second;
            meas[(size_t)mi].bs.push_back(bi);
        }
    }

    // ------------------------------------------------------------------ helpers
    double tOf(uint64_t i) const { return tStart + (double)(i + 1) * dt; }
    double trueOf(double tStream) const { return tStream - clockOff; }

    float meanX(const Meas& m, double ta, double tb) const {
        if (fi == 0) return NAN;
        long i0 = (long)std::ceil((ta - tStart) / dt) - 1, i1 = (long)std::floor((tb - tStart) / dt) - 1;
        i0 = std::max<long>(i0, std::max<long>((long)m.firstFi, (long)fi > (long)kRingCap ? (long)fi - (long)kRingCap + 1 : 0));
        i1 = std::min<long>(i1, (long)fi - 1);
        if (i1 < i0) return NAN;
        double s = 0;
        for (long i = i0; i <= i1; i++) s += m.xr[(size_t)i % kRingCap];
        return (float)(s / (double)(i1 - i0 + 1));
    }

    void flushMinute(Stat& s, double minute) {
        if (s.minute >= 0 && (s.minHeard || true)) {
            HfbHistPt p; p.t = s.minute + 30; p.snr = s.minHeard ? s.minSnr : NAN; p.heard = s.minHeard;
            s.hist.push_back(p);
            while (!s.hist.empty() && s.hist.front().t < p.t - kDay) s.hist.pop_front();
        }
        s.minute = minute; s.minSnr = NAN; s.minHeard = false;
    }
    void minuteTick(Stat& s, double t) {
        const double mn = std::floor(t / 60) * 60;
        if (mn != s.minute) flushMinute(s, mn);
    }

    void markHeard(int bi, double t, float snr, int conf) {
        Stat& s = stat[(size_t)bi];
        s.lastHeard = t; s.snr = snr; s.conf = conf;
        if (list[(size_t)bi].kind != Ncdxf) {
            minuteTick(s, t);
            s.minHeard = true;
            if (std::isnan(s.minSnr) || snr > s.minSnr) s.minSnr = snr;
        }
    }

    // ------------------------------------------------------------------ keying evaluation
    // fit the last runs to the Morse of `letter`; returns matching and non-matching complete letters
    void letterMatch(const Meas& m, char letter, int& ok, int& bad, double& unit) const {
        ok = bad = 0; unit = 0;
        const size_t n = m.runs.size();
        std::vector<Run> r(m.runs.end() - (long)std::min<size_t>(n, 60), m.runs.end());
        std::vector<double> offs;
        for (const Run& x : r) if (!x.on) offs.push_back(x.dur);
        if (offs.size() < 3) return;
        std::sort(offs.begin(), offs.end());
        double u = 0; int c = 0;
        for (double o : offs) if (o <= 1.6 * offs[0]) { u += o; c++; }
        u /= c;
        unit = u;
        if (u < 0.03 || u > 0.7) return;
        // groups of on runs split at gaps over 2 units; the first group may have lost its start, the last has no gap after it
        std::vector<std::string> g;
        std::string cur; bool started = false;
        for (const Run& x : r) {
            if (x.on) { cur += x.dur < 2 * u ? '.' : '-'; }
            else if (x.dur > 2 * u) { if (started) g.push_back(cur); cur.clear(); started = true; }
        }
        const char* want = morseOf(letter);
        const size_t from = g.size() > 8 ? g.size() - 8 : 0;
        for (size_t i = from; i < g.size(); i++) { if (g[i] == want) ok++; else bad++; }
    }

    void evalLetter(Meas& m, double t) {
        for (int bi : m.bs) {
            const Beacon& b = list[(size_t)bi];
            int ok, bad; double u;
            letterMatch(m, b.letter, ok, bad, u);
            Stat& s = stat[(size_t)bi];
            if (ok >= 2 && bad * 3 <= ok) {
                // SNR: the mean excess of the last on runs
                double sum = 0; int c = 0;
                for (auto it = m.runs.rbegin(); it != m.runs.rend() && c < 12; ++it) if (it->on) { sum += it->mean; c++; }
                markHeard(bi, t, (float)snrFromExcess(c ? sum / c : 0, r1, m.gain), 2);
                char n[64]; snprintf(n, sizeof n, "'%c' keyed, unit %.0f ms", b.letter, u * 1000); s.note = n;
            } else if (!m.runs.empty()) { s.lastEnergy = t; if (s.note.empty() || s.note[0] != 'e') s.note = "energy, keying not matched"; }
        }
    }

    static double circDiff(double a, double b) { double d = std::fmod(a - b, 27.0); if (d < 0) d += 27; return std::min(d, 27 - d); }

    void evalDash(Meas& m, const Run& run) {
        if (run.dur < 2.2 || run.dur > 3.6) return;
        for (int bi : m.bs) {
            const Beacon& b = list[(size_t)bi];
            Stat& s = stat[(size_t)bi];
            DashEv ev; ev.t0 = run.t0; ev.snr = (float)snrFromExcess(run.mean, r1, m.gain);
            bool confirm = false;
            for (const DashEv& o : s.dash) {
                const double d = ev.t0 - o.t0;
                for (int k = 1; k <= 4; k++) if (std::fabs(d - 27.0 * k) < 0.9) confirm = true;
                if (b.dashIdx == 0 && (std::fabs(d - 3.0) < 0.5 || std::fabs(d - 24.0) < 0.9)) confirm = true;
            }
            // another frequency's dash in the 3 s ladder of the same cycle
            auto phases = [&](int idx0, std::vector<double>& ph, double t0) { ph.push_back(std::fmod(t0 - 3.0 * idx0 + 54, 27.0)); if (idx0 == 0) ph.push_back(std::fmod(t0 - 3.0 + 54, 27.0)); };
            std::vector<double> mine;
            phases(b.dashIdx, mine, ev.t0);
            bool crossBeacon = false;
            for (const auto& pr : allDash) {
                if (pr.first == bi || ev.t0 - pr.second.t0 > 100 || ev.t0 - pr.second.t0 < -1) continue;
                std::vector<double> theirs;
                phases(list[(size_t)pr.first].dashIdx, theirs, pr.second.t0);
                for (double a : mine) for (double c : theirs) if (circDiff(a, c) < 0.6) crossBeacon = true;
                if (crossBeacon) { markHeard(pr.first, run.t0 + run.dur, pr.second.snr, 2); stat[(size_t)pr.first].note = "dash in step with another frequency"; }
            }
            s.dash.push_back(ev);
            while (!s.dash.empty() && ev.t0 - s.dash.front().t0 > 130) s.dash.pop_front();
            allDash.push_back({bi, ev});
            while (!allDash.empty() && ev.t0 - allDash.front().second.t0 > 120) allDash.pop_front();
            if (confirm || crossBeacon) { markHeard(bi, run.t0 + run.dur, ev.snr, 2); s.note = confirm ? "dash repeats on the 27 s cycle" : "dash in step with another frequency"; }
            else { s.lastEnergy = run.t0 + run.dur; s.note = "dash seen, waiting for the next cycle"; }
        }
    }

    void evalCarrier(Meas& m, double t) {
        if (t - m.carrierT < 1.0) return;
        m.carrierT = t;
        const float x = meanX(m, t - 3, t);
        for (int bi : m.bs) {
            if (!std::isnan(x) && x >= 1.5) markHeard(bi, t, (float)snrFromExcess(x, r1, m.gain), 2);
        }
    }

    void evalSlot(Meas& m, double slotStart) {   // slotStart in stream time
        const double slotEnd = slotStart + kNcdxfSlotSec;
        // frames must cover the whole slot
        if (tOf(m.firstFi) > slotStart) return;
        double t4 = slotStart + 4.4; bool found100 = false;
        int callRuns = 0;
        for (const Run& r : m.runs) {
            if (!r.on) continue;
            if (r.t0 >= slotStart + 0.05 && r.t0 < slotStart + 3.6 && r.t0 + r.dur < slotStart + 4.6) callRuns++;
            if (!found100 && r.t0 >= slotStart + 2.8 && r.t0 < slotStart + 7 && r.dur >= 0.75 && r.dur <= 4.6) { found100 = true; t4 = r.t0; }
        }
        if (found100) {   // the dash edge: the first frame near the dash's own level, not the threshold crossing
            const float lv = meanX(m, t4 + 0.15, t4 + 0.5);
            const long i0 = (long)std::floor((t4 - 0.25 - tStart) / dt) - 1, i1 = (long)std::floor((t4 + 0.3 - tStart) / dt) - 1;
            float sm = 0;
            for (long i = std::max<long>(i0, (long)m.firstFi); i <= i1 && i < (long)fi; i++) {
                const float v = m.xr[(size_t)i % kRingCap];
                const float w = (v + sm) * 0.5f; sm = v;   // two frame mean against single frame noise
                if (lv > 0 && w >= 0.5f * lv) { t4 = tOf((uint64_t)i) - 0.5 * dt; break; }
            }
        }
        if (!found100) {   // too weak for a run: the 1 s stretch of highest level after the callsign (the plateau of windows at its top, centred)
            float best = -1e9f; double sum = 0; int n = 0;
            for (int pass = 0; pass < 2; pass++) {
                sum = 0; n = 0;
                for (double tau = slotStart + 2.8; tau <= slotStart + 6.0; tau += 0.05) {
                    const float v = meanX(m, tau + 0.1, tau + 0.9);
                    if (std::isnan(v)) continue;
                    if (pass == 0) best = std::max(best, v);
                    else if (v >= 0.9f * best) { sum += tau; n++; }
                }
            }
            if (n && best > 0) t4 = sum / n;
        }
        float mk[4];
        int steps = 0; bool contiguous = true;
        for (int k = 0; k < 4; k++) {
            mk[k] = meanX(m, t4 + k + 0.15, t4 + k + 0.85);
            if (contiguous && mk[k] >= kSnrDash) steps++; else contiguous = false;
        }
        const bool callOk = callRuns >= 4;
        // too weak to key the on / off detector: the shape of the slot instead - the first dash well above the noise, the callsign before it carrying
        // energy, and the quiet after the last dash (a steady carrier fails the last)
        const float callMean = meanX(m, slotStart + 0.1, t4 - 0.3), tail = meanX(m, t4 + 4.3, t4 + 5.7);
        const bool weakShape = mk[0] >= 1.4f && !std::isnan(callMean) && !std::isnan(tail) && callMean >= 0.3f * mk[0] && tail <= 0.4f * mk[0];
        const bool heard = steps >= 1 && (found100 || callOk || weakShape) && slotEnd - slotStart > 0;
        const int bi_idx = ncdxfBeaconAt(m.band, trueOf(slotStart) + 1.0);
        for (int bi : m.bs) {
            const Beacon& b = list[(size_t)bi];
            if (b.ncdxfIdx != bi_idx) continue;
            Stat& s = stat[(size_t)bi];
            HfbHistPt p; p.t = slotStart + 5;
            if (heard) {
                p.snr = (float)snrFromExcess(mk[0], r1, m.gain); p.heard = 1;
                s.lastHeard = slotEnd; s.snr = p.snr; s.conf = (found100 && callOk) ? 2 : 1; s.steps = steps;
                for (int k = 0; k < 4; k++) s.stepSnr[k] = mk[k] >= kSnrDash ? (float)snrFromExcess(mk[k], r1, m.gain) : NAN;
                char n[96]; snprintf(n, sizeof n, "%d of 4 power steps (%s)%s", steps, steps >= 4 ? "100 W to 0.1 W" : steps == 3 ? "to 1 W" : steps == 2 ? "to 10 W" : "100 W only", callOk ? ", callsign keyed" : "");
                s.note = n;
            } else {
                s.steps = 0;
                for (float& v : s.stepSnr) v = NAN;
                s.note = "slot listened, nothing heard";
            }
            s.hist.push_back(p);
            while (!s.hist.empty() && s.hist.front().t < p.t - kDay) s.hist.pop_front();
        }
    }

    void endRun(Meas& m, const Run& r0) {
        Run r = r0;
        if (r.on) {   // the level of the run: the middle half (the edges are ramps of the 32 point window and the smoothing)
            const float c = r.dur > 0.06 ? meanX(m, r.t0 + 0.25 * r.dur, r.t0 + 0.75 * r.dur) : NAN;
            if (!std::isnan(c)) r.mean = c;
        }
        if (r.on) falseRuns++;
        m.runs.push_back(r);
        while (m.runs.size() > 90) m.runs.pop_front();
        if (m.kind == Dashes && r.on) evalDash(m, r);
        if (m.kind == Letter) evalLetter(m, r.t0 + r.dur);
    }

    // ------------------------------------------------------------------ the signal path
    void spectrum(Group& g) {
        float P[kM];
        std::vector<cf32> tmp(kM);
        for (int i = 0; i < kM; i++) tmp[(size_t)i] = g.y[(size_t)((g.wr + i) % kM)] * w2[(size_t)i];
        fft2->forward(tmp.data());
        for (int j = 0; j < kM; j++) P[j] = std::norm(tmp[(size_t)j]);
        const uint64_t i = fi;
        const double t = tOf(i), half = 0.5 * dt;
        for (int mi : g.meas) {
            Meas& m = meas[(size_t)mi];
            // the channel: the three bins at the beacon (the strongest of them within the search for a wide search)
            double s3; int jc = m.j0;
            if (!m.wide) s3 = P[(m.j0 + kM - 1) % kM] + P[m.j0] + P[(m.j0 + 1) % kM];
            else {
                s3 = 0;
                const int lo = m.on && m.jlock >= 0 ? -1 : -m.w, hi = m.on && m.jlock >= 0 ? 1 : m.w, base = m.on && m.jlock >= 0 ? m.jlock : m.j0;
                for (int d = lo; d <= hi; d++) {
                    const int j = ((base + d) % kM + kM) % kM;
                    const double sj = P[(j + kM - 1) % kM] + P[j] + P[(j + 1) % kM];
                    if (sj > s3) { s3 = sj; jc = j; }
                }
                m.jlock = jc;
                m.gain = gainAt((double)(((jc + kM / 2) % kM) - kM / 2) / kM);   // the peak's place in the bin (the 32 spectrum bins span one stage 1 bin)
            }
            // the noise floor: a low quantile of the other bins (the channel's own and two on each side left out; the other beacons of a cluster fill a
            // third of the 32 bins, which would lift a median), scaled to a mean
            float v[kM]; int nv = 0;
            for (int j = 0; j < kM; j++) { int dj = std::abs(j - jc); dj = std::min(dj, kM - dj); if (dj > 2) v[nv++] = P[j]; }
            const int rk = std::max(1, (nv + 2) / 4);   // the 25 % point
            std::nth_element(v, v + rk - 1, v + nv);
            const double nbInst = v[rk - 1] * rankToMean(nv, rk);
            const double a = std::max(1.0 - std::exp(-dt / 3.0), 1.0 / (m.nbCount + 1));
            m.nb = m.nbCount == 0 ? nbInst : m.nb + a * (nbInst - m.nb);
            m.nbCount++;
            const float x = m.nb > 0 ? (float)((s3 - 3 * m.nb) / m.nb) : 0.f;
            m.xr[(size_t)(i % kRingCap)] = x;
            const float xs = (x + m.x1 + m.x2) / 3.f;
            m.x2 = m.x1; m.x1 = x;
            if (m.nbCount < 20) continue;     // the noise estimate settles first
            const double te = t - dt;         // the smoothed value belongs to the middle frame
            // on / off runs
            if (!m.on) {
                if (xs > m.onThr) {
                    if (++m.streak == 1) m.streakT = te;
                    if (m.streak >= 2) {
                        m.on = true; m.streak = 0; m.onT0 = m.streakT - half; m.onSum = 0; m.onCnt = 0;
                        if (m.lastEnd > 0 && m.onT0 > m.lastEnd) { Run o; o.t0 = m.lastEnd; o.dur = m.onT0 - m.lastEnd; o.on = false; endRun(m, o); }
                    }
                } else m.streak = 0;
            }
            if (m.on) {
                m.onSum += x; m.onCnt++;
                if (xs < m.offThr) {
                    if (++m.streak == 1) m.streakT = te;
                    if (m.streak >= 3) {
                        Run r; r.t0 = m.onT0; r.dur = m.streakT - half - m.onT0; r.on = true; r.mean = m.onCnt ? (float)(m.onSum / m.onCnt) : 0.f;
                        m.on = false; m.streak = 0; m.lastEnd = r.t0 + r.dur;
                        endRun(m, r);
                    }
                } else m.streak = 0;
            }
            // per kind
            if (m.kind == Ncdxf && clockValid) {
                // the latest slot that ended 0.5 s ago
                const double tt = trueOf(t) - 0.5;
                const double slotTrue = std::floor(tt / kNcdxfSlotSec) * kNcdxfSlotSec - kNcdxfSlotSec;
                const double slotStream = slotTrue + clockOff;
                if (slotStream > m.slotDone) { m.slotDone = slotStream; evalSlot(m, slotStream); }
            } else if (m.kind == Carrier) evalCarrier(m, t);
        }
    }

    void frame() {
        // stage 1: window, FFT, one sample per watched bin
        std::vector<cf32>& b = fbuf;
        for (int i = 0; i < N; i++) b[(size_t)i] *= w1[(size_t)i];
        fft1->forward(b.data());
        for (Group& g : groups) {
            g.y[(size_t)g.wr] = b[(size_t)g.k];
            g.wr = (g.wr + 1) % kM;
            if (g.count < kM) g.count++;
            g.since++;
        }
        bool any = false;
        for (Group& g : groups) if (g.count >= kM && g.since >= kHop) { any = true; }
        if (any) {
            for (Group& g : groups) { if (g.count >= kM) { spectrum(g); g.since = 0; } }
            fi++; frames++;
        }
    }
};

Analyzer::Analyzer() : p_(new Impl) {}
Analyzer::~Analyzer() = default;

double snrFromExcess(double x, double rate1, double gain) {
    const double v = std::max(x, 1e-3) * gain / kM * rate1 / 100.0;
    return 10 * std::log10(v);
}

const char* morseOf(char c) {
    switch (c) {
    case 'D': return "-.."; case 'P': return ".--."; case 'S': return "..."; case 'C': return "-.-."; case 'A': return ".-";
    case 'F': return "..-."; case 'K': return "-.-"; case 'M': return "--"; default: return "";
    }
}

void Analyzer::setBeacons(std::vector<Beacon> b) {
    Impl& m = *p_;
    // keep the stats of beacons that stay
    std::map<std::pair<std::string, long>, Stat> old;
    for (size_t i = 0; i < m.list.size(); i++) old[{m.list[i].name, std::lround(m.list[i].khz * 1000)}] = std::move(m.stat[i]);
    m.list = std::move(b);
    m.stat.assign(m.list.size(), Stat());
    for (size_t i = 0; i < m.list.size(); i++) {
        auto it = old.find({m.list[i].name, std::lround(m.list[i].khz * 1000)});
        if (it != old.end()) m.stat[i] = std::move(it->second);
    }
    m.allDash.clear();
    m.rebuild();
}

void Analyzer::configure(double rateHz, double centerHz, double originUtc, bool clockValid) {
    Impl& m = *p_;
    m.rate = rateHz; m.center = centerHz; m.tStart = originUtc; m.clockValid = clockValid;
    m.configured = rateHz > 0;
    m.fi = 0; m.allDash.clear();
    m.rebuild();
}
void Analyzer::setPpm(double ppm) { if (ppm != p_->ppm) { p_->ppm = ppm; p_->rebuild(); } }
void Analyzer::setClockOffset(double s) { p_->clockOff = s; for (auto& m : p_->meas) m.slotDone = -1; }
void Analyzer::setObserver(bool have, double lat, double lon) { p_->haveObs = have; p_->obsLat = lat; p_->obsLon = lon; }
void Analyzer::setSelected(int i) { p_->sel = i; }
const std::vector<Beacon>& Analyzer::beacons() const { return p_->list; }
double Analyzer::rate1() const { return p_->r1; }
std::string Analyzer::debugRuns(int bi) const {
    const Impl& m = *p_;
    std::string o;
    for (const Meas& me : m.meas)
        for (int b : me.bs) {
            if (b != bi) continue;
            char t[160];
            snprintf(t, sizeof t, "nb %.3g gain %.2f thr %.0f/%.1f j0 %d runs %zu\n", me.nb, me.gain, me.onThr, me.offThr, me.j0, me.runs.size());
            o += t;
            for (const Run& r : me.runs) { snprintf(t, sizeof t, "%s %.3f s %.3f x %.1f\n", r.on ? "on " : "off", r.t0, r.dur, (double)r.mean); o += t; }
            int ok, bad; double u;
            m.letterMatch(me, m.list[(size_t)bi].letter, ok, bad, u);
            snprintf(t, sizeof t, "letter match ok %d bad %d unit %.3f\n", ok, bad, u); o += t;
        }
    return o;
}
uint64_t Analyzer::falseRuns() const { return p_->falseRuns; }

void Analyzer::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (!m.configured || m.groups.empty()) {
        if (n) { double s = 0; for (size_t i = 0; i < n; i += 7) s += std::norm(x[i]); m.powerDb = 10 * std::log10(std::max(1e-20, s / ((n + 6) / 7))); }
        return;
    }
    double e = 0;
    size_t i = 0;
    while (i < n) {
        const size_t take = std::min<size_t>(n - i, (size_t)(m.N - m.fpos));
        std::memcpy(&m.fbuf[(size_t)m.fpos], x + i, take * sizeof(cf32));
        for (size_t k = 0; k < take; k += 5) e += std::norm(x[i + k]);
        m.fpos += (int)take; i += take;
        if (m.fpos == m.N) { m.frame(); m.fpos = 0; }
    }
    m.powerDb = 0.9 * m.powerDb + 0.1 * 10 * std::log10(std::max(1e-20, e / ((n + 4) / 5)));
}

void Analyzer::clear() {
    Impl& m = *p_;
    for (Stat& s : m.stat) s = Stat();
    m.allDash.clear();
}

void Analyzer::fill(HfbTelemetry& t) const {
    const Impl& m = *p_;
    t.state = m.configured ? 1 : 0;
    t.rate = m.rate; t.centerHz = m.center; t.frames = m.frames;
    t.nowUtc = m.fi ? m.tOf(m.fi - 1) : m.tStart;
    t.clockValid = m.clockValid; t.clockOffsetSec = m.clockOff;
    t.spanLoHz = m.center - 0.4 * m.rate; t.spanHiHz = m.center + 0.4 * m.rate;
    t.channels = (int)m.meas.size();
    t.haveObserver = m.haveObs; t.obsLat = m.obsLat; t.obsLon = m.obsLon;
    t.powerDb = m.powerDb;
    t.sel = m.sel;
    const double now = t.nowUtc;
    std::vector<bool> inSpan(m.list.size(), false);
    for (const Meas& me : m.meas) for (int bi : me.bs) inSpan[(size_t)bi] = true;
    t.beacons.assign(m.list.size(), HfbState());
    t.bands.assign(7, HfbBand());
    for (int i = 0; i < 7; i++) t.bands[(size_t)i].name = i == 5 ? "other below 10 MHz" : i == 6 ? "other above 10 MHz" : bandName(i == 0 ? 28500 : i == 1 ? 24900 : i == 2 ? 21100 : i == 3 ? 18100 : 14100);
    t.heardNow = 0;
    for (size_t i = 0; i < m.list.size(); i++) {
        const Beacon& b = m.list[i];
        const Stat& s = m.stat[i];
        HfbState& o = t.beacons[i];
        o.inSpan = inSpan[i];
        const double hold = b.kind == Ncdxf ? 200 : b.kind == Letter ? 60 : b.kind == Dashes ? 90 : 20;
        o.lastHeard = s.lastHeard;
        o.heard = s.lastHeard > 0 && now - s.lastHeard < hold && s.conf > 0;
        o.conf = o.heard ? s.conf : (s.lastEnergy > 0 && now - s.lastEnergy < 20 ? 1 : 0);
        o.snr = s.snr; o.steps = o.heard ? s.steps : 0;
        for (int k = 0; k < 4; k++) o.stepSnr[k] = o.heard ? s.stepSnr[k] : NAN;
        o.note = s.note;
        float b1 = NAN, b24 = NAN;
        for (const HfbHistPt& p : s.hist) {
            if (!p.heard) continue;
            if (std::isnan(b24) || p.snr > b24) b24 = p.snr;
            if (now - p.t < 3600 && (std::isnan(b1) || p.snr > b1)) b1 = p.snr;
        }
        if (o.heard && s.lastHeard > 0) { if (std::isnan(b1) || s.snr > b1) b1 = s.snr; if (std::isnan(b24) || s.snr > b24) b24 = s.snr; }
        o.best1h = b1; o.best24h = b24;
        const size_t from = s.hist.size() > 48 ? s.hist.size() - 48 : 0;
        for (size_t k = from; k < s.hist.size(); k++) o.spark.push_back(s.hist[k].snr);
        if (m.haveObs) { double km, br; greatCircle(m.obsLat, m.obsLon, b.lat, b.lon, km, br); o.distKm = km; o.bearing = br; }
        HfbBand& bd = t.bands[(size_t)bandIndexOf(b.khz)];
        bd.total++;
        if (o.inSpan) bd.inSpan++;
        if (o.heard) {
            bd.heard++; t.heardNow++;
            if (!std::isnan(o.distKm) && (std::isnan(bd.bestKm) || o.distKm > bd.bestKm)) bd.bestKm = o.distKm;
        }
    }
    if (m.sel >= 0 && m.sel < (int)m.stat.size()) t.history.assign(m.stat[(size_t)m.sel].hist.begin(), m.stat[(size_t)m.sel].hist.end());
}

std::string Analyzer::saveResults() const {
    const Impl& m = *p_;
    std::vector<SavedHist> v;
    for (size_t i = 0; i < m.list.size(); i++) {
        const Stat& s = m.stat[i];
        if (s.hist.empty() && s.lastHeard == 0) continue;
        SavedHist h;
        char k[160]; snprintf(k, sizeof k, "%s|%ld", m.list[i].name.c_str(), std::lround(m.list[i].khz * 1000));
        h.key = k; h.lastHeard = s.lastHeard; h.snr = s.snr;
        h.pts.assign(s.hist.begin(), s.hist.end());
        v.push_back(std::move(h));
    }
    return results::pack(v);
}

bool Analyzer::loadResults(const std::string& blob) {
    std::vector<SavedHist> v;
    if (!results::unpack(blob, v)) return false;
    Impl& m = *p_;
    std::map<std::string, size_t> idx;
    for (size_t i = 0; i < m.list.size(); i++) { char k[160]; snprintf(k, sizeof k, "%s|%ld", m.list[i].name.c_str(), std::lround(m.list[i].khz * 1000)); idx[k] = i; }
    for (const SavedHist& h : v) {
        auto it = idx.find(h.key);
        if (it == idx.end()) continue;
        Stat& s = m.stat[it->second];
        s.hist.assign(h.pts.begin(), h.pts.end());
        s.lastHeard = h.lastHeard; s.snr = h.snr;
        s.conf = h.lastHeard > 0 ? 2 : 0;
        s.minute = -1;
    }
    return true;
}

} // namespace dect2::hfb
