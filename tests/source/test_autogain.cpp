// Automatic gain tests (docs/AUTOGAIN.md): the radio models and the stage planner, the measurements (ADC, floor, intermodulation), the mode
// targets, and the controller's three modes against a simulated radio (autogain_sim.h) with synthetic scenes. No radio, a simulated clock.
#include "autogain_sim.h"
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

// ------------------------------------------------------------------------------------------------------------------------------ models
static void testProfiles() {
    const char* ids[] = {"hackrf", "rtl-r820t", "rtl-blog-v3", "rtl-r828d", "rtl-blog-v4", "rtl-blog-v4l", "rtl-e4000", "rtltcp", "airspy", "airspyhf",
                         "sdrplay", "pluto", "lime", "usrp", "bladerf", "fobos", "rx888", "spyserver", "kiwisdr", "generic"};
    for (const char* id : ids) {
        const RadioProfile* p = radioProfileById(id);
        CHECK(p != nullptr, "profile %s missing", id);
        if (!p) continue;
        CHECK(p->settableCount() >= 1, "%s: nothing to set", id);
        for (const auto& s : p->stages) {
            CHECK(!s.steps.empty(), "%s/%s: no steps", id, s.id.c_str());
            for (size_t k = 1; k < s.steps.size(); k++) CHECK(s.steps[k] > s.steps[k - 1], "%s/%s: steps not ascending", id, s.id.c_str());
            if (s.kind == StageKind::Amp) CHECK(s.userOnly, "%s: the amp must be the user's", id);
            CHECK(!s.note.empty(), "%s/%s: no note", id, s.id.c_str());
        }
        CHECK(p->maxTotal(false) > p->minTotal(false), "%s: empty range", id);
    }
    const RadioProfile h = prof("hackrf");
    CHECK(h.settableCount() == 2 && h.hasAmp && h.hasBias, "hackrf: LNA + VGA settable, amp + bias");
    CHECK(prof("rtl-r820t").singleControl() && prof("rtl-r820t").stages[0].steps.size() == 29, "rtl: one control, 29 steps");
    CHECK(prof("rtltcp").remote && prof("rtltcp").settleSec > prof("rtl-r820t").settleSec, "rtl_tcp: remote, settles longer");
    const RadioProfile sp = prof("sdrplay");
    CHECK(sp.stages[(size_t)sp.stageIndex("rfgr")].reduction && sp.stages[(size_t)sp.stageIndex("ifgr")].reduction, "sdrplay: RFGR / IFGR are reductions");
    // detection
    DeviceInfo d; d.kind = DeviceInfo::Native; d.board = "rtlsdr"; d.name = "RTL-SDR Blog V4 (00000001)"; d.gainMaxDb = 49.6;
    CHECK(radioProfileFor(d).id == "rtl-blog-v4", "Blog V4 by name (%s)", radioProfileFor(d).id.c_str());
    d.name = "Generic RTL2832U OEM"; CHECK(radioProfileFor(d).id == "rtl-r820t", "plain RTL");
    d.board = "rtltcp"; CHECK(radioProfileFor(d).remote, "rtl_tcp remote");
    DeviceInfo hk; hk.kind = DeviceInfo::HackRF; hk.board = "HackRF"; CHECK(radioProfileFor(hk).id == "hackrf", "hackrf");
    DeviceInfo u; u.kind = DeviceInfo::Soapy; u.board = "weird"; u.name = "Mystery"; u.gainMinDb = 3; u.gainMaxDb = 33;
    const RadioProfile ur = radioProfileFor(u);
    CHECK(ur.singleControl() && std::fabs(ur.minTotal(false) - 3) < 0.01 && std::fabs(ur.maxTotal(false) - 33) < 0.01, "unknown: its own range");
    DeviceInfo s2; s2.kind = DeviceInfo::Soapy; s2.board = "sdrplay"; s2.name = "SDRplay RSP1A"; s2.gainMinDb = 0; s2.gainMaxDb = 59;
    RadioSetting rs; rs.key = "rfgr"; rs.values = {"auto", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"}; s2.settings.push_back(rs);
    const RadioProfile spr = radioProfileFor(s2);
    const int ri = spr.stageIndex("rfgr");
    CHECK(ri >= 0 && spr.stages[(size_t)ri].settable && spr.stages[(size_t)ri].settingIsIndex, "sdrplay with an rfgr setting: OnAir sets the LNA state");
    if (ri >= 0) {
        CHECK(stageSettingValue(spr.stages[(size_t)ri], spr.stages[(size_t)ri].maxDb()) == "0", "most RF gain = state 0 (%s)", stageSettingValue(spr.stages[(size_t)ri], spr.stages[(size_t)ri].maxDb()).c_str());
        CHECK(stageSettingValue(spr.stages[(size_t)ri], 0) == "9", "least RF gain = top state");
        TuneSettings t;
        StagePlan p = planStages(spr, 40, Policy::Linearity, 0xffffffffu, {}, false);
        applyPlan(spr, p, t);
        const StagePlan back = planFromTune(spr, t);
        CHECK(std::fabs(back.totalDb - p.totalDb) < 0.01, "sdrplay plan round trip %.1f / %.1f", back.totalDb, p.totalDb);
    }
}

static void testPlanner() {
    const RadioProfile h = prof("hackrf");
    const int L = h.stageIndex("lna"), V = h.stageIndex("vga"), A = h.stageIndex("amp");
    StagePlan s = planStages(h, 40, Policy::Sensitivity, 0xffffffffu, {}, false);
    StagePlan l = planStages(h, 40, Policy::Linearity, 0xffffffffu, {}, false);
    CHECK(s.db[(size_t)L] == 40 && s.db[(size_t)V] == 0, "sensitivity: LNA high, VGA low (%g/%g)", s.db[(size_t)L], s.db[(size_t)V]);
    CHECK(l.db[(size_t)L] == 0 && l.db[(size_t)V] == 40, "linearity: VGA first (%g/%g)", l.db[(size_t)L], l.db[(size_t)V]);
    CHECK(s.nfDb < l.nfDb - 3, "LNA first has the lower noise figure (%.1f / %.1f)", s.nfDb, l.nfDb);
    CHECK(l.iip3Dbm > s.iip3Dbm + 10, "VGA first is more linear (%.1f / %.1f)", l.iip3Dbm, s.iip3Dbm);
    CHECK(s.db[(size_t)A] == 0 && l.db[(size_t)A] == 0 && !s.ampOn, "the amp stays off");
    StagePlan a = planStages(h, 60, Policy::Balanced, 0xffffffffu, {}, true);
    CHECK(a.ampOn && a.db[(size_t)A] == 14 && std::fabs(a.totalDb - 60) < 0.01, "user's amp kept on, inside the total (%g)", a.totalDb);
    StagePlan c = planStages(h, 70, Policy::Sensitivity, 0xffffffffu, {}, false, 16);
    CHECK(c.db[(size_t)L] <= 16 && std::fabs(c.totalDb - 70) < 0.01, "front cap: LNA <= 16 (%g), total %g", c.db[(size_t)L], c.totalDb);
    StagePlan m = planStages(h, 50, Policy::Sensitivity, 1u << V, {0, 8, 0}, false);
    CHECK(m.db[(size_t)L] == 8 && m.db[(size_t)V] == 42, "masked LNA stays at 8 (%g/%g)", m.db[(size_t)L], m.db[(size_t)V]);
    for (Policy p : {Policy::Sensitivity, Policy::Balanced, Policy::Linearity}) {
        const auto st = totalSteps(h, p, 0xffffffffu, false);
        CHECK(st.size() == 52 && st.front() == 0 && st.back() == 102, "hackrf totals 0..102 in 2 dB (%zu)", st.size());
    }
    const RadioProfile r = prof("rtl-r820t");
    for (double t : {-5.0, 0.0, 10.0, 33.0, 49.6, 70.0}) {
        const StagePlan p = planStages(r, t, Policy::Balanced, 0xffffffffu, {}, false);
        bool inTable = false;
        for (double v : r.stages[0].steps) if (std::fabs(v - p.totalDb) < 1e-6) inTable = true;
        CHECK(inTable, "rtl: %g -> %g in the table", t, p.totalDb);
    }
    CHECK(std::fabs(planStages(r, 33, Policy::Balanced, 0xffffffffu, {}, false).totalDb - 32.8) < 0.01, "rtl snaps to the nearest step");
}

// ------------------------------------------------------------------------------------------------------------------------------ measurement
static void testMeasure() {
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0, 0.05f);
    std::vector<std::complex<float>> x(200000);
    for (auto& v : x) v = {nd(rng), nd(rng)};
    BlockStats b = measureBlock(x.data(), x.size());
    CHECK(std::fabs(b.crestDb - 7.4) < 1.0, "noise crest %.1f", b.crestDb);
    CHECK(std::fabs(b.rmsDbfs - 10 * std::log10(2 * 0.05 * 0.05)) < 0.2, "noise rms %.1f", b.rmsDbfs);
    CHECK(b.clipFraction == 0, "noise no clip");
    for (size_t i = 0; i < x.size(); i++) x[i] = std::polar(0.5f, (float)i * 0.01f);
    b = measureBlock(x.data(), x.size());
    CHECK(std::fabs(b.crestDb) < 0.6 && std::fabs(b.headroomDb - 6.0) < 0.3, "tone crest %.1f headroom %.1f", b.crestDb, b.headroomDb);
    for (size_t i = 0; i < x.size(); i++) { auto v = std::polar(1.4f, (float)i * 0.01f); x[i] = {std::clamp(v.real(), -1.f, 1.f), std::clamp(v.imag(), -1.f, 1.f)}; }
    b = measureBlock(x.data(), x.size());
    CHECK(b.clipFraction > 0.2 && b.headroomDb < 0.1, "clipped tone: %.2f clipped", b.clipFraction);
    SignalStats ss; ss.peak = 0.3f; ss.rmsDbfs = -20; ss.clipFraction = 0;
    for (int k = 0; k < 64; k++) ss.hist[k] = k < 19 ? 1000 : 0;
    b = fromSignalStats(ss);
    CHECK(b.p999 <= 0.3 + 1e-6 && b.headroomDb > 10 && b.headroomDb < 12, "stats headroom %.1f", b.headroomDb);

    // floor
    std::vector<float> sp(1024, -90.f);
    for (int k = 512 - 20; k < 512 + 20; k++) sp[(size_t)k] = -70.f;
    const FloorEstimate f = estimateFloor(sp, 1e6, 40e3, 0, 8);
    CHECK(f.valid && std::fabs(f.floorDbfs + 90) < 0.5 && std::fabs(f.snrDb - 20) < 0.5, "floor %.1f snr %.1f", f.floorDbfs, f.snrDb);
    const FloorEstimate f12 = estimateFloor(sp, 1e6, 40e3, 0, 12);
    CHECK(f12.adcMarginDb > f.adcMarginDb + 20, "a deeper ADC has more margin under the same floor");

    // intermodulation: a real line moves with the gain, a product three times as fast
    std::vector<float> lo(1024, -90.f), hi(1024, -87.f);
    lo[300] = -40; hi[300] = -37;   // a strong signal: +3 dB for +3 dB
    lo[700] = -75; hi[700] = -66;   // a third-order product: +9 dB for +3 dB
    ImdReport r = imdCompare(lo, hi, 3);
    CHECK(r.valid && r.spurs == 1 && r.overload && std::fabs(r.worstExcessDb - 6) < 0.6, "imd spur found: %d spurs, %.1f", r.spurs, r.worstExcessDb);
    hi[700] = -72;
    r = imdCompare(lo, hi, 3);
    CHECK(r.spurs == 0 && !r.overload && std::fabs(r.slopeDbPerDb - 1) < 0.15, "clean: %d spurs, slope %.2f", r.spurs, r.slopeDbPerDb);
    std::vector<float> hc(lo);
    for (auto& v : hc) v += 1.0f;   // the power rose 1 dB for 3 dB of gain: compression
    r = imdCompare(lo, hc, 3);
    CHECK(r.compressed && r.overload, "compression slope %.2f", r.slopeDbPerDb);

    // the mode's quality
    RxTelemetry t; t.standard = 0; t.dataValid = true; t.dataSnrDb = 21; t.plpValid = true; t.plpMerDb = 23; t.plpPreBer = 1e-4; t.blocksOk = 90; t.blocksBad = 10;
    const Quality q = qualityFrom(t);
    CHECK(q.locked && q.snrValid && q.merValid && q.berValid && std::fabs(q.fecOk - 0.9) < 1e-9, "quality from telemetry");
}

