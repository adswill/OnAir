// Smart auto gain: the band bias, the quick bracketing search of Auto, the explanation line and Re-optimise, against the simulated radio of
// autogain_sim.h with scenarios: weak target beside strong stations, an empty band, the FM band, bursts, a drifting level. For every scene the
// controller's result is compared with the exhaustively found best safe gain (the highest in-channel SNR over all reachable totals that do
// not clip). Prints the measured numbers (ctest autogain_smart; -v for the table).
#include "autogain_sim.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace dect2;
using namespace dect2::ag;
using namespace agsim;

static int fails = 0;
static bool verbose = false;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static RadioProfile prof(const char* id) { const RadioProfile* p = radioProfileById(id); if (!p) { printf("no profile %s\n", id); exit(1); } return *p; }

struct Scene { const char* name; int std_; double freqHz, bw, rate, off, sig; std::vector<Tone> blockers; };

// the best SNR over every reachable total that does not clip and keeps 1 dB of headroom
static double bestSnr(const RadioProfile& rp, const Scene& sc, const ModeTarget& tg, double* bestGain) {
    double best = -1e9, bg = 0;
    // the controller's own rule: a strong neighbour gets the linear stage split (it costs noise figure); the optimum is taken within that rule
    const Policy pol = sc.blockers.empty() ? tg.policy : Policy::Linearity;
    for (double g : totalSteps(rp, pol, 0xffffffffu, false)) {
        SimRadio s(rp, g, false);
        s.rateHz = sc.rate; s.chanBw = sc.bw; s.chanOff = sc.off; s.sigDbm = sc.sig; s.blockers = sc.blockers; s.jitterDb = 0;
        s.live = planStages(rp, g, pol, 0xffffffffu, {}, false);
        const BlockStats a = s.adc();
        if (a.clipFraction > tg.clipLimit || a.headroomDb < tg.headroomDb - 1.0) continue;   // the mode headroom target (the aim is the most SNR that still keeps it)
        const double v = s.snrDb();
        if (v > best + 0.01) { best = v; bg = g; }
    }
    if (bestGain) *bestGain = bg;
    return best;
}

struct Out { double gain = 0, snr = 0, bestSnr = 0, bestGain = 0, convSec = 0; int changes60 = 0; double clipSec = 0; int bracketChanges = 0; std::string expl; };

static Out runScene(const RadioProfile& rp, const Scene& sc, double startDb, double sec = 90) {
    SimRadio sim(rp, startDb);
    sim.rateHz = sc.rate; sim.chanBw = sc.bw; sim.chanOff = sc.off; sim.sigDbm = sc.sig; sim.blockers = sc.blockers;
    const ModeTarget tg = modeTargetFor(sc.std_, sc.bw, sc.rate, sc.freqHz);
    AutoGainController c;
    AgSettings s; s.mode = AgMode::Auto; s.v2 = false;
    c.configure(rp, tg, s);
    double now = 0;
    c.start(now, sim.live.totalDb, false, &sim.live);
    Out o;
    double prev = sim.live.totalDb, lastChange = 0, brEnd = -1;
    bool wasBr = false;
    run(c, sim, now, sec, [&](double t) {
        if (c.bracketing()) wasBr = true;
        else if (wasBr && brEnd < 0) brEnd = t;
        if (std::fabs(c.current().totalDb - prev) > 0.01) { prev = c.current().totalDb; lastChange = t; if (t > 10) o.changes60++; }
        if (sim.lastClip > tg.clipLimit) o.clipSec += 0.05;
    });
    o.gain = c.current().totalDb;
    SimRadio fin(rp, o.gain, false);
    fin.rateHz = sc.rate; fin.chanBw = sc.bw; fin.chanOff = sc.off; fin.sigDbm = sc.sig; fin.blockers = sc.blockers; fin.jitterDb = 0;
    fin.live = c.current();
    o.snr = fin.snrDb();
    o.bestSnr = bestSnr(rp, sc, tg, &o.bestGain);
    o.convSec = std::max(brEnd, 0.0);
    (void)lastChange;
    o.expl = c.explanation();
    if (getenv("AGTR") && o.bestSnr - o.snr > 2) for (const auto& t : c.trials()) printf("   trial %.0f dB head %.1f clip %.4f imd %d snr %.1f\n", t.plan.totalDb, t.headroomMin, t.clipMax, (int)t.imd, t.chanSnr());
    if (getenv("AGLOG") && (o.changes60 > 3 || o.bestSnr - o.snr > 2 || o.convSec <= 0)) for (const auto& e : c.log()) printf("     %.1f %.1f -> %.1f %s\n", e.t, e.fromDb, e.toDb, e.why.c_str());
    return o;
}

