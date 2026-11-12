// DTMB receiver against what real radios do (part 2): level steps (AGC), impulse noise, a carrier wave inside the band, DC offset, IQ imbalance, a low
// level, dropouts of zeros and of missing samples, reset. Same method as test_dtmb_impair.cpp. Pass an argument to run the cases whose name contains it.
#include "dect2/dtmb/dtmb_testkit.h"
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
    // ---- a radio whose buffers overflow now and then (USB, Ethernet): runs of samples lost at random times, as many as 40 a second. The lock must
    // hold (the frame grid found again at once, lost frames counted from the PN phase), and what the gaps did not take must decode.
    CASE("random drops") {
        struct D { const char* name; Header h; Mapping m; Rate rt; bool mode2; double perSec; int lo, hi; double cfo, ppm; bool echo; double minFrac; bool bw6 = false; };
        const D cases[] = {
            {"PN945 16QAM 0.6, 20/s of 0.1 .. 3k samples", Header::Pn945, Mapping::Qam16, Rate::R06, false, 20, 100, 3000, 0, 0, false, 0.80},
            {"PN945 16QAM 0.8 mode 2, echo, cfo, ppm", Header::Pn945, Mapping::Qam16, Rate::R08, true, 20, 100, 3000, 3200, 20, true, 0.45},
            {"PN945 64QAM 0.6, drops of 1 .. 9 frames", Header::Pn945, Mapping::Qam64, Rate::R06, false, 8, 7000, 55000, -800, -5, false, 0.55},
            {"PN420 4QAM 0.4, 40/s", Header::Pn420, Mapping::Qam4, Rate::R04, false, 40, 100, 5000, 1500, 10, true, 0.75},
            {"PN595 32QAM 0.8, 20/s", Header::Pn595, Mapping::Qam32, Rate::R08, false, 20, 100, 3000, 0, 0, false, 0.70},
            {"PN595 4QAM-NR 0.8, drops into the header", Header::Pn595, Mapping::Qam4Nr, Rate::R08, false, 30, 300, 1200, -2500, 0, false, 0.70},
            {"6 MHz PN945 16QAM 0.6 fixed phase, 20/s", Header::Pn945, Mapping::Qam16, Rate::R06, false, 20, 100, 3000, 900, -15, true, 0.70, true},
        };
        for (const D& c : cases) {
            Scenario o = base(c.h, 10e6, 28, 3.0);
            o.sc = makeSignal(c.h, c.m, c.rt, c.mode2, 10e6, 28);
            o.sc.cfoHz = c.cfo; o.sc.sroPpm = c.ppm;
            if (c.bw6) { o.sc.symbolRate = symbolRateFor(6); o.sc.tx.phaseRotate = false; }
            if (c.echo) o.sc.echoes.push_back({-4.0, 37.0});
            o.dropsPerSec = c.perSec; o.dropMin = c.lo; o.dropMax = c.hi;
            const Result r = run(o);
            int lost = 0;
            for (const std::string& l : r.log) if (l.find("lock lost") != std::string::npos || l.find("frames found") != std::string::npos) lost++;
            printf("  %-48s %llu samples lost; %llu good, %llu wrong, %llu missing, last packet %.2f s, lock found %d time(s)\n", c.name, (unsigned long long)r.dropped,
                   (unsigned long long)r.good, (unsigned long long)r.wrong, (unsigned long long)r.missing, r.lastGoodSec, lost);
            if (getenv("DTMB_LOG")) for (const std::string& l : r.log) printf("      %s\n", l.c_str());
            CHECK(r.wrong == 0, "%s: wrong packets", c.name);
            CHECK(lost == 1, "%s: the lock was found %d times", c.name, lost);
            CHECK(r.lastGoodSec > 2.6, "%s: last packet at %.2f s", c.name, r.lastGoodSec);
            CHECK(r.good >= c.minFrac * (r.good + r.missing), "%s: %.2f of the packets", c.name, (double)r.good / std::max<uint64_t>(1, r.good + r.missing));
        }
    }
    // ---- #435: SpyServer over Wi-Fi to a phone at 10 Msps, a fixed-phase PN945 4QAM 0.4 interleaver-720 signal (the parameters in the user's log).
    // The link loses runs of 2 .. 10 ms several times a second. A fixed-phase header cannot tell how many frames a gap took, so missing samples
    // shift the deinterleaver; the same gaps filled with zeros of the right length (what the SpyServer source now does from the message sequence
    // numbers) keep the frame count. The removed-samples run is printed for comparison; the zero-filled one must keep decoding, with no wrong packet.
    CASE("wifi link") {
        for (int zero = 0; zero < 2; zero++) {
            Scenario o = base(Header::Pn945, 10e6, 25, 4.0);
            o.sc = makeSignal(Header::Pn945, Mapping::Qam4, Rate::R04, true, 10e6, 25);
            o.sc.tx.phaseRotate = false;
            o.sc.cfoHz = -110; o.sc.sroPpm = 8;
            o.dropsPerSec = 5; o.dropMin = 20000; o.dropMax = 100000; o.dropZero = zero != 0;
            const Result r = run(o);
            int lost = 0;
            for (const std::string& l : r.log) if (l.find("lock lost") != std::string::npos || l.find("frames found") != std::string::npos) lost++;
            const double frac = (double)r.good / std::max<uint64_t>(1, r.good + r.missing);
            printf("  Wi-Fi gaps %-18s %.1f %% of the samples; %llu good, %llu wrong, %llu missing (%.2f), last packet %.2f s, lock found %d time(s)\n",
                   zero ? "filled with zeros:" : "removed:", 100.0 * r.dropped / (o.seconds * o.sc.rate), (unsigned long long)r.good,
                   (unsigned long long)r.wrong, (unsigned long long)r.missing, frac, r.lastGoodSec, lost);
            if (getenv("DTMB_LOG")) for (const std::string& l : r.log) printf("      %s\n", l.c_str());
            CHECK(r.wrong == 0, "Wi-Fi gaps (%s): %llu wrong packets", zero ? "zeros" : "removed", (unsigned long long)r.wrong);
            if (zero) {
                CHECK(r.lastGoodSec > o.seconds - 0.5, "Wi-Fi gaps filled with zeros: last packet at %.2f s", r.lastGoodSec);
                CHECK(frac >= 0.5, "Wi-Fi gaps filled with zeros: %.2f of the packets", frac);
            }
        }
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
