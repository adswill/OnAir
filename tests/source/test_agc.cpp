// Auto-gain tests: the device gain models, the mode profiles, the measurements and the SmartAgc control loop against a simulated front end
// (agc_fake.h). No radio: the loop is driven with a simulated clock.
#include "agc_fake.h"
#include <cstdio>
#include <cstdlib>
#include <set>
using namespace dect2;
using namespace agcsim;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static GainModel hackrf() { DeviceInfo d; d.kind = DeviceInfo::HackRF; d.board = "HackRF One"; return gainModelFor(d); }
static GainModel generic(const char* board, double lo, double hi) { DeviceInfo d; d.kind = DeviceInfo::Soapy; d.board = board; d.gainMinDb = lo; d.gainMaxDb = hi; return gainModelFor(d); }

struct Run {
    int changes = 0, lastChangeStep = 0, steps = 0;
    bool ampEverOn = false, reverted = false;
    double firstSettledAt = -1;
};

// Runs the loop for `sec` seconds at 20 Hz; hook(t) may change the scene
template <class Hook>
static Run drive(SmartAgc& a, FrontEnd& fe, double t0, double sec, const bool useQuality, Hook hook) {
    Run r;
    for (double t = t0; t < t0 + sec; t += 0.05) {
        hook(t);
        AgcCommand c;
        const AgcQuality q = useQuality ? fe.quality() : AgcQuality{};
        const AgcMeasure me = fe.measure();
        if (a.update(t, me, q, c)) {
            if (getenv("AGC_TRACE")) printf("t=%.2f rms=%.1f oob=%.1f margin=%.1f clip=%.3f gain %d -> %d (%s) cap %d\n", t, me.rmsDbfs, me.oobDbfs, me.floorMarginDb, me.clipFraction, fe.gain(), c.totalDb, agcReasonName(c.reason), c.lnaCap);
            fe.setTotal(c.totalDb, c.ampOn, c.lnaCap);
            r.changes++;
            if (c.reason == AgcReason::Reverted) r.reverted = true;
            r.lastChangeStep = r.steps;
        }
        if (fe.st.amp && fe.model.kind == GainModel::HackRf) r.ampEverOn = true;
        r.steps++;
    }
    return r;
}
static void noHook(double) {}

static SmartAgc make(const GainModel& m, const AgcProfile& p, double t0, int startTotal, bool amp = false, bool allow = false) {
    SmartAgc a;
    a.configure(m, p);
    a.setAmpAllowed(allow);
    a.begin(t0, startTotal, amp);
    return a;
}

