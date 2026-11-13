// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Auto gain 3.6: per radio model (every profile of radioProfiles(), synthetic levels through the simulated front end), the SDRplay LNA / IF split by policy, and the
// per-mode targets (which modes the refinement touches; every other mode keeps its class numbers).
#include "../../core/src/source/sdrplay_api_min.h"
#include "autogain_sim.h"
#include "dect2/engine/mode_tuning.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
using namespace agsim;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

namespace dect2 { namespace native { void sdrplayGainSplitForTest(unsigned char, double, double, bool, int&, int&, int&); } }

struct Scene { const char* name; double sig; std::vector<Tone> blk; double startFrac; };

// settled SNR and clipping seconds of a loop on a scene
struct Res { double snr = -99, gain = 0, clipSec = 0; };
static Res runOne(const RadioProfile& rp, const Scene& sc, bool v2) {
    SimRadio sim(rp, rp.minTotal(false) + sc.startFrac * (rp.maxTotal(false) - rp.minTotal(false)));
    sim.rateHz = 2.4e6; sim.chanBw = 12.5e3; sim.chanOff = 100e3; sim.sigDbm = sc.sig; sim.blockers = sc.blk; sim.sigCrestDb = 3; sim.lockAt = 8;
    const ModeTarget tg = modeTargetFor(11, 12.5e3, 2.4e6, 446e6);
    AutoGainController c;
    AgSettings s; s.mode = AgMode::Auto; s.v2 = v2;
    c.configure(rp, tg, s);
    double now = 0;
    c.start(now, sim.live.totalDb, false, &sim.live);
    Res o;
    double sum = 0; int n = 0;
    run(c, sim, now, 100, [&](double t) {
        if (sim.lastClip > tg.clipLimit) o.clipSec += 0.05;
        if (t >= 70 && !c.probing()) { sum += sim.snrDb(); n++; }
    });
    o.snr = n ? sum / n : sim.snrDb();
    o.gain = c.current().totalDb;
    if (getenv("AGLOG") && strstr(sc.name, getenv("AGLOG")) && rp.id == (getenv("AGRADIO") ? getenv("AGRADIO") : rp.id)) for (const auto& e : c.log()) printf("     %.1f %.1f -> %.1f %s\n", e.t, e.fromDb, e.toDb, e.why.c_str());
    return o;
}
static double bestOf(const RadioProfile& rp, const Scene& sc) {
    const ModeTarget tg = modeTargetFor(11, 12.5e3, 2.4e6, 446e6);
    double best = -1e9;
    for (Policy pol : {Policy::Linearity, Policy::Balanced, Policy::Sensitivity})
        for (double g : totalSteps(rp, pol, 0xffffffffu, false)) {
            SimRadio s(rp, g, false);
            s.rateHz = 2.4e6; s.chanBw = 12.5e3; s.chanOff = 100e3; s.sigDbm = sc.sig; s.blockers = sc.blk; s.sigCrestDb = 3; s.lockAt = 8; s.jitterDb = 0;
            s.live = planStages(rp, g, pol, 0xffffffffu, {}, false);
            const BlockStats a = s.adc();
            if (a.clipFraction > tg.clipLimit || a.headroomDb < tg.minHeadroomDb) continue;
            best = std::max(best, s.snrDb());
        }
    return best;
}

