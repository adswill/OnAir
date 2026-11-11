// Lightning position (network), simulated signals only: geodesy, the sample clock discipline, the sferic leading-edge timer and the MSK
// common-view timer against known truths, the TDOA solver over timing errors and station geometries (position error, ellipse coverage),
// outliers, clock groups, ambiguity folding, ionosphere hops, false matches, the packet format (authentication, replays), and the whole chain:
// IQ at five stations with different timing sources -> StationCore -> packets -> ServerCore -> strikes. ONAIR_TEST_LONG=1 runs longer sweeps.
#include "dect2/sferics/ltpos.h"
#include "dect2/sferics/ltpos_core.h"
#include "dect2/sferics/ltpos_gen.h"
#include "dect2/sferics/ltpos_rx.h"
#include "dect2/source/hw_time.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace dect2::ltpos;
using dect2::SynthConfig;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

static bool longRun() { const char* e = std::getenv("ONAIR_TEST_LONG"); return e && *e == '1'; }
static double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(p * (double)(v.size() - 1) + 0.5))];
}

// ---------------------------------------------------------------- geodesy
static void testGeodesy() {
    // Vincenty's own test line (Flinders Peak to Buninyong, on the GRS-80 / WGS-84 ellipsoid): 54 972.271 m
    const double d = geodesicM(-(37 + 57 / 60.0 + 3.72030 / 3600), 144 + 25 / 60.0 + 29.52440 / 3600, -(37 + 39 / 60.0 + 10.15610 / 3600), 143 + 55 / 60.0 + 35.38390 / 3600);
    CHECK(std::fabs(d - 54972.271) < 0.01, "Vincenty distance %.4f", d);
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> u(-1, 1);
    double worst = 0;
    for (int i = 0; i < 200; i++) {
        const double la = 60 * u(rng), lo = 180 * u(rng), az = 180 + 180 * u(rng), s = 1e3 + 4e6 * (u(rng) + 1);
        double la2, lo2, az1;
        geodesicDirect(la, lo, az, s, la2, lo2);
        const double back = geodesicM(la, lo, la2, lo2, &az1);
        worst = std::max(worst, std::fabs(back - s));
    }
    CHECK(worst < 1e-3, "direct / inverse round trip %.6f m", worst);
    PropModel pm;
    pm.hops = true;
    CHECK(propagationS(pm, 1000e3) == 1000e3 / (kC * pm.velocityFactor), "short path stays on the ground wave");
    CHECK(propagationS(pm, 2000e3) > 2000e3 / kC, "a hop path is longer than the ground");
    std::printf("geodesy: Vincenty %.4f m (54972.271), round trip %.2g m\n", d, worst);
}

// ---------------------------------------------------------------- sample clock
static void testSampleClock() {
    SampleClock c;
    std::mt19937 rng(5);
    std::normal_distribution<double> nd(0, 1);
    const double ppm = 23.7;
    const int64_t t0 = 1762800000LL * 1000000000LL;
    // GPS PPS marks: true second k is sample time k (1 + ppm); 30 ns jitter
    for (int k = 0; k < 60; k++) {
        TimeMark m{TimeSrc::GpsHw, k * (1 + ppm * 1e-6), t0 + (int64_t)k * 1000000000LL + (int64_t)std::llround(30 * nd(rng)), 30, 0};
        c.add(m);
    }
    CHECK(std::fabs(c.ppm() - ppm) < 0.01, "ppm %.4f", c.ppm());
    const double ppmFound = c.ppm();
    int64_t u; double s;
    c.stamp(59.5 * (1 + ppm * 1e-6), u, s);
    const double e = (double)(u - (t0 + 59500000000LL));
    const double s59 = s;
    CHECK(std::fabs(e) < 40 && s < 40, "stamp between marks: error %.1f ns, sigma %.1f ns", e, s);
    // an outlier is refused; three in a row restart the clock
    TimeMark bad{TimeSrc::GpsHw, 60 * (1 + ppm * 1e-6), t0 + 60000000000LL + 5000000, 30, 0};
    CHECK(!c.add(bad), "a 5 ms jump is refused");
    bad.sampleS += 1; bad.utcNs += 1000000000LL; c.add(bad);
    bad.sampleS += 1; bad.utcNs += 1000000000LL; c.add(bad);
    CHECK(c.restarts() == 1 && c.marks() == 1, "three outliers restart (restarts %llu marks %zu)", (unsigned long long)c.restarts(), c.marks());
    // one mark only: the error grows with the unknown drift (50 ppm prior)
    SampleClock one;
    one.add(TimeMark{TimeSrc::Ntp, 0, t0, 1000, 5e6});
    one.stamp(10, u, s);
    CHECK(s > 5e6 && s < 5.2e6, "one NTP mark: sigma %.0f ns", s);
    std::printf("sample clock: ppm %.4f (23.7), stamp error %.1f ns (sigma %.1f); one NTP mark 10 s later: sigma %.2f ms\n", ppmFound, e, s59, s / 1e6);
}

