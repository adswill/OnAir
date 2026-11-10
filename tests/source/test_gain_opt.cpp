// "Optimise gain": the selection logic on synthetic SNR-vs-gain curves (with a clipping region that looks deceptively good), the sweep's state machine,
// undo/cancel, the spectrum SNR estimate, the telemetry SNR picker; and, with the argument "engine", a sweep against the engine's generated DVB-T2 signal
// whose level follows the gain and clips like an 8-bit ADC (slow: about 40 s, registered as gain_opt_engine).
#include "dect2/engine/engine.h"
#include "dect2/engine/engine_testkit.h"
#include "dect2/source/gain_opt.h"
#include "dect2/source/gain.h"
#include "dect2/source/source.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
using namespace dect2;

#include "../common/test_util.h"

// the radio of the model: SNR climbs with gain until the ADC quantisation noise no longer matters, then compression takes a little away;
// from 55 dB the ADC clips, and the clipped signal even reads a higher SNR (the trap the clipping test is there for)
struct Model {
    int clipFrom = 55;
    bool lock = true;
    double snr(int g) const {
        double s = g < 20 ? 5 + 0.5 * g : g < 45 ? 15 + 0.8 * (g - 20) : 35 - 0.1 * (g - 45);
        if (g >= clipFrom) s += 4;
        return s;
    }
    GainOptimiser::Sample sample(int g) const {
        GainOptimiser::Sample x;
        x.snrValid = lock; x.snrDb = snr(g);
        x.rms = (float)std::min(-4.0, -50.0 + 0.7 * g);
        x.peak = g >= clipFrom ? 1.f : 0.45f;
        x.clip = g >= clipFrom ? 0.02f : 0.f;
        return x;
    }
};

// runs a whole sweep at 0.1 s steps; the "radio" applies a gain the moment it is asked for
static int runSweep(GainOptimiser& o, const Model& m, int minDb, int maxDb, int startDb, int* applied = nullptr) {
    o.start(0, minDb, maxDb, startDb);
    int cur = startDb;
    double t = 0;
    int changes = 0;
    while (o.active() && t < 600) {
        t += 0.1;
        if (o.update(t, m.sample(cur), cur)) changes++;
    }
    if (applied) *applied = changes;
    return cur;
}

static void testCurve() {
    Model m;
    GainOptimiser o;
    int changes = 0;
    const int g = runSweep(o, m, 0, 100, 30, &changes);
    CHECK(!o.active(), "sweep did not finish");
    CHECK(o.result() == GainOptimiser::Result::Snr, "result %d", (int)o.result());
    CHECK(o.refining(), "no refinement ran");
    CHECK(g == o.bestGain(), "final gain %d is not best %d", g, o.bestGain());
    CHECK(g < m.clipFrom, "winner %d is in the clipping region", g);
    // the winner is within tolerance of the best non-clipping SNR, and nothing lower is within tolerance of it
    double bestSafe = -1e9;
    for (const auto& p : o.points()) if (p.done && !p.clipped(0.001)) bestSafe = std::max(bestSafe, p.snrDb);
    const double mine = m.snr(g);
    CHECK(mine >= bestSafe - 0.5 - 1e-9, "winner SNR %.2f vs best safe %.2f", mine, bestSafe);
    for (const auto& p : o.points()) CHECK(!(p.done && !p.clipped(0.001) && p.gainDb < g && p.snrDb >= bestSafe - 0.5), "gain %d would also do and is lower than %d", p.gainDb, g);
    CHECK(g >= 40 && g <= 50, "winner %d dB is far from the 45 dB peak", g);
    CHECK(changes == (int)o.points().size() + 1, "changes %d, points %d", changes, (int)o.points().size());   // every step, then the winner
    bool clipSeen = false;
    for (const auto& p : o.points()) if (p.gainDb >= m.clipFrom) { CHECK(p.clipped(0.001), "gain %d should be marked clipped", p.gainDb); clipSeen = true; }
    CHECK(clipSeen, "the sweep never reached the clipping region");
    printf("curve: winner %d dB (SNR %.1f), %d steps\n", g, mine, (int)o.points().size());
}