static void testTargets() {
    CHECK(modeTargetFor(1, 8e6, 9.14e6).kind == ModeKind::BroadcastTv && modeTargetFor(1, 8e6, 9.14e6).metric == Metric::Mer, "DVB-T2: broadcast TV, MER");
    const ModeTarget ad = modeTargetFor(13, 2e6, 2.4e6);
    CHECK(ad.kind == ModeKind::Pulsed && ad.metric == Metric::Sensitivity && ad.policy == Policy::Sensitivity, "ADS-B: sensitivity");
    const ModeTarget dmr = modeTargetFor(11, 12.5e3, 2.4e6);
    CHECK(dmr.kind == ModeKind::NarrowVoice && dmr.metric == Metric::ChannelSnr, "DMR: SNR in the channel");
    CHECK(modeTargetFor(44, 10e6, 10e6).metric == Metric::Headroom, "analyser: headroom");
    CHECK(modeTargetFor(7, 200e3, 2.4e6).policy == Policy::Linearity, "FM broadcast: linearity");
    CHECK(dmr.headroomDb > modeTargetFor(1, 8e6, 9.14e6).headroomDb, "narrow in a wide span keeps more headroom than TV");
}

// ------------------------------------------------------------------------------------------------------------------------------ Auto
struct Trace { int changes = 0, reversals = 0, clippedTicks = 0, lastDir = 0; double lastChange = -1; double last = 0; };