// ---------------------------------------------------------------- the sferic timer
static void testPulseTimer() {
    const double rate = 500e3;
    std::printf("pulse timer, 500 kHz, leading edge (half of the peak) against the true onset; the rise is 12 us, so a constant offset of about 6 us is expected:\n");
    std::vector<double> biases;
    for (double amp : {0.02, 0.05, 0.2, 0.8}) {
        SimScene sc;
        SimStation st;
        st.ppm = 12; st.noiseRms = 0.004; st.seed = 9;
        std::mt19937 rng(11);
        std::uniform_real_distribution<double> ud(0, 1);
        for (int i = 0; i < 40; i++) { SimStrike k; k.lat = 0; k.lon = 0.9; k.tS = 0.6 + i * 0.0611 + 1e-6 * ud(rng); k.ampAt100km = amp; sc.strikes.push_back(k); }
        const size_t N = (size_t)(3.2 * rate);
        std::vector<std::complex<float>> x(N);
        simRender(sc, st, rate, 0, N, x.data(), nullptr);
        PulseTimer pt;
        pt.configure(rate);
        pt.setThresholdDb(12);
        std::vector<Pulse> ps;
        pt.feed(x.data(), N, ps);
        std::vector<double> err;
        double sig = 0;
        for (const SimStrike& k : sc.strikes) {
            const double want = simSampleTime(st, simArrivalS(sc, k, st));
            double best = 1;
            const Pulse* bp = nullptr;
            for (const Pulse& p : ps) if (std::fabs(p.sampleS - want) < std::fabs(best)) { best = p.sampleS - want; bp = &p; }
            if (bp) { err.push_back(best); sig += bp->sigmaS; }
        }
        double mean = 0, rms = 0;
        for (double v : err) mean += v;
        mean /= std::max<size_t>(1, err.size());
        for (double v : err) rms += (v - mean) * (v - mean);
        rms = std::sqrt(rms / std::max<size_t>(1, err.size()));
        const double snr = 20 * std::log10(std::min(0.9, amp * 100e3 / geodesicM(0, 0, 0, 0.9)) / 0.004);
        std::printf("  peak SNR %5.1f dB: found %zu/40, bias %+.3f us, scatter %.3f us, claimed %.3f us\n", snr, err.size(), mean * 1e6, rms * 1e6, sig / std::max<size_t>(1, err.size()) * 1e6);
        CHECK(err.size() == 40, "all strikes found at amp %.2f", amp);
        biases.push_back(mean);
        if (snr > 25) CHECK(rms < 0.4e-6, "scatter %.3f us at %.0f dB", rms * 1e6, snr);
    }
    const double spread = *std::max_element(biases.begin(), biases.end()) - *std::min_element(biases.begin(), biases.end());
    CHECK(spread < 0.5e-6, "the half-peak edge is the same at every amplitude (spread %.3f us)", spread * 1e6);
    std::printf("  offset spread over a 32 dB range of amplitude: %.3f us (the same at every station, so it cancels in the differences)\n", spread * 1e6);
}

// ---------------------------------------------------------------- the MSK timer
static void testMsk() {
    const double rate = 250e3;
    std::printf("MSK common-view timer, 200 Bd at +40 kHz, 1 s blocks:\n");
    for (double ramp : {0.005, 0.02, 0.1}) {
        for (double cfo : {0.0, 1.2}) {
            SimScene sc;
            sc.ref = RefTx{3, "test", 0, 200, 46.0, 10.0};
            SimStation st;
            st.lat = 48; st.lon = 11; st.ppm = -17; st.cfoHz = cfo; st.noiseRms = 0.02; st.refAmp = ramp; st.refOffsetHz = 40e3; st.startS = 0.37;
            MskRefTimer m;
            m.configure(rate, 40e3, 200, 3, 1.0);
            MskState ms;
            std::vector<RefObs> obs;
            const size_t blk = 25000;
            std::vector<std::complex<float>> x(blk);
            for (uint64_t p = 0; p < (uint64_t)(16 * rate); p += blk) { simRender(sc, st, rate, p, blk, x.data(), &ms); m.feed(x.data(), blk, obs); }
            // truth: a boundary at true time refEpoch + delay + k T
            const double delay = propagationS(sc.pm, geodesicM(sc.ref.lat, sc.ref.lon, st.lat, st.lon));
            std::vector<double> e;
            double claimed = 0;
            for (size_t i = 2; i < obs.size(); i++) {
                const double tt = simTrueTime(st, obs[i].sampleS) - delay - sc.refEpochS;
                e.push_back(wrapPeriod(tt, 1.0 / 200));
                claimed += obs[i].sigmaS;
            }
            double mean = 0, rms = 0;
            for (double v : e) mean += v;
            mean /= std::max<size_t>(1, e.size());
            for (double v : e) rms += (v - mean) * (v - mean);
            rms = std::sqrt(rms / std::max<size_t>(1, e.size()));
            const double snr = 20 * std::log10(ramp / 0.02) + 10 * std::log10(rate / 200);
            std::printf("  SNR %4.1f dB in 200 Hz, offset %.1f Hz: %zu blocks, error %+.2f us, scatter %.2f us, claimed %.2f us, carrier found %+.2f Hz\n",
                        snr, cfo, obs.size(), mean * 1e6, rms * 1e6, claimed / std::max<size_t>(1, e.size()) * 1e6, m.freqErrHz());
            CHECK(obs.size() >= 6, "MSK locks (ramp %.3f cfo %.1f): %zu", ramp, cfo, obs.size());
            if (ramp >= 0.02) CHECK(std::fabs(mean) < 3e-6 && rms < 4e-6, "MSK timing within a few us (mean %.2f rms %.2f)", mean * 1e6, rms * 1e6);
        }
    }
}

// ---------------------------------------------------------------- the solver over geometry and timing error
struct Net { std::vector<std::pair<double, double>> st; const char* name; };
static Net network(int n, double radiusKm, double clat, double clon, const char* name) {
    Net net;
    net.name = name;
    for (int i = 0; i < n; i++) {
        double la, lo;
        geodesicDirect(clat, clon, i * 360.0 / n + 10 * (i % 2), radiusKm * 1e3 * (i % 3 == 0 ? 0.7 : 1.0), la, lo);
        net.st.push_back({la, lo});
    }
    return net;
}