static void testBands() {
    const ModeTarget fm = modeTargetFor(11, 12.5e3, 2.4e6, 98e6), hf = modeTargetFor(11, 12.5e3, 2.4e6, 7.1e6), uhf = modeTargetFor(11, 12.5e3, 2.4e6, 433e6),
                     lb = modeTargetFor(11, 12.5e3, 2.4e6, 1.65e9), plain = modeTargetFor(11, 12.5e3, 2.4e6);
    CHECK(fm.headroomDb > uhf.headroomDb + 2 && fm.policy == Policy::Linearity, "FM band: more headroom, linear (%.0f vs %.0f)", fm.headroomDb, uhf.headroomDb);
    CHECK(hf.headroomDb > uhf.headroomDb + 2 && hf.marginGoalDb < uhf.marginGoalDb, "HF: more headroom, lower sensitivity goal");
    CHECK(lb.headroomDb < uhf.headroomDb && lb.marginGoalDb > uhf.marginGoalDb && lb.policy == Policy::Sensitivity, "L-band: sensitivity");
    CHECK(std::fabs(uhf.headroomDb - plain.headroomDb) < 0.01 && !*plain.bandNote, "no frequency or a neutral band: unchanged");
    CHECK(*fm.bandNote && *hf.bandNote && *lb.bandNote, "bands explain themselves");
    CHECK(modeTargetFor(13, 2e6, 2.4e6, 1090e6).headroomDb == modeTargetFor(13, 2e6, 2.4e6).headroomDb, "ADS-B keeps its own target");
    CHECK(std::string(bandIdFor(98e6)) == "fm" && std::string(bandIdFor(7e6)) == "hf" && std::string(bandIdFor(0)) == "", "band ids");
}

static void testScenes() {
    struct Row { const char* radio; Scene sc; double maxErr; };
    const std::vector<Tone> strongAdj = {{600e3, -44}, {-700e3, -46}};
    const std::vector<Tone> fmStations = {{-900e3, -40}, {-300e3, -42}, {500e3, -38}, {1000e3, -46}};
    const Scene weakBeside = {"weak beside strong", 11, 446e6, 12.5e3, 2.4e6, 100e3, -112, strongAdj};
    const Scene empty = {"empty UHF", 11, 446e6, 12.5e3, 2.4e6, 100e3, -125, {}};
    const Scene fmBand = {"FM band stations", 7, 98e6, 200e3, 2.4e6, 0, -55, fmStations};
    const Scene tvScene = {"DVB-T2 channel", 1, 586e6, 7.6e6, 9.14e6, 0, -65, {{4e6, -45}}};
    const Scene lband = {"quiet L-band", 11, 1.65e9, 12.5e3, 2.4e6, 100e3, -125, {}};
    double sumErr = 0; int n = 0, worstSteps = 0; double worstConv = 0, worstErr = 0;
    if (verbose) printf("%-9s %-20s %6s -> %6s (best %6s) snr %6s best %6s  conv  changes(10..90s) clipSec\n", "radio", "scene", "start", "gain", "gain", "dB", "dB");
    for (const char* id : {"hackrf", "rtl-r820t", "airspy", "sdrplay", "pluto"}) {
        const RadioProfile rp = prof(id);
        for (const Scene* sc : {&weakBeside, &empty, &fmBand, &tvScene, &lband}) {
            const double mid = (rp.minTotal(false) + rp.maxTotal(false)) / 2;
            for (double start : {rp.minTotal(false), mid, rp.maxTotal(false)}) {
                const Out o = runScene(rp, *sc, start);
                const double err = o.bestSnr - o.snr;
                if (verbose) printf("%-9s %-20s %6.1f -> %6.1f (best %6.1f) snr %6.1f best %6.1f  %4.1fs  %d  %.2f\n", id, sc->name, start, o.gain, o.bestGain, o.snr, o.bestSnr, o.convSec, o.changes60, o.clipSec);
                CHECK(o.bestSnr > -1e8, "%s/%s: no safe gain at all?", id, sc->name);
                // within 2 dB of the exhaustive optimum (a radio with 8 / 10 dB steps cannot do better than its steps)
                const double allow = 2.0 + (rp.driverSplits ? 6.0 : rp.settableCount() <= 1 ? 3.0 : 0.0);   // a driver that splits the gain itself hides stages from us
                // with the wanted signal far below the noise the spectrum's SNR cannot tell neighbouring gains apart, so the headroom target decides (2 dB more)
                const double allowB = allow + (sc->sig < -110 && !sc->blockers.empty() ? 2.0 : 0.0);
                CHECK(err <= allowB, "%s/%s from %.0f: SNR %.1f is %.1f dB under the best %.1f (gain %.1f vs %.1f)", id, sc->name, start, o.snr, err, o.bestSnr, o.gain, o.bestGain);
                CHECK(o.convSec > 0 && o.convSec < 10, "%s/%s: the quick search took %.1f s", id, sc->name, o.convSec);
                CHECK(o.changes60 <= 3, "%s/%s: %d changes after 10 s (hunting)", id, sc->name, o.changes60);
                CHECK(o.clipSec < (start > (rp.minTotal(false) + rp.maxTotal(false)) / 2 + 1 ? 1.2 : 0.9), "%s/%s from %.0f: clipped for %.2f s in total", id, sc->name, start, o.clipSec);
                CHECK(!o.expl.empty() && o.expl.find("Gain set to") == 0, "%s/%s: explanation '%s'", id, sc->name, o.expl.c_str());
                sumErr += std::max(0.0, err); n++; worstErr = std::max(worstErr, err); worstConv = std::max(worstConv, o.convSec);
            }
        }
    }
    printf("autogain_smart: %d runs, mean SNR shortfall %.2f dB, worst %.2f dB, quick search done by %.1f s at worst\n", n, sumErr / n, worstErr, worstConv);
    (void)worstSteps;
}

