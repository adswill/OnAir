// Auto gain, the whole matrix (docs/AUTOGAIN-3.7.md): every mode class on every radio gain model through the scenarios that define "works": settles quickly, ends within
// about 1 dB of the best quality the radio can give in that scene (an oracle sweeps every gain and stage split), never clips, handles a strong interferer that appears and
// goes away, does not hunt, does not interrupt the decoder once settled, recovers from a retune and a mode switch, stays still on an idle or bursty channel.
//
// The front end extends autogain_sim.h with what makes real radios hard: clipped samples are counted over the real number of samples per meter window (one clipped sample in
// half a million is a real event on an OFDM signal and harmless), the meter delivers a new window every 50 ms while the GUI ticks at 60 Hz, a decoder reports with its own
// latency and is deaf for a moment after each gain change, TDMA decoders flicker, a call ends and the channel goes idle, ADS-B is a population of aircraft with a message rate.
//
// Usage: test_autogain_matrix [--csv file] [--only radio] [--mode class] [--verbose]. Exit status 1 when a cell is outside its limit.
#include "autogain_sim.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>

using namespace dect2::ag;
using namespace agsim;

enum ModeClass { OFDM, FMB, TDMA, PULSED, WIDE, kModes };
static const char* kModeName[kModes] = {"ofdm", "fm", "tdma", "pulsed", "wide"};
static const int kStd[kModes] = {0, 7, 11, 13, 44};

struct RadioRow { const char* id; double rate; };
static const RadioRow kRadios[] = {{"rtl-r820t", 2.4e6}, {"hackrf", 10e6}, {"airspy", 6e6}, {"sdrplay", 6e6}, {"pluto", 10e6}, {"usrp", 10e6}, {"rtltcp", 2.4e6}, {"spyserver", 2.4e6}};

struct SceneDef {
    double sig = -70;                 // dBm in the channel; -200 = nothing there
    std::vector<Tone> blockers;
};

struct Fe : SimRadio {
    ModeClass mode = OFDM;
    double merCap = 35, latency = 0.4, glitch = 0.15;
    bool gaussian = false;            // the wanted signal has a Gaussian envelope (OFDM)
    std::vector<double> planes;       // pulsed: the aircraft (dBm per pulse at the antenna)
    double msgs = 0, loud = 0;
    double hold = 0, holdT = 0;
    double glitchUntil = -1;
    bool flicker = false; double flickerUntil = -1, flickerNext = 0;
    struct QS { double t; Quality q; };
    std::deque<QS> qq;

    explicit Fe(const RadioProfile& p, double total) : SimRadio(p, total) { jitterDb = 0.2; }

    double gauss() { double s = 0; for (int i = 0; i < 12; i++) s += r01(); return s - 6.0; }
    double toneAmp() const {
        const double g = outGain();
        double a = gaussian ? 0.0 : std::sqrt(lin(sigDbm + g + sigCrestDb));
        for (const auto& b : blockers) a += std::sqrt(lin(b.dbm + g)) * 0.8;
        return a;
    }
    double sigma() const {
        const double g = outGain();
        double p = lin(noiseDbmHz() + dbv(rateHz) + g) + lin(-(6.02 * prof.adcBits + 1.76));
        if (gaussian) p += lin(sigDbm + g);
        return std::sqrt(p / 2);
    }
    double clipProb() const {
        const double a = toneAmp(), s = sigma();
        if (a >= 1) return std::min(0.5, (a - 1) / a * 0.4);
        return std::min(1.0, 2.0 * std::erfc((0.995 - a) / (s * std::sqrt(2.0))));
    }
    double headroomNow() const { return -20 * std::log10(std::max(1e-9, std::min(1.0, toneAmp() + 3.9 * sigma()))); }