struct SweepOut { double med = 0, p95 = 0, cover = 0, ell = 0; int ok = 0, n = 0; };
static SweepOut sweep(const Net& net, double sigmaNs, double strikeKm, int trials, const PropModel& pmSim, const PropModel& pmSolve, uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0, 1);
    std::uniform_real_distribution<double> ud(0, 360);
    double clat = 0, clon = 0;
    for (auto& s : net.st) { clat += s.first; clon += s.second; }
    clat /= net.st.size(); clon /= net.st.size();
    std::vector<double> errs, ells;
    int inside = 0, ok = 0;
    for (int t = 0; t < trials; t++) {
        double la, lo;
        geodesicDirect(clat, clon, ud(rng), strikeKm * 1e3 * (0.2 + 0.8 * std::fabs(nd(rng)) / 2), la, lo);
        std::vector<Obs> ob;
        for (size_t i = 0; i < net.st.size(); i++) {
            Obs o;
            o.station = (uint32_t)i + 1;
            o.lat = net.st[i].first; o.lon = net.st[i].second;
            o.tS = 0.0123 + propagationS(pmSim, geodesicM(la, lo, o.lat, o.lon)) + sigmaNs * 1e-9 * nd(rng);
            o.sigmaS = sigmaNs * 1e-9;
            ob.push_back(o);
        }
        const Fix f = solveTdoa(ob, pmSolve);
        if (!f.ok) continue;
        ok++;
        double az;
        const double e = geodesicM(la, lo, f.lat, f.lon, &az);
        errs.push_back(e);
        ells.push_back(f.semiMajorM);
        // inside the 95 % ellipse?
        const double th = (az - f.majorAzDeg) * 3.14159265358979 / 180;
        const double a = e * std::cos(th), b = e * std::sin(th);
        if ((a * a) / (f.semiMajorM * f.semiMajorM + 1e-9) + (b * b) / (f.semiMinorM * f.semiMinorM + 1e-9) <= 1.0) inside++;
    }
    SweepOut o;
    o.n = trials; o.ok = ok;
    o.med = pct(errs, 0.5); o.p95 = pct(errs, 0.95); o.ell = pct(ells, 0.5);
    o.cover = ok ? (double)inside / ok : 0;
    return o;
}

static void testSolver() {
    PropModel pm;
    const int trials = longRun() ? 400 : 60;
    std::printf("TDOA position error (median / 95 %%, km) against timing error and geometry, %d strikes each, strikes within the given range of the centre:\n", trials);
    std::printf("  %-28s %-7s %10s %10s %10s %10s %8s\n", "network", "strikes", "0.1 us", "0.3 us", "1 us", "3 us", "10 us");
    struct G { Net net; double strikeKm; };
    std::vector<G> gs = {{network(4, 150, 50, 10, "4 stations, 150 km"), 300}, {network(6, 150, 50, 10, "6 stations, 150 km"), 300},
                         {network(6, 400, 50, 10, "6 stations, 400 km"), 600}, {network(8, 800, 45, 15, "8 stations, 800 km"), 1000},
                         {network(6, 150, 50, 10, "6 stations, 150 km, far"), 1500}};
    for (const G& g : gs) {
        std::printf("  %-28s %5.0f km", g.net.name, g.strikeKm);
        for (double sg : {100.0, 300.0, 1000.0, 3000.0, 10000.0}) {
            const SweepOut o = sweep(g.net, sg, g.strikeKm, trials, pm, pm, 17);
            std::printf(" %4.2f/%5.2f", o.med / 1e3, o.p95 / 1e3);
            if (g.net.st.size() >= 6 && g.strikeKm <= 600) {
                CHECK(o.ok >= trials * 0.97, "%s %.0f ns: %d of %d solved", g.net.name, sg, o.ok, trials);
                CHECK(o.med < sg * 1e-9 * kC * 2.0, "%s %.0f ns: median %.0f m", g.net.name, sg, o.med);
            }
            if (sg == 1000.0 && g.net.st.size() >= 6) CHECK(o.cover > 0.85 && o.cover <= 1.0, "%s: the 95 %% ellipse covers %.2f", g.net.name, o.cover);
        }
        std::printf("\n");
    }
    // ellipse coverage at 1 us
    const SweepOut c6 = sweep(gs[1].net, 1000, 300, trials * 2, pm, pm, 99);
    std::printf("  95 %% ellipse coverage, 6 stations, 1 us: %.2f (median semi-major %.2f km)\n", c6.cover, c6.ell / 1e3);
    // a wrong velocity in the solver (0.1 %) is a bias that the ellipse does not show
    PropModel wrong = pm;
    wrong.velocityFactor = pm.velocityFactor * 0.999;
    const SweepOut wv = sweep(gs[2].net, 300, 600, trials, pm, wrong, 7);
    std::printf("  velocity 0.1 %% wrong, 6 stations 400 km, 0.3 us: median %.2f km, 95 %% %.2f km\n", wv.med / 1e3, wv.p95 / 1e3);
    // long baselines with ionosphere hops: the hop model in both, and the ground model in the solver on hop data
    PropModel hop = pm;
    hop.hops = true;
    const Net wide = network(6, 1500, 45, 10, "6 stations, 1500 km");
    const SweepOut h1 = sweep(wide, 1000, 2500, trials, hop, hop, 21);
    const SweepOut h2 = sweep(wide, 1000, 2500, trials, hop, pm, 21);
    std::printf("  hops: 6 stations 1500 km, strikes within 2500 km, 1 us: hop model %.2f / %.2f km, ground model %.2f / %.2f km (%d of %d pass)\n",
                h1.med / 1e3, h1.p95 / 1e3, h2.med / 1e3, h2.p95 / 1e3, h2.ok, trials);
    CHECK(h1.ok >= trials * 0.9 && h1.med < 2e3, "hop model on hop data: median %.0f m, %d ok", h1.med, h1.ok);
    CHECK(h2.ok < trials * 0.5 || h2.med > 5 * h1.med, "the ground model on hop data fails the chi-square test or is far off");
}

