// ADS-B automatic gain on message statistics (core/include/dect2/source/pulse_gain.h; docs/AUTOGAIN.md): the loop climbs the good-message rate and
// stops at a share of loud messages, instead of servoing the headroom of the samples. Here a simulated sky (aircraft at all levels from -105 to -45 dBm,
// each sending Mode S bursts at a Poisson rate) is received through the simulated front end of tests/source/autogain_sim.h: a message is decoded with a
// probability that follows its SNR over the radio's noise and falls once it overloads the ADC; "loud" is a decoded burst a peak within 3 dB of full scale. The loop must end
// near the best fixed gain (found by sweeping every gain, in expected messages per second), from a start that is too low and from one that is far too hot,
// with weak-only traffic, with strong aircraft close by, and with a strong off-channel carrier (the radio's noise rises, the near aircraft vanish).
#include "../source/autogain_sim.h"
#include <cstdio>
#include <cstdlib>
#include <random>

using namespace dect2;
using namespace dect2::ag;
using namespace agsim;
static int fails = 0;
static bool verbose = false;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// ---- the PulseJudge on its own
static void testJudge() {
    PulseJudge j;
    CHECK(!j.ready() && !j.tooLoud() && j.rate() == 0, "empty");
    j.restart(10.0, 1000, 50);
    j.update(11.0, 1030, 52);
    CHECK(!j.ready(), "1 s is too short");
    j.update(14.5, 1100, 53);
    CHECK(j.ready() && std::fabs(j.rate() - 100.0 / 4.5) < 1e-9 && std::fabs(j.loudShare() - 0.03) < 1e-9, "rate %.2f share %.3f", j.rate(), j.loudShare());
    CHECK(!j.tooLoud(), "3 percent is fine");
    CHECK(std::fabs(j.objective() - 10 * std::log10(100.0 / 4.5 + 0.2)) < 1e-6, "no dock at 3 percent");
    j.update(15.0, 1130, 80);   // 130 messages, 30 loud
    CHECK(j.loudShare() > 0.15 && j.tooLoud(), "loud share %.2f", j.loudShare());
    PulseJudge k; k.restart(0, 0, 0); k.update(5, 100, 0);
    PulseJudge l; l.restart(0, 0, 0); l.update(5, 100, 20);
    CHECK(k.objective() - l.objective() > 8 && k.objective() - l.objective() < 14, "20 percent loud costs %.1f dB", k.objective() - l.objective());
    // few messages: wait for the long window, not the short one
    PulseJudge q; q.restart(0, 0, 0); q.update(6, 3, 0);
    CHECK(!q.ready(), "3 messages in 6 s is not enough"); q.update(21, 9, 0); CHECK(q.ready(), "a quiet sky is judged after 20 s");
    // a reset receiver (the counters go back to zero) restarts the window
    PulseJudge r; r.restart(0, 5000, 100); r.update(3, 5100, 100); r.update(4, 10, 0);
    CHECK(r.msgs() == 0 && r.secs() == 0, "counters went back");
}

// ---- the sky
struct Sky {
    int n = 60;
    double loDbm = -105, hiDbm = -45;
    double msgPerAc = 4.0;       // decodable-length messages per aircraft per second
    double ac(int i) const { return loDbm + (hiDbm - loDbm) * std::pow((double)i / std::max(1, n - 1), 0.7); }   // more weak than strong ones
};
struct Radio {
    SimRadio sim;
    Sky sky;
    explicit Radio(const RadioProfile& p, double total) : sim(p, total) {}
    double noiseDbfs() const {   // noise in the 2 MHz of the receiver, at the output
        const double g = sim.outGain();
        const double n = SimRadio::lin(sim.noiseDbmHz() + 10 * std::log10(2e6) + g) + SimRadio::lin(-(6.02 * sim.prof.adcBits + 1.76));
        return SimRadio::dbv(n);
    }
    // expected (decoded, loud) messages per second at the present gain
    void expect(double& ok, double& loud) const {
        ok = loud = 0;
        const double g = sim.outGain(), nd = noiseDbfs(), comp = sim.compression();
        for (int i = 0; i < sky.n; i++) {
            const double lvl = sky.ac(i) + g;   // dBFS of the pulse (the compression is in outGain already)
            const double snr = lvl - nd;
            double p = 1.0 / (1.0 + std::exp(-(snr - 9.0) / 1.2));
            if (lvl > -1.0) p *= std::exp(-(lvl + 1.0) / 2.5);   // the ADC clips the pulse: the CRC fails
            (void)comp;
            ok += sky.msgPerAc * p;
            if (lvl > kAdsbLoudDbfs + 4.0) loud += sky.msgPerAc * p;
        }
    }
    double strongestDbfs() const { return sky.hiDbm + sim.outGain(); }
};

struct Result { double gain = 0, rate = 0, loudShare = 0, bestRate = 0, bestGain = 0, loudAtBest = 0; int changes = 0; std::string status; };

