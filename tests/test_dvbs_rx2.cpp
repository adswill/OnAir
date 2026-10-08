// DVB-S/S2 receiver, front end faults: carrier offset, symbol clock offset, signal level, DC offset, IQ imbalance, odd chunk sizes, the input sample rates the
// radios give and the range of symbol rates. Every packet that comes out has to be right.
#include "dect2/dvbs_testkit.h"
#include "jobs.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>

using namespace dect2;
using namespace dect2::dvbs;
using testjobs::jprintf;

static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: "); jprintf(__VA_ARGS__); jprintf("\n"); fails++; } } while (0)

static void clean(const RunResult& r, const char* what, double minPackets, double maxLock) {
    CHECK(r.good >= minPackets, "%s: %llu good packets (wanted %.0f); %s", what, (unsigned long long)r.good, minPackets, dvbsSummary(r.tel).c_str());
    CHECK(r.bad == 0, "%s: %llu damaged packets", what, (unsigned long long)r.bad);
    CHECK(r.gaps == 0, "%s: %llu jumps in the packet counter, %llu packets lost", what, (unsigned long long)r.gaps, (unsigned long long)r.lost);
    CHECK(r.firstPacketSecs >= 0 && r.firstPacketSecs <= maxLock, "%s: first packet after %.2f s", what, r.firstPacketSecs);
}