    BlockStats adcWindow(double now, double win) {
        BlockStats s;
        const double N = std::max(1.0, rateHz * win), p = clipProb(), l = N * p;
        double k = l < 30 ? 0 : std::max(0.0, l + std::sqrt(l) * gauss());
        if (l < 30) { const double e = std::exp(-l); double pr = 1; do { pr *= r01(); if (pr > e) k += 1; } while (pr > e); }
        const double a = toneAmp(), sg = sigma();
        s.clipFraction = std::min(1.0, k / N);
        s.n = (size_t)N;
        s.p999 = std::min(1.0, (a + 3.9 * sg) * (1 + jit() * 0.02));
        s.peak = k > 0 ? 1.0 : std::min(0.994, a + sg * std::sqrt(2 * std::log(N)));
        s.rmsDbfs = std::min(0.0, totalOutDbfs() + jit());
        s.crestDb = 20 * std::log10(std::max(s.p999, 1e-9)) - s.rmsDbfs;
        s.headroomDb = -20 * std::log10(std::max(s.p999, 1e-9));
        lastClip = p;
        if (mode == PULSED) {
            double top = -300;
            for (double d : planes) top = std::max(top, d);
            const double g = outGain();
            const double pk = std::sqrt(lin(top + g + 3));
            s.peak = std::min(1.0, std::max(s.peak, pk));
            if (pk >= 1) { s.clipFraction = std::min(1.0, s.clipFraction + 1e-5); s.peak = 1.0; }
        }
        hold = std::max(hold * std::pow(10.0, -(now - holdT) / 2.0), s.peak);
        holdT = now;
        s.holdHeadroomDb = hold > 1e-6 ? -20 * std::log10(hold) : 120;
        return s;
    }
    // ADS-B: messages this aircraft population gives per second at the present gain (and how many are near full scale)
    void pulseRates(double& rate, double& loudRate) const {
        rate = loudRate = 0;
        const double g = outGain(), n = noiseDbmHz() + 63;
        double in = lin(n);
        for (const auto& p : im3()) if (std::fabs(p.offHz) < 1e6) in += lin(p.dbm);
        const double noise = dbv(in + lin(-(6.02 * prof.adcBits + 1.76) - g + dbv(2e6 / rateHz)));
        for (double d : planes) {
            const double pk = d + g + 3;
            if (d - noise < 9) continue;
            if (pk > 6) continue;
            rate += 6;
            if (pk > -3) loudRate += 6;
        }
    }
    double trueQuality() const {
        if (mode == PULSED) { double r, l; pulseRates(r, l); return 10 * std::log10(r + 0.2); }
        return std::min(snrDb(), merCap);
    }
    Quality decoderNow(double now, double dt) {
        Quality q;
        if (mode == PULSED) {
            double r, l;
            pulseRates(r, l);
            msgs += r * dt; loud += l * dt;
            q.pulseValid = true; q.pulseMsgs = (uint64_t)msgs; q.pulseLoud = (uint64_t)loud;
            q.locked = r > 0;
            return q;
        }
        if (!decoder) return q;
        const double s = snrDb();
        const bool deaf = now < glitchUntil;
        if (flicker && now >= flickerNext) { flickerNext = now + 0.2; if (r01() < 0.05) flickerUntil = now + 0.2; }
        if (s < lockAt || deaf || now < flickerUntil) return q;
        q.locked = true;
        q.snrValid = true; q.snrDb = std::min(s + jit() * 2, 38.0);
        q.merValid = mode == OFDM; q.merDb = std::min(s + jit() * 2, merCap);
        return q;
    }
};

static ModeTarget targetFor(ModeClass m, double rate, double bw) {
    const double f = m == PULSED ? 1090e6 : m == FMB ? 98e6 : m == WIDE ? 450e6 : m == TDMA ? 446e6 : 522e6;
    return modeTargetFor(kStd[m], bw, rate, f);
}
static double bwOf(ModeClass m, double rate) {
    switch (m) {
    case OFDM: return std::min(0.75 * rate, 7.6e6);
    case FMB: return 200e3;
    case TDMA: return 12.5e3;
    case PULSED: return 2e6;
    default: return 0.8 * rate;
    }
}
static void applyScene(Fe& r, ModeClass m, const SceneDef& s) {
    r.sigDbm = s.sig; r.blockers = s.blockers;
    if (m == WIDE) r.sigDbm = s.sig;
}
static SceneDef defaultScene(ModeClass m, double rate) {
    SceneDef s;
    switch (m) {
    case OFDM: s.sig = -62; break;
    case FMB: s.sig = -58; for (int i = 0; i < 6; i++) s.blockers.push_back({-1.0e6 + i * 0.37e6, -42.0 - 2.0 * i}); break;
    case TDMA: s.sig = -105; s.blockers.push_back({0.31 * rate, -55}); break;
    case PULSED: s.sig = -200; break;
    default: s.sig = -80; for (int i = 0; i < 4; i++) s.blockers.push_back({-0.3 * rate + i * 0.17 * rate, -38.0 - 3 * i}); break;
    }
    return s;
}