static AutoGainController makeCtl(const RadioProfile& r, const ModeTarget& t, AgMode m, bool ampAllowed = false) {
    AutoGainController c;
    AgSettings s; s.mode = m; s.v2 = false; s.ampAllowed = ampAllowed;
    c.configure(r, t, s);
    return c;
}

// one Auto run; returns the gain it settled at
static double autoRun(const RadioProfile& rp, int stdMode, double chanBw, double rate, double sigDbm, double startDb, Trace& tr, double sec = 120,
                      std::vector<Tone> blockers = {}, bool ampAllowed = false, bool userAmp = false, SimRadio* outSim = nullptr) {
    SimRadio sim(rp, startDb, userAmp);
    sim.rateHz = rate; sim.chanBw = chanBw; sim.sigDbm = sigDbm; sim.blockers = blockers;
    if (chanBw < 1e6) sim.chanOff = 100e3;   // a narrow mode is tuned beside the DC spike
    const ModeTarget tg = modeTargetFor(stdMode, chanBw, rate);
    AutoGainController c = makeCtl(rp, tg, AgMode::Auto, ampAllowed);
    double now = 0;
    c.start(now, sim.live.totalDb, userAmp, &sim.live);
    double prev = sim.live.totalDb;
    run(c, sim, now, sec, [&](double t) {
        if (c.bracketing() || t < 12) { prev = c.current().totalDb; tr.lastDir = 0; }   // the quick search at the start climbs and settles back: not hunting
        if (std::fabs(c.current().totalDb - prev) > 0.01) {
            const int d = c.current().totalDb > prev ? 1 : -1;
            if (tr.lastDir && d != tr.lastDir) tr.reversals++;
            tr.lastDir = d; tr.changes++; tr.lastChange = t; prev = c.current().totalDb;
        }
        if (t > sec * 0.5 && sim.lastClip > tg.clipLimit) tr.clippedTicks++;
    });
    if (verbose) {
        printf("  %-10s std %2d sig %6.1f dBm start %5.1f -> %5.1f dB, %d changes (last %.1f s), %d reversals, headroom %.1f, margin %.1f, snr %.1f\n", rp.id.c_str(), stdMode,
               sigDbm, startDb, c.current().totalDb, tr.changes, tr.lastChange, tr.reversals, c.meters().headroomDb, c.meters().adcMarginDb, sim.snrDb());
        for (const auto& e : c.log()) if (e.t > sec - 1e9 && verbose && getenv("AGLOG")) printf("     %.1f %.1f -> %.1f %s\n", e.t, e.fromDb, e.toDb, e.why.c_str());
    }
    if (outSim) *outSim = sim;
    tr.last = c.current().totalDb;
    return c.current().totalDb;
}

