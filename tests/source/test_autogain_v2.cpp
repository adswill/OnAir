// Auto gain v2 against the simulated front end of autogain_sim.h: the old headroom servo (AgSettings::v2 = false) and the new hill climb on the measured
// objective run the same scenes (ADC clipping, intermodulation from strong off-channel carriers, weak wanted signals, pulsed signals, a barely locked
// decoder, a stale remembered gain, a unit whose real gain steps are not the table's) and the final in-channel SNR is compared. Also: the engine-thread
// GainMeter on synthetic IQ, the per-unit gain-step calibration against a simulated tuner with non-nominal steps, the status line. ctest autogain_v2; -v prints the table.
#include "autogain_sim.h"
#include "dect2/source/gain_cal.h"
#include "dect2/source/gain_meter.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

using namespace dect2;
using namespace dect2::ag;
using namespace agsim;

static int fails = 0;
static bool verbose = false;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static RadioProfile prof(const char* id) { const RadioProfile* p = radioProfileById(id); if (!p) { printf("no profile %s\n", id); exit(1); } return *p; }

struct Scene {
    const char* name; const char* radio; int std_; double freqHz, bw, rate, off, sig; std::vector<Tone> blockers;
    double crest = 3, lockAt = 8, unitDb = -1, startDb = -1000;
    bool prior = false; double priorDb = 0;   // a remembered gain (stale)
    double minGainBetter = 0.3;                // v2 must beat the old loop by this much (0: parity is enough)
};
struct Res { double scale = 1, snr = -99, avg = -99, gain = 0, clipSec = 0, conv = 0, probeFrac = 0; std::string status; };

static void setup(SimRadio& s, const Scene& sc) {
    s.rateHz = sc.rate; s.chanBw = sc.bw; s.chanOff = sc.off; s.sigDbm = sc.sig; s.blockers = sc.blockers; s.sigCrestDb = sc.crest; s.lockAt = sc.lockAt;
    if (sc.unitDb > 0) s.unitDb = sc.unitDb;
}

static Res runScene(const Scene& sc, bool v2, double sec = 120, const GainCal* cal = nullptr) {
    RadioProfile rp = prof(sc.radio);
    if (cal) applyCalibration(rp, *cal);
    const double mid = (rp.minTotal(false) + rp.maxTotal(false)) / 2;
    const double start = sc.startDb > -999 ? sc.startDb : mid;
    SimRadio sim(rp, start);
    setup(sim, sc);
    const ModeTarget tg = modeTargetFor(sc.std_, sc.bw, sc.rate, sc.freqHz);
    AutoGainController c;
    AgSettings s; s.mode = AgMode::Auto; s.v2 = v2;
    c.configure(rp, tg, s);
    double now = 0;
    c.start(now, sim.live.totalDb, false, &sim.live);
    if (sc.prior) {
        StagePlan p = planStages(rp, sc.priorDb, tg.policy, 0xffffffffu, {}, false);
        sim.live = p; sim.pending = p;
        c.start(now, p.totalDb, false, &p);
        if (v2) c.setPrior(p); else c.skipQuickSearch();   // the old app skipped the search; v2 searches around the prior
    }
    Res o;
    double sumAll = 0, sumSet = 0; int nAll = 0, nSet = 0, nProbe = 0;
    run(c, sim, now, sec, [&](double t) {
        if (sim.lastClip > tg.clipLimit) o.clipSec += 0.05;
        if (t >= sec - 30) {   // the last 30 s: the SNR the user had (probing included) and the SNR of the settled gain
            const double v = sim.snrDb();
            sumAll += v; nAll++;
            if (v2 && c.probing()) nProbe++; else { sumSet += v; nSet++; }
        }
    });
    o.avg = nAll ? sumAll / nAll : -99;
    o.probeFrac = nAll ? (double)nProbe / nAll : 0;
    o.gain = c.current().totalDb;
    SimRadio fin(rp, o.gain, false);
    setup(fin, sc);
    fin.jitterDb = 0;
    fin.live = c.current();
    fin.adc();
    o.snr = nSet ? sumSet / nSet : fin.snrDb();   // the settled gain (the old loop never probes: all of the last 30 s)
    o.status = c.status();
    o.scale = c.unitScale();
    return o;
}