static void testOutliersGroupsAmbiguity() {
    PropModel pm;
    const Net net = network(6, 200, 48, 8, "");
    const double la = 48.3, lo = 9.1;
    auto obsFor = [&](double sgNs) {
        std::vector<Obs> ob;
        for (size_t i = 0; i < net.st.size(); i++) {
            Obs o;
            o.station = (uint32_t)i + 1; o.lat = net.st[i].first; o.lon = net.st[i].second;
            o.tS = 0.002 + propagationS(pm, geodesicM(la, lo, o.lat, o.lon));
            o.sigmaS = sgNs * 1e-9;
            ob.push_back(o);
        }
        return ob;
    };
    // one station 80 us off: dropped
    std::vector<Obs> ob = obsFor(500);
    ob[2].tS += 80e-6;
    Fix f = solveTdoa(ob, pm);
    CHECK(f.ok && f.dropped.size() == 1 && f.dropped[0] == 3 && geodesicM(la, lo, f.lat, f.lon) < 500, "outlier dropped (ok %d dropped %zu err %.0f m)", f.ok, f.dropped.size(), geodesicM(la, lo, f.lat, f.lon));
    // clock groups: three stations on GPS, three on a common-view clock with an unknown 3.7 ms offset
    ob = obsFor(300);
    for (size_t i = 3; i < 6; i++) { ob[i].group = 103; ob[i].tS += 3.7e-3; }
    f = solveTdoa(ob, pm);
    CHECK(f.ok && geodesicM(la, lo, f.lat, f.lon) < 500 && f.dof == 6 - 2 - 2, "two clock groups: err %.0f m dof %d", geodesicM(la, lo, f.lat, f.lon), f.dof);
    // ambiguity: a common-view station tied to UTC but one 5 ms symbol period off
    ob = obsFor(300);
    ob[4].tS += 5e-3; ob[4].ambS = 5e-3;
    f = solveTdoa(ob, pm);
    CHECK(f.ok && f.dropped.empty() && geodesicM(la, lo, f.lat, f.lon) < 500, "a whole symbol period folded (ok %d dropped %zu)", f.ok, f.dropped.size());
    // three stations: exact, flagged unchecked
    ob = obsFor(300);
    ob.resize(3);
    f = solveTdoa(ob, pm);
    CHECK(f.ok && f.unchecked && f.dof == 0, "three stations: unchecked fix (ok %d unchecked %d)", f.ok, f.unchecked);
    // two strikes mixed in one group: rejected by the chi-square test
    ob = obsFor(300);
    for (size_t i = 0; i < 2; i++) ob[i].tS = 0.002 + propagationS(pm, geodesicM(49.5, 6.0, ob[i].lat, ob[i].lon));
    ob.resize(4);
    f = solveTdoa(ob, pm);
    CHECK(!f.ok, "a mixed group of two strikes is rejected (%s)", f.why.c_str());
    std::printf("solver: outlier dropped, two clock groups, a 5 ms ambiguity folded, three-station fix flagged, a mixed group rejected\n");
}

// ---------------------------------------------------------------- matching with false alarms and close strikes
static void testMatcher() {
    PropModel pm;
    const Net net = network(6, 250, 47, 9, "");
    std::mt19937 rng(31);
    std::normal_distribution<double> nd(0, 1);
    std::uniform_real_distribution<double> ud(0, 1);
    Matcher m(MatchOpts(), pm);
    const int64_t t0 = 1762800000LL * 1000000000LL;
    struct Truth { double la, lo; int64_t t; };
    std::vector<Truth> truth;
    const int strikes = longRun() ? 600 : 150;
    double t = 0;
    for (int k = 0; k < strikes; k++) {
        t += 0.002 + 0.05 * ud(rng);   // flashes close together: down to 2 ms apart
        Truth tr{47 + 4 * (ud(rng) - 0.5), 9 + 6 * (ud(rng) - 0.5), t0 + (int64_t)(t * 1e9)};
        truth.push_back(tr);
        for (size_t i = 0; i < net.st.size(); i++) {
            if (ud(rng) < 0.1) continue;   // a station misses one in ten
            Det d;
            d.station = (uint32_t)i + 1; d.lat = net.st[i].first; d.lon = net.st[i].second;
            d.sigmaNs = 500; d.fine = true; d.widthUs = 40 + 10 * (float)ud(rng);
            d.tNs = tr.t + (int64_t)std::llround(propagationS(pm, geodesicM(tr.la, tr.lo, d.lat, d.lon)) * 1e9 + 500 * nd(rng));
            m.add(d);
        }
    }
    // false alarms: 3 per second per station, uniformly in time
    int falseN = 0;
    for (size_t i = 0; i < net.st.size(); i++)
        for (double ft = 0; ft < t; ft += -std::log(1 - ud(rng)) / 3.0) {
            Det d;
            d.station = (uint32_t)i + 1; d.lat = net.st[i].first; d.lon = net.st[i].second;
            d.sigmaNs = 500; d.fine = true; d.widthUs = 40;
            d.tNs = t0 + (int64_t)(ft * 1e9);
            m.add(d);
            falseN++;
        }
    const std::vector<Strike> out = m.process(t0 + (int64_t)((t + 10) * 1e9));
    int good = 0, wrong = 0;
    std::vector<double> e;
    for (const Strike& s : out) {
        double best = 1e18;
        for (const Truth& tr : truth) if (std::llabs(tr.t - s.timeNs) < 1000000) best = std::min(best, geodesicM(tr.la, tr.lo, s.fix.lat, s.fix.lon));
        if (best < 3000) { good++; e.push_back(best); } else wrong++;
    }
    const MatchStats& ms = m.stats();
    std::printf("matcher: %d strikes, %d false alarms, 6 stations missing 10 %%: %d located (median error %.2f km), %d wrong, %llu groups rejected, %llu unmatched\n",
                strikes, falseN, good, pct(e, 0.5) / 1e3, wrong, (unsigned long long)ms.rejected, (unsigned long long)ms.unmatched);
    CHECK(good >= strikes * 0.85, "located %d of %d", good, strikes);
    CHECK(wrong <= std::max(1, strikes / 100), "false strikes %d", wrong);
}

