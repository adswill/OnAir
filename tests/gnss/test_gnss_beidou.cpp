// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// BeiDou B1I: the ranging codes, the BCH(15,11,1) code and its interleaving checked against the ICD; then the simulated sky on its own tune
// (1561.098 MHz): the MEO and IGSO satellites with the D1 message under the NH code, the GEO satellites with D2, the ephemeris compared with what
// was sent, the position fixed with BeiDou alone; and GPS and BeiDou together in one 20 Msps tune at 1568.26 MHz, BeiDou in the combined fix with
// the broadcast BDT - GPS offset or a clock unknown of its own. The faults of tests/common/impair.h: +-5 kHz, a sample clock 20 ppm off, low C/N0, a start
// in the middle of a subframe.
#include "dect2/gnss/gnss_bds.h"
#include "dect2/gnss/gnss_codes.h"
#include "dect2/gnss/gnss_testkit.h"
#include "dect2/engine/test_parallel.h"
#include "gnss_faults.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "dect2/common/mathconst.h"
using namespace dect2;
using namespace dect2::gnsstest;
using dect2::testpar::CaseOut;
#define CHECK(c, ...) do { if (!(c)) out.fail(__LINE__, __VA_ARGS__); } while (0)

static int unitChecks() {
    int fails = 0;
    auto fail = [&](const char* what) { printf("FAIL: %s\n", what); fails++; };
    // ICD Table 5-2: the syndrome D3D2D1D0 of a single error and the 15 bit pattern that corrects it (the first bit sent on the left)
    static const char* rom[16] = {"000000000000000", "000000000000001", "000000000000010", "000000000010000", "000000000000100", "000000100000000",
                                  "000000000100000", "000010000000000", "000000000001000", "100000000000000", "000001000000000", "000000010000000",
                                  "000000001000000", "010000000000000", "000100000000000", "001000000000000"};
    for (int syn = 1; syn < 16; syn++) {
        unsigned pat = 0;
        for (int k = 0; k < 15; k++) pat = pat << 1 | (unsigned)(rom[syn][k] - '0');
        // a codeword with that error must be corrected by flipping exactly that bit
        const unsigned info = 0x5A3u;
        const unsigned cw = (info << 4) | bch1511Parity(info);
        unsigned rx = cw ^ pat;
        if (bch1511Correct(&rx) != 1 || rx != cw) fail("BCH correction differs from ICD Table 5-2");
        // and the remainder of the error pattern is the syndrome of the table
        unsigned r = pat;
        for (int b = 14; b >= 4; b--) if (r & (1u << b)) r ^= 0x13u << (b - 4);
        if ((int)r != syn) fail("BCH syndrome differs from ICD Table 5-2");
    }
    // a subframe through the interleaver and back, one error in every block
    uint8_t info[300] = {}, tx[300], back[300];
    unsigned seed = 12345;
    for (int i = 0; i < 300; i++) { seed = seed * 1103515245u + 12345u; info[i] = (uint8_t)((seed >> 16) & 1u); }
    bdsPut(info, 1, 11, kBdsPreamble);
    bdsEncodeSubframe(info, tx);
    for (int w = 1; w < 10; w++) { tx[30 * w + 2 * (w % 11)] ^= 1; tx[30 * w + 2 * ((w + 3) % 15) + 1] ^= 1; }
    tx[20] ^= 1;
    const int fixed = bdsDecodeSubframe(tx, back);
    for (int w = 0; w < 10; w++)
        for (int k = 0; k < (w == 0 ? 26 : 22); k++)
            if (back[30 * w + k] != info[30 * w + k]) { fail("subframe round trip"); w = 10; break; }
    if (fixed != 19) fail("expected 19 corrected bits");
    // the ranging codes: balanced to within a chip or two after the truncation, distinct, low cross-correlation
    uint8_t a[kBdsLen], b[kBdsLen];
    for (int p = 1; p <= kBdsMaxPrn; p++) {
        if (!bdsB1iChips(p, a)) { fail("no code"); continue; }
        int ones = 0;
        for (int i = 0; i < kBdsLen; i++) ones += a[i];
        if (std::abs(2 * ones - kBdsLen) > 66) fail("code not balanced");
        bdsB1iChips(p % kBdsMaxPrn + 1, b);
        int worst = 0;
        for (int s = 0; s < kBdsLen; s++) {
            int c = 0;
            for (int i = 0; i < kBdsLen; i++) c += (a[i] ^ b[(i + s) % kBdsLen]) ? -1 : 1;
            worst = std::max(worst, std::abs(c));
        }
        if (worst > 200) fail("cross-correlation too high");
    }
    // ICD v3.0 Table 4-1 three-tap codes are new sequences, not copies of the BDS-2 ones
    for (int p = 38; p <= kBdsMaxPrn; p++) {
        bdsB1iChips(p, a);
        for (int q = 1; q < p; q++) { bdsB1iChips(q, b); if (!memcmp(a, b, kBdsLen)) { fail("duplicate ranging code"); break; } }
    }
    if (!bdsIsGeo(1) || !bdsIsGeo(5) || bdsIsGeo(6) || !bdsIsGeo(59) || !bdsIsGeo(63)) fail("GEO ranging codes");
    return fails;
}