// the best SNR over every reachable total that does not clip and keeps the mode's v2 headroom floor (the reference both loops are compared with)
static double bestOf(const Scene& sc, double* bestGain = nullptr) {
    const RadioProfile rp = prof(sc.radio);
    const ModeTarget tg = modeTargetFor(sc.std_, sc.bw, sc.rate, sc.freqHz);
    double best = -1e9, bg = 0;
    for (Policy pol : {Policy::Linearity, Policy::Balanced, Policy::Sensitivity})
        for (double g : totalSteps(rp, pol, 0xffffffffu, false)) {
            SimRadio s(rp, g, false);
            setup(s, sc);
            s.jitterDb = 0;
            s.live = planStages(rp, g, pol, 0xffffffffu, {}, false);
            const BlockStats a = s.adc();
            if (a.clipFraction > tg.clipLimit || a.headroomDb < tg.minHeadroomDb) continue;
            const double v = s.snrDb();
            if (v > best) { best = v; bg = g; }
        }
    if (bestGain) *bestGain = bg;
    return best;
}

static std::vector<Scene> sceneList() {
    // third-order products of 600 kHz and 1100 kHz land at 100 kHz (the channel): 2*600 - 1100
    return {
        {"weak beside strong", "hackrf", 11, 446e6, 12.5e3, 2.4e6, 100e3, -100, {{600e3, -50}, {-700e3, -52}}},
        {"weak, FM neighbours", "rtl-r820t", 11, 98e6, 12.5e3, 2.4e6, 100e3, -100, {{-900e3, -60}, {-300e3, -62}, {500e3, -58}}},
        {"IMD pair into channel", "hackrf", 11, 446e6, 12.5e3, 2.4e6, 100e3, -95, {{600e3, -62}, {1100e3, -62}}},
        {"IMD pair, rtl-sdr", "rtl-r820t", 11, 446e6, 12.5e3, 2.4e6, 100e3, -95, {{600e3, -56}, {1100e3, -56}}},
        {"IMD pair, airspy", "airspy", 11, 446e6, 12.5e3, 2.4e6, 100e3, -105, {{600e3, -56}, {1100e3, -56}}},
        {"ADC clip from max gain", "hackrf", 11, 446e6, 12.5e3, 2.4e6, 100e3, -80, {{300e3, -55}}, 3, 8, -1, 100},
        {"DVB-T2 beside a strong tower", "hackrf", 1, 586e6, 7.6e6, 9.14e6, 0, -100, {{4e6, -60}}, 3, 8, -1, 25},
        {"pulsed ADS-B", "hackrf", 13, 1090e6, 2e6, 2.4e6, 0, -90, {{800e3, -50}}, 15},
        {"stale memory prior", "hackrf", 11, 446e6, 12.5e3, 2.4e6, 100e3, -100, {{600e3, -50}, {-700e3, -52}}, 3, 8, -1, 20, true, 70},
        {"quiet band, weak (parity)", "pluto", 11, 1.65e9, 12.5e3, 2.4e6, 100e3, -122, {}, 3, 8, -1, -1000, false, 0, -0.9},
    };
}