// strong-signal and quiet scenes must end in different, sensible states
static void testStatesDiffer() {
    const RadioProfile rp = prof("hackrf");
    const Out fmO = runScene(rp, {"fm", 7, 98e6, 200e3, 2.4e6, 0, -55, {{-900e3, -34}, {-300e3, -36}, {500e3, -32}}}, 40);
    const Out quiet = runScene(rp, {"quiet", 11, 1.65e9, 12.5e3, 2.4e6, 100e3, -125, {}}, 40);
    CHECK(quiet.gain > fmO.gain + 10, "quiet L-band gets far more gain (%.1f) than the FM band (%.1f)", quiet.gain, fmO.gain);
    CHECK(fmO.expl.find("FM") != std::string::npos, "the FM scene's explanation names the FM stations: '%s'", fmO.expl.c_str());
    if (verbose) printf("FM: %s\nquiet: %s\n", fmO.expl.c_str(), quiet.expl.c_str());
}

// bursts and a drifting level: no sustained clipping, no pumping
static void testDynamic() {
    const RadioProfile rp = prof("hackrf");
    {   // a transmitter keys up 5 times for 1.5 s
        SimRadio sim(rp, 30);
        sim.rateHz = 2.4e6; sim.chanBw = 12.5e3; sim.chanOff = 100e3; sim.sigDbm = -108;
        const ModeTarget tg = modeTargetFor(11, 12.5e3, 2.4e6, 446e6);
        AutoGainController c; AgSettings s; s.mode = AgMode::Auto; s.v2 = false; c.configure(rp, tg, s);
        double now = 0; c.start(now, sim.live.totalDb, false, &sim.live);
        run(c, sim, now, 30);
        if (getenv("AGLOG")) for (const auto& e : c.log()) printf("   B %.1f %.1f -> %.1f %s\n", e.t, e.fromDb, e.toDb, e.why.c_str());
        double clipSec = 0; int changes = 0; double prev = c.current().totalDb;
        for (int k = 0; k < 5; k++) {
            sim.blockers = {{500e3, -28}};
            run(c, sim, now, 1.5, [&](double) { if (sim.lastClip > tg.clipLimit) clipSec += 0.05; if (std::fabs(c.current().totalDb - prev) > 0.01) { prev = c.current().totalDb; changes++; } });
            sim.blockers.clear();
            run(c, sim, now, 15, [&](double) { if (std::fabs(c.current().totalDb - prev) > 0.01) { prev = c.current().totalDb; changes++; } });
        }
        if (getenv("AGLOG")) for (const auto& e : c.log()) if (e.t > 30) printf("   B %.1f %.1f -> %.1f %s\n", e.t, e.fromDb, e.toDb, e.why.c_str());
        CHECK(clipSec < 3.5, "bursts: clipped %.2f s over 5 bursts", clipSec);
        CHECK(changes < 40, "bursts: %d gain changes over 5 bursts (pumping)", changes);
        if (verbose) printf("bursts: clip %.2f s, %d changes\n", clipSec, changes);
    }
    {   // the level drifts from -115 to -75 dBm in 60 s and back
        SimRadio sim(rp, 40);
        sim.rateHz = 2.4e6; sim.chanBw = 12.5e3; sim.chanOff = 100e3; sim.sigDbm = -115;
        const ModeTarget tg = modeTargetFor(11, 12.5e3, 2.4e6, 446e6);
        AutoGainController c; AgSettings s; s.mode = AgMode::Auto; s.v2 = false; c.configure(rp, tg, s);
        double now = 0; c.start(now, sim.live.totalDb, false, &sim.live);
        double clipSec = 0; int changes = 0, rev = 0, dir = 0; double prev = c.current().totalDb;
        run(c, sim, now, 120, [&](double t) {
            sim.sigDbm = -95 + 20 * std::sin(t / 120.0 * 6.2831853 * 1.0);
            if (sim.lastClip > tg.clipLimit) clipSec += 0.05;
            if (std::fabs(c.current().totalDb - prev) > 0.01) { const int d = c.current().totalDb > prev ? 1 : -1; if (dir && d != dir) rev++; dir = d; prev = c.current().totalDb; changes++; }
        });
        CHECK(clipSec < 1.0, "drift: clipped %.2f s", clipSec);
        CHECK(changes <= 30 && rev <= 6, "drift: %d changes, %d reversals over 120 s", changes, rev);
        if (verbose) printf("drift: clip %.2f s, %d changes, %d reversals\n", clipSec, changes, rev);
    }
}