static void testAutoConverge() {
    struct Case { const char* id; };
    for (const char* id : {"hackrf", "rtl-r820t", "rtl-blog-v4", "airspy", "airspyhf", "sdrplay", "pluto", "generic", "rtltcp"}) {
        const RadioProfile rp = prof(id);
        double weakG = 0, strongG = 0;
        for (int sc = 0; sc < 2; sc++) {
            const double sig = sc == 0 ? -115 : -45;
            const double mid = (rp.minTotal(false) + rp.maxTotal(false)) / 2;
            for (double start : {rp.minTotal(false), mid, rp.maxTotal(false)}) {
                Trace tr;
                const double g = autoRun(rp, 11, 12.5e3, 2.4e6, sig, start, tr, 120);
                CHECK(tr.lastChange < 60, "%s sig %g start %g: still moving at %.1f s", id, sig, start, tr.lastChange);
                CHECK(tr.clippedTicks == 0, "%s sig %g start %g: clipped %d ticks after converging", id, sig, start, tr.clippedTicks);
                CHECK(tr.reversals <= 2, "%s sig %g start %g: %d reversals (hunting)", id, sig, start, tr.reversals);
                if (start == mid) { if (sc == 0) weakG = g; else strongG = g; }
            }
        }
        CHECK(weakG > strongG + 6, "%s: weak signal gets more gain (%.1f) than a strong one (%.1f)", id, weakG, strongG);
    }
}

static void testAutoNoHunting() {
    for (const char* id : {"hackrf", "rtl-r820t", "airspyhf", "generic"}) {
        Trace tr;
        SimRadio sim(prof(id));
        autoRun(prof(id), 1, 8e6, 9.14e6, -70, 20, tr, 400);
        CHECK(tr.reversals <= 2 && tr.lastChange < 120, "%s TV: %d reversals, last change %.1f s", id, tr.reversals, tr.lastChange);
    }
}