static void testScenes() {
    const std::vector<Scene> scenes = sceneList();
    if (verbose) printf("%-26s %-9s %8s %8s %8s  %6s %6s %6s  %s\n", "scene", "radio", "old SNR", "new SNR", "best", "old dB", "new dB", "best", "clip s (old/new)");
    double sumGain = 0;
    int n = 0;
    for (const Scene& sc : scenes) {
        const Res a = runScene(sc, false), b = runScene(sc, true);
        double bg = 0;
        const double best = bestOf(sc, &bg);
        if (verbose) printf("%-26s %-9s %8.1f %8.1f %8.1f  %6.1f %6.1f %6.1f  %.2f / %.2f  avg %.1f (probing %.0f%%)\n", sc.name, sc.radio, a.snr, b.snr, best, a.gain, b.gain, bg, a.clipSec, b.clipSec, b.avg, 100 * b.probeFrac);
        CHECK(b.snr >= a.snr + sc.minGainBetter, "%s: new loop %.1f dB is not better than the old %.1f dB (gain %.1f vs %.1f; best %.1f at %.1f)", sc.name, b.snr, a.snr, b.gain, a.gain, best, bg);
        CHECK(b.clipSec <= std::max(1.5, a.clipSec + 0.5), "%s: the new loop clipped %.2f s (old %.2f s)", sc.name, b.clipSec, a.clipSec);
        CHECK(b.status.find("Gain ") == 0, "%s: status line '%s'", sc.name, b.status.c_str());
        sumGain += b.snr - a.snr; n++;
    }
    printf("autogain_v2: %d scenes, mean final SNR improvement %.2f dB over the old loop\n", n, sumGain / n);
}

// -grid: a map of old / new / best over radios, signal levels and neighbour levels (used to choose and tune the scenes)
static void grid() {
    printf("%-10s %5s %6s | %7s %7s %7s %7s | %5s %5s %5s | clip  probe%%\n", "radio", "sig", "blk", "old", "new", "(avg)", "best", "old", "new", "best");
    for (const char* r : {"hackrf", "rtl-r820t", "airspy", "sdrplay"})
        for (double sig : {-110.0, -100.0, -90.0})
            for (double blk : {-999.0, -60.0, -50.0, -40.0}) {
                Scene sc = {"g", r, 11, 446e6, 12.5e3, 2.4e6, 100e3, sig, {}};
                if (blk > -900) sc.blockers = {{600e3, blk}, {-700e3, blk - 2}};
                const Res a = runScene(sc, false, 90), b = runScene(sc, true, 90);
                double bg = 0;
                const double best = bestOf(sc, &bg);
                printf("%-10s %5.0f %6.0f | %7.1f %7.1f %7.1f %7.1f | %5.0f %5.0f %5.0f | %.1f/%.1f %.0f\n", r, sig, blk, a.snr, b.snr, b.avg, best, a.gain, b.gain, bg, a.clipSec, b.clipSec, 100 * b.probeFrac);
            }
}

// -grid2: broadcast TV (DVB-T2, a 7.6 MHz channel in 9.14 Msps) with the decoder locking above 8 dB; -imd: two tones whose product falls into the channel
static void grid2() {
    printf("%-10s %5s %6s | %7s %7s %7s %7s | %5s %5s %5s | clip  probe%%\n", "radio", "sig", "blk", "old", "new", "(avg)", "best", "old", "new", "best");
    for (const char* r : {"hackrf", "sdrplay"})
        for (double sig : {-100.0, -90.0, -80.0, -70.0})
            for (double blk : {-999.0, -60.0, -45.0}) {
                Scene sc = {"g", r, 1, 586e6, 7.6e6, 9.14e6, 0, sig, {}, 3, 8, -1, 25};
                if (blk > -900) sc.blockers = {{4e6, blk}};
                const Res a = runScene(sc, false, 90), b = runScene(sc, true, 90);
                double bg = 0;
                const double best = bestOf(sc, &bg);
                printf("%-10s %5.0f %6.0f | %7.1f %7.1f %7.1f %7.1f | %5.0f %5.0f %5.0f | %.1f/%.1f %.0f\n", r, sig, blk, a.snr, b.snr, b.avg, best, a.gain, b.gain, bg, a.clipSec, b.clipSec, 100 * b.probeFrac);
            }
}
static void gridImd() {
    printf("%-10s %5s %6s | %7s %7s %7s %7s | %5s %5s %5s | clip  probe%%\n", "radio", "sig", "tones", "old", "new", "(avg)", "best", "old", "new", "best");
    for (const char* r : {"hackrf", "airspy", "rtl-r820t"})
        for (double sig : {-105.0, -95.0})
            for (double blk : {-70.0, -62.0, -56.0, -50.0, -44.0}) {
                Scene sc = {"g", r, 11, 446e6, 12.5e3, 2.4e6, 100e3, sig, {{600e3, blk}, {1100e3, blk}}};
                const Res a = runScene(sc, false, 90), b = runScene(sc, true, 90);
                double bg = 0;
                const double best = bestOf(sc, &bg);
                printf("%-10s %5.0f %6.0f | %7.1f %7.1f %7.1f %7.1f | %5.0f %5.0f %5.0f | %.1f/%.1f %.0f\n", r, sig, blk, a.snr, b.snr, b.avg, best, a.gain, b.gain, bg, a.clipSec, b.clipSec, 100 * b.probeFrac);
            }
}