static void testModels() {
    const GainModel h = hackrf();
    CHECK(h.kind == GainModel::HackRf && h.ampSwitch && h.ampDb == 14, "HackRF model");
    for (int t = 0; t <= 102; t += 2) {
        const GainSetting g = h.split(t, false, 40);
        CHECK(g.total() == t && !g.amp && g.lna % 8 == 0 && g.lna <= 40 && g.vga >= 0 && g.vga <= 62 && g.vga % 2 == 0, "split %d -> %d/%d", t, g.lna, g.vga);
        CHECK(t < 16 || g.lna >= 8 || t < 8, "LNA first at %d", t);
        // LNA before VGA: with room to spare the LNA is full before the VGA exceeds 8 dB
        if (t >= 48) CHECK(g.lna == 40 && g.vga == t - 40, "high totals: LNA 40 (%d)", t);
        const GainSetting c = h.split(t, false, 16);
        CHECK(c.lna <= 16, "LNA cap 16 at %d", t);
        if (t <= 16 + 62) CHECK(c.total() == t, "capped split keeps the total %d", t);
    }
    CHECK(h.split(70, true, 40).amp && h.split(70, true, 40).total() == 70, "amp split keeps the total");
    CHECK(h.maxTotal(false, false, 40) == 102 && h.maxTotal(true, false, 40) == 116 && h.maxTotal(false, false, 16) == 78, "HackRF maxima");
    // RTL-SDR: the R820T table
    DeviceInfo r; r.kind = DeviceInfo::Native; r.board = "rtlsdr"; r.gainMinDb = 0; r.gainMaxDb = 49.6;
    const GainModel rm = gainModelFor(r);
    CHECK(rm.kind == GainModel::Table && rm.table.front() == 0 && rm.table.back() == 50 && rm.table.size() >= 25, "RTL table (%zu entries)", rm.table.size());
    CHECK(rm.snap(30, 0, 50) == 30 && rm.snap(31, 0, 50) == 30 && rm.snap(26, 0, 50) == 25 && rm.snap(100, 0, 50) == 50, "RTL snap");
    for (int i = 0; i < (int)rm.table.size(); i++) CHECK(i == 0 || rm.table[i] > rm.table[i - 1], "RTL table ascending");
    CHECK(rm.nextToward(20, +2.0, 0, 50) > 20 && rm.nextToward(20, -2.0, 0, 50) < 20, "RTL moves at least one entry");
    CHECK(!rm.ampSwitch, "RTL has no amp");
    // AirspyHF+: 6 dB steps, the preamp is the top 6 dB of the slider
    DeviceInfo hf; hf.kind = DeviceInfo::Native; hf.board = "airspyhf"; hf.gainMinDb = 0; hf.gainMaxDb = 48;
    const GainModel hm = gainModelFor(hf);
    CHECK(hm.ampSwitch && hm.ampIsTopOfScale && hm.stepDb == 6, "HF+ model");
    CHECK(hm.maxTotal(false, false, 40) == 42 && hm.maxTotal(false, true, 40) == 48 && hm.maxTotal(true, false, 40) == 48, "HF+ keeps below the preamp unless allowed");
    CHECK(hm.snap(20, 0, 48) == 18 && hm.snap(22, 0, 48) == 24, "HF+ 6 dB grid");
    // a plain range: Airspy, SDRplay, Pluto, Lime ...
    const GainModel pm = generic("pluto", 0, 73);
    CHECK(pm.kind == GainModel::Range && !pm.ampSwitch && pm.maxTotal(false, false, 40) == 73 && pm.snap(80, 0, 73) == 73, "Pluto range");
    const GainModel am = generic("airspy", 0, 21);
    CHECK(am.maxTotal(false, false, 40) == 21 && am.nextToward(10, 3.2, 0, 21) == 13, "Airspy range");
}

static void testProfiles() {
    const AgcProfile dvb = agcProfileFor(0, 8e6, 10e6, 8), nb = agcProfileFor(27, 3e3, 2e6, 8), ads = agcProfileFor(13, 1e6, 2e6, 8), fm = agcProfileFor(7, 150e3, 2e6, 8);
    CHECK(dvb.cls == AgcClass::Wideband && nb.cls == AgcClass::Narrow && ads.cls == AgcClass::Pulsed && fm.cls == AgcClass::Strong, "classes");
    CHECK(dvb.targetRms > nb.targetRms && nb.marginGoal > dvb.marginGoal, "wideband wants a hotter ADC, narrow a bigger noise margin");
    CHECK(fm.lnaCap < dvb.lnaCap, "FM protects the LNA");
    CHECK(agcProfileFor(0, 8e6, 10e6, 12).targetRms < dvb.targetRms, "a deeper ADC keeps more headroom");
}

static void testMeasure() {
    FrontEnd fe; fe.model = hackrf(); fe.setTotal(40);
    fe.chanBw = 3e3; fe.rateHz = 2e6; fe.sigDb = -70; fe.noiseDb = -85; fe.blockers = {{300e3, -50}};
    fe.jitterDb = 0;
    AgcMeasure m = fe.measure();
    CHECK(m.haveSpec && m.haveFloor, "spec measured");
    // the blocker is 20 dB below full scale at the ADC (-50 + 40 + 0 spread): the out-of-band power must see it
    CHECK(std::fabs(m.oobDbfs - (-10.0)) < 3.0, "out-of-band power %.1f dBFS, expected about -10", m.oobDbfs);
    CHECK(m.blockerBinDb > m.oobDbfs - 3 && m.blockerBinDb < m.oobDbfs + 1, "blocker bin %.1f", m.blockerBinDb);
    CHECK(m.dcSpurDb > 10, "DC spur found (%.1f dB)", m.dcSpurDb);
    // the noise margin follows the gain 1:1
    FrontEnd lo = fe, hi = fe; lo.blockers.clear(); hi.blockers.clear(); lo.setTotal(50); hi.setTotal(70);
    const double dm = hi.measure().floorMarginDb - lo.measure().floorMarginDb;
    CHECK(std::fabs(dm - 20) < 1.5, "margin follows gain: %.1f for 20 dB", dm);
    // a capture hardly wider than the channel gives no floor
    FrontEnd w = lo; w.chanBw = 1.99e6;
    CHECK(!w.measure().haveFloor, "no floor when the channel fills the capture");
}