// ---------------------------------------------------------------- packets
static void testPackets() {
    const auto key = networkKey("correct horse battery staple");
    Packet p;
    p.type = PktType::Detections; p.station = stationIdOf("alpha"); p.seq = 7; p.sendNs = 1762800000LL * 1000000000LL;
    p.hdr.lat = 54.6872; p.hdr.lon = 25.2797; p.hdr.altM = 112; p.hdr.src = TimeSrc::CommonView; p.hdr.flags = 3; p.hdr.clockSigmaNs = 2500;
    for (int i = 0; i < 5; i++) { DetRec d; d.utcNs = p.sendNs + i * 1234567; d.sigmaNs = 300 + i; d.peakDb = 23.45f; d.amp = 0.25f; d.widthUs = 41.2f; d.riseUs = 9.8f; d.freqHz = 24990; p.dets.push_back(d); }
    std::vector<uint8_t> b = encodePacket(p, key);
    Packet q;
    CHECK(decodePacket(b.data(), b.size(), key, q) == PktErr::Ok && q.dets.size() == 5 && q.dets[3].utcNs == p.dets[3].utcNs && std::fabs(q.hdr.lat - 54.6872) < 1e-6 && q.hdr.src == TimeSrc::CommonView && std::fabs(q.dets[0].widthUs - 41.2f) < 0.06f, "round trip");
    CHECK(b.size() == 22 + 16 + 1 + 5 * 32 + 16, "compact: %zu bytes for five detections", b.size());
    CHECK(decodePacket(b.data(), b.size(), networkKey("wrong"), q) == PktErr::Auth, "wrong key refused");
    std::vector<uint8_t> t = b; t[30] ^= 1;
    CHECK(decodePacket(t.data(), t.size(), key, q) == PktErr::Auth, "a changed byte is refused");
    CHECK(decodePacket(b.data(), 40, key, q) == PktErr::Short, "short");
    std::mt19937 rng(2);
    int accepted = 0;
    for (int i = 0; i < 2000; i++) { std::vector<uint8_t> r(1 + rng() % 300); for (auto& c : r) c = (uint8_t)rng(); if (r.size() > 4) { r[0] = 'O'; r[1] = 'A'; r[2] = 'L'; r[3] = 'P'; r[4] = 1; } if (decodePacket(r.data(), r.size(), key, q) == PktErr::Ok) accepted++; }
    CHECK(accepted == 0, "random packets accepted: %d", accepted);
    Packet h; h.type = PktType::Hello; h.station = 9; h.seq = 1; h.sendNs = p.sendNs; h.name = "Vilnius roof";
    b = encodePacket(h, key);
    CHECK(decodePacket(b.data(), b.size(), key, q) == PktErr::Ok && q.name == "Vilnius roof", "hello");
    Packet r; r.type = PktType::Reference; r.station = 9; r.seq = 2; r.sendNs = p.sendNs; r.ref = RefRec{8, p.sendNs + 77, 1800, 21.5f, 200};
    b = encodePacket(r, key);
    CHECK(decodePacket(b.data(), b.size(), key, q) == PktErr::Ok && q.ref.anchorNs == p.sendNs + 77 && q.ref.refId == 8, "reference");
    // replays
    ReplayGuard g(600);
    const int64_t now = p.sendNs;
    CHECK(g.check(1, 100, now, now) == ReplayGuard::Fresh, "first");
    CHECK(g.check(1, 100, now, now) == ReplayGuard::Replay, "the same packet twice");
    CHECK(g.check(1, 99, now, now) == ReplayGuard::Fresh && g.check(1, 99, now, now) == ReplayGuard::Replay, "late but new, then replayed");
    CHECK(g.check(1, 101, now - 700000000000LL, now) == ReplayGuard::Stale, "old send time");
    CHECK(g.check(1, 10, now - 1000, now) == ReplayGuard::Stale, "far below the window");
    CHECK(g.check(1, 10, now + 5000000000LL, now + 5000000000LL) == ReplayGuard::Fresh, "a restarted station (lower number, newer time)");
    std::printf("packets: %zu bytes for 5 detections, wrong key / tampering / random / replay refused\n", (size_t)(22 + 16 + 1 + 5 * 32 + 16));
}

