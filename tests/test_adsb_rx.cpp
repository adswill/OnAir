// ADS-B receiver: generator -> impairments -> receiver, with the list of what was sent checked off frame by frame.
// The signal comes from adsb_gen (pulses, carrier offsets, overlapping bursts, noise) and adsb_sim (impairments, 8 bit rounding, bookkeeping).
// SNR figures are pulse power over noise power in 2 MHz, which is what the generator's snr setting means; the receiver's own SNR (telemetry) is the pulse
// power over the noise power per sample and so depends on the sample rate.
#include "dect2/adsb_sim.h"
#include "jobs.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <atomic>
#include <string>
#include <thread>
using namespace dect2;
using testjobs::jprintf;
static std::atomic<int> fails{0};
// Most parts run their independent cases on several threads (testjobs::Jobs): the output keeps the order of the cases.
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL line %d: ", __LINE__); jprintf(__VA_ARGS__); jprintf("\n"); fails++; } } while (0)

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif
#endif
#ifndef SANITIZED
#define SANITIZED 0
#endif

struct Lcg {
    uint32_t x;
    explicit Lcg(uint32_t s) : x(s * 2654435761u + 12345u) {}
    uint32_t next() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
    double uni() { return (next() >> 8) * (1.0 / 16777216.0); }
};

// n frames, one every millisecond, of every kind the receiver knows, at the pulse amplitude that gives the SNR (in 2 MHz) with the default noise of a 30 dB setting
static std::vector<AdsbTx> spaced(int n, double snrDb, uint32_t seed, bool withReplies = true) {
    std::vector<AdsbTx> v;
    Lcg r(seed);
    const int na = 24;
    uint32_t icao[na];
    for (int i = 0; i < na; i++) icao[i] = 0x400000 + (r.next() % 0x400000);
    // an aircraft keeps its altitude (within 1000 ft) from one frame to the next: replies that are far off the ADS-B altitude are refused by the receiver
    auto alt = [&](size_t k) { return 20000 + 600 * (int)(k % na) + 25 * (int)(r.next() % 40); };
    for (int k = 0; k < n; k++) {
        AdsbTx t;
        t.t = 0.001 * (k + 1) + r.uni() * 1e-6;     // a random fraction of a microsecond: the phase against the sample clock
        const uint32_t a = icao[(size_t)k % na];
        const double lat = 25.0 + r.uni(), lon = 55.0 + r.uni();
        int kind = k < na ? 0 : (int)(r.next() % (withReplies ? 14 : 9));
        if (k >= na && k < 2 * na) kind = 3;
        switch (kind) {
        case 0: t.frame = adsb::encodeAllCall(a, 5, 0); break;
        case 1: t.frame = adsb::encodeIdentification(a, 5, 4, 3, "ABC" + std::to_string(100 + r.next() % 900)); break;
        case 2: t.frame = adsb::encodeAirbornePosition(a, 5, 11, alt((size_t)k % na), false, r.next() & 1, lat, lon); break;
        case 3: t.frame = adsb::encodeVelocity(a, 5, 200 + r.uni() * 300, r.uni() * 360, 64 * ((int)(r.next() % 60) - 30), true, 25 * ((int)(r.next() % 9) - 4)); break;
        case 4: t.frame = adsb::encodeAirspeed(a, 5, 250 + r.uni() * 100, r.next() & 1, r.uni() * 360, 64 * ((int)(r.next() % 40) - 20)); break;
        case 5: t.frame = adsb::encodeAircraftStatus(a, 5, 1, 7700); break;
        case 6: t.frame = adsb::encodeTargetState(a, 5, 32 * (int)(r.next() % 1000), 1013.2, r.uni() * 359, 9); break;
        case 7: t.frame = adsb::encodeOperationalStatus(a, 5, 2, 10, 3); break;
        case 8: t.frame = adsb::encodeAirbornePosition(a, 5, 11, alt((size_t)k % na) / 100 * 100, true, r.next() & 1, lat, lon); break;
        case 9: t.frame = adsb::encodeAltitudeReply(a, 0, alt((size_t)k % na), false); break;
        case 10: t.frame = adsb::encodeIdentityReply(a, 0, 1000 + (int)(r.next() % 7000)); break;
        case 11: { uint8_t mb[7]; adsb::mbTrackAndTurn(mb, 5.0, 100, 450, 0.5, 470); t.frame = adsb::encodeCommB(a, false, 0, alt((size_t)k % na), mb); break; }
        case 12: t.frame = adsb::encodeAirAir(a, false, alt((size_t)k % na), r.next() & 1); break;
        default: { uint8_t mb[7]; adsb::mbCallsign(mb, "XYZ" + std::to_string(r.next() % 1000)); t.frame = adsb::encodeCommB(a, true, 0, 2000 + (int)(r.next() % 5000), mb); break; }
        }
        t.amp = (float)(0.1 * std::pow(10.0, (snrDb - 30.0) / 20.0));
        t.cfoHz = ((int)(r.next() % 400) - 200) * 1e3;
        t.phase = (float)(r.uni() * 6.2832);
        v.push_back(t);
    }
    return v;
}

static size_t decodedCount(const AdsbSimResult& r) { return r.decoded; }