static Result runSky(const char* radioId, Sky sky, double startDb, bool v2, double sec, const std::vector<Tone>& blockers = {}, double noiseRise = 0, bool noHold = false) {
    const RadioProfile* rp = radioProfileById(radioId);
    if (!rp) { printf("no profile %s\n", radioId); exit(1); }
    Radio r(*rp, startDb);
    r.sky = sky; r.sim.rateHz = 2.4e6; r.sim.chanBw = 2e6; r.sim.sigDbm = sky.hiDbm; r.sim.sigCrestDb = 0; r.sim.blockers = blockers; r.sim.antNoiseDb = noiseRise; r.sim.decoder = false;
    const ModeTarget tg = modeTargetFor(13, 2e6, 2.4e6, 1090e6);
    CHECK(tg.kind == ModeKind::Pulsed, "target kind");
    AutoGainController c;
    AgSettings s; s.mode = AgMode::Auto; s.v2 = v2;
    c.configure(*rp, tg, s);
    double now = 0;
    c.start(now, r.sim.live.totalDb, false, &r.sim.live);
    std::mt19937 rng(4242);
    uint64_t msgs = 0, loud = 0;
    Result o;
    double sumRate = 0, sumLoud = 0, sumOk = 0; int nLast = 0;
    std::vector<float> spec;
    const double dt = 0.05;
    for (; now < sec; now += dt) {
        r.sim.advance(now);
        double okR, loudR; r.expect(okR, loudR);
        const uint64_t dm = std::poisson_distribution<int>(std::max(1e-9, okR * dt))(rng);
        uint64_t dl = 0; if (dm > 0 && okR > 0) { std::binomial_distribution<int> b((int)dm, std::min(1.0, loudR / okR)); dl = (uint64_t)b(rng); }
        msgs += dm; loud += dl;
        AgInput in;
        in.now = now;
        in.adc = r.sim.adc();
        // the 2 s peak-hold of the strongest pulse (the sim's adc() has none): the aircraft's pulse, the crest of the noise is under it
        const double peakDbfs = r.strongestDbfs();
        in.adc.holdHeadroomDb = noHold ? 120.0 : std::max(0.0, -peakDbfs);
        spec = r.sim.spectrum();
        in.spectrum = &spec; in.rateHz = r.sim.rateHz; in.chanBwHz = r.sim.chanBw; in.offsetHz = 0;
        in.q.pulseValid = true; in.q.pulseMsgs = msgs; in.q.pulseLoud = loud; in.q.locked = dm > 0;
        AgCommand out;
        if (c.tick(in, out)) r.sim.apply(out.plan, now);
        if (now >= sec - 30) { sumRate += okR; sumLoud += loudR; sumOk += 1; nLast++; }
    }
    o.gain = r.sim.live.totalDb; o.rate = sumRate / std::max(1, nLast); o.loudShare = sumRate > 0 ? sumLoud / sumRate : 0; o.changes = c.changes(); o.status = c.status();
    // the best fixed gain, by expected rate (at the loud share it brings)
    const double lo = rp->minTotal(false), hi = rp->maxTotal(false);
    for (double g = lo; g <= hi; g += 1.0) {
        r.sim.live = planStages(*rp, g, Policy::Sensitivity, 0xffffffffu, {}, false);
        // the guards stay as they are by design: no gain whose samples clip or whose strongest pulse leaves less than the headroom floor
        const BlockStats bs = r.sim.adc();
        if (bs.clipFraction > tg.clipLimit || (!noHold && -r.strongestDbfs() < tg.minHeadroomDb)) continue;
        double okR, loudR; r.expect(okR, loudR);
        if (okR > o.bestRate) { o.bestRate = okR; o.bestGain = r.sim.live.totalDb; o.loudAtBest = okR > 0 ? loudR / okR : 0; }
    }
    return o;
}

