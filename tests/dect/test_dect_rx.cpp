// DECT receiver on the generated signal: base stations found with their identities, handset bursts counted, CRC forms, noise gives nothing.
#include "dect2/dect/dect_gen.h"
#include "dect2/dect/dect_rx.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static DectTelemetry run(const DectGenConfig& c, double sec) {
    DectGenerator g(c);
    DectReceiver rx;
    rx.configure(c.rate);
    rx.setSignalOffset(c.dialOffsetHz);
    std::vector<cf32> buf(8192);
    const size_t total = (size_t)(sec * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    DectTelemetry t;
    rx.telemetry(t, 0);
    return t;
}
static const DectBase* find(const DectTelemetry& t, uint64_t rfpi) { for (const auto& b : t.bases) if (b.rfpi == rfpi) return &b; return nullptr; }

int main() {
    {   // two base stations and handsets, clean
        DectGenConfig c; c.rate = 5e6; c.snrDb = 30; c.dialOffsetHz = -1e6; c.handset = true; c.bases = dectTestBases(0);
        const DectTelemetry t = run(c, 2.0);
        printf("  clean: %s, %llu good %llu bad, snr %.1f dB\n", dectSummary(t).c_str(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.snrDb);
        const DectBase* a = find(t, 0x0100A5C3D1ULL);
        const DectBase* b = find(t, 0x0100F31A22ULL);
        CHECK(a && b && t.bases.size() == 2, "%zu base stations", t.bases.size());
        CHECK(a && a->frames >= 150 && a->ntSeen >= 80 && a->qtSeen >= 80, "frames %llu nt %d qt %d (2 s = 200 frames)", a ? (unsigned long long)a->frames : 0ull, a ? a->ntSeen : 0, a ? a->qtSeen : 0);
        CHECK(a && a->emc == 0x0 + ((0x0100A5C3D1ULL >> 19) & 0xFFFF) && a->arc == 0, "emc %d arc %d", a ? a->emc : -1, a ? a->arc : -1);
        CHECK(a && a->qtLast == 0x0123456789ULL, "system information tail");
        CHECK(t.handsetBursts >= 100 && t.baseBursts >= 300, "bursts base %llu handset %llu", (unsigned long long)t.baseBursts, (unsigned long long)t.handsetBursts);
        CHECK(t.blocksBad <= t.blocksOk / 20, "bad %llu", (unsigned long long)t.blocksBad);
        CHECK(a && __builtin_popcount(a->slotsMask) <= 2, "one slot per base: mask %x", a ? a->slotsMask : 0);
        CHECK(t.crcPlain > 0 && t.crcInverted == 0, "crc forms %llu / %llu", (unsigned long long)t.crcPlain, (unsigned long long)t.crcInverted);
    }
    {   // the complemented CRC form is accepted and counted
        DectGenConfig c; c.rate = 5e6; c.snrDb = 30; c.dialOffsetHz = -1e6; c.bases = dectTestBases(1); c.bases[0].inverted = true;
        const DectTelemetry t = run(c, 1.0);
        CHECK(t.crcInverted > 50 && t.crcPlain == 0, "inverted %llu plain %llu", (unsigned long long)t.crcInverted, (unsigned long long)t.crcPlain);
    }
    {   // 28 dB with a 60 kHz carrier offset, at 8 Msps
        DectGenConfig c; c.rate = 8e6; c.snrDb = 28; c.cfoHz = 60e3; c.dialOffsetHz = -1e6; c.bases = dectTestBases(1);
        const DectTelemetry t = run(c, 2.0);
        const DectBase* a = find(t, 0x0100A5C3D1ULL);
        printf("  28 dB, +60 kHz, 8 Msps: %llu good, snr %.1f dB, cfo %.0f Hz\n", (unsigned long long)t.blocksOk, t.snrDb, t.cfoHz);
        CHECK(a && a->frames >= 80, "frames %llu", a ? (unsigned long long)a->frames : 0ull);
        CHECK(std::fabs(t.cfoHz - 60e3) < 10e3, "cfo %.0f", t.cfoHz);
    }
    {   // sensitivity does not depend on the input rate: 12 dB in-band SNR (the generator's deviation is +-288 kHz at every rate)
        const double rates[3] = {4.608e6, 8e6, 10e6};
        for (double r : rates) {
            DectGenConfig c; c.rate = r; c.snrDb = 12; c.dialOffsetHz = -1e6; c.bases = dectTestBases(1);
            const DectTelemetry t = run(c, 1.0);
            printf("  12 dB, %.3f Msps: %llu good of 100\n", r / 1e6, (unsigned long long)t.blocksOk);
            CHECK(t.blocksOk >= 90, "%.3f Msps: %llu fields", r / 1e6, (unsigned long long)t.blocksOk);
        }
    }
    {   // no noise at all (digital silence between the bursts, as a transmitter's own signal or a squelched recording has it): the power floor's
        // running sum once drifted below zero there and every sample after the first burst looked like a burst start (docs/QA-SWEEP-3.9.md)
        DectGenConfig c; c.rate = 5e6; c.snrDb = 400; c.dialOffsetHz = -1e6; c.handset = true; c.bases = dectTestBases(0);
        const DectTelemetry t = run(c, 1.0);
        CHECK(t.blocksOk >= 380, "no noise: %llu fields of 400", (unsigned long long)t.blocksOk);
    }
    {   // wrong CRC: counted bad, no base station listed
        DectGenConfig c; c.rate = 5e6; c.snrDb = 30; c.dialOffsetHz = -1e6; c.bases = dectTestBases(1); c.bases[0].badCrc = true;
        const DectTelemetry t = run(c, 1.0);
        CHECK(t.bases.empty() && t.blocksBad >= 50, "bad-CRC beacons: %zu bases, %llu bad", t.bases.size(), (unsigned long long)t.blocksBad);
    }
    {   // noise only
        DectGenConfig c; c.rate = 5e6; c.snrDb = 0; c.dialOffsetHz = -1e6; c.noiseOnly = true; c.bases = dectTestBases(1);
        const DectTelemetry t = run(c, 2.0);
        CHECK(t.blocksOk == 0 && t.bases.empty(), "noise gave %llu fields", (unsigned long long)t.blocksOk);
    }
    printf(fails ? "dect rx: %d FAILED\n" : "dect rx: all passed\n", fails);
    return fails ? 1 : 0;
}