static AdsbSimResult run(AdsbSimConfig c, int n, double snr, uint32_t seed = 1) {
    c.custom = spaced(n, snr, seed);
    c.snrDb = 30;
    c.seconds = n * 1e-3 + 0.01;
    AdsbReceiver rx;
    rx.setLogCallback([](const std::string&) {});
    return adsbSimulate(c, rx);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------

static void testRoundTrip() {
    // every kind of message, exactly: the bytes that come out are the bytes that went in, at the time they were sent, at about the level they were sent
    AdsbSimConfig c; c.rate = 4e6;
    AdsbSimResult r = run(c, 400, 30);
    jprintf("round trip 4 Msps, 30 dB: %zu of %zu decoded, %zu phantoms\n", r.decoded, r.sent.size(), r.phantom);
    CHECK(r.decoded == r.sent.size() && r.phantom == 0, "decoded %zu of %zu, phantoms %zu", r.decoded, r.sent.size(), r.phantom);
    double dl = 0; int n = 0;
    for (auto& f : r.sent) { if (!f.decoded) continue; dl += f.levelDbfs - 20 * std::log10(f.amp); n++; }
    dl /= std::max(n, 1);
    CHECK(std::fabs(dl) < 2.5, "level estimate is %.1f dB off on average", dl);
    // the receiver's own figures
    CHECK(r.tel.blocksOk == r.decoded && r.tel.dataValid == false || r.tel.blocksOk == r.decoded, "blocksOk %llu", (unsigned long long)r.tel.blocksOk);
    CHECK(r.tel.dfCount[17] > 0 && r.tel.dfCount[11] > 0 && r.tel.dfCount[4] > 0 && r.tel.dfCount[5] > 0 && r.tel.dfCount[20] > 0 && r.tel.dfCount[21] > 0 && r.tel.dfCount[0] + r.tel.dfCount[16] > 0, "formats seen");
    CHECK(r.tel.frames.size() == 64, "frame list %zu", r.tel.frames.size());
    // the expected noise floor: white noise of the 30 dB setting at this rate (4 Msps: not filtered)
    const double expect = 10 * std::log10(2 * r.noiseSigma * r.noiseSigma);
    CHECK(std::fabs(r.tel.noiseDbfs - expect) < 8.0, "noise floor %.1f dBFS, the generator made %.1f (8 bit rounding adds some)", r.tel.noiseDbfs, expect);
}

static void testRates() {
    // the same signal at every rate the radios use, 8 bit like a HackRF
    const double rates[] = {2.0e6, 2.4e6, 3.2e6, 4e6, 5e6, 6e6, 8e6, 10e6, 12.5e6, 16e6, 20e6};
    testjobs::Jobs jobs;
    for (double rate : rates) jobs.add([=] {
        AdsbSimConfig c; c.rate = rate;
        AdsbSimResult r = run(c, 300, 28);
        const double pct = 100.0 * r.decoded / r.sent.size();
        jprintf("  %5.1f Msps: %5.1f %% of %zu decoded, phantoms %zu, noise %.1f dBFS\n", rate / 1e6, pct, r.sent.size(), r.phantom, r.tel.noiseDbfs);
        // at exactly 2 Msps a run of equal bits is a 1 MHz tone sampled at its zero crossings at one sampling phase in four: the demodulator then
        // takes the bits it cannot tell apart to be the same as the bit before them (measured 84 % without that, 100 % with it)
        CHECK(pct >= 99.0, "%.1f Msps: only %.1f %% decoded", rate / 1e6, pct);
        CHECK(r.phantom == 0, "%.1f Msps: %zu phantom messages", rate / 1e6, r.phantom);
    });
    jobs.run();
}

static void testPhase2Msps() {
    // at 2 Msps the position of the burst against the sample clock matters: at about one phase in four the samples sit on the edges of the pulses and a
    // run of equal bits shows no contrast. Every phase, float samples, strong signal: every frame must decode
    const int nph = 16, per = 40;
    Lcg r(21);
    std::vector<std::vector<AdsbTx>> sets(nph);   // made one after the other: the phases share one random sequence
    for (int ph = 0; ph < nph; ph++) {
        std::vector<AdsbTx>& v = sets[ph];
        for (int k = 0; k < per; k++) {
            AdsbTx t;
            t.t = 0.001 * (k + 1) + (ph + 0.5) / nph * 0.5e-6;                 // phases over one sample interval (0.5 us)
            t.frame = adsb::encodeAirbornePosition(0x400000 + (r.next() & 0xFFFFF), 5, 11, 20000 + 25 * (int)(r.next() % 800), false, r.next() & 1, 25.0 + r.uni(), 55.0 + r.uni());
            t.amp = 0.3f; t.cfoHz = ((int)(r.next() % 400) - 200) * 1e3; t.phase = (float)(r.uni() * 6.2832);
            v.push_back(t);
        }
    }
    int worst = per;
    std::vector<AdsbSimResult> results(nph);
    testjobs::Jobs jobs;
    for (int ph = 0; ph < nph; ph++) jobs.add([&, ph] {
        AdsbSimConfig c; c.rate = 2e6; c.custom = sets[ph]; c.snrDb = 30; c.seconds = per * 1e-3 + 0.01; c.quantise = false;
        AdsbReceiver rx;
        rx.setLogCallback([](const std::string&) {});
        results[ph] = adsbSimulate(c, rx);
    });
    jobs.run();
    for (int ph = 0; ph < nph; ph++) {
        const AdsbSimResult& res = results[ph];
        worst = std::min(worst, (int)res.decoded);
        CHECK(res.decoded >= (size_t)per - 1 && res.phantom == 0, "2 Msps, phase %.3f of a sample: %zu of %d decoded, %zu phantoms", (ph + 0.5) / nph, res.decoded, per, res.phantom);
    }
    jprintf("  2 Msps, 16 sampling phases, 30 dB: worst phase %d of %d\n", worst, per);
}

static void testChunks() {
    // any chunk size, any alignment: the same frames come out
    AdsbSimConfig c; c.rate = 4e6;
    const int n = 150;
    std::vector<bool> ref;
    const std::vector<std::vector<size_t>> patterns = {{65536}, {1}, {7}, {4096}, {1, 7, 4096, 65536, 333, 20000}, {3, 1000}};
    // the runs go side by side; they are compared with the first one in order afterwards
    std::vector<AdsbSimResult> runs(patterns.size());
    testjobs::Jobs jobs;
    for (size_t p = 0; p < patterns.size(); p++) jobs.add([&, p] {
        AdsbSimConfig cp = c;
        cp.chunks = patterns[p];
        runs[p] = run(cp, n, 25);
    });
    jobs.run();
    for (size_t p = 0; p < patterns.size(); p++) {
        const AdsbSimResult& r = runs[p];
        std::vector<bool> got;
        for (auto& f : r.sent) got.push_back(f.decoded);
        if (p == 0) { ref = got; CHECK(r.decoded >= (size_t)n - 1, "reference run: %zu of %d", r.decoded, n); }
        CHECK(got == ref, "chunk pattern %zu (first %zu): a different set of frames was decoded (%zu against %zu)", p, patterns[p][0], r.decoded, (size_t)std::count(ref.begin(), ref.end(), true));
        jprintf("  chunks starting with %zu: %zu of %d\n", patterns[p][0], r.decoded, n);
    }
}

static void testImpairments() {
    struct Case { const char* name; AdsbSimConfig c; double minPct; };
    std::vector<Case> cases;
    auto base = []() { AdsbSimConfig c; c.rate = 4e6; return c; };
    { Case k{"clean", base(), 99.5}; cases.push_back(k); }
    { Case k{"DC offset 0.05 (-26 dBFS)", base(), 99.0}; k.c.dcOffset = 0.05; cases.push_back(k); }
    { Case k{"DC offset 0.15", base(), 98.0}; k.c.dcOffset = 0.15; cases.push_back(k); }
    { Case k{"IQ imbalance 1.5 dB, 8 degrees", base(), 99.0}; k.c.iqGainDb = 1.5; k.c.iqPhaseDeg = 8; cases.push_back(k); }
    { Case k{"carrier offset +300 kHz", base(), 98.0}; k.c.cfoHz = 300e3; cases.push_back(k); }
    { Case k{"carrier offset -300 kHz", base(), 98.0}; k.c.cfoHz = -300e3; cases.push_back(k); }
    { Case k{"clock +100 ppm", base(), 99.0}; k.c.sroPpm = 100; cases.push_back(k); }
    { Case k{"clock -100 ppm", base(), 99.0}; k.c.sroPpm = -100; cases.push_back(k); }
    { Case k{"no 8 bit rounding", base(), 99.5}; k.c.quantise = false; cases.push_back(k); }
    { Case k{"all together", base(), 97.0}; k.c.dcOffset = 0.05; k.c.iqGainDb = 1; k.c.iqPhaseDeg = 5; k.c.cfoHz = 200e3; k.c.sroPpm = 50; cases.push_back(k); }
    // a carrier inside the band that is not much weaker than the pulses hides them: the receiver has no notch (see the limits in docs/modes/adsb.md)
    { Case k{"CW 14 dB below the pulses, 700 kHz off", base(), 99.0}; k.c.cwDb = -20; k.c.cwHz = 700e3; cases.push_back(k); }
    { Case k{"CW 14 dB below the pulses, 50 kHz off", base(), 99.0}; k.c.cwDb = -20; k.c.cwHz = 50e3; cases.push_back(k); }
    { Case k{"filter 1.75 MHz at 4 Msps", base(), 97.0}; k.c.filterMHz = 1.75; cases.push_back(k); }
    testjobs::Jobs jobs;
    for (auto& k : cases) jobs.add([&k] {
        AdsbSimResult r = run(k.c, 300, 24);
        const double pct = 100.0 * r.decoded / r.sent.size();
        jprintf("  %-40s %5.1f %%  phantoms %zu\n", k.name, pct, r.phantom);
        CHECK(pct >= k.minPct, "%s: %.1f %% decoded (needs %.1f)", k.name, pct, k.minPct);
        CHECK(r.phantom == 0, "%s: %zu phantom messages", k.name, r.phantom);
    });
    jobs.run();
}

static void testLevels() {
    // a gain step in the middle (an AGC or the user turning the gain): the noise floor and the thresholds follow within about a millisecond
    testjobs::Jobs jobs;
    for (double step : {20.0, -20.0}) jobs.add([=] {
        AdsbSimConfig c; c.rate = 4e6;
        c.custom = spaced(300, 34, 11);
        c.snrDb = 30; c.seconds = 0.31;
        c.gainStepAtSec = 0.15; c.gainStepDb = step;
        AdsbReceiver rx;
        rx.setLogCallback([](const std::string&) {});
        AdsbSimResult r = adsbSimulate(c, rx);
        int before = 0, nb = 0, soon = 0, ns = 0, later = 0, nl = 0;
        for (auto& f : r.sent) {
            if (f.t < 0.1495) { nb++; before += f.decoded; }
            else if (f.t < 0.1535) { ns++; soon += f.decoded; }
            else { nl++; later += f.decoded; }
        }
        jprintf("  gain step %+.0f dB at 150 ms: before %d/%d, in the next 4 ms %d/%d, later %d/%d, phantoms %zu\n", step, before, nb, soon, ns, later, nl, r.phantom);
        CHECK(before == nb && later >= nl - 1, "around a %+.0f dB step: %d/%d before, %d/%d after", step, before, nb, later, nl);
        CHECK(soon >= ns - (step > 0 ? 1 : 3), "%+.0f dB step: %d of %d frames in the 4 ms after it", step, soon, ns);
        CHECK(r.phantom == 0, "phantoms");
    });
    // signals that clip the converter: the pulses are flat topped and the noise around them too, but the bits are still there
    for (double k : {5.0, 40.0}) jobs.add([=] {
        AdsbSimConfig c; c.rate = 4e6; c.clipFactor = k;
        AdsbSimResult r = run(c, 200, 40);
        const double pct = 100.0 * r.decoded / r.sent.size();
        jprintf("  input %.0fx too strong (clipped at full scale): %.1f %% decoded, phantoms %zu\n", k, pct, r.phantom);
        CHECK(pct >= 98.0 && r.phantom == 0, "clipped input x%.0f: %.1f %%, %zu phantoms", k, pct, r.phantom);
    });
    jobs.run();
    // calls with no samples, and the rate set twice
    AdsbReceiver rx;
    rx.configure(4e6);
    rx.feed(nullptr, 0);
    cf32 one(0.1f, 0.f);
    rx.feed(&one, 1);
    rx.configure(10e6);
    rx.feed(nullptr, 0);
    AdsbTelemetry t;
    rx.telemetry(t, 0);
    CHECK(rx.ready(), "not ready after configure(10e6)");
}

static void testStrongCw() {
    // a carrier 10 dB above the pulses: nothing can be decoded, and nothing false may come out, and the receiver must come back when it goes away
    AdsbSimConfig c; c.rate = 4e6; c.cwDb = 6; c.cwHz = 300e3;
    AdsbSimResult r = run(c, 100, 24);
    jprintf("  CW 12 dB over the pulses: %zu of %zu decoded, %zu phantoms, %llu failed\n", r.decoded, r.sent.size(), r.phantom, (unsigned long long)r.tel.blocksBad);
    CHECK(r.phantom == 0 && r.tel.aircraftCount <= 24, "false messages with a strong carrier on the channel");
}

static void testSensitivity() {
    // detection against signal-to-noise ratio (float samples, so that 8 bit rounding does not enter); the numbers are the receiver's, kept as limits
    struct P { double rate; double snr; double minPct; };
    const P pts[] = {{4e6, 10, 10}, {4e6, 12, 80}, {4e6, 14, 99}, {4e6, 16, 99.5}, {10e6, 12, 65}, {10e6, 14, 95}, {10e6, 16, 99.5}, {2.4e6, 14, 93}, {2.4e6, 16, 99}, {20e6, 14, 95}, {20e6, 16, 99},
                         {2e6, 20, 93}, {2e6, 24, 98}, {2e6, 28, 99.5}};   // 2 Msps needs about 5 dB more than 4 Msps
    testjobs::Jobs jobs;
    for (const P& p : pts) jobs.add([&p] {
        AdsbSimConfig c; c.rate = p.rate; c.quantise = false;
        AdsbSimResult r = run(c, 500, p.snr, 2);
        const double pct = 100.0 * r.decoded / r.sent.size();
        jprintf("  %5.1f Msps, %4.1f dB: %5.1f %%\n", p.rate / 1e6, p.snr, pct);
        CHECK(pct >= p.minPct, "%.1f Msps %.1f dB: %.1f %% (needs %.1f)", p.rate / 1e6, p.snr, pct, p.minPct);
        CHECK(r.phantom == 0, "%.1f Msps %.1f dB: %zu phantoms, e.g. %s", p.rate / 1e6, p.snr, r.phantom, r.phantoms.empty() ? "" : r.phantoms[0].c_str());
    });
    jobs.run();
}

static void testNoiseOnly() {
    // a band without aircraft: nothing is decoded, and the search does not keep finding preambles. 8 bit rounding of noise that is smaller than one
    // step (the 30 dB setting is) makes the noise a handful of single steps, which the receiver has to cope with as well
    const double rates[] = {2e6, 2.4e6, 4e6, 10e6, 20e6};
    testjobs::Jobs jobs;
    for (double rate : rates) for (int q = 0; q < 2; q++) jobs.add([=] {
        AdsbSimConfig c; c.rate = rate; c.seconds = 10; c.aircraft = 0; c.snrDb = 30; c.quantise = q == 1;
        AdsbReceiver rx;
        rx.setLogCallback([](const std::string&) {});
        AdsbSimResult r = adsbSimulate(c, rx);
        const double perSec = (double)r.tel.preambles / 10.0;
        jprintf("  %5.1f Msps %s: %llu good, %.1f preamble candidates per second, state %d\n", rate / 1e6, q ? "8 bit" : "float", (unsigned long long)r.tel.blocksOk, perSec, r.tel.state);
        CHECK(r.tel.blocksOk == 0 && r.phantom == 0, "%.1f Msps: %llu messages decoded from noise", rate / 1e6, (unsigned long long)r.tel.blocksOk);
        CHECK(perSec < (q && rate < 3e6 ? 80.0 : 5.0), "%.1f Msps %s: %.1f candidates per second", rate / 1e6, q ? "8 bit" : "float", perSec);
        CHECK(r.tel.aircraftCount == 0, "aircraft in the table");
    });
    jobs.run();
}

static void testGarble() {
    // two bursts on top of each other: the stronger one is decoded when the weaker is far enough down, and nothing false comes out
    testjobs::Jobs jobs;
    for (double rate : {4e6, 10e6}) jobs.add([=] {
        int strongOk = 0, total = 0, phantoms = 0;
        for (int k = 0; k < 40; k++) {
            AdsbSimConfig c; c.rate = rate;
            std::vector<AdsbTx> tx;
            Lcg r(k + 7);
            AdsbTx a, b;
            a.t = 0.002 + r.uni() * 1e-6;
            a.frame = adsb::encodeAirbornePosition(0x4A0001 + k, 5, 11, 30000, false, false, 25.0 + k * 0.01, 55.0);
            a.amp = 0.2f; a.phase = (float)(r.uni() * 6.28); a.cfoHz = ((int)(r.next() % 400) - 200) * 1e3;
            b = a;
            b.t = a.t + (10 + (r.next() % 90)) * 1e-6;          // starts 10 to 100 us into the strong one
            b.frame = adsb::encodeVelocity(0x4A1001 + k, 5, 450, 90, 0, false, 0);
            b.amp = 0.02f;                                      // 20 dB weaker
            b.phase = (float)(r.uni() * 6.28); b.cfoHz = ((int)(r.next() % 400) - 200) * 1e3;
            c.custom = {a, b};
            c.snrDb = 30; c.seconds = 0.004;
            AdsbReceiver rx;
            rx.setLogCallback([](const std::string&) {});
            AdsbSimResult res = adsbSimulate(c, rx);
            total++; strongOk += res.sent[0].decoded; phantoms += (int)res.phantom;
        }
        jprintf("  %4.1f Msps: the strong burst decoded %d of %d times with a burst 20 dB down on top of it, phantoms %d\n", rate / 1e6, strongOk, total, phantoms);
        CHECK(strongOk >= total - 2, "%.1f Msps: the strong burst was decoded %d of %d times", rate / 1e6, strongOk, total);
        CHECK(phantoms == 0, "%d phantom messages", phantoms);
    });
    // a weak burst starting before the strong one: the receiver must not lock on to the weak preamble and lose the strong frame
    jobs.add([] {
    int ok = 0, total = 0;
    for (int k = 0; k < 40; k++) {
        AdsbSimConfig c; c.rate = 4e6;
        Lcg r(k + 99);
        AdsbTx a, b;
        a.t = 0.002 + r.uni() * 1e-6;
        a.frame = adsb::encodeAirbornePosition(0x4A2001 + k, 5, 11, 30000, false, false, 25.0, 55.0 + k * 0.01);
        a.amp = 0.02f; a.phase = (float)(r.uni() * 6.28);
        b = a;
        b.t = a.t + (8 + (r.next() % 80)) * 1e-6;
        b.frame = adsb::encodeVelocity(0x4A3001 + k, 5, 450, 90, 0, false, 0);
        b.amp = 0.2f; b.phase = (float)(r.uni() * 6.28);
        c.custom = {a, b}; c.snrDb = 30; c.seconds = 0.004;
        AdsbReceiver rx;
        rx.setLogCallback([](const std::string&) {});
        AdsbSimResult res = adsbSimulate(c, rx);
        total++; ok += res.sent[1].decoded;
    }
    jprintf("  the strong burst that starts inside a weak one: decoded %d of %d\n", ok, total);
    CHECK(ok >= total - 6, "strong burst after a weak one: %d of %d", ok, total);
    });
    jobs.run();
}

// Mode A / C replies (ICAO Annex 10 Vol. IV, secondary radar): a frame of two bracket pulses 20.3 us apart with up to 13 code pulses between them on a 1.45 us
// grid (C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4), each 0.45 us wide. They are on the same frequency as ADS-B, and near an airport there are hundreds a second.
static std::vector<AdsbTx> modeAC(double t0, double seconds, double perSec, double snrDb, uint32_t seed) {
    std::vector<AdsbTx> v;
    Lcg r(seed);
    double t = t0;
    for (;;) {
        t += -std::log(std::max(r.uni(), 1e-9)) / perSec;
        if (t > t0 + seconds) break;
        AdsbTx x;
        x.t = t;
        x.pulses = {{0.0, 0.45}, {14 * 1.45, 14 * 1.45 + 0.45}};
        for (int k = 1; k <= 13; k++) if (k != 7 && (r.next() & 1)) x.pulses.push_back({1.45 * k, 1.45 * k + 0.45});
        std::sort(x.pulses.begin(), x.pulses.end());
        x.amp = (float)(0.1 * std::pow(10.0, (snrDb + (r.uni() - 0.5) * 10.0 - 30.0) / 20.0));
        x.cfoHz = ((int)(r.next() % 400) - 200) * 1e3; x.phase = (float)(r.uni() * 6.2832);
        v.push_back(x);
    }
    return v;
}

// impulse noise: single pulses of 0.1 to 3 us at random times and random strength (sparks, switching, a radar that leaks in)
static std::vector<AdsbTx> impulses(double t0, double seconds, double perSec, double snrDb, uint32_t seed) {
    std::vector<AdsbTx> v;
    Lcg r(seed);
    double t = t0;
    for (;;) {
        t += -std::log(std::max(r.uni(), 1e-9)) / perSec;
        if (t > t0 + seconds) break;
        AdsbTx x;
        x.t = t;
        const double w = 0.1 + r.uni() * 2.9;
        x.pulses = {{0.0, w}};
        x.amp = (float)(0.1 * std::pow(10.0, (snrDb + r.uni() * 20.0 - 30.0) / 20.0));
        x.phase = (float)(r.uni() * 6.2832);
        v.push_back(x);
    }
    return v;
}

static void testInterference() {
    // impulse noise, 3000 a second, from the level of the signals to 20 dB above them
    testjobs::Jobs jobs;
    for (double rate : {2.4e6, 4e6, 10e6}) jobs.add([=] {
        AdsbSimConfig c; c.rate = rate;
        const int n = 400;
        c.custom = spaced(n, 24, 9);
        std::vector<AdsbTx> im = impulses(0.0, n * 1e-3 + 0.01, 3000, 24, 55);
        c.custom.insert(c.custom.end(), im.begin(), im.end());
        c.snrDb = 30; c.seconds = n * 1e-3 + 0.01;
        AdsbReceiver rx;
        rx.setLogCallback([](const std::string&) {});
        AdsbSimResult r = adsbSimulate(c, rx);
        size_t alone = 0, aloneOk = 0, hit = 0, hitOk = 0;
        for (auto& f : r.sent) { if (f.overlapped) { hit++; hitOk += f.decoded; } else { alone++; aloneOk += f.decoded; } }
        jprintf("  %4.1f Msps, 3000 impulses a second: %zu of %zu clean frames decoded, %zu of %zu touched, phantoms %zu, noise floor %.1f dBFS\n", rate / 1e6, aloneOk, alone, hitOk, hit, r.phantom, r.tel.noiseDbfs);
        CHECK(aloneOk >= alone - (alone + 99) / 100, "%.1f Msps with impulse noise: %zu of %zu untouched frames", rate / 1e6, aloneOk, alone);
        CHECK(r.phantom == 0, "%zu false messages with impulse noise", r.phantom);
    });

    // secondary radar replies on the channel: no false messages, and the ADS-B frames that none of them touches are decoded as before
    for (double rate : {2.4e6, 4e6, 10e6}) for (double perSec : {500.0, 2000.0}) jobs.add([=] {
        AdsbSimConfig c; c.rate = rate;
        const int n = 400;
        c.custom = spaced(n, 24, 8);
        std::vector<AdsbTx> ac = modeAC(0.0, n * 1e-3 + 0.01, perSec, 24, 77);
        c.custom.insert(c.custom.end(), ac.begin(), ac.end());
        c.snrDb = 30; c.seconds = n * 1e-3 + 0.01;
        AdsbReceiver rx;
        rx.setLogCallback([](const std::string&) {});
        AdsbSimResult r = adsbSimulate(c, rx);
        size_t alone = 0, aloneOk = 0, hit = 0, hitOk = 0;
        for (auto& f : r.sent) { if (f.overlapped) { hit++; hitOk += f.decoded; } else { alone++; aloneOk += f.decoded; } }
        jprintf("  %4.1f Msps, %4.0f Mode A/C replies a second: %zu of %zu clean frames decoded, %zu of %zu touched, phantoms %zu\n", rate / 1e6, perSec, aloneOk, alone, hitOk, hit, r.phantom);
        CHECK(aloneOk >= alone - (alone + 99) / 100, "%.1f Msps with %.0f replies a second: %zu of %zu untouched frames", rate / 1e6, perSec, aloneOk, alone);
        CHECK(r.phantom == 0, "%zu false messages with Mode A/C replies on the channel", r.phantom);
    });
    jobs.run();
}

static void testDropoutAndReset() {
    const int n = 400;
    AdsbSimConfig c; c.rate = 4e6;
    c.custom = spaced(n, 26, 5);
    c.snrDb = 30; c.seconds = n * 1e-3 + 0.01;
    c.gapAtSec = 0.1005; c.gapMs = 5.0;       // a stretch of zeros: the radio dropped data
    AdsbReceiver rx;
    rx.setLogCallback([](const std::string&) {});
    AdsbSimResult r = adsbSimulate(c, rx);
    int before = 0, nBefore = 0, after = 0, nAfter = 0, inside = 0, nInside = 0, soon = 0, nSoon = 0;
    for (auto& f : r.sent) {
        if (f.t < 0.1004) { nBefore++; before += f.decoded; }              // a frame is 120 us long: these end before the gap
        else if (f.t < 0.1055) { nInside++; inside += f.decoded; }
        else if (f.t < 0.1075) { nSoon++; soon += f.decoded; }
        else { nAfter++; after += f.decoded; }
    }
    jprintf("  5 ms dropout: before %d/%d, in the gap %d/%d, in the first 2 ms after %d/%d, later %d/%d, phantoms %zu, bad %llu\n", before, nBefore, inside, nInside, soon, nSoon, after, nAfter, r.phantom, (unsigned long long)r.tel.blocksBad);
    CHECK(before >= nBefore - 1 && after >= nAfter - 2, "frames away from the gap: %d/%d and %d/%d", before, nBefore, after, nAfter);
    CHECK(inside == 0, "frames inside the gap decoded");
    CHECK(soon >= nSoon - 1, "the receiver was slow to recover: %d of %d in the 2 ms after the gap", soon, nSoon);
    CHECK(r.phantom == 0, "phantoms");
    CHECK(r.tel.blocksBad < 400, "%llu failed candidates (a gap of zeros must not start a flood of them)", (unsigned long long)r.tel.blocksBad);
    // reset in the middle: the table is emptied, decoding continues, the report number only grows
    AdsbSimConfig d; d.rate = 4e6;
    d.custom = spaced(n, 26, 6); d.snrDb = 30; d.seconds = n * 1e-3 + 0.01;
    d.resetAtSec = 0.2005;
    AdsbReceiver rx2;
    rx2.setLogCallback([](const std::string&) {});
    AdsbSimResult r2 = adsbSimulate(d, rx2);
    int a2 = 0, na2 = 0, b2 = 0, nb2 = 0;
    // (replies of the address / parity kind need their aircraft to have been heard since the reset: only DF11 and DF17 are counted)
    for (auto& f : r2.sent) {
        if (f.df != 11 && f.df != 17) continue;
        if (f.t < 0.2) { na2++; a2 += f.decoded; } else if (f.t > 0.2008) { nb2++; b2 += f.decoded; }
    }
    jprintf("  reset at 200.5 ms: before %d/%d, after %d/%d\n", a2, na2, b2, nb2);
    CHECK(a2 >= na2 - 1 && b2 >= nb2 - 1, "frames around the reset: %d/%d and %d/%d", a2, na2, b2, nb2);
    CHECK(r2.tel.aircraftCount <= 24 && r2.tel.blocksOk < r2.decoded, "the tables were not emptied by reset(): ok %llu of %zu", (unsigned long long)r2.tel.blocksOk, r2.decoded);
    // a reset in the middle of a frame loses that frame and nothing else
    AdsbSimConfig d2 = d;
    d2.resetAtSec = 0.20005;
    AdsbReceiver rx2b;
    rx2b.setLogCallback([](const std::string&) {});
    AdsbSimResult r2b = adsbSimulate(d2, rx2b);
    int a3 = 0, na3 = 0, b3 = 0, nb3 = 0;
    for (auto& f : r2b.sent) {
        if (f.df != 11 && f.df != 17) continue;
        if (f.t < 0.1995) { na3++; a3 += f.decoded; } else if (f.t > 0.2008) { nb3++; b3 += f.decoded; }
    }
    CHECK(a3 >= na3 - 1 && b3 >= nb3 - 1 && r2b.phantom == 0, "reset inside a frame: %d/%d before, %d/%d after, %zu phantoms", a3, na3, b3, nb3, r2b.phantom);
    // telemetry numbers grow across a reset
    AdsbReceiver rx3;
    rx3.configure(4e6);
    std::vector<cf32> noise(40000);
    Lcg g(3);
    for (auto& v : noise) v = cf32((float)(g.uni() - 0.5) * 0.01f, (float)(g.uni() - 0.5) * 0.01f);
    uint64_t seq = 0, lastSeq = 0;
    bool grew = true;
    for (int i = 0; i < 400; i++) {
        rx3.feed(noise.data(), noise.size());
        if (i == 200) rx3.reset();
        AdsbTelemetry t;
        if (rx3.telemetry(t, seq)) { grew &= t.seq > lastSeq; lastSeq = t.seq; seq = t.seq; }
    }
    CHECK(grew && lastSeq > 10, "report numbers do not grow (%llu)", (unsigned long long)lastSeq);
    AdsbTelemetry t;
    CHECK(!rx3.telemetry(t, lastSeq), "a report for a number that was already seen");
    // too low a rate
    AdsbReceiver slow;
    slow.configure(1.5e6);
    CHECK(!slow.ready(), "ready at 1.5 Msps");
    slow.feed(noise.data(), noise.size());
}

static void testAirspace() {
    // twelve aircraft, 20 s, with the traffic as the generator makes it: overlapping bursts, replies, the lot
    testjobs::Jobs jobs;
    for (double rate : {4e6, 2.4e6, 10e6}) jobs.add([=] {
        AdsbSimConfig c; c.rate = rate; c.seconds = 20; c.snrDb = 30; c.aircraft = 12; c.seed = 3; c.setRef = true;
        AdsbReceiver rx;
        rx.setLogCallback([](const std::string&) {});
        AdsbSimResult r = adsbSimulate(c, rx);
        size_t alone = 0, aloneOk = 0, ovl = 0, ovlOk = 0;
        for (auto& f : r.sent) { if (f.overlapped) { ovl++; ovlOk += f.decoded; } else { alone++; aloneOk += f.decoded; } }
        jprintf("  airspace at %.1f Msps: %zu sent, %zu alone: %.2f %% decoded, %zu overlapped: %.1f %%, phantoms %zu, %.1f msg/s, %u aircraft (%u with a position)\n", rate / 1e6, r.sent.size(), alone,
               100.0 * aloneOk / std::max<size_t>(alone, 1), ovl, 100.0 * ovlOk / std::max<size_t>(ovl, 1), r.phantom, r.tel.msgsPerSec, r.tel.aircraftCount, r.tel.withPosition);
        CHECK(aloneOk >= alone * 995 / 1000, "%.1f Msps: only %zu of %zu clean frames decoded", rate / 1e6, aloneOk, alone);
        CHECK(r.phantom == 0, "phantoms");
        CHECK(r.tel.aircraftCount == 12 && r.tel.withPosition == 12, "%u aircraft, %u with a position", r.tel.aircraftCount, r.tel.withPosition);
        CHECK(r.tel.state == 2 && r.tel.dataValid, "state %d", r.tel.state);
        // compare the table with what flew
        AdsbAirspaceConfig ac; ac.aircraft = 12; ac.seed = 3; ac.rateMultiplier = 1; ac.replies = true;
        AdsbAirspace air(ac);
        std::vector<AdsbTx> tx;
        air.run(c.seconds, tx);
        int matched = 0;
        for (auto& truth : air.aircraft()) {
            for (auto& a : r.tel.aircraft) {
                if ((a.icao & 0xFFFFFF) != truth.icao) continue;
                matched++;
                CHECK(a.callsign == truth.callsign, "%06X callsign '%s' (flew '%s')", truth.icao, a.callsign.c_str(), truth.callsign.c_str());
                CHECK(a.hasPos && std::fabs(a.lat - truth.lat) < 0.03 && std::fabs(a.lon - truth.lon) < 0.03, "%06X position %.4f %.4f, flew %.4f %.4f", truth.icao, a.lat, a.lon, truth.lat, truth.lon);
                CHECK(truth.ground ? a.ground : (a.hasAlt && std::fabs(a.altFt - truth.altFt) < 250), "%06X altitude %d, flew %.0f", truth.icao, a.altFt, truth.altFt);
                // one aircraft in eleven sends its indicated airspeed instead of the ground speed
                const double wantKt = truth.airspeedMessage ? truth.iasKt : truth.gsKt;
                CHECK(a.hasSpeed && std::fabs(a.speedKt - wantKt) < 3.0 && a.speedKind == (truth.airspeedMessage ? 1 : 0), "%06X speed %.0f (kind %d), flew %.0f", truth.icao, a.speedKt, a.speedKind, wantKt);
                CHECK(a.hasSquawk && a.squawk == truth.squawk, "%06X squawk %04d, flew %04d", truth.icao, a.squawk, truth.squawk);
                CHECK(a.hasRange && std::fabs(a.distNm - truth.distNm) < 2.0, "%06X range %.1f NM, flew %.1f", truth.icao, a.distNm, truth.distNm);
                CHECK(a.posKind >= 1 && a.track.size() >= 3, "%06X track points %zu", truth.icao, a.track.size());
            }
        }
        CHECK(matched == 12, "only %d of 12 aircraft matched by address", matched);
    });
    jobs.run();
}

static void testSpeed() {
    // seconds of signal per second of CPU in feed(), on the busy default airspace
    for (double rate : {2e6, 4e6, 10e6, 20e6}) {
        AdsbSimConfig c; c.rate = rate; c.seconds = rate > 8e6 ? 8 : 15; c.snrDb = 30; c.aircraft = 40; c.seed = 5;
        AdsbReceiver rx;
        rx.setLogCallback([](const std::string&) {});
        AdsbSimResult r = adsbSimulate(c, rx);
        const double rtf = r.signalSec / std::max(r.cpuSec, 1e-9);
        jprintf("  %5.1f Msps, 40 aircraft (%.0f msg/s sent): feed() runs %.1fx real time\n", rate / 1e6, r.sent.size() / r.signalSec, rtf);
        if (!SANITIZED) CHECK(rtf >= 3.0, "%.1f Msps: only %.1fx real time", rate / 1e6, rtf);
    }
}

static void testBadSamples() {
    // a radio or a driver that hands over NaN, infinity or absurd values for a while: the sums must not be poisoned for good
    const double rate = 4e6;
    const int n = 200;
    std::vector<AdsbTx> tx = spaced(n, 28, 12);
    AdsbMixer mix(rate, 0);
    for (auto& t : tx) mix.add(t);
    AdsbReceiver rx;
    rx.setLogCallback([](const std::string&) {});
    rx.configure(rate);
    std::set<std::string> got;
    rx.setFrameCallback([&](const AdsbFrame& f) { got.insert(adsb::toHex(f.bytes, f.bits)); });
    AdsbNoise noise(rate, 0, 30, 0.1, 3);
    const size_t total = (size_t)((n * 1e-3 + 0.12) * rate), block = 4096;   // long enough for a report
    std::vector<cf32> buf(block);
    const float nan = std::nanf(""), inf = INFINITY;
    for (size_t done = 0; done < total; done += block) {
        const size_t m = std::min(block, total - done);
        mix.render(buf.data(), m);
        noise.add(buf.data(), m);
        const double t0 = (double)done / rate;
        for (size_t i = 0; i < m; i++) {
            const double t = t0 + (double)i / rate;
            if (t >= 0.050 && t < 0.0505) buf[i] = cf32(nan, nan);                       // half a millisecond of NaN
            else if (t >= 0.100 && t < 0.1002) buf[i] = cf32(inf, -inf);
            else if (t >= 0.150 && t < 0.1501) buf[i] = cf32(1e30f, -1e30f);
            else if (i % 997 == 0 && t > 0.01) buf[i] = cf32(nan, 0.1f);                // single bad values all the time
        }
        rx.feed(buf.data(), m);
    }
    size_t ok = 0, laterOk = 0, later = 0;
    for (auto& f : tx) {
        const bool d = got.count(adsb::toHex(f.frame.b, f.frame.bits)) > 0;
        ok += d;
        if (f.t > 0.160) { later++; laterOk += d; }
    }
    AdsbTelemetry t;
    rx.telemetry(t, 0);
    jprintf("  NaN, infinity and 1e30 in the stream: %zu of %d decoded, %zu of %zu after the last bad stretch, noise floor %.1f dBFS\n", ok, n, laterOk, later, t.noiseDbfs);
    CHECK(ok >= (size_t)n * 90 / 100 && laterOk >= later - later / 50, "bad sample values: %zu of %d, %zu of %zu later", ok, n, laterOk, later);
    CHECK(std::isfinite(t.noiseDbfs) && t.noiseDbfs < -35, "noise floor %.1f after bad samples", t.noiseDbfs);
}

static void testThreads() {
    // telemetry(), the setters and reset() from another thread while feed() runs (run under the thread sanitiser for the real check)
    AdsbSimConfig c; c.rate = 4e6; c.seconds = 6; c.aircraft = 20; c.snrDb = 30; c.seed = 9;
    AdsbReceiver rx;
    rx.setLogCallback([](const std::string&) {});
    std::atomic<bool> stop{false};
    std::atomic<int> reports{0};
    std::thread ui([&]() {
        uint64_t seq = 0;
        int i = 0;
        while (!stop) {
            AdsbTelemetry t;
            if (rx.telemetry(t, seq)) { seq = t.seq; reports++; }
            if (++i % 40 == 0) rx.setReference(25.25 + (i % 80) * 0.001, 55.36);
            if (i % 97 == 0) rx.clearReference();
            if (i % 211 == 0) rx.setCorrection(i % 3);
            if (i % 331 == 0) rx.setExpiry(30 + i % 60);
            if (i % 997 == 0) rx.clearAircraft();
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });
    AdsbSimResult r = adsbSimulate(c, rx);
    stop = true;
    ui.join();
    jprintf("  %d reports read while %zu frames went by\n", reports.load(), r.sent.size());
    CHECK(reports.load() > 10, "reports %d", reports.load());
}

int main(int argc, char** argv) {
    if (argc > 1) {   // one part only (the thread sanitiser run): test_adsb_rx threads
        const std::string a = argv[1];
        if (a == "threads") testThreads();
        else if (a == "chunks") testChunks();
        else if (a == "airspace") testAirspace();
        else { jprintf("unknown part '%s'\n", a.c_str()); return 2; }
        jprintf(fails ? "adsb_rx %s: %d FAILED\n" : "adsb_rx %s: passed\n", a.c_str(), fails.load());
        return fails ? 1 : 0;
    }
    testRoundTrip();
    jprintf("rates\n"); testRates();
    jprintf("2 Msps phases\n"); testPhase2Msps();
    jprintf("chunks\n"); testChunks();
    jprintf("impairments\n"); testImpairments();
    jprintf("levels\n"); testLevels();
    jprintf("strong carrier\n"); testStrongCw();
    jprintf("sensitivity\n"); testSensitivity();
    jprintf("noise only\n"); testNoiseOnly();
    jprintf("garble\n"); testGarble();
    jprintf("Mode A/C interference\n"); testInterference();
    jprintf("dropout, reset\n"); testDropoutAndReset();
    jprintf("airspace\n"); testAirspace();
    jprintf("speed\n"); testSpeed();
    jprintf("bad samples\n"); testBadSamples();
    jprintf("threads\n"); testThreads();
    jprintf(fails ? "adsb_rx: %d FAILED\n" : "adsb_rx: all passed\n", fails.load());
    return fails ? 1 : 0;
}