static void testChoose() {
    GainOptimiser::Config cfg;
    auto P = [](int g, double snr, float clip = 0, bool over = false, int n = 10, int nAll = 10, float rms = -20) {
        GainOptimiser::Point p; p.gainDb = g; p.snrDb = snr; p.clip = clip; p.overload = over; p.n = n; p.nAll = nAll; p.rms = rms; p.done = true; return p;
    };
    GainOptimiser::Result how;
    CHECK(GainOptimiser::choose({}, cfg, &how) == -1 && how == GainOptimiser::Result::None, "empty set");
    // overload without any clip fraction (the ADC status alone) excludes a step
    std::vector<GainOptimiser::Point> v = {P(10, 20), P(20, 30), P(30, 40, 0, true), P(40, 45, 0.05f)};
    int i = GainOptimiser::choose(v, cfg, &how);
    CHECK(i == 1 && how == GainOptimiser::Result::Snr, "overload/clip steps must not win (got %d)", i);
    // tolerance: a plateau goes to its lowest gain
    v = {P(10, 20), P(20, 34.8), P(30, 35.0), P(40, 34.9)};
    CHECK(GainOptimiser::choose(v, cfg) == 1, "plateau: lowest gain within tolerance");
    cfg.snrTolDb = 0.1;
    CHECK(GainOptimiser::choose(v, cfg) == 2 || GainOptimiser::choose(v, cfg) == 3, "tight tolerance: peak (%d)", GainOptimiser::choose(v, cfg));
    cfg.snrTolDb = 0.5;
    // everything clips: the lowest gain, flagged
    v = {P(50, 30, 0.1f), P(60, 31, 0.2f), P(70, 32, 0.3f)};
    i = GainOptimiser::choose(v, cfg, &how);
    CHECK(i == 0 && how == GainOptimiser::Result::AllClipped, "all clipped -> lowest (%d)", i);
    // a step that locked only now and then is not trusted
    v = {P(10, 20), P(20, 28), P(30, 50, 0, false, 2, 10)};
    CHECK(GainOptimiser::choose(v, cfg) == 1, "intermittent lock must not win");
    // no SNR anywhere: the ADC level nearest -16 dBFS, the lower gain on a tie
    v = {P(10, -100, 0, false, 0, 10, -40), P(20, -100, 0, false, 0, 10, -22), P(30, -100, 0, false, 0, 10, -10), P(40, -100, 0, false, 0, 10, -10)};
    i = GainOptimiser::choose(v, cfg, &how);
    CHECK(i == 1 && how == GainOptimiser::Result::Level, "level fallback (%d)", i);
    // the spectrum's SNR decides when the mode never gave a number, and is never mixed with the mode's
    v = {P(10, -100, 0, false, 0), P(20, -100, 0, false, 0), P(30, -100, 0, false, 0), P(40, -100, 0.1f, false, 0)};
    const double alt[] = {12, 25, 24.8, 40};
    for (size_t k = 0; k < v.size(); k++) { v[k].n2 = 10; v[k].altDb = alt[k]; }
    i = GainOptimiser::choose(v, cfg, &how);
    CHECK(i == 1 && how == GainOptimiser::Result::Spectrum, "spectrum SNR choice (%d)", i);
    v[0].n = 10; v[0].snrDb = 3;   // one step has the mode's number: only the mode's numbers count, so that step wins although its SNR is low
    i = GainOptimiser::choose(v, cfg, &how);
    CHECK(i == 0 && how == GainOptimiser::Result::Snr, "the mode's number takes precedence (%d)", i);
    // not-yet-measured steps are ignored
    v = {P(10, 20), P(20, 99)};
    v[1].done = false;
    CHECK(GainOptimiser::choose(v, cfg) == 0, "unfinished step ignored");
}

