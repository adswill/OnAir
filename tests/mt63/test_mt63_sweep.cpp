// MT63 receiver sweeps over real-world impairments: the tuning error across the whole +-120 Hz of the specification for every width,
// the symbol clock error, sample rates from 250 kHz to 2 Msps, the signal level (the weakest that still decodes is printed), a signal that
// is already running when the receiver starts, and a retune in the middle.
#include "dect2/mt63/mt63_gen.h"
#include "dect2/mt63/mt63_rx.h"
#include "dect2/mt63/mt63_tel.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/test_long.h"

static Mt63Telemetry run(const Mt63GenSignal& s, double secs, double rate = 250000, double cfo = 0, double dial = -20000, bool retune = false) {
    Mt63GenConfig c;
    c.rate = rate; c.dialOffsetHz = dial; c.cfoHz = cfo; c.signals = {s}; c.seed = 11; c.noiseSigma = 0.02;
    Mt63Generator g(c);
    Mt63Receiver r;
    r.configure(rate);
    r.setSignalOffset(dial);
    Mt63Telemetry res;
    std::vector<cf32> x((size_t)(rate / 10));
    uint64_t seq = 0;
    for (double t = 0; t < secs; t += 0.1) {
        g.generate(x.data(), x.size());
        if (retune && t > secs * 0.5 && t < secs * 0.5 + 0.05) r.reset();
        r.feed(x.data(), x.size());
        Mt63Telemetry tt;
        if (r.telemetry(tt, seq)) { seq = tt.seq; res = std::move(tt); }
    }
    return res;
}

static Mt63GenSignal sig(int bw, bool lng, double centre, double snr) {
    Mt63GenSignal s;
    s.bandwidth = bw; s.longInterleave = lng; s.centreHz = centre; s.snrDb = snr; s.pauseSec = 1.0;
    s.text = "CQ CQ DE LY2ABC LY2ABC K\n";
    return s;
}
static double secs(int bw) { return bw == 500 ? 50 : bw == 1000 ? 36 : 30; }
static bool ok(const Mt63Telemetry& t, int bw) { return t.bandwidth == bw && t.text.find("LY2ABC") != std::string::npos; }

int main() {
    // the default run: both ends of the tuning range for every width, the largest clock error of each sign, the lowest and highest rate, and the
    // required 8 dB rung of the level search; ONAIR_TEST_LONG=1 the whole sweep and the search down to -4 dB
    const bool L = testLong();
    for (int bw : {500, 1000, 2000})
        for (double cfo : {-120.0, -77.0, -31.0, 0.0, 17.0, 59.0, 120.0}) {
            if (!L && std::fabs(cfo) != 120.0) continue;
            const auto t = run(sig(bw, false, 1700, 15), secs(bw), 250000, cfo);
            printf("  bw %4d cfo %+5.0f Hz: centre %7.1f %s\n", bw, cfo, t.centreHz, ok(t, bw) ? "ok" : "MISS");
            CHECK(ok(t, bw), "bw %d cfo %.0f: bw %d text \"%s\"", bw, cfo, t.bandwidth, t.text.c_str());
        }
    for (double ppm : {-300.0, -120.0, 120.0, 300.0})
        for (int bw : {500, 1000}) {
            if (!L && !((ppm == -300.0 && bw == 500) || (ppm == 300.0 && bw == 1000))) continue;
            Mt63GenSignal s = sig(bw, true, 1500, 15);
            s.clockPpm = ppm;
            const auto t = run(s, bw == 500 ? 90 : 60);
            printf("  bw %4d clock %+4.0f ppm: %s\n", bw, ppm, ok(t, bw) ? "ok" : "MISS");
            CHECK(ok(t, bw), "bw %d clock %.0f ppm: text \"%s\"", bw, ppm, t.text.c_str());
        }
    for (double rate : {250000.0, 500000.0, 1000000.0, 2000000.0}) {
        if (!L && rate != 250000.0 && rate != 2000000.0) continue;
        const auto t = run(sig(1000, false, 1500, 15), 36, rate);
        printf("  rate %.0f: %s\n", rate, ok(t, 1000) ? "ok" : "MISS");
        CHECK(ok(t, 1000), "rate %.0f: text \"%s\"", rate, t.text.c_str());
    }
    // the weakest level that still decodes (printed; the requirement is the first rung)
    for (int bw : {500, 1000, 2000}) {
        double lowest = 99;
        for (double snr = 8; snr >= (L ? -4 : 8); snr -= 2) {
            const auto t = run(sig(bw, false, 1500, snr), bw == 500 ? 110 : 80);
            if (ok(t, bw)) lowest = snr; else break;
        }
        printf("  bw %4d short: decodes down to %.0f dB (generator level in the signal bandwidth)\n", bw, lowest);
        CHECK(lowest <= 8, "bw %d does not decode at 8 dB", bw);
    }
    {   // a retune in the middle: the lock is dropped and found again
        const auto t = run(sig(1000, false, 1500, 15), 60, 250000, 0, -20000, true);
        printf("  retune: %s, %d syncs\n", ok(t, 1000) ? "ok" : "MISS", t.syncs);
        CHECK(ok(t, 1000) && t.syncs >= 1, "retune: text \"%s\"", t.text.c_str());
    }
    return testSummary("mt63 sweep");
}