// -log N: the new loop's change log on scene N
static void logScene(int idx, const Scene* custom = nullptr, bool v2 = true) {
    const std::vector<Scene> scenes = sceneList();
    const Scene& sc = custom ? *custom : scenes[(size_t)idx];
    RadioProfile rp = prof(sc.radio);
    const double mid = (rp.minTotal(false) + rp.maxTotal(false)) / 2;
    const double start = sc.startDb > -999 ? sc.startDb : mid;
    SimRadio sim(rp, start);
    setup(sim, sc);
    const ModeTarget tg = modeTargetFor(sc.std_, sc.bw, sc.rate, sc.freqHz);
    AutoGainController c;
    AgSettings s; s.mode = AgMode::Auto; s.v2 = v2;
    c.configure(rp, tg, s);
    double now = 0;
    c.start(now, sim.live.totalDb, false, &sim.live);
    if (sc.prior) {
        StagePlan p = planStages(rp, sc.priorDb, tg.policy, 0xffffffffu, {}, false);
        sim.live = p; sim.pending = p;
        c.start(now, p.totalDb, false, &p);
        if (v2) c.setPrior(p); else c.skipQuickSearch();
    }
    run(c, sim, now, 120, [&](double t) { static int k = 0; if (getenv("AGTRACE") && (++k % 20) == 0) printf("   t=%5.1f gain %5.1f snr %6.1f clip %.4f head %.1f J %.1f chan %.1f %s\n", t, c.current().totalDb, sim.snrDb(), sim.lastClip, c.meters().headroomDb, c.meters().objective, c.meters().chanSnrDb, c.probing() ? "probe" : ""); });
    for (const auto& e : c.log()) printf("  %6.1f  %6.1f -> %6.1f  %s\n", e.t, e.fromDb, e.toDb, e.why.c_str());
    printf("status: %s\n", c.status().c_str());
}

// a unit whose real gain steps are 2 dB per table step (an Airspy-like table the profile counts as 1): uncalibrated, the slope test misreads the
// product-free front end; calibrated, it is right
static void testCalibratedUnit() {
    Scene sc = {"calibrated unit", "airspy", 11, 446e6, 12.5e3, 2.4e6, 100e3, -108, {{600e3, -60}, {-700e3, -62}}};
    sc.unitDb = 2.0;
    // the calibration a GainCalibrator would find: every step 2 dB
    RadioProfile rp = prof("airspy");
    GainCal cal; cal.serial = "T1"; cal.radioId = rp.id;
    for (const auto& st : rp.stages) if (st.settable) { cal.nominal = st.steps; for (size_t i = 0; i < st.steps.size(); i++) cal.actual.push_back(st.steps[0] + 2.0 * (double)i); }
    cal.floorDbfs.assign(cal.nominal.size(), -80.0); cal.sure.assign(cal.nominal.size(), 1);
    const Res un = runScene(sc, true), ca = runScene(sc, true, 120, &cal), old = runScene(sc, false);
    if (verbose) printf("unit with 2 dB steps: old %.1f dB, v2 uncalibrated %.1f dB, v2 calibrated %.1f dB\n", old.snr, un.snr, ca.snr);
    CHECK(ca.snr >= old.snr - 0.5, "calibrated v2 %.1f dB is worse than old %.1f", ca.snr, old.snr);
    CHECK(ca.snr >= un.snr - 0.3, "calibration made it worse: %.1f vs %.1f", ca.snr, un.snr);
    CHECK(un.scale > 1.5 && un.scale < 2.6, "the uncalibrated loop learned this unit's step size (x%.2f, really x2)", un.scale);
    CHECK(un.snr >= old.snr - 3.0, "uncalibrated v2 falls too far under the old loop: %.1f vs %.1f", un.snr, old.snr);
}