// ---------------------------------------------------------------- the whole chain from IQ
static void testChain() {
    const double rate = 250e3;
    const double secs = longRun() ? 16 : 9;
    SimScene sc;
    sc.ref = *refById(8);   // DHO38 23.4 kHz
    sc.oscHz = 20e3;
    // five stations around Germany / Poland; the reference is in north-west Germany
    struct S { const char* name; double lat, lon; TimeSrc src; double ppm, start; double ntpErrMs; };
    const std::vector<S> ss = {
        {"gps-a", 52.52, 13.40, TimeSrc::GpsHw, 14.0, 0.0213, 1.3},
        {"gnss-b", 50.11, 8.68, TimeSrc::Gnss, -8.0, 0.0371, -2.2},
        {"cv-c", 51.34, 12.37, TimeSrc::CommonView, 31.0, 0.0117, 0.8},
        {"cv-d", 53.55, 10.00, TimeSrc::CommonView, -22.0, 0.0441, 7.3},   // its NTP is off by more than half a symbol period
        {"gps-e", 52.23, 21.01, TimeSrc::GpsHw, 5.0, 0.0090, 0.4},
        {"ntp-f", 48.14, 11.58, TimeSrc::Ntp, 2.0, 0.0302, 1.7},
    };
    std::mt19937 rng(77);
    std::uniform_real_distribution<double> ud(0, 1);
    for (double t = 5.0; t < secs - 0.3; t += 0.12 + 0.2 * ud(rng)) {   // after the reference has locked
        SimStrike k;
        k.lat = 49.5 + 4 * ud(rng); k.lon = 8.5 + 11 * ud(rng); k.tS = t; k.ampAt100km = 0.15 + 0.2 * ud(rng);
        sc.strikes.push_back(k);
    }
    ServerCore server("lightning test key");
    const int64_t epoch = 1762800000LL * 1000000000LL;   // true time 0 of the scene
    std::normal_distribution<double> nd(0, 1);
    std::vector<std::vector<uint8_t>> packets;
    for (size_t si = 0; si < ss.size(); si++) {
        const S& s = ss[si];
        SimStation st;
        st.lat = s.lat; st.lon = s.lon; st.ppm = s.ppm; st.startS = s.start; st.noiseRms = 0.004; st.seed = 100 + (uint32_t)si;
        st.refAmp = s.src == TimeSrc::Ntp ? 0.0 : 0.03; st.refOffsetHz = 60e3;   // the NTP station's radio does not hear the reference
        StationCfg cfg;
        cfg.sysKind = 1;
        cfg.refOn = s.src != TimeSrc::Ntp;
        cfg.ref = sc.ref; cfg.refOffsetHz = 60e3;
        StationCore core;
        core.configure(rate, cfg);
        MskState ms;
        const size_t blk = 8192;
        std::vector<std::complex<float>> x(blk);
        uint32_t seq = 1000;
        double nextPps = std::ceil(s.start);
        const uint32_t id = stationIdOf(s.name);
        auto hdr = [&](TimeSrc src, double sg) { StationHdr h; h.lat = s.lat; h.lon = s.lon; h.src = src; h.clockSigmaNs = sg; h.flags = (uint8_t)(1 | (src == TimeSrc::CommonView ? 2 : 0)); return h; };
        { Packet h; h.type = PktType::Hello; h.station = id; h.seq = seq++; h.sendNs = epoch; h.name = s.name; packets.push_back(encodePacket(h, networkKey("lightning test key"))); }
        for (uint64_t p = 0; p < (uint64_t)(secs * rate); p += blk) {
            simRender(sc, st, rate, p, blk, x.data(), &ms);
            const double endTrue = simTrueTime(st, (double)(p + blk) / rate);
            // the timing sources: GPS PPS marks every second (30 ns), GNSS marks (40 ns + 300 ns alignment), the system clock with its NTP error
            while (nextPps <= endTrue) {
                const double sS = simSampleTime(st, nextPps);
                if (s.src == TimeSrc::GpsHw) core.addMark(TimeMark{TimeSrc::GpsHw, sS, epoch + (int64_t)std::llround(nextPps * 1e9 + 30 * nd(rng)), 30, 20});
                if (s.src == TimeSrc::Gnss) core.addMark(TimeMark{TimeSrc::Gnss, sS + 150e-9, epoch + (int64_t)std::llround(nextPps * 1e9 + 40 * nd(rng)), 40, 300});
                nextPps += 1;
            }
            const int64_t sysNow = epoch + (int64_t)std::llround((endTrue + 0.002 * ud(rng) + s.ntpErrMs * 1e-3) * 1e9);
            core.feed(x.data(), blk, sysNow);
            TimeSrc src; double sg;
            std::vector<DetRec> d = core.takeDetections(src, sg);
            if (!d.empty()) { Packet pk; pk.type = PktType::Detections; pk.station = id; pk.seq = seq++; pk.sendNs = sysNow; pk.hdr = hdr(src, sg); pk.dets = d; packets.push_back(encodePacket(pk, networkKey("lightning test key"))); }
            RefRec rr;
            if (core.takeRef(rr)) { double a, b2, c2; const TimeSrc as = core.activeSource(a, b2, c2); Packet pk; pk.type = PktType::Reference; pk.station = id; pk.seq = seq++; pk.sendNs = sysNow; pk.hdr = hdr(as, a); pk.ref = rr; packets.push_back(encodePacket(pk, networkKey("lightning test key"))); }
        }
        double sg, ppm, pps;
        const TimeSrc a = core.activeSource(sg, ppm, pps);
        std::printf("  station %-7s %-17s clock error %9.3f us, ppm %+7.3f (true %+.1f), %llu sferics\n", s.name, timeSrcName(a), sg / 1e3, ppm, s.ppm, (unsigned long long)core.pulseCount());
        CHECK(a == s.src, "%s uses %s", s.name, timeSrcName(a));
        if (a != TimeSrc::Ntp) CHECK(std::fabs(ppm - s.ppm) < 0.5, "%s drift %.3f against %.1f", s.name, ppm, s.ppm);
    }
    // the packets arrive in the order they were sent, neighbours swapped now and then by the network, and some twice
    const auto netKey = networkKey("lightning test key");
    std::vector<std::pair<int64_t, size_t>> order;
    for (size_t i = 0; i < packets.size(); i++) { Packet q; decodePacket(packets[i].data(), packets[i].size(), netKey, q); order.push_back({q.sendNs, i}); }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 6; i + 1 < order.size(); i += 5) std::swap(order[i], order[i + 1]);
    ServerCore& srv2 = server;
    int replays = 0;
    for (size_t i = 0; i < order.size(); i++) {
        const std::vector<uint8_t>& pk = packets[order[i].second];
        const int64_t now = order[i].first + 50000000;
        const ServerCore::In r = server.ingest(pk.data(), pk.size(), now);
        CHECK(r == ServerCore::In::Ok, "packet %zu refused (%d)", i, (int)r);
        if (i % 17 == 0 && server.ingest(pk.data(), pk.size(), now) == ServerCore::In::Replay) replays++;
    }
    const std::vector<Strike> out = srv2.process(epoch + (int64_t)((secs + 5) * 1e9));
    std::vector<double> e, et;
    int good = 0, alsoNtp = 0;
    for (const Strike& k : out) {
        double best = 1e18, bt = 0;
        for (const SimStrike& tr : sc.strikes) { const double d = geodesicM(tr.lat, tr.lon, k.fix.lat, k.fix.lon); if (d < best) { best = d; bt = (double)(k.timeNs - epoch) * 1e-9 - tr.tS; } }
        e.push_back(best);
        if (best < 3000) { good++; et.push_back(std::fabs(bt)); }
        if (!k.alsoHeard.empty()) alsoNtp++;
    }
    std::printf("  chain: %zu strikes simulated, %zu located, %d within 3 km; position error median %.0f m, 95 %% %.0f m; time error median %.2f us; NTP station associated in %d; %llu groups, %llu rejected, %llu unmatched\n",
                sc.strikes.size(), out.size(), good, pct(e, 0.5), pct(e, 0.95), pct(et, 0.5) * 1e6, alsoNtp, (unsigned long long)srv2.stats().groups, (unsigned long long)srv2.stats().rejected, (unsigned long long)srv2.stats().unmatched);
    for (const auto& kv : srv2.stations())
        std::printf("    server sees %-7s %-17s sigma %9.3f us, correction %+10.3f us%s, %llu detections, %llu used\n", kv.second.name.c_str(), timeSrcName(kv.second.hdr.src),
                    kv.second.sigmaNs / 1e3, kv.second.corrNs / 1e3, kv.second.hasRef ? (kv.second.tied ? " (tied to UTC)" : " (reference only)") : "", (unsigned long long)kv.second.dets, (unsigned long long)kv.second.used);
    CHECK(good >= (int)sc.strikes.size() * 8 / 10, "chain located %d of %zu", good, sc.strikes.size());
    CHECK(pct(e, 0.5) < 1000, "chain median error %.0f m", pct(e, 0.5));
    CHECK(alsoNtp >= good / 2, "the NTP station only associates (%d)", alsoNtp);
    CHECK(replays > 0 && server.replays() >= (uint64_t)replays, "duplicates refused as replays (%d)", replays);
    for (const auto& kv : srv2.stations()) if (kv.second.name == "ntp-f") CHECK(kv.second.used == 0, "the NTP station is never in a fix");
}