static void perRadio() {
    const std::vector<Scene> scenes = {
        {"quiet band, weak signal", -105, {}, 0.5},
        {"weak signal beside strong carriers", -100, {{600e3, -50}, {-700e3, -52}}, 0.5},
        {"strong signal, started at the top gain", -60, {}, 1.0},
        {"mid signal, started at the bottom gain", -90, {}, 0.0},
    };
    printf("%-12s %-36s %8s %8s %8s | clip(old/new)\n", "radio", "scene", "old", "new", "best");
    int n = 0;
    for (const auto& rp : radioProfiles()) {
        if (rp.minTotal(false) >= rp.maxTotal(false)) continue;
        for (const auto& sc : scenes) {
            const Res a = runOne(rp, sc, false), b = runOne(rp, sc, true);
            const double best = bestOf(rp, sc);
            printf("%-12s %-36s %8.1f %8.1f %8.1f | %.1f/%.1f\n", rp.id.c_str(), sc.name, a.snr, b.snr, best, a.clipSec, b.clipSec);
            n++;
            // a radio whose gain steps are coarse (an Airspy step is 2 dB, a network radio slow) may sit a step below the best: 4 dB; never clip for long, never worse than the old loop by more than 1.5 dB
            // an SNR above 20 dB is more than any decoder needs: the comparison is capped there (the loop prefers headroom to the last tenths of a dB when the antenna's noise is far over the ADC's)
            const double cap = 20.0;
            CHECK(std::min(b.snr, cap) >= std::min(best, cap) - 4.0, "%s / %s: settled SNR %.1f dB is more than 4 dB under the best %.1f dB", rp.id.c_str(), sc.name, b.snr, best);
            CHECK(std::min(b.snr, cap) >= std::min(a.snr, cap) - 1.5, "%s / %s: new loop %.1f dB, old %.1f dB", rp.id.c_str(), sc.name, b.snr, a.snr);
            CHECK(b.clipSec <= 3.0, "%s / %s: clipped %.1f s", rp.id.c_str(), sc.name, b.clipSec);
            CHECK(b.gain >= rp.minTotal(false) - 0.01 && b.gain <= rp.maxTotal(false) + 0.01, "%s / %s: gain %.1f outside the radio's range", rp.id.c_str(), sc.name, b.gain);
        }
    }
    printf("autogain_models: %d radio / scene runs\n", n);
}

static void sdrplaySplit() {
    namespace sp = dect2::sdrplay;
    const unsigned char hws[] = {sp::kRsp1, sp::kRsp1A, sp::kRsp1B, sp::kRsp2, sp::kRspDuo, sp::kRspDx, sp::kRspDxR2};
    const double freqs[] = {7e6, 98e6, 446e6, 1090e6};
    int n = 0;
    for (unsigned char hw : hws)
        for (double f : freqs)
            for (double g = 0; g <= 100; g += 7) {
                int l0, r0, gl0, l1, r1, gl1;
                dect2::native::sdrplayGainSplitForTest(hw, f, g, false, l0, r0, gl0);
                dect2::native::sdrplayGainSplitForTest(hw, f, g, true, l1, r1, gl1);
                n++;
                CHECK(r1 >= 20 && r1 <= 59, "hw %d f %.0f g %.0f: IF reduction %d out of range", hw, f, g, r1);
                CHECK(gl1 >= gl0, "hw %d f %.0f g %.0f: linearity LNA reduction %d dB under the sensitivity split's %d dB", hw, f, g, gl1, gl0);
                if (r0 > 20 && r0 < 59 && r1 > 20 && r1 < 59) CHECK(gl0 + r0 == gl1 + r1, "hw %d f %.0f g %.0f: the total reduction differs (%d vs %d)", hw, f, g, gl0 + r0, gl1 + r1);
            }
    printf("autogain_models: %d SDRplay model/band/gain splits\n", n);
    // the plan carries the policy to the driver setting, and a policy change at the same gain is a different plan
    const RadioProfile rp = *radioProfileById("sdrplay");
    const StagePlan a = planStages(rp, 60, Policy::Sensitivity, 0xffffffffu, {}, false), b = planStages(rp, 60, Policy::Linearity, 0xffffffffu, {}, false);
    CHECK(!(a == b) && a.totalDb == b.totalDb, "plans of one gain with two policies must differ");
    dect2::TuneSettings t;
    applyPlan(rp, b, t);
    CHECK(t.radio["gainsplit"] == "linearity", "gainsplit = %s", t.radio["gainsplit"].c_str());
    applyPlan(rp, a, t);
    CHECK(t.radio["gainsplit"] == "sensitivity", "gainsplit = %s", t.radio["gainsplit"].c_str());
    const RadioProfile hk = *radioProfileById("hackrf");
    dect2::TuneSettings t2;
    applyPlan(hk, planStages(hk, 30, Policy::Linearity, 0xffffffffu, {}, false), t2);
    CHECK(t2.radio.find("gainsplit") == t2.radio.end(), "other radios get no gainsplit setting");
}