// ----------------------------------------------------------------------------------------------------------------------------- GainMeter
static void testMeter() {
    GainMeter m;
    m.configure(2.4e6, 8);
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.f, 0.05f);
    std::vector<std::complex<float>> blk(48000);
    const int N = 4;   // 4 blocks of 20 ms
    uint64_t clipped = 0, total = 0;
    for (int b = 0; b < 20; b++) {   // 1 s: noise with 3 clipped samples per block, a full-scale pulse once
        for (auto& x : blk) x = {nd(rng), nd(rng)};
        for (int k = 0; k < 3; k++) { blk[(size_t)(k * 997 + 13)] = {1.0f, 0.2f}; clipped++; }
        total += blk.size();
        m.process(blk.data(), blk.size());
    }
    (void)N;
    BlockStats a; std::vector<float> spec;
    CHECK(m.take(a, spec), "meter delivers");
    CHECK(a.n > 900000, "counts nearly all samples: %zu", a.n);
    CHECK(std::fabs(a.clipFraction - (double)clipped / (double)total) < 0.25 * (double)clipped / (double)total, "clip fraction %.2e vs %.2e (every sample counted)", a.clipFraction, (double)clipped / (double)total);
    CHECK(a.peak >= 0.999, "exact peak %.3f", a.peak);
    CHECK(std::fabs(a.rmsDbfs - 10 * std::log10(2 * 0.05 * 0.05)) < 0.4, "rms %.1f dBFS", a.rmsDbfs);
    CHECK(a.holdHeadroomDb < 0.1, "peak-hold sees the pulse (%.2f dB)", a.holdHeadroomDb);
    CHECK(spec.size() == (size_t)GainMeter::kFft, "spectrum bins %zu", spec.size());
    // the spectrum carries the power: all bins together = rms^2 (white noise: each 1/N of it)
    double tot = 0;
    for (float v : spec) tot += std::pow(10.0, v / 10.0);
    CHECK(std::fabs(10 * std::log10(tot) - a.rmsDbfs) < 1.0, "spectrum power %.1f vs rms %.1f dBFS", 10 * std::log10(tot), a.rmsDbfs);
    // a short burst the display statistics (every fourth sample) would miss: 2 clipped samples in a block, found exactly
    for (auto& x : blk) x = {nd(rng), nd(rng)};
    blk[1000] = {1.f, 1.f}; blk[1001] = {-1.f, 1.f};
    m.process(blk.data(), blk.size());
    { std::vector<std::complex<float>> q(blk.size()); for (auto& x : q) x = {nd(rng), nd(rng)}; m.process(q.data(), q.size()); m.process(q.data(), q.size()); }   // a 50 ms window needs 120000 samples
    CHECK(m.take(a, spec) && a.clipFraction * (double)a.n > 1.5 && a.clipFraction * (double)a.n < 2.5, "a 2-sample burst counted exactly (%.2f)", a.clipFraction * (double)a.n);
    // channel SNR from the meter's own spectrum: a tone 20 dB over the noise per channel
    std::vector<std::complex<float>> t(480000);
    for (size_t i = 0; i < t.size(); i++) { const float ph = 6.2831853f * 0.1f * (float)i; t[i] = {0.1f * std::cos(ph) + nd(rng) * 0.2f, 0.1f * std::sin(ph) + nd(rng) * 0.2f}; }
    m.take(a, spec);
    for (size_t o = 0; o + 48000 <= t.size(); o += 48000) m.process(t.data() + o, 48000);
    m.take(a, spec);
    const FloorEstimate f = estimateFloor(spec, 2.4e6, 25e3, 0.1 * 2.4e6, 8);
    CHECK(f.valid && f.snrDb > 15, "channel SNR from the meter spectrum %.1f dB", f.snrDb);
}