static void testConverge() {
    const GainModel hm = hackrf();
    const AgcProfile prof = agcProfileFor(0, 8e6, 10e6, 8);
    for (int start : {10, 40, 70, 100}) {
        FrontEnd fe; fe.model = hm; fe.sigDb = -60; fe.noiseDb = -85;
        fe.setTotal(start);
        SmartAgc a = make(hm, prof, 0, start);
        const Run r = drive(a, fe, 0, 20, false, noHook);
        const double rms = fe.outPowerDb();
        CHECK(rms > prof.targetRms - prof.window - 0.5 && rms < prof.targetRms + prof.window + 0.5, "start %d: settled at %.1f dBFS", start, rms);
        CHECK(r.changes <= 6, "start %d: %d changes", start, r.changes);
        CHECK(r.lastChangeStep < 12 * 20, "start %d: converged after %.1f s", start, r.lastChangeStep * 0.05);
        CHECK(!fe.st.amp, "start %d: amp stayed off", start);
    }
}

static void testNoHunting() {
    const GainModel hm = hackrf();
    for (int cls = 0; cls < 3; cls++) {
        const AgcProfile prof = cls == 0 ? agcProfileFor(0, 8e6, 10e6, 8) : cls == 1 ? agcProfileFor(27, 3e3, 2e6, 8) : agcProfileFor(7, 150e3, 2e6, 8);
        FrontEnd fe; fe.model = hm; fe.sigDb = cls == 0 ? -60 : -85; fe.noiseDb = -90; fe.chanBw = cls == 0 ? 8e6 : 150e3; fe.jitterDb = 0.8;
        fe.setTotal(30);
        SmartAgc a = make(hm, prof, 0, 30);
        drive(a, fe, 0, 30, false, noHook);
        const Run r = drive(a, fe, 30, 300, false, noHook);   // stationary input with measurement scatter
        CHECK(r.changes <= 1, "class %d: %d changes in 300 s of stationary input", cls, r.changes);
    }
    // a 6 dB-step radio (HF+) with a signal that falls between two steps
    DeviceInfo hf; hf.kind = DeviceInfo::Native; hf.board = "airspyhf"; hf.gainMinDb = 0; hf.gainMaxDb = 48;
    const GainModel m = gainModelFor(hf, 16);
    for (double sig : {-62.0, -58.0, -55.0, -50.0}) {
        FrontEnd fe; fe.model = m; fe.bits = 16; fe.sigDb = sig; fe.noiseDb = -100; fe.jitterDb = 0.8; fe.setTotal(18);
        SmartAgc a = make(m, agcProfileFor(0, 8e6, 10e6, 16), 0, 18);
        drive(a, fe, 0, 40, false, noHook);
        const Run r = drive(a, fe, 40, 300, false, noHook);
        // (level-compensated since 3.8: the least attenuation that does not clip; a gain that clipped is tried again after 5 minutes: one probe and its cut)
        CHECK(r.changes <= 2, "HF+ signal %.0f: %d changes in 300 s", sig, r.changes);
    }
    // an RTL-SDR (unequal table steps)
    DeviceInfo rt; rt.kind = DeviceInfo::Native; rt.board = "rtlsdr"; rt.gainMaxDb = 49.6;
    const GainModel rtm = gainModelFor(rt);
    for (double sig : {-70.0, -64.0, -58.0}) {
        FrontEnd fe; fe.model = rtm; fe.sigDb = sig; fe.noiseDb = -90; fe.jitterDb = 0.8; fe.setTotal(20);
        SmartAgc a = make(rtm, agcProfileFor(0, 2e6, 2.4e6, 8), 0, 20);
        drive(a, fe, 0, 40, false, noHook);
        std::set<int> seen;
        const Run r = drive(a, fe, 40, 300, false, [&](double) { seen.insert(fe.gain()); });
        CHECK(r.changes <= 1, "RTL signal %.0f: %d changes in 300 s", sig, r.changes);
        for (int g : seen) CHECK(std::find(rtm.table.begin(), rtm.table.end(), g) != rtm.table.end(), "RTL gain %d is in the table", g);
    }
}