static void setupFe(Fe& r, ModeClass m, double rate) {
    r.mode = m; r.rateHz = rate; r.chanOff = 0;
    r.chanBw = m == WIDE ? 200e3 : bwOf(m, rate);
    r.lockAt = m == OFDM ? 14 : m == FMB ? 12 : m == TDMA ? 7 : 6;
    r.merCap = m == OFDM ? 32 : m == FMB ? 45 : m == TDMA ? 28 : 60;
    r.latency = m == OFDM ? 0.5 : m == FMB ? 0.2 : m == TDMA ? 0.4 : 0;
    r.glitch = m == OFDM ? 0.2 : m == FMB ? 0.05 : m == TDMA ? 0.12 : 0.05;
    r.gaussian = m == OFDM || m == FMB;
    r.sigCrestDb = m == TDMA ? 2 : 3;
    r.decoder = m != WIDE;
    r.flicker = m == TDMA;
    r.bins = 512;
    if (m == PULSED) for (int i = 0; i < 40; i++) { const double x = i / 39.0; r.planes.push_back(-97 + 49 * x * x * x); }
}

// the best the radio can do in this scene: every gain, every stage split, under the constraints the mode asks for
static double oracle(const Fe& r0, const ModeTarget& t, double* bestGain = nullptr) {
    Fe r = r0;
    r.pendingAt = -1;
    r.lastClip = 0;
    double best = -300, bg = 0;
    uint32_t mask = 0xffffffffu;
    for (Policy pol : {Policy::Sensitivity, Policy::Balanced, Policy::Linearity})
        for (double g : totalSteps(r.prof, pol, mask, false)) {
            r.live = planStages(r.prof, g, pol, mask, {}, false);
            const double p = r.clipProb();
            if (p > t.clipLimit || r.headroomNow() < std::max(0.5, t.minHeadroomDb - 1.0)) continue;
            r.lastClip = p;
            const double q = r.trueQuality();
            if (q > best + 0.01) { best = q; bg = g; }
        }
    if (bestGain) *bestGain = bg;
    return best;
}

struct Sample { double t, gain, q, clip; bool deaf; };
struct Rig {
    RadioProfile prof;
    Fe r;
    AutoGainController c;
    ModeClass m;
    double rate, bw, now = 0, meterNext = 0, specNext = 0;
    ModeTarget tgt;
    BlockStats lastAdc;
    std::vector<float> spec;
    std::vector<Sample> log;
    int changes = 0;
    bool haveMeter = false;