// ----------------------------------------------------------------------------------------------------------------------------- calibration
// a tuner whose real steps are nominal*1.4 with an error per step and one dead stretch; a steady noise source in front of it
struct SimTuner {
    RadioProfile r; double noiseDbfsAtZero = -95; bool dead = false; std::vector<double> real; uint32_t seed = 11;
    int cur = 0; double chan = 12.5e3, rate = 2.4e6; int bins = 512;
    explicit SimTuner(const RadioProfile& p, double scale, bool deadTuner = false) : r(p), dead(deadTuner) {
        for (const auto& s : r.stages) if (s.settable) { for (size_t i = 0; i < s.steps.size(); i++) real.push_back(s.steps[0] + scale * (s.steps[i] - s.steps[0]) + (((int)i * 7) % 5 - 2) * 0.2); break; }
    }
    double rnd() { seed = seed * 1664525u + 1013904223u; return ((seed >> 8) / 16777216.0 - 0.5); }
    double rmsDbfs() const { return noiseDbfsAtZero + (dead ? real[0] : real[(size_t)cur]); }
    std::vector<float> spectrum() {
        const double g = dead ? real[0] : real[(size_t)cur];
        const double noise = noiseDbfsAtZero + g - 10 * std::log10((double)bins);
        const double q = -(6.02 * r.adcBits + 1.76) - 10 * std::log10((double)bins);
        std::vector<float> v((size_t)bins);
        for (auto& x : v) x = (float)(10 * std::log10(std::pow(10, noise / 10) + std::pow(10, q / 10)) + rnd() * 0.5);
        return v;
    }
};
static GainCal calibrate(SimTuner& t, const char* serial) {
    GainCalibrator c;
    c.begin(t.r, serial, 0, 100e6);
    double now = 0;
    for (; now < 400 && !c.done(); now += 0.05) {
        AgInput in; in.now = now;
        std::vector<float> sp = t.spectrum();
        in.spectrum = &sp; in.rateHz = t.rate; in.chanBwHz = t.chan;
        in.adc.clipFraction = t.rmsDbfs() > -15 ? 0.01 : 0.0;   // the noise source fills the ADC at high gain: the steps above are not measurable
        AgCommand out;
        if (c.tick(in, out)) {
            const StagePlan& p = out.plan;
            for (size_t i = 0; i < t.r.stages.size(); i++) if (t.r.stages[i].settable) {
                for (size_t k = 0; k < t.r.stages[i].steps.size(); k++) if (std::fabs(t.r.stages[i].steps[k] - p.db[i]) < 1e-6) t.cur = (int)k;
                break;
            }
        }
    }
    CHECK(c.done(), "calibration finished (%.0f s)", now);
    return c.result();
}
static void testCalibration() {
    const RadioProfile rp = prof("rtl-r820t");
    SimTuner t(rp, 1.3);
    t.noiseDbfsAtZero = -58;
    const GainCal c = calibrate(t, "SER1");
    CHECK(c.valid() && c.actual.size() == t.real.size(), "calibration has all steps");
    if (verbose) { printf("calibration floors:"); for (size_t i = 0; i < c.floorDbfs.size(); i += 4) printf(" %.1f", c.floorDbfs[i]); printf("\n"); }
    int sure = 0; double worst = 0;
    int first = -1;
    for (size_t i = 0; i < c.actual.size() && i < t.real.size(); i++) if (c.sure[i]) {   // relative to the first measured step (the steps under it are the table's)
        if (first < 0) first = (int)i;
        sure++;
        worst = std::max(worst, std::fabs((c.actual[i] - c.actual[(size_t)first]) - (t.real[i] - t.real[(size_t)first])));
    }
    if (verbose) printf("calibration: %d of %zu steps sure, worst cumulative error %.2f dB, flags %zu, '%s'\n", sure, c.actual.size(), worst, c.flags.size(), c.flags.empty() ? "" : c.flags[0].c_str());
    CHECK(sure >= (int)c.actual.size() / 3, "the steps inside the ADC's range are measured (%d)", sure);
    CHECK(worst < 1.2, "cumulative error of the measured steps %.2f dB", worst);
    CHECK(c.flags.empty(), "a good unit with a different step size is not flagged as bad: %s", c.flags.empty() ? "" : c.flags[0].c_str());
    RadioProfile cp = rp;
    CHECK(applyCalibration(cp, c) && cp.calibrated(), "calibration applies to the profile");
    const double nominal = rp.stages[0].steps.back() - rp.stages[0].steps.front();
    const double real = cp.realDelta(rp.stages[0].steps.front(), rp.stages[0].steps.back());
    CHECK(real > nominal * 1.1, "the controller now knows the real range (%.1f vs table %.1f)", real, nominal);
    // a dead tuner (the gain control does nothing) is flagged
    SimTuner dead(rp, 1.0, true);
    dead.noiseDbfsAtZero = -35;
    const GainCal d = calibrate(dead, "DEAD");
    CHECK(d.bad(), "a tuner whose gain control does nothing is flagged");
    if (verbose && d.bad()) printf("dead tuner: %s\n", d.flags[0].c_str());
    // a store round trip
    GainCalStore st;
    st.put(c); st.put(d);
    GainCalStore st2;
    CHECK(st2.parse(st.serialise()) && st2.find("SER1") && st2.find("DEAD"), "store round trip");
    if (st2.find("SER1")) CHECK(st2.find("SER1")->actual.size() == c.actual.size() && std::fabs(st2.find("SER1")->actual.back() - c.actual.back()) < 0.01, "store keeps the steps");
    if (st2.find("DEAD")) CHECK(st2.find("DEAD")->bad(), "store keeps the flags");
    // no calibration possible with the floor at the ADC's noise
    SimTuner quiet(rp, 1.0);
    quiet.noiseDbfsAtZero = -140;
    const GainCal q = calibrate(quiet, "Q");
    CHECK(q.bad(), "a floor at the ADC's own noise gives no calibration, and says so");
}