static void testBurst() {
    const GainModel hm = hackrf();
    const AgcProfile prof = agcProfileFor(0, 8e6, 10e6, 8);
    FrontEnd fe; fe.model = hm; fe.sigDb = -60; fe.noiseDb = -85; fe.jitterDb = 0.3;
    fe.setTotal(50);
    SmartAgc a = make(hm, prof, 0, 50);
    drive(a, fe, 0, 15, false, noHook);
    const int before = fe.gain();
    int lowest = before;
    double clippedFor = 0;
    // a strong burst for 1.5 s: 40 dB more signal
    drive(a, fe, 15, 30, false, [&](double t) {
        fe.sigDb = (t >= 20 && t < 21.5) ? -20 : -60;
        lowest = std::min(lowest, fe.gain());
        if (fe.stats().clipFraction > 0.002) clippedFor += 0.05;
    });
    CHECK(lowest <= before - 6, "gain dropped on the burst (%d -> %d)", before, lowest);
    CHECK(clippedFor < 0.8, "the burst clipped for %.2f s", clippedFor);
    const Run r = drive(a, fe, 45, 45, false, noHook);
    CHECK(fe.gain() >= before - 4, "recovered after the burst: %d (was %d, %d changes)", fe.gain(), before, r.changes);
    // repeated bursts do not leave the gain low for ever
    for (int k = 0; k < 4; k++) {
        drive(a, fe, 100 + k * 40, 40, false, [&](double t) { fe.sigDb = (t - (100 + k * 40) >= 5 && t - (100 + k * 40) < 5.5) ? -25 : -60; });
    }
    drive(a, fe, 300, 60, false, noHook);
    CHECK(fe.gain() >= before - 6, "after repeated bursts the gain is back (%d, was %d)", fe.gain(), before);
}

static void testAmp() {
    const GainModel hm = hackrf();
    const AgcProfile prof = agcProfileFor(27, 3e3, 2e6, 8);
    // a very weak signal asks for all the gain there is. Not allowed: the amp stays off however long it runs and whatever happens.
    for (int scene = 0; scene < 6; scene++) {
        FrontEnd fe; fe.model = hm; fe.chanBw = 3e3; fe.rateHz = 2e6; fe.noiseDb = -110 + scene * 5; fe.sigDb = -125 + scene * 12; fe.jitterDb = 0.5;
        if (scene == 4) fe.blockers = {{600e3, -60}};
        fe.setTotal(40);
        SmartAgc a = make(hm, prof, 0, 40, false, false);
        const Run r = drive(a, fe, 0, 120, true, [&](double t) { if (scene == 5 && t > 40 && t < 42) fe.sigDb = -40; });
        CHECK(!r.ampEverOn, "scene %d: the amp was never switched on without permission", scene);
        CHECK(fe.gain() <= 102, "scene %d: gain %d within the amp-less range", scene, fe.gain());
    }
    // the user's own amp stays as the user set it, on or off, when the AGC may not touch it
    {
        FrontEnd fe; fe.model = hm; fe.chanBw = 3e3; fe.rateHz = 2e6; fe.noiseDb = -100; fe.sigDb = -110; fe.setTotal(14 + 40, true);
        SmartAgc a = make(hm, prof, 0, 54, true, false);
        bool offSeen = false;
        drive(a, fe, 0, 60, false, [&](double t) { fe.sigDb = (t > 20 && t < 22) ? -30 : -110; if (!fe.st.amp) offSeen = true; });
        CHECK(!offSeen && fe.st.amp, "the user's amp was left on even through an overload");
    }
    // allowed: it can come on for a weak signal ...
    {
        FrontEnd fe; fe.model = hm; fe.chanBw = 3e3; fe.rateHz = 2e6; fe.noiseDb = -115; fe.sigDb = -120; fe.jitterDb = 0.3; fe.setTotal(40);
        SmartAgc a = make(hm, prof, 0, 40, false, true);
        const Run r = drive(a, fe, 0, 120, false, noHook);
        CHECK(r.ampEverOn || fe.gain() < 70, "allowed: the amp comes on when a lot of gain is wanted (gain %d)", fe.gain());
        // ... and goes off again, first, on an overload
        if (fe.st.amp) {
            bool offAtOverload = false;
            drive(a, fe, 120, 20, false, [&](double t) { fe.sigDb = (t > 125 && t < 127) ? -50 : -120; if (t > 126 && !fe.st.amp) offAtOverload = true; });
            CHECK(offAtOverload, "allowed: the amp goes off on an overload");
        }
    }
}