static void testNoLockAndEdges() {
    Model m; m.lock = false;
    GainOptimiser o;
    const int g = runSweep(o, m, 0, 100, 30);
    CHECK(o.result() == GainOptimiser::Result::Level, "no lock: level fallback, result %d", (int)o.result());
    // the level model: rms = -50 + 0.7 g -> -16 dBFS at g = 48.6
    CHECK(g >= 40 && g <= 54, "no lock: level winner %d", g);
    Model clip; clip.clipFrom = 0;
    GainOptimiser o2;
    const int g2 = runSweep(o2, clip, 0, 100, 37);
    CHECK(o2.result() == GainOptimiser::Result::AllClipped && g2 == 0, "all clipped: gain %d result %d", g2, (int)o2.result());
    // narrow range, one-dB range, reversed arguments
    CHECK(GainOptimiser::coarseGains(0, 100, 10).size() == 10 && GainOptimiser::coarseGains(0, 100, 10).front() == 0 && GainOptimiser::coarseGains(0, 100, 10).back() == 100, "coarse gains");
    CHECK(GainOptimiser::coarseGains(5, 7, 10).size() == 3, "narrow range has every dB (%d)", (int)GainOptimiser::coarseGains(5, 7, 10).size());
    CHECK(GainOptimiser::coarseGains(9, 9, 10).size() == 1, "single gain");
    GainOptimiser o3;
    Model ok;
    const int g3 = runSweep(o3, ok, 49, 40, 45);   // reversed
    CHECK(g3 >= 40 && g3 <= 49, "reversed range winner %d", g3);
}

static void testCancelUndo() {
    Model m;
    GainOptimiser o;
    o.start(0, 0, 100, 33);
    int cur = 33;
    double t = 0;
    for (int k = 0; k < 80; k++) { t += 0.1; o.update(t, m.sample(cur), cur); }
    CHECK(o.active() && cur != 33, "should be mid-sweep at a step other than the start (cur %d)", cur);
    int back = -1;
    CHECK(o.cancel(back) && back == 33 && !o.active(), "cancel gives the start gain back (%d)", back);
    CHECK(!o.cancel(back), "cancel when idle");
    CHECK(!o.update(t + 1, m.sample(cur), cur), "an idle optimiser changes nothing");
    // a finished sweep remembers where it started, for the app's undo
    GainOptimiser o2;
    runSweep(o2, m, 0, 100, 61);
    CHECK(o2.startGain() == 61, "start gain kept for undo (%d)", o2.startGain());
}

static void testSpectrum() {
    // noise at -70 dBFS per bin, the channel (1/4 of the capture, centred) 20 dB above it; averaged-FFT noise has a little ripple
    const size_t n = 4096;
    std::vector<float> s(n);
    unsigned r = 12345;
    auto rnd = [&] { r = r * 1664525u + 1013904223u; return (float)((r >> 8) & 0xFFFF) / 65535.f - 0.5f; };
    for (size_t i = 0; i < n; i++) s[i] = -70.f + 0.6f * rnd();
    const double rate = 8e6, bw = 2e6;
    for (size_t i = 0; i < n; i++) { const double f = ((double)i - n / 2.0) * rate / n; if (std::fabs(f) < bw / 2) s[i] += 20.f; }
    double snr = 0;
    CHECK(spectrumSnrDb(s, rate, bw, snr), "spectrum SNR estimate failed");
    CHECK(std::fabs(snr - 20.0) < 1.5, "spectrum SNR %.2f dB, expected about 20", snr);
    // the radio's DC spike inside the channel must not count as signal
    s[n / 2] = -10.f; s[n / 2 + 1] = -30.f;
    CHECK(spectrumSnrDb(s, rate, bw, snr) && std::fabs(snr - 20.0) < 1.5, "DC spike changed the estimate: %.2f", snr);
    // off-centre channel
    std::vector<float> t(n);
    for (size_t i = 0; i < n; i++) t[i] = -70.f + 0.6f * rnd();
    for (size_t i = 0; i < n; i++) { const double f = ((double)i - n / 2.0) * rate / n; if (std::fabs(f - 1.5e6) < bw / 2) t[i] += 12.f; }
    CHECK(spectrumSnrDb(t, rate, bw, snr, 1.5e6) && std::fabs(snr - 12.0) < 1.5, "off-centre SNR %.2f, expected about 12", snr);
    // noise only: about 0 dB or below, never a high number
    std::vector<float> z(n);
    for (size_t i = 0; i < n; i++) z[i] = -70.f + 0.6f * rnd();
    CHECK(spectrumSnrDb(z, rate, bw, snr) && snr < 1.0, "noise only gave %.2f dB", snr);
    // a capture that is no wider than the channel cannot tell: false, not a made-up number
    CHECK(!spectrumSnrDb(s, 2.2e6, 2e6, snr), "capture as wide as the channel should refuse");
    CHECK(!spectrumSnrDb(std::vector<float>(16, -50.f), rate, bw, snr), "too few bins");
}

