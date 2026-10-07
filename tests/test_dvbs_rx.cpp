// DVB-S/S2 receiver: generator -> receiver round trips. Every case sends a transport stream whose packets can be checked one by one through the
// whole chain (pulse shaping, 8-bit quantisation, AGC, symbol rate search, timing, carrier, frame search, FEC) and requires every packet to arrive
// intact and in order once the receiver has locked.
#include "dect2/dvbs_testkit.h"
#include <cstdio>
#include <cstdlib>

using namespace dect2;
using namespace dect2::dvbs;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// what a round trip has to deliver
static void expectClean(const RunResult& r, const char* what, double minPackets, double maxLockSecs) {
    CHECK(r.good >= minPackets, "%s: only %llu good packets (wanted %.0f); %s", what, (unsigned long long)r.good, minPackets, dvbsSummary(r.tel).c_str());
    CHECK(r.bad == 0, "%s: %llu damaged packets", what, (unsigned long long)r.bad);
    CHECK(r.gaps == 0, "%s: %llu jumps in the packet counter (%llu packets lost)", what, (unsigned long long)r.gaps, (unsigned long long)r.lost);
    if (std::getenv("CI")) maxLockSecs *= 3;   // the shared CI machines run several tests at once
    CHECK(r.firstPacketSecs >= 0 && r.firstPacketSecs <= maxLockSecs, "%s: first packet after %.2f s (limit %.2f s)", what, r.firstPacketSecs, maxLockSecs);
}