    Rig(const RadioProfile& p, ModeClass mc, double rt, double startDb) : prof(p), r(p, startDb), m(mc), rate(rt), bw(bwOf(mc, rt)) {
        setupFe(r, mc, rt);
        tgt = targetFor(mc, rt, bw);
        begin();
    }
    AgSettings settings() const { AgSettings s; s.mode = AgMode::Auto; return s; }
    void begin() {
        const AgSettings s = settings();
        c.configure(prof, tgt, s);
        StagePlan p = r.live;
        c.start(now, p.totalDb, p.ampOn, &p);
        // the radio's start state: the plan the controller believes (a real radio is at the settings it was started with)
        r.live = c.current(); r.pendingAt = -1;
    }
    void changeMode(ModeClass mc) {
        m = mc; setupFe(r, mc, rate); bw = bwOf(mc, rate); tgt = targetFor(mc, rate, bw);
        begin();
    }
    void step(double dt, const std::function<void(double)>* scene) {
        if (scene) (*scene)(now);
        const StagePlan before = r.live;
        r.advance(now);
        if (!(r.live == before)) r.glitchUntil = now + r.glitch;
        if (now >= meterNext) { lastAdc = r.adcWindow(now, 0.05); meterNext = now + 0.05; haveMeter = true; }
        if (now >= specNext) { spec = r.spectrum(); specNext = now + 0.1; }
        Quality q = r.decoderNow(now, dt);
        r.qq.push_back({now, q});
        while (r.qq.size() > 1 && r.qq[1].t <= now - r.latency) r.qq.pop_front();
        AgInput in;
        in.now = now;
        in.adc = lastAdc;
        in.spectrum = &spec;
        in.rateHz = rate; in.chanBwHz = bw; in.offsetHz = 0;
        in.q = r.qq.front().q;
        AgCommand out;
        if (c.tick(in, out)) { r.apply(out.plan, now); changes++; }
        log.push_back({now, r.live.totalDb, r.trueQuality(), r.clipProb(), now < r.glitchUntil});
        now += dt;
    }
    void run(double sec, const std::function<void(double)>& scene) { const double end = now + sec; while (now < end) step(1.0 / 60, &scene); }
    void run(double sec) { const double end = now + sec; while (now < end) step(1.0 / 60, nullptr); }
};

static double medianQ(const std::vector<Sample>& l, double t0, double t1) {
    std::vector<double> v;
    for (const auto& s : l) if (s.t >= t0 && s.t <= t1) v.push_back(s.q);
    if (v.empty()) return -300;
    std::nth_element(v.begin(), v.begin() + (long)(v.size() / 2), v.end());
    return v[v.size() / 2];
}
// the first time from which the quality stays within tol of the goal for hold seconds, and the ADC is clean
static double convergeAt(const std::vector<Sample>& l, double t0, double t1, double goal, double tol, double limit, double hold = 5) {
    double run = -1;
    for (const auto& s : l) {
        if (s.t < t0 || s.t > t1) continue;
        if (s.q >= goal - tol && s.clip <= limit) { if (run < 0) run = s.t; if (s.t - run >= hold) return run - t0; }
        else run = -1;
    }
    return 1e9;
}
static double clipSeconds(const std::vector<Sample>& l, double t0, double t1, double limit) {
    double n = 0;
    for (const auto& s : l) if (s.t >= t0 && s.t <= t1 && s.clip > limit) n += 1.0 / 60;
    return n;
}
static double deafSeconds(const std::vector<Sample>& l, double t0, double t1) {
    double n = 0;
    for (const auto& s : l) if (s.t >= t0 && s.t <= t1 && s.deaf) n += 1.0 / 60;
    return n;
}
static void gainSpan(const std::vector<Sample>& l, double t0, double t1, double& lo, double& hi, int& moves) {
    lo = 1e9; hi = -1e9; moves = 0;
    double prev = -1e9;
    for (const auto& s : l) {
        if (s.t < t0 || s.t > t1) continue;
        lo = std::min(lo, s.gain); hi = std::max(hi, s.gain);
        if (prev > -1e8 && std::fabs(s.gain - prev) > 0.01) moves++;
        prev = s.gain;
    }
}

struct Limits { double settle, dq, acqClip, chg, deaf, irClip, irDq, irRec, small, idleChg, idleSpan, burstChg; };
static Limits limitsFor(ModeClass m, const RadioProfile& p) {
    Limits l;
    l.settle = (m == PULSED ? 60 : 12) * (p.remote ? 2.0 : 1.0);
    l.dq = 1.0; l.acqClip = 1.5; l.chg = 30; l.deaf = 12; l.irClip = 1.5; l.irDq = 2.0;
    l.irRec = p.remote ? 45 : 30; l.small = 1; l.idleChg = 20; l.idleSpan = 6; l.burstChg = 30;
    return l;
}