static void testTelemetry() {
    RxTelemetry rx;
    double v = 0;
    const char* w = nullptr;
    CHECK(!rxSnrDb(rx, v), "not locked: no SNR");
    rx.standard = 0; rx.dataValid = true; rx.dataSnrDb = 17.5f;
    CHECK(rxSnrDb(rx, v, &w) && std::fabs(v - 17.5) < 1e-6 && w, "T2 SNR");
    rx.dataSnrDb = NAN;
    CHECK(!rxSnrDb(rx, v), "NaN is no SNR");
    rx.standard = 2; rx.dataValid = true; rx.atsc.fieldSync = false; rx.atsc.snrDb = 19;
    CHECK(!rxSnrDb(rx, v), "ATSC without field sync");
    rx.atsc.fieldSync = true;
    CHECK(rxSnrDb(rx, v) && v == 19, "ATSC SNR");
    rx.standard = 6; rx.fm.carrier = true; rx.fm.state = 2; rx.fm.snrDb = 41;
    CHECK(rxSnrDb(rx, v, &w) && v == 41 && std::strstr(w, "audio"), "FM audio SNR");
    rx.standard = 7; rx.dataValid = true; rx.dataSnrDb = 9.f;
    CHECK(rxSnrDb(rx, v) && v == 9, "DVB-S/S2 Es/N0 via the common field");
}