struct Case { const char* name; gnssfault::Faults f; bool warm; double secs; bool combined = false; bool offset = true; };

static void runCase(const Case& k, CaseOut& out) {
    GnssSimConfig cfg;
    cfg.systems = gnssSystemBit(GnssBeidou) | (k.combined ? gnssSystemBit(GnssGps) : 0u);
    cfg.warmStart = k.warm;
    cfg.bdsGpsOffset = k.offset;
    const double center = k.combined ? 1568.259e6 : kBdsB1Hz;
    const double rate = k.combined ? 20e6 : 5e6;
    cfg.centerHz = center;
    GnssSim sim(cfg, rate);
    int nGps = 0;
    std::vector<size_t> bds;
    for (size_t i = 0; i < sim.sats().size(); i++) {
        const GnssSimSat& s = sim.sats()[i];
        if (!s.transmitted) continue;
        if (s.sys == GnssGps) nGps++;
        if (s.sys == GnssBeidou) bds.push_back(i);
    }
    Options o;
    o.rate = rate; o.secs = k.secs; o.chunk = k.combined ? 64000 : 50000;
    o.centerMhz = center / 1e6;
    o.hook = gnssfault::hook(k.f, o.rate);
    GpsEphemeris eph[64];
    bool hasEph[64] = {};
    o.after = [&](GnssReceiver& rx, size_t done) {
        if (done + o.chunk < (size_t)(k.secs * o.rate)) return;
        for (int p = 1; p <= kBdsMaxPrn; p++) hasEph[p] = rx.getEphemerisOf(GnssBeidou, p, eph[p]);
    };
    Run r = run(sim, o);
    const GnssTelemetry& t = r.tel;
    double hz = 0, vt = 0;
    if (t.fix.valid) fixError(sim, t.fix, &hz, &vt);
    out.print("%s: %s, error %.2f / %.2f m, %d GPS + %d BeiDou in the fix (%d + %zu in view), BDT-GPS %s %.1f ns\n", k.name, gnssSummary(t).c_str(), hz, vt,
              t.fix.nSatsPerSystem[GnssGps], t.fix.nSatsPerSystem[GnssBeidou], nGps, bds.size(), t.fix.bdtGpsApplied ? "applied" : "estimated", t.fix.bdtGpsNs);
    double lockB = -1, ephB = -1, fixB = -1;
    for (auto& rep : r.reports) {
        for (auto& c : rep.channels) if (c.sys == GnssBeidou && c.state >= GnssChLocked && lockB < 0) lockB = rep.signalSecs;
        for (auto& n : rep.nav) if (n.sys == GnssBeidou && n.hasEphemeris && ephB < 0) ephB = rep.signalSecs;
        if (rep.fix.valid && rep.fix.nSatsPerSystem[GnssBeidou] > 0 && fixB < 0) fixB = rep.signalSecs;
    }
    out.print("  first BeiDou lock %.0f s, ephemeris %.0f s, in the fix %.0f s\n", lockB, ephB, fixB);
    const bool weak = k.f.noiseSnrDb < 1e8;
    CHECK(bds.size() >= 6, "only %zu BeiDou satellites in view", bds.size());
    CHECK(t.fix.valid, "no fix");
    CHECK(hz < 15.0 && std::fabs(vt) < 25.0, "position error %.1f / %.1f m", hz, vt);
    CHECK((t.decodeMask & gnssSystemBit(GnssBeidou)) != 0, "BeiDou not in the decode mask");
    CHECK((t.activeMask & gnssSystemBit(GnssBeidou)) != 0, "the B1I band not active");
    if (!k.combined) CHECK((t.activeMask & gnssSystemBit(GnssGps)) == 0, "L1 cannot fit on this tune");
    int high = 0, geoHigh = 0;
    for (size_t i : bds) { high += sim.sats()[i].elDeg >= 10.0; geoHigh += sim.sats()[i].elDeg >= 10.0 && sim.sats()[i].eph.geo; }
    CHECK(t.fix.nSatsPerSystem[GnssBeidou] >= high - (weak ? 3 : 1), "%d BeiDou satellites in the fix, %d above 10 degrees", t.fix.nSatsPerSystem[GnssBeidou], high);
    if (k.combined) {
        CHECK(t.fix.nSatsPerSystem[GnssGps] >= 4, "GPS left the combined fix");
        const double isb = t.fix.clockBiasM[GnssBeidou] - t.fix.clockBiasM[GnssGps];
        out.print("  receiver clock against BDT minus against GPS: %+.2f m (expected %+.2f)\n", isb, -dect2::math::kSpeedOfLight * 30e-9);
        CHECK(std::fabs(isb + dect2::math::kSpeedOfLight * 30e-9) < (t.fix.bdtGpsApplied ? 0.5 : 6.0), "inter-system clock %.2f m", isb);
        if (k.offset) CHECK(t.fix.bdtGpsApplied && std::fabs(t.fix.bdtGpsNs - 30.0) < 0.1, "BDT-GPS offset not applied (%.2f ns)", t.fix.bdtGpsNs);
        else CHECK(!t.fix.bdtGpsApplied && t.fix.systemInFix[GnssBeidou], "without the broadcast offset BeiDou needs its own clock unknown");
    }
    int ephOk = 0, geoOk = 0, bds3Ok = 0, bds3GeoOk = 0, bds3High = 0, bds3GeoHigh = 0;
    for (size_t i : bds) if (sim.sats()[i].elDeg >= 10.0 && sim.sats()[i].prn >= 38) (sim.sats()[i].eph.geo ? bds3GeoHigh : bds3High)++;
    for (size_t i : bds) {
        const GnssSimSat& s = sim.sats()[i];
        int n = 0; double sum = 0, sum2 = 0;
        for (auto& ep : r.meas) for (auto& m : ep) {
            if (m.sys != GnssBeidou || m.prn != s.prn) continue;
            const double tSim = m.rxTime / (1.0 + k.f.clockPpm * 1e-6) + (double)k.f.skip / o.rate;
            const double e = (m.txTow - (sim.svTransmitTime(i, sim.trueTime(tSim)) - kBdtGpsSeconds)) * dect2::math::kSpeedOfLight;
            sum += e; sum2 += e * e; n++;
        }
        const GnssChannel* ch = nullptr;
        for (auto& c : t.channels) if (c.sys == GnssBeidou && c.prn == s.prn) ch = &c;
        out.print("  %s %s el %4.1f: %s, C/N0 %.1f, subframes %u good %u bad, range error %+.2f m rms %.2f m (%d s)%s\n", gnssSatName(GnssBeidou, s.prn).c_str(),
                  s.eph.geo ? "GEO" : "   ", s.elDeg, ch ? gnssChStateName(ch->state) : "not tracked", ch ? ch->cn0 : 0.f, ch ? ch->framesOk : 0u, ch ? ch->framesBad : 0u,
                  n ? sum / n : 0.0, n ? std::sqrt(sum2 / n) : 0.0, n, hasEph[s.prn] ? ", ephemeris" : "");
        if (n >= 10) CHECK(std::sqrt(sum2 / n) < 8.0 && std::fabs(sum / n) < 4.0, "%s: range error %.2f m rms %.2f", gnssSatName(GnssBeidou, s.prn).c_str(), sum / n, std::sqrt(sum2 / n));
        if (ch && !weak) CHECK(ch->framesBad <= 2, "%s: %u subframes failed", gnssSatName(GnssBeidou, s.prn).c_str(), ch->framesBad);
        if (hasEph[s.prn]) {
            const GpsEphemeris& a = eph[s.prn];
            const GpsEphemeris& b = s.eph;
            const bool same = a.sqrtA == b.sqrtA && a.e == b.e && a.m0 == b.m0 && a.omega0 == b.omega0 && a.i0 == b.i0 && a.omega == b.omega && a.omegaDot == b.omegaDot &&
                              a.idot == b.idot && a.dn == b.dn && a.cuc == b.cuc && a.cus == b.cus && a.crc == b.crc && a.crs == b.crs && a.cic == b.cic && a.cis == b.cis &&
                              a.toe == b.toe && a.toc == b.toc && a.af0 == b.af0 && a.af1 == b.af1 && a.af2 == b.af2 && a.tgd == b.tgd && a.iodc == b.iodc && a.health == 0 &&
                              a.beidou && a.geo == b.geo && a.wn == b.wn;
            CHECK(same, "%s: decoded ephemeris differs from the one sent", gnssSatName(GnssBeidou, s.prn).c_str());
            ephOk += same;
            geoOk += same && b.geo;
            bds3Ok += same && s.prn >= 38 && !b.geo;
            bds3GeoOk += same && s.prn >= 59 && b.geo;
        }
    }
    CHECK(ephOk >= high - (weak ? 3 : 1), "%d BeiDou ephemerides, %d satellites above 10 degrees", ephOk, high);
    out.print("  BDS-3 (PRN 38-63): %d MEO ephemerides of %d above 10 degrees, %d GEO of %d\n", bds3Ok, bds3High, bds3GeoOk, bds3GeoHigh);
    CHECK(bds3High + bds3GeoHigh > 0, "no BDS-3 satellite in view");
    CHECK(bds3Ok >= bds3High - (weak ? 2 : 0), "%d BDS-3 MEO ephemerides, %d in view", bds3Ok, bds3High);
    if (!weak) CHECK(bds3GeoOk >= bds3GeoHigh, "%d BDS-3 GEO ephemerides of %d in view", bds3GeoOk, bds3GeoHigh);
    if (!weak) CHECK(geoOk >= geoHigh, "%d GEO (D2) ephemerides of %d in view", geoOk, geoHigh);
    int sky = 0;
    for (auto& s : t.sky) sky += s.sys == GnssBeidou;
    CHECK(sky >= ephOk, "%d BeiDou satellites in the sky plot, %d ephemerides", sky, ephOk);
}