// ---------------------------------------------------------------- the mode's test signal through one station
static void testSynth() {
    SynthConfig cfg;
    cfg.snrDb = 30; cfg.modeOpt[0] = 1; cfg.modeVal[0] = 3; cfg.modeVal[1] = 50e3; cfg.sroPpm = 7;
    const double rate = 250e3;
    auto syn = makeLtposSynth(cfg, rate);
    StationCfg c;
    c.ref = RefTx{250, "test", 0, 200, 52.0, 10.0};
    c.refOn = true; c.refOffsetHz = 50e3;
    StationCore core;
    core.configure(rate, c);
    std::vector<std::complex<float>> x(8192);
    const int64_t t0 = 1762800000LL * 1000000000LL;
    uint64_t pos = 0;
    while (pos < (uint64_t)(8 * rate)) {
        syn->generate(x.data(), x.size());
        pos += x.size();
        core.feed(x.data(), x.size(), t0 + (int64_t)((double)pos / rate * 1e9));
    }
    double sg, ppm, pps;
    const TimeSrc a = core.activeSource(sg, ppm, pps);
    std::printf("test signal through one station: %llu sferics, clock %s, error %.2f us, ppm %+.2f (true +7), reference %s at %.1f dB\n", (unsigned long long)core.pulseCount(), timeSrcName(a), sg / 1e3, ppm, core.refTimer().locked() ? "locked" : "not locked", (double)core.refTimer().lastSnrDb());
    CHECK(core.pulseCount() >= 12, "sferics from the generator: %llu", (unsigned long long)core.pulseCount());
    CHECK(a == TimeSrc::CommonView && core.refTimer().locked() && std::fabs(ppm - 7) < 1.0, "the reference locks and gives the drift (%s, ppm %.2f)", timeSrcName(a), ppm);
}

// the test signal as the engine's synthetic source delivers it (and an 8-bit radio): a noise floor below one 8-bit step is digital silence between
// the strikes, and 3.9 never found a floor in it (QA sweep 4.0)
static void testEightBit() {
    SynthConfig cfg;
    cfg.snrDb = 60;
    const double rate = 1e6;
    auto syn = makeLtposSynth(cfg, rate);
    StationCfg c;
    StationCore core;
    core.configure(rate, c);
    std::vector<std::complex<float>> x(8192);
    const int64_t t0 = 1762800000LL * 1000000000LL;
    uint64_t pos = 0;
    auto q = [](float v) { return std::round(std::min(127.f, std::max(-128.f, v * 128.f))) / 128.f; };
    while (pos < (uint64_t)(8 * rate)) {
        syn->generate(x.data(), x.size());
        for (auto& v : x) v = std::complex<float>(q(v.real()), q(v.imag()));
        pos += x.size();
        core.feed(x.data(), x.size(), t0 + (int64_t)((double)pos / rate * 1e9));
    }
    std::printf("8-bit samples, silence between strikes: %llu sferics, floor valid %d\n", (unsigned long long)core.pulseCount(), core.pulses().floorValid() ? 1 : 0);
    CHECK(core.pulses().floorValid() && core.pulseCount() >= 8, "8-bit samples: floor valid %d, %llu sferics", core.pulses().floorValid() ? 1 : 0, (unsigned long long)core.pulseCount());
}