// ----------------------------------------------------------------------------------------------------------------------------- behaviour
static void testBehaviour() {
    // no freeze while barely locked: the old loop stays where the decoder first locked (frozen); the new one keeps looking
    Scene sc = {"barely", "rtl-r820t", 11, 98e6, 12.5e3, 2.4e6, 100e3, -100, {{-900e3, -60}, {-300e3, -62}, {500e3, -58}}, 3, 10, -1, 15};
    const Res a = runScene(sc, false), b = runScene(sc, true);
    CHECK(b.snr > a.snr + 2, "barely locked: new %.1f vs old %.1f dB", b.snr, a.snr);
    // retune / re-run
    RadioProfile rp = prof("hackrf");
    SimRadio sim(rp, 30);
    setup(sim, {"x", "hackrf", 11, 446e6, 12.5e3, 2.4e6, 100e3, -110, {}});
    const ModeTarget tg = modeTargetFor(11, 12.5e3, 2.4e6, 446e6);
    AutoGainController c; AgSettings s; s.mode = AgMode::Auto; c.configure(rp, tg, s);
    double now = 0;
    c.start(now, sim.live.totalDb, false, &sim.live);
    run(c, sim, now, 40);
    CHECK(!c.bracketing(), "settled after 40 s");
    c.notifyRetune("test");
    run(c, sim, now, 0.4);
    CHECK(c.bracketing() || c.takeBracketDone() || c.changes() > 0, "a retune notice searches again");
    run(c, sim, now, 30);
    CHECK(!c.status().empty() && c.status().find("Gain") == 0, "status '%s'", c.status().c_str());
    CHECK(std::strlen(AutoGainController::traceHeader()) > 10 && c.traceLine(now).find(',') != std::string::npos, "trace line");
    // a prior searches only around itself
    SimRadio s2(rp, 70);
    setup(s2, {"x", "hackrf", 11, 446e6, 12.5e3, 2.4e6, 100e3, -110, {}});
    AutoGainController c2; c2.configure(rp, tg, s);
    double n2 = 0;
    StagePlan pr = planStages(rp, 70, tg.policy, 0xffffffffu, {}, false);
    s2.live = pr; s2.pending = pr;
    c2.start(n2, 70, false, &pr);
    c2.setPrior(pr);
    double lo = 1e9, hi = -1e9;
    run(c2, s2, n2, 8, [&](double) { if (c2.bracketing()) { lo = std::min(lo, c2.current().totalDb); hi = std::max(hi, c2.current().totalDb); } });
    CHECK(hi - lo <= 14.1 && lo >= 70 - 10, "prior: search inside +/-9 dB (%.0f..%.0f)", lo, hi);
}