// Re-optimise runs the search again; a gain from the memory skips it; quickStart off keeps the old behaviour
static void testReoptimise() {
    const RadioProfile rp = prof("hackrf");
    SimRadio sim(rp, 20);
    sim.rateHz = 2.4e6; sim.chanBw = 12.5e3; sim.chanOff = 100e3; sim.sigDbm = -110;
    const ModeTarget tg = modeTargetFor(11, 12.5e3, 2.4e6, 446e6);
    AutoGainController c; AgSettings s; s.mode = AgMode::Auto; s.v2 = false; c.configure(rp, tg, s);
    double now = 0; c.start(now, sim.live.totalDb, false, &sim.live);
    bool sawBr = false;
    run(c, sim, now, 20, [&](double) { if (c.bracketing()) sawBr = true; });
    CHECK(sawBr && c.takeBracketDone(), "the first search ran and reported done");
    CHECK(!c.takeBracketDone(), "done is reported once");
    sim.sigDbm = -90;   // the scene changed
    c.reoptimise();
    bool again = false;
    run(c, sim, now, 20, [&](double) { if (c.bracketing()) again = true; });
    CHECK(again && c.takeBracketDone(), "Re-optimise searched again");
    c.start(now, sim.live.totalDb, false, &sim.live);
    c.skipQuickSearch();
    bool none = false;
    run(c, sim, now, 15, [&](double) { if (c.bracketing()) none = true; });
    CHECK(!none, "a remembered gain skips the search");
    AutoGainController c2; AgSettings s2; s2.mode = AgMode::Auto; s2.v2 = false; s2.quickStart = false; c2.configure(rp, tg, s2);
    c2.start(now, sim.live.totalDb, false, &sim.live);
    bool b2 = false;
    run(c2, sim, now, 15, [&](double) { if (c2.bracketing()) b2 = true; });
    CHECK(!b2, "quickStart off: no search");
    // the amp is never switched by the search
    SimRadio hot(rp, 40);
    hot.sigDbm = -130; hot.chanBw = 12.5e3; hot.rateHz = 2.4e6;
    AutoGainController c3; AgSettings s3; s3.mode = AgMode::Auto; s3.v2 = false; c3.configure(rp, tg, s3);
    c3.start(now, hot.live.totalDb, false, &hot.live);
    bool amp = false;
    run(c3, hot, now, 30, [&](double) { if (hot.live.ampOn) amp = true; });
    CHECK(!amp, "the search never switches the amp without permission");
}

int main(int argc, char** argv) {
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    testBands();
    testScenes();
    testStatesDiffer();
    testDynamic();
    testReoptimise();
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("autogain_smart: all tests passed\n");
    return 0;
}