// ---- the loud rule on its own: plenty of headroom in the samples, the counters say a quarter of the messages are loud
static double loudRun(double loudFrac, std::string* why) {
    const RadioProfile* rp = radioProfileById("hackrf");
    Radio r(*rp, 50);
    r.sim.rateHz = 2.4e6; r.sim.chanBw = 2e6; r.sim.sigDbm = -100; r.sim.sigCrestDb = 0; r.sim.decoder = false;
    const ModeTarget tg = modeTargetFor(13, 2e6, 2.4e6, 1090e6);
    AutoGainController c;
    AgSettings s; s.mode = AgMode::Auto; s.v2 = true; s.quickStart = false;
    c.configure(*rp, tg, s);
    double now = 0;
    c.start(now, 50, false, &r.sim.live);
    uint64_t msgs = 0, loud = 0; double acc = 0, accL = 0;
    std::vector<float> spec;
    for (; now < 60; now += 0.05) {
        r.sim.advance(now);
        // messages follow the gain a little (a nearer aircraft is heard at any gain), loud share fixed
        acc += 100 * 0.05; accL += 100 * 0.05 * loudFrac;
        const uint64_t dm = (uint64_t)acc, dl = (uint64_t)accL; acc -= dm; accL -= dl; msgs += dm; loud += dl;
        AgInput in; in.now = now; in.adc = r.sim.adc(); in.adc.holdHeadroomDb = 120;
        spec = r.sim.spectrum(); in.spectrum = &spec; in.rateHz = r.sim.rateHz; in.chanBwHz = r.sim.chanBw;
        in.q.pulseValid = true; in.q.pulseMsgs = msgs; in.q.pulseLoud = loud; in.q.locked = true;
        AgCommand out;
        if (c.tick(in, out)) { r.sim.apply(out.plan, now); if (why && out.why.find("near full scale") != std::string::npos) *why = out.why; }
    }
    return r.sim.live.totalDb;
}
static void testLoudRule() {
    std::string why;
    const double hot = loudRun(0.25, &why), calm = loudRun(0.01, nullptr);
    printf("loud rule: 25 %% loud ends at %.0f dB (%s), 1 %% loud at %.0f dB\n", hot, why.c_str(), calm);
    CHECK(!why.empty(), "the loud share lowered the gain");
    CHECK(hot <= 50 - 6, "25 percent loud: gain only %.0f dB", hot);
    CHECK(calm > hot + 4, "1 percent loud keeps a higher gain (%.0f against %.0f)", calm, hot);
}

static void show(const char* name, const Result& o) {
    printf("%-52s gain %5.1f (best %5.1f)  rate %6.1f/s of best %6.1f (%3.0f %%)  loud %4.1f %% (best %4.1f %%)  changes %d\n", name, o.gain, o.bestGain, o.rate, o.bestRate,
           100 * o.rate / std::max(1e-9, o.bestRate), 100 * o.loudShare, 100 * o.loudAtBest, o.changes);
    if (verbose) printf("    %s\n", o.status.c_str());
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-v")) verbose = true;
    testJudge();
    testLoudRule();
    struct Case { const char* name; const char* radio; Sky sky; double start; std::vector<Tone> blockers; double noiseRise; double minFrac; double maxLoud; bool noHold = false; };
    Sky far; far.n = 40; far.loDbm = -108; far.hiDbm = -80;       // only distant aircraft
    Sky mixed;                                                      // -105 .. -45 dBm
    Sky near; near.n = 50; near.loDbm = -95; near.hiDbm = -30;     // very strong aircraft close by
    Sky sparse; sparse.n = 8; sparse.loDbm = -100; sparse.hiDbm = -60; sparse.msgPerAc = 2;   // a quiet sky
    const std::vector<Case> cases = {
        {"hackrf, far aircraft only, start low", "hackrf", far, 10, {}, 0, 0.80, 0.12},
        {"hackrf, far aircraft only, start high", "hackrf", far, 90, {}, 0, 0.78, 0.12},
        {"hackrf, mixed sky, start low", "hackrf", mixed, 10, {}, 0, 0.80, 0.12},
        {"hackrf, mixed sky, start far too hot", "hackrf", mixed, 100, {}, 0, 0.80, 0.12},
        {"hackrf, strong aircraft close by, start hot", "hackrf", near, 95, {}, 0, 0.80, 0.12},
        {"hackrf, strong aircraft close by, start low", "hackrf", near, 5, {}, 0, 0.80, 0.12},
        {"hackrf, mixed sky, city noise +10 dB", "hackrf", mixed, 40, {}, 10, 0.80, 0.12},
        {"hackrf, mixed sky, strong 940 MHz carrier", "hackrf", mixed, 60, {{-20e6 / 2.4e6 * 1e5, -35}}, 0, 0.80, 0.12},
        {"airspy, mixed sky, start low", "airspy", mixed, 5, {}, 0, 0.80, 0.12},
        {"hackrf, quiet sky (8 aircraft)", "hackrf", sparse, 50, {}, 0, 0.55, 0.15},
        {"hackrf, near aircraft, no peak-hold, start hot", "hackrf", near, 100, {}, 0, 0.75, 0.12, true},
        {"hackrf, near aircraft, no peak-hold, start low", "hackrf", near, 5, {}, 0, 0.75, 0.12, true},
    };
    for (const Case& c : cases) {
        const Result o = runSky(c.radio, c.sky, c.start, true, 240, c.blockers, c.noiseRise, c.noHold);
        show(c.name, o);
        CHECK(o.rate >= c.minFrac * o.bestRate, "%s: rate %.1f is only %.0f %% of the best fixed gain's %.1f", c.name, o.rate, 100 * o.rate / std::max(1e-9, o.bestRate), o.bestRate);
        CHECK(o.loudShare <= c.maxLoud, "%s: %.0f %% loud messages at the end", c.name, 100 * o.loudShare);
    }
    // the old headroom servo on the same skies, for the record (the new loop must not be worse where the servo was right, and must beat it where the pulses fool it)
    if (verbose) for (const Case& c : cases) show((std::string("old servo: ") + c.name).c_str(), runSky(c.radio, c.sky, c.start, false, 240, c.blockers, c.noiseRise, c.noHold));
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