static void testBlocker() {
    const GainModel hm = hackrf();
    const AgcProfile prof = agcProfileFor(27, 3e3, 2e6, 8);
    auto setup = [&](FrontEnd& fe) {
        fe.model = hm; fe.chanBw = 3e3; fe.rateHz = 2e6; fe.noiseDb = -100; fe.sigDb = -100; fe.jitterDb = 0.3;
        fe.blockers = {{700e3, -48}};   // a strong neighbour far outside the channel
    };
    FrontEnd fe; setup(fe); fe.setTotal(60);
    SmartAgc a = make(hm, prof, 0, 60);
    drive(a, fe, 0, 60, false, noHook);
    const AgcMeasure m = fe.measure();
    CHECK(m.oobDbfs <= prof.oobCeil + 2.5, "the blocker is kept under the ceiling: %.1f dBFS (ceiling %.0f)", m.oobDbfs, prof.oobCeil);
    CHECK(a.lnaCap() < 40, "the LNA was limited because of the neighbour (cap %d)", a.lnaCap());
    CHECK(fe.desenseDb() <= 12, "the LNA is not driven hard by the blocker (%.1f dB desense)", fe.desenseDb());
    // without the neighbour the cap is back
    fe.blockers.clear();
    drive(a, fe, 60, 90, false, noHook);
    CHECK(a.lnaCap() == 40, "the LNA limit lifts when the neighbour is gone (%d)", a.lnaCap());
    // what the LNA hands on stays under the ceiling (a blocker amplified by the LNA in full is what desensitises a real front end)
    FrontEnd fe2; setup(fe2); fe2.setTotal(60);
    SmartAgc c = make(hm, prof, 0, 60);
    drive(c, fe2, 0, 60, true, noHook);
    CHECK(fe2.lnaOutBlockerDb() <= prof.lnaOutCeil + 3, "the blocker at the end of the LNA is %.1f dB (ceiling %.0f)", fe2.lnaOutBlockerDb(), prof.lnaOutCeil);
    CHECK(fe2.snrDb() > 0, "the signal is still decodable (%.1f dB SNR)", fe2.snrDb());
}

static void testWeakNarrow() {
    // a narrow weak-signal mode wants the noise above the ADC's quantisation noise: the gain rises until the margin goal is met
    const GainModel hm = hackrf();
    const AgcProfile prof = agcProfileFor(27, 3e3, 2e6, 8);
    FrontEnd fe; fe.model = hm; fe.chanBw = 3e3; fe.rateHz = 2e6; fe.noiseDb = -105; fe.sigDb = -115; fe.jitterDb = 0.3; fe.setTotal(10);
    SmartAgc a = make(hm, prof, 0, 10);
    drive(a, fe, 0, 90, false, noHook);
    const AgcMeasure m = fe.measure();
    CHECK(m.floorMarginDb >= prof.marginGoal - 3, "noise margin %.1f dB (goal %.0f)", m.floorMarginDb, prof.marginGoal);
    CHECK(fe.outPowerDb() < prof.targetRms + prof.window + 1, "and the ADC is not hot (%.1f dBFS)", fe.outPowerDb());
}

static void testLockFreeze() {
    const GainModel hm = hackrf();
    const AgcProfile prof = agcProfileFor(0, 8e6, 10e6, 8);
    FrontEnd fe; fe.model = hm; fe.sigDb = -60; fe.noiseDb = -85; fe.jitterDb = 0.1; fe.setTotal(44);
    SmartAgc a = make(hm, prof, 0, 44);
    drive(a, fe, 0, 15, false, noHook);
    // locked on a good signal; the level drifts just past the dead band: no change while it is locked ...
    fe.sigDb += prof.window + 1.5 - (prof.targetRms - fe.outPowerDb()) * 0 ;
    const int g0 = fe.gain();
    int moved = 0;
    for (double t = 15; t < 40; t += 0.05) {
        AgcCommand c; AgcQuality q; q.locked = true; q.snrValid = true; q.snrDb = 20;
        if (a.update(t, fe.measure(), q, c)) { fe.setTotal(c.totalDb, c.ampOn, c.lnaCap); moved++; }
    }
    CHECK(moved == 0 && fe.gain() == g0, "gain frozen while locked (%d changes)", moved);
    // ... but an overload still cuts it
    fe.sigDb = -25;
    for (double t = 40; t < 44; t += 0.05) {
        AgcCommand c; AgcQuality q; q.locked = true; q.snrValid = true; q.snrDb = 20;
        if (a.update(t, fe.measure(), q, c)) { fe.setTotal(c.totalDb, c.ampOn, c.lnaCap); moved++; }
    }
    CHECK(moved > 0 && fe.gain() < g0, "an overload still cuts the gain while locked");
    // locked with a poor SNR does not freeze it
    FrontEnd f2; f2.model = hm; f2.sigDb = -80; f2.noiseDb = -85; f2.jitterDb = 0.1; f2.setTotal(30);
    SmartAgc b = make(hm, prof, 0, 30);
    int mv = 0;
    for (double t = 0; t < 30; t += 0.05) {
        AgcCommand c; AgcQuality q; q.locked = true; q.snrValid = true; q.snrDb = 3;
        if (b.update(t, f2.measure(), q, c)) { f2.setTotal(c.totalDb, c.ampOn, c.lnaCap); mv++; }
    }
    CHECK(mv > 0, "a lock with a poor SNR does not freeze the gain");
}