static void testAutoBurst() {
    const RadioProfile rp = prof("hackrf");
    SimRadio sim(rp, 30);
    sim.rateHz = 2.4e6; sim.chanBw = 12.5e3; sim.sigDbm = -105;
    const ModeTarget tg = modeTargetFor(11, 12.5e3, 2.4e6);
    AutoGainController c = makeCtl(rp, tg, AgMode::Auto);
    double now = 0;
    c.start(now, sim.live.totalDb, false, &sim.live);
    run(c, sim, now, 60);
    const double settled = c.current().totalDb;
    // a strong transmitter keys up for 6 s
    sim.blockers = {{400e3, -30}};
    double clipStart = -1, clipLong = 0;
    run(c, sim, now, 6, [&](double t) {
        if (sim.lastClip > tg.clipLimit) { if (clipStart < 0) clipStart = t; clipLong = std::max(clipLong, t - clipStart); } else clipStart = -1;
    });
    const double during = c.current().totalDb;
    CHECK(during < settled - 6, "burst: gain cut (%.1f -> %.1f)", settled, during);
    CHECK(clipLong < 1.5, "burst: clipping lasted %.2f s", clipLong);
    sim.blockers.clear();
    run(c, sim, now, 90);
    CHECK(c.current().totalDb > settled - 8, "after the burst the gain recovers (%.1f, was %.1f)", c.current().totalDb, settled);
    CHECK(c.meters().overloads >= 1, "overload counted");
}

static void testAmpRules() {
    const RadioProfile rp = prof("hackrf");
    // weak signal, no permission: the amp never comes on; the advice suggests it
    {
        SimRadio sim(rp, 40);
        sim.sigDbm = -130; sim.chanBw = 12.5e3; sim.rateHz = 2.4e6;
        AutoGainController c = makeCtl(rp, modeTargetFor(11, 12.5e3, 2.4e6), AgMode::Auto, false);
        double now = 0;
        c.start(now, sim.live.totalDb, false, &sim.live);
        bool ampEver = false;
        run(c, sim, now, 150, [&](double) { if (sim.live.ampOn) ampEver = true; });
        CHECK(!ampEver, "the amp must never be switched on without permission");
        bool sugg = false;
        for (const auto& a : c.advice()) if (a.kind == Advice::EnableAmp) sugg = true;
        CHECK(sugg, "weak at full gain: suggests switching the amp on (gain %.1f, margin %.1f)", c.current().totalDb, c.meters().adcMarginDb);
    }
    // the user's own amp on, no permission: it stays on, even through an overload
    {
        SimRadio sim(rp, 50, true);
        sim.sigDbm = -60; sim.chanBw = 12.5e3; sim.rateHz = 2.4e6;
        AutoGainController c = makeCtl(rp, modeTargetFor(11, 12.5e3, 2.4e6), AgMode::Auto, false);
        double now = 0;
        c.start(now, sim.live.totalDb, true, &sim.live);
        bool ampOff = false;
        run(c, sim, now, 60, [&](double) { if (!sim.live.ampOn) ampOff = true; });
        sim.blockers = {{300e3, -25}};
        run(c, sim, now, 10, [&](double) { if (!sim.live.ampOn) ampOff = true; });
        CHECK(!ampOff, "the user's amp must not be switched off without permission");
    }
    // with permission: it may come on for a weak signal at full gain, and goes off first on an overload
    {
        SimRadio sim(rp, 90);
        sim.sigDbm = -135; sim.chanBw = 12.5e3; sim.rateHz = 2.4e6;
        AutoGainController c = makeCtl(rp, modeTargetFor(11, 12.5e3, 2.4e6), AgMode::Auto, true);
        double now = 0;
        c.start(now, sim.live.totalDb, false, &sim.live);
        run(c, sim, now, 120);
        CHECK(sim.live.ampOn, "allowed: amp on for a weak signal at full gain (gain %.1f margin %.1f)", c.current().totalDb, c.meters().adcMarginDb);
        sim.blockers = {{300e3, -20}};
        run(c, sim, now, 3);
        CHECK(!sim.live.ampOn, "allowed: amp off first on an overload");
    }
    // Optimise once and Safe start never touch the amp
    for (AgMode m : {AgMode::Once, AgMode::Safe}) {
        SimRadio sim(rp, 40);
        sim.sigDbm = -130; sim.chanBw = 12.5e3; sim.rateHz = 2.4e6;
        AutoGainController c = makeCtl(rp, modeTargetFor(11, 12.5e3, 2.4e6), m, false);
        double now = 0;
        c.start(now, sim.live.totalDb, false, &sim.live);
        bool ampEver = false;
        run(c, sim, now, 120, [&](double) { if (sim.live.ampOn) ampEver = true; });
        CHECK(!ampEver && !c.active(), "%s: amp untouched, finished", agModeName(m));
    }
}

