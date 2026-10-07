// DTMB receiver against what real radios and channels do (part 1): carrier offsets up to the edge of the capture range, clock offsets, sample rates
// that are not multiples of the symbol rate, echoes before and after the main path for every header. Every case runs the numbered test packets
// through generator -> channel -> 8 bit -> receiver and counts what comes out. Pass an argument to run the cases whose name contains it.
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

    // ---- carrier offset: the search tries 0, +-8, +-16, +-24 kHz and fits the header; the loop then holds it
    for (double cfo : {19990.0, -19990.0, 4010.0, -27300.0}) {
        char name[64]; snprintf(name, sizeof name, "carrier offset %+.0f Hz", cfo);
        CASE(name) {
            Scenario o = base(cfo < -27000 ? Header::Pn420 : Header::Pn945);
            o.sc.cfoHz = cfo;
            const Result r = run(o);
            expect(name, r, o, 0.45, 0.97);
            CHECK(std::fabs(r.tel.cfoHz - cfo) < 40, "%s: estimate %.1f", name, r.tel.cfoHz);
        }
    }
    CASE("carrier offset PN595") {
        Scenario o = base(Header::Pn595); o.sc.cfoHz = -13200;
        const Result r = run(o);
        expect("carrier offset -13200 Hz, PN595", r, o, 0.45, 0.97);
        CHECK(std::fabs(r.tel.cfoHz + 13200) < 40, "estimate %.1f", r.tel.cfoHz);
    }

    // ---- clock offset and sample rates that are not multiples of the symbol rate
    for (double ppm : {100.0, -100.0}) {
        char name[64]; snprintf(name, sizeof name, "clock %+.0f ppm, 12.5 Msps, carrier -12 kHz", ppm);
        CASE(name) {
            Scenario o = base(Header::Pn945, 12.5e6); o.sc.sroPpm = ppm; o.sc.cfoHz = -12000; o.seconds = 2.2;
            const Result r = run(o);
            expect(name, r, o, 0.6, 0.97);
            CHECK(std::fabs(r.tel.clockPpm + ppm) < 6, "%s: clock estimate %.1f ppm", name, r.tel.clockPpm);
        }
    }
    for (double rate : {9.6e6, 10.24e6, 15.12e6, 17.3e6}) {
        char name[64]; snprintf(name, sizeof name, "input rate %.2f Msps", rate / 1e6);
        CASE(name) {
            Scenario o = base(Header::Pn420, rate); o.seconds = 1.2;
            const Result r = run(o);
            expect(name, r, o, 0.45, 0.97);
        }
    }
    CASE("rate too low") {
        DtmbReceiver rx;
        rx.configure(7.56e6);
        CHECK(!rx.ready(), "7.56 Msps is below the 7.94 MHz the signal occupies");
        rx.configure(8e6);
        CHECK(rx.ready(), "8 Msps is the lowest rate that works");
        std::vector<cf32> junk(1000, cf32(0.1f, 0.f));
        rx.configure(2e6);
        rx.feed(junk.data(), junk.size());   // must not crash or block
    }

    // ---- echoes: before and after the main path, each header (the window of the channel estimate is different for each)
    struct Echo { const char* name; Header h; double db, delay; double snr; };
    const Echo echoes[] = {
        {"PN945 0 dB echo +200 samples (20 us)", Header::Pn945, 0.5, 200, 30},
        {"PN945 3 dB echo -200 samples (20 us before)", Header::Pn945, 3, -200, 30},
        {"PN420 0 dB echo +60 samples (6 us)", Header::Pn420, 0.5, 60, 30},
        {"PN420 3 dB echo -70 samples (7 us before)", Header::Pn420, 3, -70, 30},
        {"PN595 3 dB echo +140 samples (14 us)", Header::Pn595, 3, 140, 30},
        {"PN595 6 dB echo -100 samples (10 us before)", Header::Pn595, 6, -100, 30},
    };
    for (const Echo& e : echoes) {
        CASE(e.name) {
            Scenario o = base(e.h, 10e6, e.snr); o.sc.echoes.push_back({e.db, e.delay});
            const Result r = run(o);
            expect(e.name, r, o, 0.45, 0.97);
            CHECK(r.tel.echoSpanUs > std::fabs(e.delay) / 10.0 * 0.8 && r.tel.echoSpanUs < std::fabs(e.delay) / 10.0 * 1.3 + 0.5, "%s: echo span %.1f us", e.name, r.tel.echoSpanUs);
            // the impulse response of the display: the main path at 0 dB, the echo at its attenuation, a delay of `delay` output samples later or earlier
            {
                const auto& c = r.tel.cirDb;
                int mainI = 0;
                for (size_t i = 0; i < c.size(); i++) if (c[i] > c[(size_t)mainI]) mainI = (int)i;
                const double symDelay = e.delay * kSymbolRate / 10e6;   // in symbols
                int echoI = -1; float echoV = -200;
                for (int i = 0; i < (int)c.size(); i++) if (std::abs(i - mainI) > 8 && c[(size_t)i] > echoV) { echoV = c[(size_t)i]; echoI = i; }
                // (the strongest tap is the reference: with an echo of 0.5 dB the roles may swap)
                const double lag = (double)(echoI - mainI);
                CHECK(c.size() >= 64 && std::fabs(c[(size_t)mainI]) < 0.01f, "%s: impulse response of %zu taps", e.name, c.size());
                CHECK(echoI >= 0 && std::fabs(std::fabs(lag) - std::fabs(symDelay)) < 2.0 && -echoV - e.db > -1.0 && -echoV - e.db < 3.5,   // a path between two symbol instants shows up to 3 dB low on the strongest of its taps
                       "%s: impulse response shows an echo %.1f dB down at %+.1f symbols (set: %.1f dB at %+.1f)", e.name, -echoV, lag, e.db, symDelay);
            }
        }
    }
    CASE("two echoes") {
        Scenario o = base(); o.sc.echoes.push_back({4, 90}); o.sc.echoes.push_back({7, -120}); o.sc.cfoHz = 3300;
        const Result r = run(o);
        expect("two echoes (+9 us 4 dB, -12 us 7 dB)", r, o, 0.45, 0.97);
        CHECK(r.tel.echoSpanUs > 18.f && r.tel.echoSpanUs < 23.f, "echo span %.1f", r.tel.echoSpanUs);
    }

    printf(failures ? "dtmb_impair: %d FAILED\n" : "dtmb_impair: all passed\n", failures);
    return failures ? 1 : 0;
}