static void testRetune() {
    const GainModel hm = hackrf();
    const AgcProfile prof = agcProfileFor(0, 8e6, 10e6, 8);
    FrontEnd fe; fe.model = hm; fe.sigDb = -60; fe.noiseDb = -85; fe.setTotal(44);
    SmartAgc a = make(hm, prof, 0, 44);
    drive(a, fe, 0, 20, false, noHook);
    // retune to a channel 25 dB stronger: begin() again, acquisition is quick
    fe.sigDb = -35;
    a.begin(20, fe.gain(), fe.st.amp);
    const Run r = drive(a, fe, 20, 10, false, noHook);
    const double rms = fe.outPowerDb();
    CHECK(rms > prof.targetRms - prof.window - 0.5 && rms < prof.targetRms + prof.window + 0.5, "after a retune: %.1f dBFS", rms);
    CHECK(r.lastChangeStep < 8 * 20, "converged within 8 s of the retune (%.1f s)", r.lastChangeStep * 0.05);
}

static void testQualityRevert() {
    // the decoder is locked with a poor SNR and the level is low, so the AGC may raise the gain; above a knee the decoder gets worse without the
    // levels showing it: the step that crossed the knee is undone and the gain stays below it
    const GainModel hm = hackrf();
    const AgcProfile prof = agcProfileFor(0, 8e6, 10e6, 8);
    FrontEnd fe; fe.model = hm; fe.sigDb = -84; fe.noiseDb = -85; fe.jitterDb = 0.05; fe.lockAt = -20; fe.kneeDb = 48; fe.setTotal(40);
    SmartAgc a = make(hm, prof, 0, 40);
    const Run r = drive(a, fe, 0, 60, true, noHook);
    CHECK(r.reverted, "an upward step that made the decoder worse was undone");
    CHECK(fe.gain() <= 52, "and the gain stayed near the knee (%d)", fe.gain());
}

static void testSweepAmp() {
    // the gain helper keeps the amp as the user set it, with the same totals
    GainSweep free_; free_.start(0, 0, 0, 0);
    GainSweep off; off.start(0, 0, 0, 1);
    GainSweep on; on.start(0, 0, 0, 2);
    CHECK(free_.entries().size() == off.entries().size() && off.entries().size() == on.entries().size(), "same number of combinations");
    bool freeHasAmp = false;
    for (size_t i = 0; i < off.entries().size(); i++) {
        const auto& f = free_.entries()[i].g; const auto& o = off.entries()[i].g; const auto& n = on.entries()[i].g;
        freeHasAmp |= f.amp;
        CHECK(!o.amp && n.amp, "entry %zu keeps the amp off / on", i);
        CHECK(o.total() == f.total() || o.vga == 62 || o.vga == 0, "entry %zu keeps its total (%d vs %d)", i, o.total(), f.total());
    }
    CHECK(freeHasAmp, "the legacy sweep still tries the amp");
}

