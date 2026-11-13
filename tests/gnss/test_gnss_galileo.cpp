// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Galileo E1-B next to GPS in the simulated sky: the BOC(1,1) signal is acquired and tracked, the I/NAV pages decoded (sync pattern, deinterleaving,
// Viterbi, CRC), the ephemeris assembled from words 1-5 and compared with what was sent, the GST-GPS offset of word type 10 read, and the position
// fixed with GPS and Galileo together (Galileo time moved to GPS time with that offset; before it has arrived, a clock unknown of its own).
// The same with the faults of tests/common/impair.h: +-5 kHz frequency offset, a sample clock 20 ppm off, low C/N0, and a start in the middle of a page.
#include "dect2/gnss/gnss_testkit.h"
#include "dect2/engine/test_parallel.h"
#include "gnss_faults.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "dect2/common/mathconst.h"
using namespace dect2;
using namespace dect2::gnsstest;
using dect2::testpar::CaseOut;
#define CHECK(c, ...) do { if (!(c)) out.fail(__LINE__, __VA_ARGS__); } while (0)

struct Case { const char* name; gnssfault::Faults f; int gpsSats; bool warm; double secs; bool ggto = true; };

static void runCase(const Case& k, CaseOut& out) {
    GnssSimConfig cfg;
    cfg.systems = gnssSystemBit(GnssGps) | gnssSystemBit(GnssGalileo);
    cfg.maxSats = k.gpsSats;
    cfg.warmStart = k.warm;
    cfg.galGgto = k.ggto;
    GnssSim sim(cfg, 4e6);
    int nGps = 0;
    std::vector<size_t> gal;
    for (size_t i = 0; i < sim.sats().size(); i++) {
        const GnssSimSat& s = sim.sats()[i];
        if (!s.transmitted) continue;
        if (s.sys == GnssGps) nGps++;
        if (s.sys == GnssGalileo) gal.push_back(i);
    }
    Options o;
    o.rate = 4e6; o.secs = k.secs; o.chunk = 64000;
    o.hook = gnssfault::hook(k.f, o.rate);
    GpsEphemeris eph[37];
    bool hasEph[37] = {};
    o.after = [&](GnssReceiver& rx, size_t done) {
        if (done + o.chunk < (size_t)(k.secs * o.rate)) return;      // the last chunk: read the ephemerides out
        for (int p = 1; p <= 36; p++) hasEph[p] = rx.getEphemerisOf(GnssGalileo, p, eph[p]);
    };
    Run r = run(sim, o);
    const GnssTelemetry& t = r.tel;
    double hz = 0, vt = 0;
    if (t.fix.valid) fixError(sim, t.fix, &hz, &vt);
    out.print("%s: %s, error %.2f / %.2f m, %d GPS + %d Galileo in the fix (%d + %zu in view), GST-GPS %s %.1f ns\n", k.name, gnssSummary(t).c_str(), hz, vt,
              t.fix.nSatsPerSystem[GnssGps], t.fix.nSatsPerSystem[GnssGalileo], nGps, gal.size(), t.fix.ggtoApplied ? "applied" : "estimated", t.fix.ggtoNs);
    double lockE = -1, ephE = -1, fixE = -1;
    for (auto& rep : r.reports) {
        for (auto& c : rep.channels) if (c.sys == GnssGalileo && c.state >= GnssChLocked && lockE < 0) lockE = rep.signalSecs;
        for (auto& n : rep.nav) if (n.sys == GnssGalileo && n.hasEphemeris && ephE < 0) ephE = rep.signalSecs;
        if (rep.fix.valid && rep.fix.nSatsPerSystem[GnssGalileo] > 0 && fixE < 0) fixE = rep.signalSecs;
    }
    out.print("  first Galileo lock %.0f s, ephemeris %.0f s, in the fix %.0f s\n", lockE, ephE, fixE);
    const bool weak = k.f.noiseSnrDb < 1e8;
    CHECK(gal.size() >= 5, "only %zu Galileo satellites in view", gal.size());
    CHECK(t.fix.valid, "no fix");
    CHECK(hz < 12.0 && std::fabs(vt) < 20.0, "position error %.1f / %.1f m", hz, vt);
    // the satellites 10 degrees or more above the horizon must make it; the lower ones (C/N0 near 30 dB-Hz) may come too late
    int galHigh = 0;
    for (size_t i : gal) galHigh += sim.sats()[i].elDeg >= 10.0;
    CHECK(t.fix.nSatsPerSystem[GnssGalileo] >= galHigh - (weak ? 2 : 0), "%d Galileo satellites in the fix, %d above 10 degrees", t.fix.nSatsPerSystem[GnssGalileo], galHigh);
    CHECK(t.fix.type.find("Galileo") != std::string::npos, "fix type '%s'", t.fix.type.c_str());
    // the clock against GST is the clock against GPS time less the 25 ns the simulation puts between them
    if (t.fix.systemInFix[GnssGalileo]) {
        const double isb = t.fix.clockBiasM[GnssGalileo] - t.fix.clockBiasM[GnssGps];
        out.print("  receiver clock against GST minus against GPS: %+.2f m (expected %+.2f)\n", isb, -dect2::math::kSpeedOfLight * 25e-9);
        CHECK(std::fabs(isb + dect2::math::kSpeedOfLight * 25e-9) < (t.fix.ggtoApplied ? 0.5 : 6.0), "inter-system clock %.2f m", isb);
    }
    if (!weak && k.ggto) CHECK(t.fix.ggtoApplied && std::fabs(t.fix.ggtoNs - 25.0) < 0.1, "GST-GPS offset not applied (%.2f ns)", t.fix.ggtoNs);
    if (!k.ggto) CHECK(!t.fix.ggtoApplied && t.fix.systemInFix[GnssGalileo], "without the broadcast offset Galileo needs its own clock unknown");
    int ephOk = 0;
    for (size_t i : gal) {
        const GnssSimSat& s = sim.sats()[i];
        // pseudorange accuracy: the GST transmit time the receiver read against the truth
        int n = 0; double sum = 0, sum2 = 0;
        for (auto& ep : r.meas) for (auto& m : ep) {
            if (m.sys != GnssGalileo || m.prn != s.prn) continue;
            const double tSim = m.rxTime / (1.0 + k.f.clockPpm * 1e-6) + (double)k.f.skip / o.rate;
            const double e = (m.txTow - sim.svTransmitTime(i, sim.trueTime(tSim))) * dect2::math::kSpeedOfLight;
            sum += e; sum2 += e * e; n++;
        }
        const GnssChannel* ch = nullptr;
        for (auto& c : t.channels) if (c.sys == GnssGalileo && c.prn == s.prn) ch = &c;
        out.print("  %s el %4.1f: %s, C/N0 %.1f, pages %u good %u bad, range error %+.2f m rms %.2f m (%d s)%s\n", gnssSatName(GnssGalileo, s.prn).c_str(), s.elDeg,
                  ch ? gnssChStateName(ch->state) : "not tracked", ch ? ch->cn0 : 0.f, ch ? ch->framesOk : 0u, ch ? ch->framesBad : 0u, n ? sum / n : 0.0,
                  n ? std::sqrt(sum2 / n) : 0.0, n, hasEph[s.prn] ? ", ephemeris" : "");
        if (n >= 10) CHECK(std::sqrt(sum2 / n) < 8.0 && std::fabs(sum / n) < 4.0, "%s: range error %.2f m rms %.2f", gnssSatName(GnssGalileo, s.prn).c_str(), sum / n, std::sqrt(sum2 / n));
        if (ch && !weak) CHECK(ch->framesBad <= 1, "%s: %u pages failed the CRC", gnssSatName(GnssGalileo, s.prn).c_str(), ch->framesBad);
        if (hasEph[s.prn]) {
            // the decoded ephemeris is exactly what the simulation sent (it keeps the values of the transmitted integers)
            const GpsEphemeris& a = eph[s.prn];
            const GpsEphemeris& b = s.eph;
            const bool same = a.sqrtA == b.sqrtA && a.e == b.e && a.m0 == b.m0 && a.omega0 == b.omega0 && a.i0 == b.i0 && a.omega == b.omega && a.omegaDot == b.omegaDot &&
                              a.idot == b.idot && a.dn == b.dn && a.cuc == b.cuc && a.cus == b.cus && a.crc == b.crc && a.crs == b.crs && a.cic == b.cic && a.cis == b.cis &&
                              a.toe == b.toe && a.toc == b.toc && a.af0 == b.af0 && a.af1 == b.af1 && a.tgd == b.tgd && a.iodc == b.iodc && a.health == 0 && a.galileo;
            CHECK(same, "%s: decoded ephemeris differs from the one sent", gnssSatName(GnssGalileo, s.prn).c_str());
            ephOk += same;
        }
    }
    CHECK(ephOk >= galHigh - (weak ? 2 : 0), "%d Galileo ephemerides, %d satellites above 10 degrees", ephOk, galHigh);
    // the sky plot and the channel table name the system
    int skyGal = 0;
    for (auto& s : t.sky) skyGal += s.sys == GnssGalileo;
    CHECK(skyGal >= ephOk, "%d Galileo satellites in the sky plot, %d ephemerides", skyGal, ephOk);
    CHECK((t.decodeMask & gnssSystemBit(GnssGalileo)) != 0, "Galileo not in the decode mask");
}

int main() {
    // chunks of 64000 samples: 5 kHz is 80 whole cycles in one, so impair::shift keeps the phase from chunk to chunk
    const Case cases[] = {
        {"clean", {}, 0, true, 85},
        {"+5 kHz offset, four GPS satellites", {5000.0, 0, 1e9, 0}, 4, true, 85},
        {"-5 kHz offset, sample clock +20 ppm", {-5000.0, 20, 1e9, 0}, 0, true, 85},
        {"low C/N0 (noise doubled), sample clock -20 ppm", {0.0, -20, 0.0, 0}, 0, true, 95},
        {"start 2.6 s into the stream (in a page), cold", {0.0, 0, 1e9, 10400000}, 0, false, 90},
        {"no GST-GPS offset in word type 10: Galileo with its own clock", {}, 0, true, 85, false},
    };
    if (getenv("GNSS_CASE")) { CaseOut out; runCase(cases[atoi(getenv("GNSS_CASE"))], out); fputs(out.text.c_str(), stdout); return out.fails ? 1 : 0; }
    const int fails = dect2::testpar::runCases(6, [&](size_t i, CaseOut& out) { runCase(cases[i], out); }, {3, 4, 0, 5, 1, 2});
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("gnss galileo: ok\n");
    return 0;
}
