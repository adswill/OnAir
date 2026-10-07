// DTMB receiver against what real radios do (part 2): level steps (AGC), impulse noise, a carrier wave inside the band, DC offset, IQ imbalance, a low
// level, dropouts of zeros and of missing samples, reset. Same method as test_dtmb_impair.cpp. Pass an argument to run the cases whose name contains it.
#include "dect2/dtmb_testkit.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace dect2;
using namespace dect2::dtmb;
using namespace dect2::dtmb::kit;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char* only = nullptr;
#define CASE(NAME) if (!only || strstr(NAME, only))

static Scenario base(Header h = Header::Pn945, double rate = 10e6, double snr = 30, double seconds = 1.5) {
    Scenario o;
    o.sc = makeSignal(h, Mapping::Qam16, Rate::R06, false, rate, snr);
    o.seconds = seconds;
    return o;
}

// The stream came out right: nothing wrong, in order, at most `badCw` failed codewords, at least `minFraction` of what the signal carries after `lockSec`
static void expect(const char* name, const Result& r, const Scenario& o, double lockSec, double minFraction, uint64_t badCw = 0) {
    printf("  %-46s first packet %.2f s, %llu good, %llu wrong, %llu missing; codewords ok %llu bad %llu; C/N %.1f MER %.1f cfo %.0f Hz clock %.1f ppm echo %.1f us; feed() %.1fx real time\n", name, r.firstGoodSec,
           (unsigned long long)r.good, (unsigned long long)r.wrong, (unsigned long long)r.missing, (unsigned long long)r.tel.blocksOk, (unsigned long long)r.tel.blocksBad, r.tel.snrPnDb, r.tel.merDb, r.tel.cfoHz,
           r.tel.clockPpm, r.tel.echoSpanUs, r.signalSec / std::max(r.rxCpu, 1e-9));
    CHECK(r.firstGoodSec >= 0 && r.firstGoodSec < lockSec, "%s: first packet at %.2f s", name, r.firstGoodSec);
    CHECK(r.wrong == 0, "%s: %llu wrong packets", name, (unsigned long long)r.wrong);
    CHECK(r.backwards == 0 && r.seqBack == 0, "%s: order of packets or telemetry", name);
    CHECK(r.tel.blocksBad <= badCw, "%s: %llu codewords failed", name, (unsigned long long)r.tel.blocksBad);
    CHECK(r.goodFraction(lockSec) >= minFraction, "%s: %.3f of the expected packets", name, r.goodFraction(lockSec));
    (void)o;
}