// ---------------------------------------------------------------- hardware time and the receiver over UDP on this computer
static void testHwTimeAndReceiver() {
    using namespace dect2;
    int key = 0;
    hwtime::Mark m; uint64_t consumed = 0;
    hwtime::board().consumed(&key, 500);
    CHECK(!hwtime::board().latest(&key, m, consumed), "nothing before arming");
    hwtime::board().arm(&key);
    hwtime::board().consumed(&key, 1000);
    hwtime::board().publish(&key, 800, 1234567890, 1e6);
    CHECK(hwtime::board().latest(&key, m, consumed) && consumed == 1000 && m.srcIndex == 800 && m.seq == 1, "a mark and the samples handed on");
    int other = 0;
    CHECK(!hwtime::board().latest(&other, m, consumed), "another radio sees nothing");
    hwtime::board().clear(&key);
    CHECK(!hwtime::board().latest(&key, m, consumed), "cleared");
    CHECK(hwtime::ppsTimeToUtcNs(250000000, 1762800000LL * 1000000000LL + 310000000) == 1762800000LL * 1000000000LL + 250000000, "whole seconds from the computer, the fraction from the PPS");
    CHECK(hwtime::ppsTimeToUtcNs(990000000, 1762800001LL * 1000000000LL + 40000000) == 1762800000LL * 1000000000LL + 990000000, "across a second boundary");

    // a server and four clients with GPS marks, and one client with the wrong key, over loopback
    const double rate = 250e3;
    const int64_t wall = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    // the scene starts a few seconds ahead: the server holds each detection 2 s for the others, and feeding five stations on a slow computer
    // (Windows runners) can take longer than that, which would make it solve strikes from the first station's packets alone
    const int64_t epoch = wall + 6000000000LL;
    int port = 47500 + (int)(wall / 1000000 % 400);
    const std::string keyText = "a shared network key";
    LtPosReceiver server;
    server.configure(rate);
    LtPosSettings ss;
    ss.role = 2; ss.port = port; ss.key = keyText; ss.name = "server"; ss.serverIsStation = false; ss.minStations = 4;
    server.apply(ss);
    LtPosTelemetry t;
    for (int i = 0; i < 20 && !(server.telemetry(t, 0) && t.listening); i++) { ss.port = ++port; server.apply(ss); }
    CHECK(t.listening, "the server listens");
    SimScene sc;
    sc.oscHz = 20e3;
    SimStrike k1; k1.lat = 50.4; k1.lon = 11.2; k1.tS = 0.7; k1.ampAt100km = 0.3;
    SimStrike k2; k2.lat = 51.9; k2.lon = 13.3; k2.tS = 1.1; k2.ampAt100km = 0.3;
    sc.strikes = {k1, k2};
    struct C { double lat, lon; const char* name; };
    const std::vector<C> cs = {{52.52, 13.40, "b"}, {50.11, 8.68, "f"}, {48.14, 11.58, "m"}, {53.55, 10.00, "h"}, {51.05, 13.74, "wrong"}};
    std::vector<std::unique_ptr<LtPosReceiver>> rx;
    const size_t N = (size_t)(1.5 * rate);
    std::vector<std::complex<float>> x(N);
    std::mt19937 rng(4);
    std::normal_distribution<double> nd(0, 1);
    for (size_t i = 0; i < cs.size(); i++) {
        auto r = std::make_unique<LtPosReceiver>();
        r->configure(rate);
        LtPosSettings s;
        s.role = 1; s.host = "127.0.0.1"; s.port = port; s.key = cs[i].name == std::string("wrong") ? "not the right key" : keyText; s.name = cs[i].name;
        s.lat = cs[i].lat; s.lon = cs[i].lon; s.posSet = true; s.useHwTime = false;
        r->apply(s);
        SimStation st;
        st.lat = s.lat; st.lon = s.lon; st.ppm = 3.0 * (double)i; st.startS = 0.01 * (double)i; st.noiseRms = 0.004; st.seed = 40 + (uint32_t)i;
        simRender(sc, st, rate, 0, N, x.data(), nullptr);
        for (int q = 0; q <= 2; q++) {
            const double trueT = (double)q + 0.25;
            TimeMark gm{TimeSrc::GpsHw, simSampleTime(st, trueT), epoch + (int64_t)std::llround(trueT * 1e9 + 25 * nd(rng)), 25, 20};
            r->addExternalMark(gm);
        }
        for (size_t p = 0; p < N; p += 16384) r->feed(x.data() + p, std::min<size_t>(16384, N - p));
        rx.push_back(std::move(r));
    }
    // the network thread sends in the background and the server solves every 200 ms
    bool done = false;
    for (int i = 0; i < 300 && !done; i++) {   // up to 30 s: the scene lies ahead, then the hold
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        LtPosTelemetry tt;
        if (server.telemetry(tt, 0)) { t = tt; done = t.strikes.size() >= 2 && t.packetsAuth > 0; }
    }
    CHECK(t.strikes.size() == 2, "two strikes located over UDP (%zu; %llu packets, %llu refused)", t.strikes.size(), (unsigned long long)t.packetsOk, (unsigned long long)t.packetsAuth);
    CHECK(t.packetsAuth > 0, "the wrong key's packets are refused");
    CHECK(t.stations.size() == 4, "only the four with the key are listed (%zu)", t.stations.size());
    double worst = 0;
    for (const LtStrikeRow& r : t.strikes) worst = std::max(worst, std::min(geodesicM(r.lat, r.lon, k1.lat, k1.lon), geodesicM(r.lat, r.lon, k2.lat, k2.lon)));
    CHECK(!t.strikes.empty() && worst < 3000, "position over UDP within 3 km (%.0f m)", worst);
    std::printf("receiver over UDP on this computer: %zu strikes, worst error %.0f m, %llu packets accepted, %llu refused\n", t.strikes.size(), worst, (unsigned long long)t.packetsOk, (unsigned long long)t.packetsAuth);
}

int main() {
    testGeodesy();
    testSampleClock();
    testPulseTimer();
    testMsk();
    testSolver();
    testOutliersGroupsAmbiguity();
    testMatcher();
    testPackets();
    std::printf("whole chain, IQ at 250 kHz, six stations (GPS, GNSS, two common view, GPS, NTP):\n");
    testChain();
    testSynth();
    testEightBit();
    testHwTimeAndReceiver();
    std::printf(fails ? "ltpos: %d FAILED\n" : "ltpos: all passed\n", fails);
    return fails ? 1 : 0;
}