// v2 probes now and then; it must not hunt: in a static scene the number of gain moves per minute stays small once it has settled, and the gain does not drift
static void testNoHunting() {
    const Scene sc = {"static", "hackrf", 11, 446e6, 12.5e3, 2.4e6, 100e3, -95, {{600e3, -62}, {-700e3, -64}}};
    RadioProfile rp = prof(sc.radio);
    SimRadio sim(rp, 40);
    setup(sim, sc);
    const ModeTarget tg = modeTargetFor(sc.std_, sc.bw, sc.rate, sc.freqHz);
    AutoGainController c; AgSettings s; s.mode = AgMode::Auto; c.configure(rp, tg, s);
    double now = 0;
    c.start(now, sim.live.totalDb, false, &sim.live);
    run(c, sim, now, 60);
    const double g60 = c.current().totalDb;
    int moves = 0; double prev = g60, lo = g60, hi = g60;
    run(c, sim, now, 180, [&](double) { const double g = c.current().totalDb; if (std::fabs(g - prev) > 0.01) { moves++; prev = g; } lo = std::min(lo, g); hi = std::max(hi, g); });
    if (verbose) printf("no hunting: %d moves in 180 s, gain %.1f..%.1f (at 60 s %.1f)\n", moves, lo, hi, g60);
    CHECK(moves <= 24, "%d gain moves in 3 minutes of a static scene (probes included)", moves);
    CHECK(hi - lo <= 8.0, "the gain wandered over %.1f dB", hi - lo);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-v")) verbose = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-grid")) { grid(); return 0; }
        if (!strcmp(argv[i], "-grid2")) { grid2(); return 0; }
        if (!strcmp(argv[i], "-imd")) { gridImd(); return 0; }
        if (!strcmp(argv[i], "-log") && i + 1 < argc) { logScene(atoi(argv[i + 1])); return 0; }
        if (!strcmp(argv[i], "-logd") && i + 3 < argc) {   // -logd radio sig blocker [old]: DVB-T2
            Scene sc = {"g", argv[i + 1], 1, 586e6, 7.6e6, 9.14e6, 0, atof(argv[i + 2]), {}, 3, 8, -1, 25};
            const double blk = atof(argv[i + 3]);
            if (blk > -900) sc.blockers = {{4e6, blk}};
            logScene(0, &sc, !(i + 4 < argc && !strcmp(argv[i + 4], "old")));
            return 0;
        }
        if (!strcmp(argv[i], "-logg") && i + 3 < argc) {   // -logg radio sig blocker [old]
            Scene sc = {"g", argv[i + 1], 11, 446e6, 12.5e3, 2.4e6, 100e3, atof(argv[i + 2]), {}};
            const double blk = atof(argv[i + 3]);
            if (blk > -900) sc.blockers = {{600e3, blk}, {-700e3, blk - 2}};
            logScene(0, &sc, !(i + 4 < argc && !strcmp(argv[i + 4], "old")));
            return 0;
        }
    }
    testMeter();
    testCalibration();
    testScenes();
    testCalibratedUnit();
    testBehaviour();
    testNoHunting();
    printf(fails ? "autogain_v2: %d FAILED\n" : "autogain_v2: all passed\n", fails);
    return fails ? 1 : 0;
}