int main(int argc, char** argv) {
    if (argc > 1) only = argv[1];

    // ---- level changes (AGC): the whole signal moves by 15 dB in the middle of a frame body
    CASE("level step") {
        Scenario o = base(Header::Pn945, 10e6, 35, 2.2); o.stepAt = 0.9; o.stepDb = -15;
        Result r = run(o);
        expect("level step -15 dB", r, o, 0.45, 0.97, 2);
        o.stepDb = 12; o.levelDb = -14;
        r = run(o);
        expect("level step +12 dB", r, o, 0.45, 0.97, 2);
    }
    CASE("low level") {
        Scenario o = base(Header::Pn945, 10e6, 40); o.levelDb = -20;   // 2.8 LSB rms: the 8 bit converter limits C/N to about 18 dB
        const Result r = run(o);
        expect("signal at -33 dBFS (3 LSB rms)", r, o, 0.45, 0.97);
        CHECK(r.tel.levelDbfs < -30.f && r.tel.levelDbfs > -36.f, "level %.1f dBFS", r.tel.levelDbfs);
    }

    // ---- impulse noise: the damaged frames are known by their decision error and weighted down
    CASE("impulse noise") {
        Scenario o = base(Header::Pn945, 10e6, 30, 2.2); o.burstsPerSec = 200; o.burstUs = 20; o.burstDb = 10;
        Result r = run(o);
        expect("impulse noise 200/s, 20 us, +10 dB", r, o, 0.45, 0.97, 30);
        o.burstsPerSec = 50; o.burstUs = 100; o.burstDb = 20;
        r = run(o);
        expect("impulse noise 50/s, 100 us, +20 dB", r, o, 0.45, 0.97, 30);
    }

    // ---- interference and radio faults
    CASE("carrier wave") {
        Scenario o = base(); o.toneDb = -3; o.toneHz = 700e3;
        Result r = run(o);
        expect("carrier wave 3 dB below the signal at +700 kHz", r, o, 0.45, 0.97);
        o.toneDb = 0; o.toneHz = -3.1e6;
        r = run(o);
        expect("carrier wave as strong as the signal at -3.1 MHz", r, o, 0.45, 0.97, 5);
        o.sc.snrDb = 35; o.toneDb = 5; o.toneHz = -1e6; o.levelDb = -7;   // the gain comes down so that the sum stays inside the converter
        r = run(o);
        expect("carrier wave 5 dB above the signal at -1 MHz", r, o, 0.45, 0.97, 5);
        o.toneDb = 20; o.toneHz = 4.5e6; o.levelDb = -22;   // just outside the signal: the matched filter rejects it
        r = run(o);
        expect("carrier wave 20 dB above the signal at +4.5 MHz", r, o, 0.45, 0.97, 5);
    }
    CASE("DC and IQ") {
        Scenario o = base(); o.dc = cf32(0.06f, -0.045f); o.iqGainDb = 2; o.iqPhaseDeg = 6; o.chunkMode = 1;
        const Result r = run(o);
        expect("DC offset 0.075, IQ imbalance 2 dB / 6 deg, odd chunks", r, o, 0.45, 0.97);
    }

    // ---- dropouts and reset
    CASE("dropout") {
        Scenario o = base(); o.seconds = 2.4; o.gapAt = 0.8; o.gapSec = 0.004;
        Result r = run(o);
        printf("  zeros for 4 ms: %llu good, %llu wrong, %llu missing, last packet at %.2f s\n", (unsigned long long)r.good, (unsigned long long)r.wrong, (unsigned long long)r.missing, r.lastGoodSec);
        CHECK(r.wrong == 0 && r.lastGoodSec > 2.3, "recovery after 4 ms of zeros");
        CHECK(r.missing < 3 * 480, "4 ms of zeros cost %llu packets", (unsigned long long)r.missing);
        o.gapSec = 0.0033; o.gapRemove = true;
        r = run(o);
        printf("  3.3 ms of samples missing: %llu good, %llu wrong, %llu missing, last packet at %.2f s\n", (unsigned long long)r.good, (unsigned long long)r.wrong, (unsigned long long)r.missing, r.lastGoodSec);
        CHECK(r.wrong == 0 && r.lastGoodSec > 2.3, "recovery after missing samples");
        o.gapSec = 0.1; o.gapRemove = false;   // a long dropout: lock is lost and found again
        r = run(o);
        printf("  zeros for 100 ms: %llu good, %llu wrong, %llu missing, last packet at %.2f s\n", (unsigned long long)r.good, (unsigned long long)r.wrong, (unsigned long long)r.missing, r.lastGoodSec);
        CHECK(r.wrong == 0 && r.lastGoodSec > 2.3, "recovery after 100 ms of zeros");
        CHECK(r.missing < 16000 * 0.5, "100 ms of zeros cost %llu packets (the stream is about 14400 packets per second)", (unsigned long long)r.missing);
    }
    CASE("reset") {
        Scenario o = base(); o.seconds = 2.0; o.resetAt = 0.8;
        const Result r = run(o);
        printf("  reset at 0.9 s: %llu good, after the reset %llu\n", (unsigned long long)r.good, (unsigned long long)r.goodAfterReset);
        CHECK(r.wrong == 0 && r.goodAfterReset > 8000 && r.seqBack == 0, "reset: %llu packets after, telemetry went back %llu", (unsigned long long)r.goodAfterReset, (unsigned long long)r.seqBack);
    }
    printf(failures ? "dtmb_faults: %d FAILED\n" : "dtmb_faults: all passed\n", failures);
    return failures ? 1 : 0;
}