int main() {
    // ---- DVB-S: every code rate, QPSK, 2 Msym/s at 4 Msps, 8 bit samples, 12 dB
    for (int rate = 0; rate < 5; rate++) {
        RunConfig rc = makeRun(1, kQpsk, rate, 2e6, 4e6, 12.0, 2.5);
        const RunResult r = runCase(rc);
        static const char* rn[5] = {"1/2", "2/3", "3/4", "5/6", "7/8"};
        char what[64];
        snprintf(what, sizeof what, "DVB-S %s", rn[rate]);
        const double perSec = dvbsNetBitrate(rc.sig.tx) / 1504.0;       // transport stream packets a second
        expectClean(r, what, perSec * 1.6, 0.9);
        CHECK(r.tel.standard == 1 && r.tel.tsLock && r.tel.codeRate == rn[rate], "%s: reported as %s", what, dvbsSummary(r.tel).c_str());
        CHECK(std::fabs(r.tel.symbolRate / 2e6 - 1.0) < 2e-4, "%s: symbol rate %.1f Hz", what, r.tel.symbolRate);
        printf("%s: %llu packets, first after %.2f s, MER %.1f dB, symbol rate %.1f Hz\n", what, (unsigned long long)r.good, r.firstPacketSecs, r.tel.merDb, r.tel.symbolRate);
    }

    // ---- DVB-S2: a spread of MODCODs at 4 dB above the Es/N0 of table 13, 2 Msym/s at 4 Msps
    struct Mc { int mod, rate; };
    const Mc mcs[] = {{kQpsk, 0}, {kQpsk, 3}, {kQpsk, 5}, {kQpsk, 10}, {k8psk, 4}, {k8psk, 5}, {k8psk, 10}, {k16apsk, 5}, {k16apsk, 10}, {k32apsk, 6}, {k32apsk, 10}};
    for (const Mc& m : mcs) {
        RunConfig rc = makeRun(2, m.mod, m.rate, 2e6, 4e6, s2QefEsN0(m.mod, m.rate, false) + 4.0, 3.0);
        const RunResult r = runCase(rc);
        char what[64];
        snprintf(what, sizeof what, "DVB-S2 %s %s", s2ModName(m.mod), s2RateName(m.rate));
        const double net = dvbsNetBitrate(rc.sig.tx) / 1504.0;
        expectClean(r, what, net * 1.8, 1.0);
        CHECK(r.tel.standard == 2 && r.tel.tsLock && r.tel.modulation == m.mod && r.tel.codeRate == s2RateName(m.rate) && r.tel.frameSize == 1, "%s: reported as %s", what, dvbsSummary(r.tel).c_str());
        CHECK(std::fabs(r.tel.symbolRate / 2e6 - 1.0) < 2e-4, "%s: symbol rate %.1f Hz", what, r.tel.symbolRate);
        printf("%s: %llu packets, first after %.2f s, MER %.1f dB, symbol rate %.1f Hz, LDPC iterations %.1f\n", what, (unsigned long long)r.good, r.firstPacketSecs, r.tel.merDb, r.tel.symbolRate, r.tel.ldpcIterAvg);
    }

    // ---- DVB-S2 frame variants: short frames, pilots, both
    for (int v = 1; v < 4; v++) {
        RunConfig rc = makeRun(2, k8psk, 5, 2e6, 4e6, s2QefEsN0(k8psk, 5, false) + 4.0, 3.0);
        rc.sig.tx.shortFrame = (v & 1) != 0;
        rc.sig.tx.pilots = (v & 2) != 0;
        const RunResult r = runCase(rc);
        char what[64];
        snprintf(what, sizeof what, "DVB-S2 8PSK 2/3%s%s", rc.sig.tx.shortFrame ? " short" : "", rc.sig.tx.pilots ? " pilots" : "");
        expectClean(r, what, dvbsNetBitrate(rc.sig.tx) / 1504.0 * 1.8, 1.0);
        CHECK(r.tel.frameSize == (rc.sig.tx.shortFrame ? 2 : 1) && r.tel.pilots == rc.sig.tx.pilots, "%s: reported as %s", what, dvbsSummary(r.tel).c_str());
        printf("%s: %llu packets, first after %.2f s\n", what, (unsigned long long)r.good, r.firstPacketSecs);
    }

    // ---- spectral inversion (an LNB with a high side local oscillator): both standards
    for (int std = 1; std <= 2; std++) {
        RunConfig rc = makeRun(std, kQpsk, std == 1 ? 1 : 5, 2e6, 4e6, 12.0, 2.5);
        rc.sig.inverted = true;
        const RunResult r = runCase(rc);
        char what[64];
        snprintf(what, sizeof what, "%s inverted", std == 1 ? "DVB-S QPSK 2/3" : "DVB-S2 QPSK 2/3");
        expectClean(r, what, dvbsNetBitrate(rc.sig.tx) / 1504.0 * 1.4, 1.2);
        CHECK(r.tel.inverted, "%s: not reported as inverted", what);
        printf("%s: %llu packets, first after %.2f s\n", what, (unsigned long long)r.good, r.firstPacketSecs);
    }

    // ---- roll-offs 0.25 and 0.20 (signalled in the BBHEADER of DVB-S2), the receiver finds them from the spectrum or the stream
    for (double ro : {0.25, 0.20}) {
        RunConfig rc = makeRun(2, kQpsk, 5, 2e6, 4e6, 12.0, 3.0);
        rc.sig.tx.rollOff = ro;
        const RunResult r = runCase(rc);
        char what[64];
        snprintf(what, sizeof what, "DVB-S2 roll-off %.2f", ro);
        expectClean(r, what, dvbsNetBitrate(rc.sig.tx) / 1504.0 * 1.8, 1.0);
        CHECK(std::fabs(r.tel.rollOff - ro) < 0.011 && r.tel.rollOffSource >= 1, "%s: roll-off %.2f (source %d)", what, r.tel.rollOff, r.tel.rollOffSource);
        printf("%s: %llu packets, roll-off %.2f (source %d)\n", what, (unsigned long long)r.good, r.tel.rollOff, r.tel.rollOffSource);
    }

    printf(fails ? "dvbs rx: FAILED (%d)\n" : "dvbs rx: ok\n", fails);
    return fails ? 1 : 0;
}