struct Row {
    std::string radio, mode;
    double settle = 0, dq = -99, acqClip = 0, chg = 0, deaf = 0, irClip = 0, irDq = -99, irRec = 0, rtS = 0, rtDq = -99, small = 0, modeS = 0, idleChg = 0, idleSpan = 0, burstChg = 0, burstDq = -99, clipSteady = 0;
    std::string fails;
};

static bool gVerbose = false;

static Row runCell(const RadioRow& rr, ModeClass m) {
    Row row;
    row.radio = rr.id; row.mode = kModeName[m];
    const RadioProfile prof = *radioProfileById(rr.id);
    const double lo = prof.minTotal(false), hi = prof.maxTotal(false), mid = (lo + hi) / 2;
    const Limits lim = limitsFor(m, prof);
    const SceneDef sc0 = defaultScene(m, rr.rate);
    const double clipLim = targetFor(m, rr.rate, bwOf(m, rr.rate)).clipLimit;
    // 1. acquisition from the bottom, the middle and the top of the range, then 20 minutes steady
    for (double start : {lo, mid, hi}) {
        Rig g(prof, m, rr.rate, start);
        applyScene(g.r, m, sc0);
        const double best = oracle(g.r, g.tgt);
        g.run(60);
        const double conv = convergeAt(g.log, 0, 60, best, 1.0, clipLim);
        if (gVerbose) { printf("  start %.0f: conv %.1f, q at 60 s %.1f (best %.1f), gain %.1f split", start, conv, g.log.back().q, best, g.r.live.totalDb); for (double x : g.r.live.db) printf(" %.0f", x); printf("\n"); if (start != mid) { for (const auto& e : g.c.log()) if (e.t < 30) printf("      %6.1f %5.1f -> %5.1f %s\n", e.t, e.fromDb, e.toDb, e.why.c_str()); } }
        row.settle = std::max(row.settle, conv);
        row.acqClip = std::max(row.acqClip, clipSeconds(g.log, 0, 60, clipLim));
        if (start == mid) {
            g.run(1200);
            row.dq = std::max(row.dq, best - medianQ(g.log, 1200, 1260));
            int moves; double a, b;
            gainSpan(g.log, 60, 1260, a, b, moves);
            row.chg = moves * 3.0;
            row.deaf = deafSeconds(g.log, 60, 1260) * 3.0;
            row.clipSteady = clipSeconds(g.log, 60, 1260, clipLim);
            if (gVerbose) {
                double bg = 0;
                oracle(g.r, g.tgt, &bg);
                printf("  %s/%s best %.1f at gain %.1f, final gain %.1f (true q %.1f) changes %d, status: %s\n", rr.id, kModeName[m], best, bg, g.r.live.totalDb, g.r.trueQuality(), moves, g.c.status().c_str());
                const auto& lg = g.c.log();
                for (size_t i = 0; i < lg.size(); i++) printf("    %7.1f %5.1f -> %5.1f  %s\n", lg[i].t, lg[i].fromDb, lg[i].toDb, lg[i].why.c_str());
            }
        } else row.dq = std::max(row.dq, best - medianQ(g.log, 45, 60));
    }
    // 2. a strong interferer comes (t = 60) and goes (t = 180)
    {
        Rig g(prof, m, rr.rate, mid);
        applyScene(g.r, m, sc0);
        g.run(60);
        SceneDef with = sc0;
        with.blockers.push_back({0.22 * rr.rate, -22});
        applyScene(g.r, m, with);
        const double bestWith = oracle(g.r, g.tgt);
        const double t0 = g.now;
        g.run(120);
        row.irClip = clipSeconds(g.log, t0, t0 + 120, clipLim);
        row.irDq = bestWith - medianQ(g.log, t0 + 90, t0 + 120);
        applyScene(g.r, m, sc0);
        const double best = oracle(g.r, g.tgt);
        const double t1 = g.now;
        g.run(150);
        row.irRec = convergeAt(g.log, t1, t1 + 150, best, 1.0, clipLim, 8);
    }
    // 3. a retune to a much weaker channel, the app's way (a fresh start); a small VFO move inside the span; a mode switch
    {
        Rig g(prof, m, rr.rate, mid);
        applyScene(g.r, m, sc0);
        g.run(60);
        SceneDef weak = sc0;
        if (m != PULSED) weak.sig = sc0.sig - 18;
        weak.blockers.clear();
        applyScene(g.r, m, weak);
        const double best = oracle(g.r, g.tgt);
        g.begin();
        const double t0 = g.now;
        g.run(90);
        row.rtS = convergeAt(g.log, t0, t0 + 90, best, 1.0, clipLim);
        row.rtDq = best - medianQ(g.log, t0 + 75, t0 + 90);
        // the small move: the VFO steps two channels inside the span and the gain must not be searched again
        applyScene(g.r, m, sc0);
        g.run(30);
        const int c0 = g.changes;
#if defined(AUTOGAIN_BEFORE)
        g.begin();     // what the app did on any frequency change
#else
        if (retuneNeedsSearch(25e3, rr.rate, false)) g.begin();
#endif
        g.run(20);
        row.small = g.changes - c0;
        // a mode switch
        ModeClass nx = (ModeClass)((m + 1) % kModes);
        applyScene(g.r, nx, defaultScene(nx, rr.rate));
        g.changeMode(nx);
        const double bestN = oracle(g.r, g.tgt);
        const double t1 = g.now;
        g.run(60);
        row.modeS = convergeAt(g.log, t1, t1 + 60, bestN, 1.0, g.tgt.clipLimit);
    }
    // 4. idle: nothing in the channel for 20 minutes
    {
        Rig g(prof, m, rr.rate, mid);
        SceneDef idle = sc0;
        idle.sig = -200;
        if (m == PULSED) g.r.planes.clear();
        applyScene(g.r, m, idle);
        g.run(1200);
        double a, b; int moves;
        gainSpan(g.log, 60, 1200, a, b, moves);
        row.idleChg = moves * 3600.0 / 1140.0;
        row.idleSpan = b - a;
    }
    // 5. bursty: calls of 8-30 s with 10-60 s of silence
    if (m != PULSED) {
        Rig g(prof, m, rr.rate, mid);
        applyScene(g.r, m, sc0);
        const double best = oracle(g.r, g.tgt);
        double next = 30; bool on = true;
        std::vector<double> qOn;
        uint32_t seed = 7;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (double)(seed >> 8) / 16777216.0; };
        std::function<void(double)> sceneFn = [&](double t) {
            if (t >= next) { on = !on; next = t + (on ? 8 + 22 * rnd() : 10 + 50 * rnd()); SceneDef s = sc0; if (!on) s.sig = -200; applyScene(g.r, m, s); }
        };
        g.run(1200, sceneFn);
        double a, b; int moves;
        gainSpan(g.log, 60, 1200, a, b, moves);
        row.burstChg = moves * 3600.0 / 1140.0;
        for (const auto& s : g.log) if (s.t > 60 && s.q > -100) qOn.push_back(s.q);
        if (!qOn.empty()) { std::nth_element(qOn.begin(), qOn.begin() + (long)(qOn.size() * 0.9), qOn.end()); row.burstDq = best - qOn[(size_t)(qOn.size() * 0.9)]; }
    }
    auto flag = [&](bool bad, const char* s) { if (bad) { if (!row.fails.empty()) row.fails += ","; row.fails += s; } };
    flag(row.settle > lim.settle, "settle");
    flag(row.dq > lim.dq, "quality");
    flag(row.acqClip > lim.acqClip, "acqclip");
    flag(row.chg > lim.chg, "hunt");
    flag(row.deaf > lim.deaf, "deaf");
    flag(row.clipSteady > 0.1, "clip");
    flag(row.irClip > lim.irClip, "irclip");
    flag(row.irDq > lim.irDq, "irq");
    flag(row.irRec > lim.irRec, "irrec");
    flag(row.rtS > lim.settle, "retune");
    flag(row.rtDq > lim.dq, "rtq");
    flag(row.small > lim.small, "small");
    flag(row.modeS > lim.settle, "modesw");
    flag(row.idleChg > lim.idleChg, "idle");
    flag(row.idleSpan > lim.idleSpan, "idlespan");
    flag(row.burstChg > lim.burstChg, "bursty");
    flag(m != PULSED && row.burstDq > 1.5, "burstq");
    return row;
}