// ------------------------------------------------------------------------------------------------------------------------------ Once
struct OnceOut { double gain = 0; double snr = 0; double bestSnr = -100; double bestAt = 0; double minGap = 1e9; int steps = 0; bool clipped = false; double dur = 0; std::string text; };

static OnceOut onceRun(const RadioProfile& rp, int stdMode, double chanBw, double rate, double sigDbm, std::vector<Tone> blockers, bool decoder = true, AgMode mode = AgMode::Once) {
    OnceOut o;
    SimRadio sim(rp, (rp.minTotal(false) + rp.maxTotal(false)) / 2);
    sim.rateHz = rate; sim.chanBw = chanBw; sim.sigDbm = sigDbm; sim.blockers = blockers; sim.decoder = decoder;
    if (chanBw < 1e6) sim.chanOff = 100e3;
    // the truth: the SNR at every reachable gain of the mode's split (as a decoder reports it: MER levels off at 35 dB)
    const ModeTarget tg = modeTargetFor(stdMode, chanBw, rate);
    const double cap = decoder ? 35 : 1e9;
    for (double g : totalSteps(rp, tg.policy, 0xffffffffu, false)) {
        SimRadio s2 = sim;
        s2.live = planStages(rp, g, tg.policy, 0xffffffffu, {}, false);
        s2.adc();
        const double snr = std::min(cap, s2.snrDb());
        if (s2.lastClip <= tg.clipLimit && snr > o.bestSnr + 0.01) { o.bestSnr = snr; o.bestAt = g; }
    }
    AutoGainController c = makeCtl(rp, tg, mode);
    double now = 0;
    c.start(now, sim.live.totalDb, false, &sim.live);
    double lastCmd = -1;
    std::vector<double> cmdTimes;
    StagePlan prev = sim.live;
    run(c, sim, now, 200, [&](double t) {
        if (!(c.current() == prev)) { cmdTimes.push_back(t); prev = c.current(); }
        if (!c.active() && o.dur == 0) o.dur = t;
    });
    (void)lastCmd;
    for (size_t i = 1; i + 1 < cmdTimes.size(); i++) o.minGap = std::min(o.minGap, cmdTimes[i] - cmdTimes[i - 1]);
    o.steps = (int)c.trials().size();
    o.gain = c.current().totalDb;
    sim.adc();
    o.snr = std::min(cap, sim.snrDb());
    o.clipped = sim.lastClip > tg.clipLimit;
    o.text = c.resultText();
    if (verbose) printf("  once %-10s std %2d sig %6.1f: %5.1f dB snr %.1f (best %.1f at %.1f) %d steps, %.1f s, min gap %.2f s: %s\n", rp.id.c_str(), stdMode, sigDbm, o.gain, o.snr,
                        o.bestSnr, o.bestAt, o.steps, o.dur, o.minGap, o.text.c_str());
    if (verbose && getenv("AGLOG")) for (const auto& e : c.log()) printf("     %.1f %.1f -> %.1f %s\n", e.t, e.fromDb, e.toDb, e.why.c_str());
    return o;
}