// engine-level: the sweep against the generated DVB-T2 signal; the gain is the HackRF total (gainForTotal)
static double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void testEngine() {
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.bandwidthMhz = 8;
    t.synth.snrDb = 32;
    t.synth.tx.s2field1 = 1;
    t.synth.tx.giIdx = 3;
    t.synth.gainModel = true;
    t.synth.pace = 1.0;
    GainSetting gs = gainForTotal(62);
    t.lnaDb = gs.lna; t.vgaDb = gs.vga; t.ampOn = gs.amp;
    FileOptions fo;
    e.setStandard(1);
    if (!e.start(dev, t, fo)) { printf("FAIL: cannot start\n"); fails++; return; }
    GainOptimiser::Config cfg;
    cfg.settleSec = 1.0; cfg.measureSec = 2.0; cfg.maxPoints = 8;
    GainOptimiser o(cfg);
    int cur = 62;
    o.start(0, 0, 100, cur);
    SpectrumFrame sf; uint64_t seq = 0;
    RxTelemetry rx; uint64_t rseq = 0;
    const double t0 = nowS();
    // The optimiser's clock stops while the samples made before the last gain change are still being analysed: a busy runner leaves up to
    // the input ring (about 1.8 s at this rate) waiting, and a wall clock would credit them to the new gain. (A live radio's ring is cut back
    // to the newest samples when the receiver falls behind, Engine::catchUp; the synthetic source waits for the receiver instead.)
    double paused = 0, lastWall = 0, drainTo = -1;
    while (o.active() && nowS() - t0 < 150) {
        const double wall = nowS() - t0;
        if (drainTo >= 0 && enginetest::signalSecs(e) >= drainTo) drainTo = -1;
        if (drainTo >= 0) { paused += wall - lastWall; lastWall = wall; std::this_thread::sleep_for(std::chrono::milliseconds(15)); continue; }
        lastWall = wall;
        const double tSig = wall - paused;
        if (e.latestSpectrum(sf, seq)) seq = sf.seq;
        if (e.latestRx(rx, rseq)) rseq = rx.seq;
        GainOptimiser::Sample s;
        double snr = 0;
        s.snrValid = rxSnrDb(rx, snr); s.snrDb = snr;
        s.clip = sf.stats.clipFraction; s.rms = sf.stats.rmsDbfs; s.peak = sf.stats.peak;
        if (o.update(tSig, s, cur)) {
            const GainSetting g = gainForTotal(cur);
            t.lnaDb = g.lna; t.vgaDb = g.vga; t.ampOn = g.amp;
            e.retune(t);
            drainTo = enginetest::signalSecs(e) + e.backlogSec();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    CHECK(!o.active(), "engine sweep did not finish in time (%d/%d steps)", o.stepsDone(), o.stepsTotal());
    for (const auto& p : o.points())
        printf("  gain %3d dB: SNR %6.1f dB (%d/%d samples) level %6.1f dBFS clip %.3f%% overload %d\n", p.gainDb, p.snrDb, p.n, p.nAll, p.rms, p.clip * 100, p.overload ? 1 : 0);
    printf("engine sweep: winner %d dB (%s)\n", o.bestGain(), o.result() == GainOptimiser::Result::Snr ? "by SNR" : o.result() == GainOptimiser::Result::Level ? "by level" : "all clipped");
    CHECK(o.result() == GainOptimiser::Result::Snr, "the generated T2 signal locks: expected an SNR-based choice");
    const int bi = GainOptimiser::choose(o.points(), cfg);
    CHECK(bi >= 0 && o.points()[(size_t)bi].gainDb == o.bestGain(), "winner is the pure choice of the measured points");
    const GainOptimiser::Point* w = nullptr;
    for (const auto& p : o.points()) if (p.gainDb == o.bestGain()) w = &p;
    CHECK(w && !w->clipped(cfg.clipLimit), "the winner clips");
    double bs = -1e9;
    for (const auto& p : o.points()) if (!p.clipped(cfg.clipLimit) && p.hasSnr()) bs = std::max(bs, p.snrDb);
    CHECK(w && w->snrDb >= bs - cfg.snrTolDb - 1e-9, "winner is not within tolerance of the best SNR");
    // and the radio really sits at a healthy ADC level with it
    CHECK(w && w->rms > -40 && w->rms < -8, "winner level %.1f dBFS", w ? w->rms : 0.f);
    e.stop();
}

// No signal: every step's "SNR" is noise (about 0 dB, within the tolerance of each other). The winner must not be the bottom of the range (0 dB)
// but come from the ADC level; a real signal still wins by its SNR. The amp is off by default in every settings struct.
static void testNoSignalAndAmpDefaults() {
    GainOptimiser::Config cfg;
    auto P = [](int g, double snr, float rms) {
        GainOptimiser::Point p; p.gainDb = g; p.snrDb = snr; p.n = 10; p.nAll = 10; p.rms = rms; p.done = true; return p;
    };
    GainOptimiser::Result how;
    std::vector<GainOptimiser::Point> v = {P(0, 0.2, -60), P(20, 0.1, -40), P(40, 0.3, -20), P(60, 0.2, -10)};
    const int i = GainOptimiser::choose(v, cfg, &how);
    CHECK(i == 2 && how == GainOptimiser::Result::Level, "no signal must not end at the lowest gain (got index %d)", i);
    v = {P(0, 0.2, -60), P(20, 12, -40), P(40, 12.2, -20)};
    CHECK(GainOptimiser::choose(v, cfg, &how) == 1 && how == GainOptimiser::Result::Snr, "a real signal still wins by SNR");
    CHECK(!TuneSettings().ampOn && !GainSetting().amp, "the amp is off by default");
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "engine")) {
        testEngine();
    } else {
        testCurve();
        testChoose();
        testNoLockAndEdges();
        testCancelUndo();
        testSpectrum();
        testTelemetry();
        testNoSignalAndAmpDefaults();
    }
    printf(fails ? "gain_opt tests FAILED (%d)\n" : "gain_opt tests passed\n", fails);
    return fails ? 1 : 0;
}