static std::string fmtS(double v) { char b[24]; if (v > 1e8) snprintf(b, sizeof b, "never"); else snprintf(b, sizeof b, "%.1f", v); return b; }

int main(int argc, char** argv) {
    const char* csv = nullptr, *only = nullptr, *modeArg = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv = argv[++i];
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) modeArg = argv[++i];
        else if (!strcmp(argv[i], "--verbose")) gVerbose = true;
    }
    FILE* f = csv ? fopen(csv, "w") : nullptr;
    const char* hdr = "radio,mode,settle_s,dq_db,acq_clip_s,chg_per_h,deaf_s_per_h,clip_steady_s,ir_clip_s,ir_dq_db,ir_rec_s,rt_s,rt_dq_db,small_chg,mode_s,idle_chg_per_h,idle_span_db,burst_chg_per_h,burst_dq_db,fails";
    if (f) fprintf(f, "%s\n", hdr);
    printf("%-10s %-6s %7s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %5s %6s %6s %5s %6s %6s  %s\n", "radio", "mode", "settle", "dQ", "acqclp", "chg/h", "deaf/h", "clip", "irclp", "irdQ", "irrec", "rt", "rtdQ", "small", "modesw", "idl/h", "idlsp", "brst/h", "brstdQ", "FAILS");
    int cells = 0, bad = 0, failCount[32] = {}; std::vector<std::string> names;
    std::map<std::string, int> byFail;
    for (const auto& rr : kRadios) {
        if (only && strcmp(only, rr.id)) continue;
        for (int m = 0; m < kModes; m++) {
            if (modeArg && strcmp(modeArg, kModeName[m])) continue;
            const Row r = runCell(rr, (ModeClass)m);
            cells++;
            if (!r.fails.empty()) bad++;
            size_t a = 0;
            while (a < r.fails.size()) { size_t b = r.fails.find(',', a); if (b == std::string::npos) b = r.fails.size(); byFail[r.fails.substr(a, b - a)]++; a = b + 1; }
            printf("%-10s %-6s %7s %6.1f %6.1f %6.0f %6.1f %6.1f %6.1f %6.1f %6s %6s %6.1f %5.0f %6s %6.0f %5.1f %6.0f %6.1f  %s\n", r.radio.c_str(), r.mode.c_str(), fmtS(r.settle).c_str(), r.dq, r.acqClip, r.chg, r.deaf, r.clipSteady, r.irClip, r.irDq,
                   fmtS(r.irRec).c_str(), fmtS(r.rtS).c_str(), r.rtDq, r.small, fmtS(r.modeS).c_str(), r.idleChg, r.idleSpan, r.burstChg, r.burstDq, r.fails.c_str());
            fflush(stdout);
            if (f) fprintf(f, "%s,%s,%s,%.2f,%.2f,%.1f,%.2f,%.2f,%.2f,%.2f,%s,%s,%.2f,%.0f,%s,%.1f,%.2f,%.1f,%.2f,%s\n", r.radio.c_str(), r.mode.c_str(), fmtS(r.settle).c_str(), r.dq, r.acqClip, r.chg, r.deaf, r.clipSteady, r.irClip, r.irDq,
                           fmtS(r.irRec).c_str(), fmtS(r.rtS).c_str(), r.rtDq, r.small, fmtS(r.modeS).c_str(), r.idleChg, r.idleSpan, r.burstChg, r.burstDq, r.fails.empty() ? "-" : ("\"" + r.fails + "\"").c_str());
        }
    }
    (void)failCount;
    printf("\ncells %d, cells with a failure %d\n", cells, bad);
    for (const auto& kv : byFail) printf("  %-9s %d\n", kv.first.c_str(), kv.second);
    if (f) fclose(f);
    return bad ? 1 : 0;
}