static void testOnce() {
    for (const char* id : {"hackrf", "rtl-r820t", "sdrplay", "airspyhf", "generic"}) {
        const RadioProfile rp = prof(id);
        // broadcast TV judged by MER: weak and strong
        const OnceOut w = onceRun(rp, 1, 8e6, 9.14e6, -85, {});
        const OnceOut s = onceRun(rp, 1, 8e6, 9.14e6, -35, {});
        CHECK(w.dur > 0 && w.dur < 120, "%s: finished in %.1f s", id, w.dur);
        CHECK(!w.clipped && !s.clipped, "%s: result clips", id);
        CHECK(w.gain > s.gain + 6, "%s: weak %.1f dB > strong %.1f dB", id, w.gain, s.gain);
        CHECK(w.snr > w.bestSnr - 1.5, "%s weak: SNR %.1f vs best %.1f", id, w.snr, w.bestSnr);
        CHECK(s.snr > s.bestSnr - 1.5, "%s strong: SNR %.1f vs best %.1f", id, s.snr, s.bestSnr);
        // settle times respected: every step lasts at least the settle plus the measurement (an overloaded step may end early)
        const double minStep = rp.settleSec + AgSettings().settleSec + AgSettings().measureSec * 0.9;
        CHECK(w.minGap >= std::min(minStep, rp.settleSec + AgSettings().settleSec) - 0.06, "%s: a step lasted only %.2f s", id, w.minGap);
    }
    // narrowband voice with strong neighbours whose third-order product lands in the channel: the front end must stay linear
    {
        const RadioProfile rp = prof("hackrf");
        const std::vector<Tone> bl = {{300e3, -45}, {500e3, -45}};   // 2 x 300 - 500 = 100 kHz: the product falls on the channel
        const OnceOut o = onceRun(rp, 11, 12.5e3, 2.4e6, -105, bl);
        CHECK(o.snr > o.bestSnr - 2, "imd scene: SNR %.1f vs best %.1f (gain %.1f, best at %.1f)", o.snr, o.bestSnr, o.gain, o.bestAt);
        CHECK(!o.clipped, "imd scene: no clipping");
    }
    // a mode without a decoder number: the channel SNR from the spectrum decides
    {
        const OnceOut o = onceRun(prof("rtl-r820t"), 26, 12.5e3, 2.4e6, -110, {}, false);
        CHECK(o.snr > o.bestSnr - 2 && o.text.find("channel SNR") != std::string::npos, "no decoder: channel SNR (%s)", o.text.c_str());
    }
    // ADS-B: the most sensitivity, protected from a strong FM / GSM transmitter in the span
    {
        SimRadio dummy(prof("rtl-blog-v4"));
        const OnceOut o = onceRun(prof("rtl-blog-v4"), 13, 2e6, 2.4e6, -80, {{700e3, -50}}, false);
        CHECK(!o.clipped && o.gain > 10, "ADS-B: %.1f dB, no clipping", o.gain);
    }
}

// ------------------------------------------------------------------------------------------------------------------------------ Safe
static void testSafe() {
    for (const char* id : {"hackrf", "rtl-r820t", "airspy", "generic"}) {
        const RadioProfile rp = prof(id);
        SimRadio sim(rp, 0);
        sim.rateHz = 2.4e6; sim.chanBw = 12.5e3; sim.sigDbm = -110; sim.chanOff = 100e3;
        const ModeTarget tg = modeTargetFor(11, 12.5e3, 2.4e6);
        AutoGainController c = makeCtl(rp, tg, AgMode::Safe);
        double now = 0;
        c.start(now, sim.live.totalDb, false, &sim.live);
        int clipped = 0;
        double base = -1e9, peakGain = 0;
        run(c, sim, now, 200, [&](double) { if (sim.lastClip > tg.clipLimit) clipped++; peakGain = std::max(peakGain, sim.live.totalDb); });
        if (!c.trials().empty()) base = c.trials().front().floor();
        CHECK(!c.active() && c.phase() == Phase::Done, "%s safe: finished (%s)", id, phaseName(c.phase()));
        CHECK(clipped == 0, "%s safe: clipped %d ticks", id, clipped);
        CHECK(c.trials().front().plan.totalDb <= rp.minTotal(false) + 0.01, "%s safe: started at the lowest gain", id);
        const AgTrial* fin = nullptr;
        for (const auto& t : c.trials()) if (t.plan == c.current()) fin = &t;
        const double rose = fin ? fin->floor() - base : 0;
        CHECK(rose >= tg.floorRiseDb - 0.01 || c.current().totalDb >= rp.maxTotal(false) - 0.01, "%s safe: stopped after the floor rose %.1f dB (at %.1f dB)", id, rose, c.current().totalDb);
        CHECK(c.current().totalDb < rp.maxTotal(false) - 0.01 || id == std::string("airspy"), "%s safe: did not need the whole range (%.1f)", id, c.current().totalDb);
        if (verbose) printf("  safe %-10s %.1f dB, floor rose %.1f, %zu steps: %s\n", id, c.current().totalDb, rose, c.trials().size(), c.resultText().c_str());
    }
    // a strong signal: the ramp stops before the ADC clips
    {
        const RadioProfile rp = prof("hackrf");
        SimRadio sim(rp, 0);
        sim.rateHz = 2.4e6; sim.chanBw = 12.5e3; sim.sigDbm = -40;
        const ModeTarget tg = modeTargetFor(11, 12.5e3, 2.4e6);
        AutoGainController c = makeCtl(rp, tg, AgMode::Safe);
        double now = 0;
        c.start(now, sim.live.totalDb, false, &sim.live);
        run(c, sim, now, 200);
        sim.adc();
        CHECK(sim.lastClip <= tg.clipLimit && c.meters().headroomDb >= tg.headroomDb - tg.windowDb - 3, "safe strong: ends with headroom %.1f", c.meters().headroomDb);
    }
}

