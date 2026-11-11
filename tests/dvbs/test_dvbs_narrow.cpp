// DVB-S2 at low symbol rates (reduced bandwidth DATV, 3.9): QPSK 1/4 short frames with pilots at 25 to 33 ksym/s, RRC roll-off 0.20, found by Auto
// (the finer search spectrum) and set by hand, with a carrier off the centre, a transmitter clock offset and noise; and 10 ksym/s by hand.
// Before 3.9 the panel clamped the rate at 50 ksym/s and Auto started at 60 ksym/s. Every packet that comes out has to be right. Simulated signals only.
#include "dect2/dvbs/dvbs_testkit.h"
#include "../common/jobs.h"
#include <atomic>
#include <cstdio>

using namespace dect2;
using namespace dect2::dvbs;
using testjobs::jprintf;

static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: "); jprintf(__VA_ARGS__); jprintf("\n"); fails++; } } while (0)

static RunConfig narrow(double rs, double fs, double esn0, double secs, int rate = 0) {
    RunConfig a = makeRun(2, kQpsk, rate, rs, fs, esn0, secs);   // rate index 0: 1/4
    a.sig.tx.shortFrame = true;
    a.sig.tx.pilots = true;
    a.sig.tx.rollOff = 0.20;
    return a;
}

int main() {
    CHECK(dvbsMinSymbolRate() <= 10e3, "lowest symbol rate %.0f", dvbsMinSymbolRate());
    CHECK(dvbsMinAutoSymbolRate(2e6) <= 10e3 && dvbsMinAutoSymbolRate(10e6) <= 10e3, "Auto from %.0f / %.0f Hz", dvbsMinAutoSymbolRate(2e6), dvbsMinAutoSymbolRate(10e6));

    testjobs::Jobs jobs;
    struct Case { double rs, fs, esn0, cfo, ppm; bool manual; };
    const Case cases[] = {
        {32e3, 2e6, 6.0, 0, 0, false},           // the reported signal, Auto
        {32e3, 2e6, 6.0, 0, 0, true},            // by hand
        {25e3, 2e6, 4.0, 310e3, 40, false},      // off the centre, clock offset, weaker
        {33e3, 2e6, 3.0, -480e3, -60, false},
        {32e3, 10e6, 6.0, 1.2e6, 20, false},     // the default rate of the mode: the finer spectrum has 131072 bins
        {27.5e3, 4e6, 2.0, -75e3, 0, true},      // by hand, 2 dB Es/N0 (QPSK 1/4 needs about -2.4 dB)
    };
    for (const Case& c : cases) jobs.add([&, c] {
        RunConfig a = narrow(c.rs, c.fs, c.esn0, 16.0);
        a.sig.cfoHz = c.cfo; a.sig.clockPpm = c.ppm;
        if (c.manual) a.manualRs = c.rs;
        const RunResult r = runCase(a);
        char w[128];
        snprintf(w, sizeof w, "%.1f ksym/s at %.0f Msps, %.0f dB, %+.0f kHz, %+.0f ppm, %s", c.rs / 1e3, c.fs / 1e6, c.esn0, c.cfo / 1e3, c.ppm, c.manual ? "manual" : "auto");
        const double pps = dvbsNetBitrate(a.sig.tx) / 1504.0;
        jprintf("%s: %llu good %llu bad, first packet %.1f s, %.3f ksym/s, CFO %+.2f kHz (%.1f packets/s)\n", w, (unsigned long long)r.good, (unsigned long long)r.bad,
                r.firstPacketSecs, r.tel.symbolRate / 1e3, r.tel.cfoHz / 1e3, pps);
        // firstPacketSecs is signal time (the receiver runs blocking), not CPU time: the weakest case (4 dB Es/N0, 25 ksym/s) locks after 9.6 s of signal on Windows
        // (other noise rounding) and 10.5 s still leaves the 5 s of packets the next check needs in the 16 s run
        CHECK(r.firstPacketSecs >= 0 && r.firstPacketSecs < 10.5, "%s: first packet after %.1f s", w, r.firstPacketSecs);
        CHECK(r.good >= pps * 5.0, "%s: %llu good packets (wanted %.0f)", w, (unsigned long long)r.good, pps * 5.0);
        CHECK(r.bad == 0 && r.gaps == 0, "%s: %llu damaged, %llu jumps", w, (unsigned long long)r.bad, (unsigned long long)r.gaps);
        CHECK(std::fabs(r.tel.symbolRate / (c.rs * (1 + c.ppm * 1e-6)) - 1.0) < 1e-3, "%s: symbol rate %.2f Hz", w, r.tel.symbolRate);
        CHECK(std::fabs(r.tel.cfoHz - c.cfo) < 0.05 * c.rs, "%s: CFO %+.1f Hz", w, r.tel.cfoHz);
    });
    // 10 ksym/s by hand (QPSK 1/2 short with pilots: about 6 packets a second)
    jobs.add([&] {
        RunConfig a = narrow(10e3, 2e6, 8.0, 16.0, 4);
        a.sig.cfoHz = 2.5e3;
        a.manualRs = 10e3;
        const RunResult r = runCase(a);
        jprintf("10 ksym/s manual: %llu good %llu bad, first packet %.1f s\n", (unsigned long long)r.good, (unsigned long long)r.bad, r.firstPacketSecs);
        CHECK(r.good >= 20 && r.bad == 0 && r.gaps == 0, "10 ksym/s manual: %llu good %llu bad %llu jumps", (unsigned long long)r.good, (unsigned long long)r.bad, (unsigned long long)r.gaps);
    });
    // noise only: the finer spectrum must not start the receiver on nothing
    jobs.add([&] {
        RunConfig a = narrow(32e3, 2e6, -30.0, 8.0);   // the carrier 30 dB under the noise in its own band: nothing to see
        const RunResult r = runCase(a);
        jprintf("noise only: %llu packets\n", (unsigned long long)r.packets);
        CHECK(r.packets == 0, "noise only: %llu packets", (unsigned long long)r.packets);
    });
    jobs.run();
    jprintf(fails ? "dvbs narrow: FAILED\n" : "dvbs narrow: ok\n");
    return fails ? 1 : 0;
}
