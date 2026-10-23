// LTE band of a cell (#164): bands overlap, so a frequency alone does not name the band. Band 66 and 71 cells were shown as 64 (SIB1's
// freqBandIndicator is 64 for every band above 64), and 4, 13, 14 as 1, 20, 28 (the first band that holds the frequency was shown as if known).
// Values from 36.101 Table 5.7.3-1: N_DL = N_Offs-DL + 10 (F_DL - F_DL_low).
#include "dect2/cell/cell_rx.h"
#include "dect2/cell/cell_asn.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

static bool has(const std::vector<int>& v, int b) { return std::find(v.begin(), v.end(), b) != v.end(); }

int main() {
    {   // table-driven: F_DL = F_DL_low + 0.1 (N - N_Offs-DL), 36.101 5.7.3-1
        struct T { int band, n; double mhz; } t[] = {{31, 9870, 462.5}, {31, 9885, 464.0}, {31, 9892, 464.7}, {31, 9919, 467.4}, {28, 9210, 758.0}, {20, 6150, 791.0},
                                                     {72, 68936, 461.0}, {87, 70546, 420.0}, {14, 5280, 758.0}, {30, 9770, 2350.0}, {29, 9660, 717.0}};
        for (const auto& e : t) {
            CHECK(std::fabs(lteEarfcnToHz(e.n) - e.mhz * 1e6) < 1, "EARFCN %d = %.1f MHz", e.n, e.mhz);
            CHECK(lteBandOfEarfcn(e.n) == e.band, "EARFCN %d is B%d", e.n, e.band);
            CHECK(lteNearestEarfcn(e.mhz * 1e6, e.band) == e.n, "%.1f MHz in B%d is EARFCN %d", e.mhz, e.band, e.n);
        }
        CHECK(has(lteBandsOfHz(464.0e6), 31) && lteBandInfo(31) && !lteBandInfo(31)->tdd, "464.0 MHz holds B31 (FDD)");
        CHECK(lteBandOfEarfcn(9920) == 32 && lteBandOfEarfcn(9919) == 31, "B31 ends at 9919, B32 starts at 9920");
    }
    // overlaps the reports were about
    const auto b2125 = lteBandsOfHz(2125e6);
    CHECK(has(b2125, 1) && has(b2125, 4) && has(b2125, 10) && has(b2125, 66) && has(b2125, 65) && !has(b2125, 25), "2125 MHz holds B1, 4, 10, 65, 66");
    const auto b765 = lteBandsOfHz(765e6);
    CHECK(has(b765, 14) && has(b765, 28), "765 MHz holds B14 and B28");
    CHECK(has(lteBandsOfHz(750e6), 13) && !has(lteBandsOfHz(750e6), 20), "750 MHz is B13, not B20");
    CHECK(lteBandsOfHz(640e6) == std::vector<int>{71}, "640 MHz is only B71");

    // EARFCN of the same frequency differs per band
    CHECK(lteNearestEarfcn(2140e6, 66) == 66736 && lteNearestEarfcn(2140e6, 4) == 2250 && lteNearestEarfcn(2140e6, 1) == 300, "2140 MHz: B66 66736, B4 2250, B1 300");
    CHECK(lteNearestEarfcn(617e6) == 68586 && lteBandOfEarfcn(68586) == 71 && lteBandOfEarfcn(66736) == 66 && lteBandOfEarfcn(5280) == 14 && lteBandOfEarfcn(5180) == 13, "EARFCN -> band");

    // SIB1 freqBandIndicator 64: a band above 64, from the frequency
    LteBandChoice c = lteBandFor(640e6, 64);
    CHECK(c.band == 71 && c.confirmed && c.also.empty(), "B71 from freqBandIndicator 64: band %d confirmed %d", c.band, c.confirmed);
    c = lteBandFor(2140e6, 64);
    CHECK(c.band > 64 && !c.confirmed && c.also.size() == 1 && ((c.band == 65 && c.also[0] == 66) || (c.band == 66 && c.also[0] == 65)), "B65 / B66 are both above 64: not confirmed, the other is listed");
    CHECK(lteBandFor(1000e6, 64).band == 0, "no band above 64 holds 1000 MHz");

    // SIB1 with a real band: that band, the others as 'also'
    c = lteBandFor(2125e6, 4);
    CHECK(c.band == 4 && c.confirmed && has(c.also, 1) && has(c.also, 10) && has(c.also, 66) && !has(c.also, 4), "B4 from SIB1, also B1 / 10 / 66");
    c = lteBandFor(765e6, 14);
    CHECK(c.band == 14 && c.confirmed && has(c.also, 28), "B14 stays B14 (not B28)");
    c = lteBandFor(750e6, 13);
    CHECK(c.band == 13 && c.confirmed, "B13 stays B13");

    // no SIB1 yet: a guess, flagged as one, with the alternatives
    c = lteBandFor(2125e6, 0);
    CHECK(c.band > 0 && !c.confirmed && c.also.size() >= 3, "before SIB1: a guess with alternatives (%zu)", c.also.size());
    c = lteBandFor(640e6, 0);
    CHECK(c.band == 71 && !c.confirmed, "a single candidate before SIB1 is still unconfirmed");
    CHECK(lteBandFor(1000e6, 0).band == 0, "outside every band");
    // SIB1's extensions (36.331 v890 / v8h0 / v9e0 / v920): a band above 64 from freqBandIndicator-v9e0 and the MFBI list
    {
        using namespace dect2::cell;
        struct T { int band; std::vector<int> multi; bool ims; } t[] = {{66, {4}, false}, {71, {}, false}, {106, {8}, true}, {8, {106, 26}, false}, {20, {}, true}, {3, {}, false}};
        for (const auto& e : t) {
            LteSib1 s;
            Plmn p; p.mcc = 310; p.mnc = 410; p.mncDigits = 3; s.plmns.push_back(p);
            Plmn q; q.mcc = 313; q.mnc = 100; q.mncDigits = 3; s.plmns.push_back(q);
            s.tac = 77; s.cellId = 0x1234567; s.band = e.band; s.multiBands = e.multi; s.imsEmergency = e.ims;
            s.sched = {{16, {3}}}; s.siWindowMs = 20;
            LteBcch b;
            CHECK(lteParseBcch(lteEncodeSib1(s), b) && b.isSib1, "sib1 B%d parses", e.band);
            CHECK(b.sib1.band == e.band, "band %d read as %d", e.band, b.sib1.band);
            CHECK(b.sib1.multiBands == e.multi, "B%d MFBI list (%zu read)", e.band, b.sib1.multiBands.size());
            CHECK(b.sib1.imsEmergency == e.ims && b.sib1.plmns.size() == 2 && b.sib1.tac == 77, "B%d other fields", e.band);
        }
        const LteBandChoice c = lteBandFor(2140e6, 66);
        CHECK(c.band == 66 && c.confirmed, "band 66 from v9e0 is confirmed");
    }
    return testSummary("cell_bands");
}