int main() {
    const int uf = unitChecks();
    if (uf) { printf("%d unit failure(s)\n", uf); return 1; }
    // 50000 samples at 5 Msps: 5 kHz is 50 whole cycles in a chunk
    const Case cases[] = {
        {"clean", {}, true, 75},
        {"+5 kHz offset", {5000.0, 0, 1e9, 0}, true, 75},
        {"-5 kHz offset, sample clock +20 ppm", {-5000.0, 20, 1e9, 0}, true, 75},
        {"low C/N0 (noise doubled), sample clock -20 ppm", {0.0, -20, 0.0, 0}, true, 90},
        {"start 2.7 s into the stream (in a subframe), cold", {0.0, 0, 1e9, 13500000}, false, 95},
        {"GPS + BeiDou at 1568.26 MHz, 20 Msps, broadcast BDT-GPS", {}, true, 80, true, true},
        {"GPS + BeiDou, no BDT-GPS in the message: BeiDou with its own clock", {}, true, 80, true, false},
    };
    if (getenv("GNSS_CASE")) { CaseOut out; runCase(cases[atoi(getenv("GNSS_CASE"))], out); fputs(out.text.c_str(), stdout); return out.fails ? 1 : 0; }
    const int fails = dect2::testpar::runCases(7, [&](size_t i, CaseOut& out) { runCase(cases[i], out); }, {5, 6, 4, 3, 0, 1, 2});
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("gnss beidou: ok\n");
    return 0;
}