int main() {
    // the cases use DVB-S2 8PSK 3/4 (a modulation that cares about the carrier) and DVB-S QPSK 3/4, with 15 dB of Es/N0
    auto s2 = [](double rs, double fs, double secs) { return makeRun(2, k8psk, 6, rs, fs, 15.0, secs); };
    auto s1 = [](double rs, double fs, double secs) { return makeRun(1, kQpsk, 2, rs, fs, 15.0, secs); };

    // the cases are independent: each one runs on its own thread, the output keeps the order of the cases
    testjobs::Jobs jobs;

    // ---- carrier offset: up to a third of the symbol rate either way
    for (double cfo : {-600e3, -150e3, 40e3, 500e3}) jobs.add([&, cfo] {
        RunConfig a = s2(2e6, 4e6, 3.0); a.sig.cfoHz = cfo;
        RunResult r = runCase(a);
        char w[64]; snprintf(w, sizeof w, "DVB-S2 carrier %+.0f kHz", cfo / 1e3);
        clean(r, w, dvbsNetBitrate(a.sig.tx) / 1504.0 * 2.0, 1.2);
        CHECK(std::fabs(r.tel.cfoHz - cfo) < 3e3, "%s: reported %+.1f kHz", w, r.tel.cfoHz / 1e3);
        RunConfig b = s1(2e6, 4e6, 3.0); b.sig.cfoHz = cfo;
        r = runCase(b);
        snprintf(w, sizeof w, "DVB-S carrier %+.0f kHz", cfo / 1e3);
        clean(r, w, dvbsNetBitrate(b.sig.tx) / 1504.0 * 2.0, 1.2);
        jprintf("carrier %+.0f kHz: S2 and S1 ok\n", cfo / 1e3);
    });

    // ---- symbol clock offset (the transmitter's clock against the radio's): +-150 ppm; the receiver's symbol rate must follow
    for (double ppm : {-150.0, 150.0}) jobs.add([&, ppm] {
        RunConfig a = s2(2e6, 4e6, 4.0); a.sig.clockPpm = ppm;
        RunResult r = runCase(a);
        char w[64]; snprintf(w, sizeof w, "DVB-S2 clock %+.0f ppm", ppm);
        clean(r, w, dvbsNetBitrate(a.sig.tx) / 1504.0 * 3.0, 1.2);
        CHECK(std::fabs(r.tel.symbolRate / (2e6 * (1 + ppm * 1e-6)) - 1.0) < 5e-5, "%s: symbol rate %.2f Hz (%.2f expected)", w, r.tel.symbolRate, 2e6 * (1 + ppm * 1e-6));
        RunConfig b = s1(2e6, 4e6, 4.0); b.sig.clockPpm = ppm;
        r = runCase(b);
        snprintf(w, sizeof w, "DVB-S clock %+.0f ppm", ppm);
        clean(r, w, dvbsNetBitrate(b.sig.tx) / 1504.0 * 3.0, 1.2);
        jprintf("clock %+.0f ppm: S2 and S1 ok\n", ppm);
    });

    // ---- level: the receiver has its own AGC. -26 dBFS (about 6 steps of the 8 bit converter) and +10 dB (the signal clips)
    for (double scale : {0.25, 4.0}) jobs.add([&, scale] {
        RunConfig a = s2(2e6, 4e6, 3.0); a.scale = scale;
        a.sig.snrDb = 20.0;
        RunResult r = runCase(a);
        char w[64]; snprintf(w, sizeof w, "DVB-S2 level x%.2f", scale);
        clean(r, w, dvbsNetBitrate(a.sig.tx) / 1504.0 * 2.0, 1.2);
        jprintf("level x%.2f: ok, MER %.1f dB\n", scale, r.tel.merDb);
    });

    // ---- DC offset (the spike of a cheap radio) and IQ imbalance
    jobs.add([&] {
        RunConfig a = s2(2e6, 4e6, 3.0); a.sig.dcOffset = 0.4;
        RunResult r = runCase(a);
        clean(r, "DVB-S2 DC offset", dvbsNetBitrate(a.sig.tx) / 1504.0 * 2.0, 1.2);
        RunConfig b = s1(2e6, 4e6, 3.0); b.sig.dcOffset = 0.4;
        r = runCase(b);
        clean(r, "DVB-S DC offset", dvbsNetBitrate(b.sig.tx) / 1504.0 * 2.0, 1.2);
        RunConfig c = s2(2e6, 4e6, 3.0); c.sig.iqGainDb = 1.0; c.sig.iqPhaseDeg = 4.0;
        r = runCase(c);
        clean(r, "DVB-S2 IQ imbalance", dvbsNetBitrate(c.sig.tx) / 1504.0 * 2.0, 1.2);
        jprintf("DC offset and IQ imbalance: ok\n");
    });

    // ---- chunk sizes: 1, 7, 4096, 65536 samples, and a mix
    {
        const std::vector<std::vector<size_t>> patterns = {{1}, {7}, {4096}, {65536}, {1, 7, 333, 4096, 65536, 50000, 2, 12345}};
        for (const auto& p : patterns) jobs.add([&, p] {
            RunConfig a = s2(2e6, 4e6, p[0] == 1 ? 2.0 : 3.0);
            a.chunkPattern = p;
            RunResult r = runCase(a);
            char w[64]; snprintf(w, sizeof w, "DVB-S2 chunks of %zu%s", p[0], p.size() > 1 ? " and others" : "");
            clean(r, w, dvbsNetBitrate(a.sig.tx) / 1504.0 * (a.secs - 0.8), 1.2);
            jprintf("%s: ok (%llu packets)\n", w, (unsigned long long)r.good);
        });
    }

    // ---- input sample rates of the radios, with the symbol rate each can take (about 1.3 samples per symbol times (1 + roll-off))
    {
        struct R { double fs, rs; };
        const R rates[] = {{2.4e6, 1.2e6}, {3.2e6, 1.7e6}, {6e6, 3.3e6}, {8e6, 4.5e6}, {10e6, 5e6}, {12.5e6, 7.0e6}, {16e6, 9.0e6}, {20e6, 11.3e6}};
        for (const R& x : rates) jobs.add([&, x] {
            RunConfig a = makeRun(2, kQpsk, 5, x.rs, x.fs, 12.0, 2.5);
            RunResult r = runCase(a);
            char w[64]; snprintf(w, sizeof w, "DVB-S2 %.2f Msym/s at %.1f Msps", x.rs / 1e6, x.fs / 1e6);
            clean(r, w, dvbsNetBitrate(a.sig.tx) / 1504.0 * 1.5, 1.2);
            CHECK(std::fabs(r.tel.symbolRate / x.rs - 1.0) < 2e-4, "%s: symbol rate %.1f", w, r.tel.symbolRate);
            jprintf("%s: ok, rate %.0f Hz, max %.2f Msym/s\n", w, r.tel.symbolRate, dvbsMaxSymbolRate(x.fs) / 1e6);
            CHECK(x.rs <= dvbsMaxSymbolRate(x.fs) * 1.01, "%s: tested above the stated maximum", w);
        });
    }

    // ---- low symbol rates: from the spectrum where it can be measured, with a given rate below that
    jobs.add([&] {
        RunConfig a = makeRun(2, kQpsk, 5, 0.5e6, 2e6, 12.0, 8.0);
        RunResult r = runCase(a);
        clean(r, "DVB-S2 0.5 Msym/s at 2 Msps", dvbsNetBitrate(a.sig.tx) / 1504.0 * 4.5, 4.0);
        RunConfig b = makeRun(2, kQpsk, 5, 0.1e6, 4e6, 12.0, 30.0);
        b.manualRs = 0.1e6;
        r = runCase(b);
        clean(r, "DVB-S2 0.1 Msym/s at 4 Msps (rate given)", dvbsNetBitrate(b.sig.tx) / 1504.0 * 15, 14.0);
        jprintf("low symbol rates: ok (0.1 Msym/s locked after %.1f s)\n", r.firstPacketSecs);
    });
    jobs.run();

    jprintf(fails ? "dvbs rx2: FAILED (%d)\n" : "dvbs rx2: ok\n", fails.load());
    return fails ? 1 : 0;
}