// ------------------------------------------------------------------------------------------------------------------------------ memory, runner
static void testMemory() {
    GainMemory m;
    GainMemoryEntry e; e.plan.totalDb = 42; e.plan.db = {0, 32, 10}; e.plan.ampOn = false; e.metric = 27.5; e.metricName = "MER"; e.when = 1700000000;
    const std::string k = GainMemory::key("hackrf:abc", 1, 522);
    m.put(k, e);
    GainMemoryEntry e2 = e; e2.plan.totalDb = 20; m.put(GainMemory::key("rtl:1", 11, 446.1), e2);
    GainMemory m2;
    CHECK(m2.parse(m.serialise()), "parse");
    const GainMemoryEntry* f = m2.find(k);
    CHECK(f && f->plan.totalDb == 42 && f->plan.db.size() == 3 && f->plan.db[1] == 32 && f->metricName == "MER", "round trip");
    const GainMemoryEntry* g = m2.find(GainMemory::key("hackrf:abc", 1, 650));
    CHECK(g && g->plan.totalDb == 42, "another band falls back to the same radio and mode");
    CHECK(!m2.find(GainMemory::key("hackrf:abc", 2, 522)), "another mode is not used");
}

struct SimIo : AgRadioIo {
    SimRadio& r; double t = 0; std::vector<float> spec; int applied = 0;
    explicit SimIo(SimRadio& s) : r(s) {}
    double now() override { return t; }
    void sleep(double s) override { t += s; }
    bool apply(const AgCommand& c) override { r.apply(c.plan, t); applied++; return true; }
    bool measure(AgInput& in) override {
        t += 0.05;
        r.advance(t);
        in.adc = r.adc(); spec = r.spectrum(); in.spectrum = &spec; in.rateHz = r.rateHz; in.chanBwHz = r.chanBw; in.q = r.quality();
        return true;
    }
};

static void testRunner() {
    const RadioProfile rp = prof("rtl-r820t");
    SimRadio sim(rp, 10);
    sim.rateHz = 9.14e6; sim.chanBw = 8e6; sim.sigDbm = -80;
    SimIo io(sim);
    AgSettings s;
    const AgRunResult r = autogainRun(io, rp, modeTargetFor(1, 8e6, 9.14e6), s, 10, false, 120);
    CHECK(r.ok && io.applied > 3 && !r.log.empty(), "runner: %s", r.text.c_str());
    sim.advance(io.t + 1);
    CHECK(std::fabs(sim.live.totalDb - r.plan.totalDb) < 0.01, "runner leaves the radio at the result");
    // cancelled: back to the start
    struct CancelIo : SimIo { int n = 0; using SimIo::SimIo; bool cancelled() override { return ++n > 100; } };
    SimRadio sim2(rp, 10);
    sim2.rateHz = 9.14e6; sim2.chanBw = 8e6; sim2.sigDbm = -80;
    CancelIo ci(sim2);
    const double startG = sim2.live.totalDb;
    const AgRunResult r2 = autogainRun(ci, rp, modeTargetFor(1, 8e6, 9.14e6), s, sim2.live.totalDb, false, 120);
    sim2.advance(ci.t + 1);
    CHECK(!r2.ok && std::fabs(sim2.live.totalDb - startG) < 0.01, "cancelled: gain put back (%.1f, was %.1f)", sim2.live.totalDb, startG);
    AutoGainController c;
    c.configure(rp, modeTargetFor(1, 8e6, 9.14e6), s);
    const std::string j = autogainJson(c, 0);
    CHECK(j.front() == '{' && j.back() == '}' && j.find("\"meters\"") != std::string::npos, "json");
}

static void testSingleControl() {
    // a radio with one gain and coarse steps, every mode kind
    RadioProfile rp = genericProfile("coarse", 0, 30, 10);
    for (int std_ : {1, 4, 7, 11, 13, 14, 26, 44}) {
        Trace tr;
        autoRun(rp, std_, std_ == 1 || std_ == 4 ? 7e6 : 25e3, 8e6, -90, 0, tr, 120);
        CHECK(tr.reversals <= 2 && tr.clippedTicks == 0, "coarse single gain, std %d: %d reversals %d clipped", std_, tr.reversals, tr.clippedTicks);
        const OnceOut o = onceRun(rp, std_, std_ == 1 || std_ == 4 ? 7e6 : 25e3, 8e6, -90, {});
        CHECK(o.dur > 0 && !o.clipped, "coarse once std %d", std_);
    }
}

int main(int argc, char** argv) {
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    testProfiles();
    testPlanner();
    testMeasure();
    testTargets();
    testAutoConverge();
    testAutoNoHunting();
    testAutoBurst();
    testAmpRules();
    testOnce();
    testSafe();
    testMemory();
    testRunner();
    testSingleControl();
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("autogain: all tests passed\n");
    return 0;
}