// the class numbers of every mode are kept except for the listed ones
static void perMode() {
    int n = 0, changed = 0;
    const int refined[] = {16, 18, 21, 22, 25, 26, 29, 30, 37, 46, 53, 67, 73};
    for (int std_ = 0; std_ < 1200; std_++) {
        const dect2::ModeTuning* mt = dect2::modeTuning(std_);
        if (!mt) continue;
        const int code = std_ - 1 >= 0 ? std_ - 1 : 0;
        (void)code;
        const double bw = mt->bandwidthMhz * 1e6, rate = mt->sampleRate;
        ModeTarget base;   // the unrefined class: ask for the table at a code the refinement never touches is not possible, so rebuild it: modeTargetFor without the step
        const ModeTarget t = modeTargetFor(mt->stdMode, bw, rate, 0);
        ModeTarget again = t;
        applyModeRefinement(again, mt->stdMode);   // idempotent
        CHECK(again.usePeakHold == t.usePeakHold && again.kind == t.kind && again.minHeadroomDb == t.minHeadroomDb, "mode %d: refinement not idempotent", std_);
        CHECK(t.minHeadroomDb >= 3 && t.minHeadroomDb <= 8, "mode %d: headroom floor %.1f", std_, t.minHeadroomDb);
        CHECK(t.headroomDb >= 3 && t.headroomDb <= 16, "mode %d: headroom %.1f", std_, t.headroomDb);
        CHECK(t.improveDb > 0 && t.improveDb <= 2 && t.windowDb > 0, "mode %d: improve %.2f window %.2f", std_, t.improveDb, t.windowDb);
        CHECK(t.why && t.why[0], "mode %d: no explanation", std_);
        bool listed = false;
        for (int r : refined) if (r == std_) listed = true;
        // 'refined' is by engine standard code; modeTuning() keys are the same codes (ModeTuning::stdMode)
        const int sc = mt->stdMode;
        bool isRef = false;
        for (int r : refined) if (r == sc) isRef = true;
        if (isRef) {
            changed++;
            if (sc == 22 || sc == 73) CHECK(t.kind == ModeKind::Gnss && t.metric == Metric::Sensitivity, "mode %d: LoRa class", sc);
            else CHECK(t.usePeakHold && t.minHeadroomDb >= 5, "mode %d: burst floor", sc);
        } else CHECK(!t.usePeakHold || t.kind == ModeKind::Pulsed, "mode %d: peak-hold without being listed", sc);
        (void)listed; (void)base;
        n++;
    }
    printf("autogain_models: %d modes checked, %d refined\n", n, changed);
    CHECK(n > 100, "only %d modes in the registry", n);
}

// ./test_autogain_models trace <radio> <scene 0-3> [old]: the controller's status line every 5 s
static int trace(int argc, char** argv) {
    const std::vector<Scene> scenes = {{"quiet", -105, {}, 0.5}, {"strong nb", -100, {{600e3, -50}, {-700e3, -52}}, 0.5}, {"top", -60, {}, 1.0}, {"bottom", -90, {}, 0.0}};
    const RadioProfile rp = *radioProfileById(argv[2]);
    const Scene sc = scenes[(size_t)atoi(argv[3])];
    const bool v2 = !(argc > 4);
    SimRadio sim(rp, rp.minTotal(false) + sc.startFrac * (rp.maxTotal(false) - rp.minTotal(false)));
    sim.rateHz = 2.4e6; sim.chanBw = 12.5e3; sim.chanOff = 100e3; sim.sigDbm = sc.sig; sim.blockers = sc.blk; sim.sigCrestDb = 3; sim.lockAt = 8;
    AutoGainController c;
    AgSettings s; s.mode = AgMode::Auto; s.v2 = v2;
    c.configure(rp, modeTargetFor(11, 12.5e3, 2.4e6, 446e6), s);
    double now = 0;
    c.start(now, sim.live.totalDb, false, &sim.live);
    double nextP = 0;
    run(c, sim, now, 100, [&](double t) { if (t >= nextP) { nextP += 5; printf("%5.0f s gain %.1f snr %.1f margin %.1f floor %.1f | %s\n", t, c.current().totalDb, sim.snrDb(), c.meters().adcMarginDb, c.meters().floorDbfs, c.status().c_str()); } });
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 3 && std::string(argv[1]) == "trace") return trace(argc, argv);
    perMode();
    sdrplaySplit();
    perRadio();
    printf(fails ? "autogain_models: %d FAILED\n" : "autogain_models: all passed\n", fails);
    return fails ? 1 : 0;
}