// AirspyHF+ (Discord report, 3.8): the firmware keeps the output level itself (the preamp is compensated, the attenuator is the gain
// distribution), so the dBFS level hardly follows the slider. Simulated here by moving the scene against the gain: what reaches the ADC
// stays at `outDb` whatever the gain; the noise figure still improves with less attenuation (the simulator's LNA term).
static void testLevelCompensated() {
    DeviceInfo hf; hf.kind = DeviceInfo::Native; hf.board = "airspyhf"; hf.gainMinDb = 0; hf.gainMaxDb = 48;
    const GainModel m = gainModelFor(hf, 16);
    CHECK(m.levelCompensated, "HF+ model is level-compensated");
    CHECK(!hackrf().levelCompensated && !generic("airspy", 0, 21).levelCompensated, "other radios are not");
    for (double outDb : {-95.0, -80.0, -65.0, -50.0, -35.0, -20.0}) {
        for (int start : {0, 18, 30, 42, 48}) {
            for (bool allow : {false, true}) {
                FrontEnd fe; fe.model = m; fe.bits = 16; fe.jitterDb = 0.8; fe.noiseDb = -100; fe.setTotal(start);
                const auto hook = [&](double) { fe.sigDb = outDb - fe.gain(); fe.noiseDb = outDb - 20 - fe.gain(); };
                SmartAgc a = make(m, agcProfileFor(27, 3e3, 768e3, 16), 0, start, false, allow);
                int downs = 0, last = start;
                for (double t = 0; t < 60; t += 0.05) {
                    hook(t);
                    AgcCommand c;
                    if (a.update(t, fe.measure(), AgcQuality{}, c)) { if (c.totalDb < last) downs++; last = c.totalDb; fe.setTotal(c.totalDb, c.ampOn, c.lnaCap); }
                }
                const int want = allow ? 48 : 42;
                // start 48 without permission: the preamp range is left once (down to 42), never further
                CHECK(fe.gain() == want, "HF+ out %.0f dBFS start %d allow %d: ended at %d (want %d)", outDb, start, allow, fe.gain(), want);
                CHECK(downs <= (start > want ? 1 : 0), "HF+ out %.0f dBFS start %d allow %d: %d steps down on a level the gain does not move", outDb, start, allow, downs);
            }
        }
    }
    // a clipping front end above 30 dB (strong broadcast nearby): it comes down and stays at or below 30 without hunting
    {
        FrontEnd fe; fe.model = m; fe.bits = 16; fe.jitterDb = 0.5; fe.setTotal(18);
        SmartAgc a = make(m, agcProfileFor(27, 3e3, 768e3, 16), 0, 18);
        int changes = 0, top = 0;
        for (double t = 0; t < 240; t += 0.05) {
            const double outDb = fe.gain() > 30 ? 0.0 : -40.0;   // over 30 dB the ADC is driven into clipping
            fe.sigDb = outDb - fe.gain(); fe.noiseDb = outDb - 20 - fe.gain();
            AgcCommand c;
            if (a.update(t, fe.measure(), AgcQuality{}, c)) { changes++; fe.setTotal(c.totalDb, c.ampOn, c.lnaCap); }
            if (t > 120) top = std::max(top, fe.gain());
        }
        CHECK(top <= 30 && fe.gain() >= 24, "HF+ clipping above 30 dB: held at %d (max %d after 120 s)", fe.gain(), top);
        CHECK(changes <= 12, "HF+ clipping above 30 dB: %d changes in 240 s", changes);
    }
    // the ADC status: a quiet HF+ level is not "low"; clipping and silence still are what they are
    setAdcBits(16);
    setAdcLevelCompensated(true);
    CHECK(classifyAdc(-75, 0.01, 0) == AdcStatus::Good && classifyAdc(-48, 0.02, 0) == AdcStatus::Good, "compensated: a low level is good");
    CHECK(classifyAdc(-30, 1.0, 0.01) == AdcStatus::Overload && classifyAdc(-8, 0.9, 0) == AdcStatus::High && classifyAdc(-130, 0, 0) == AdcStatus::NoSignal, "compensated: overload/high/silence");
    setAdcLevelCompensated(false);
    CHECK(classifyAdc(-48, 0.02, 0) == AdcStatus::Low && classifyAdc(-75, 0.01, 0) == AdcStatus::NoSignal, "uncompensated 16-bit: unchanged");
    setAdcBits(8);
}

int main() {
    testModels();
    testProfiles();
    testMeasure();
    testConverge();
    testNoHunting();
    testBurst();
    testAmp();
    testBlocker();
    testWeakNarrow();
    testLockFreeze();
    testRetune();
    testQualityRevert();
    testSweepAmp();
    testLevelCompensated();
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("auto-gain: all passed\n");
    return 0;
}
