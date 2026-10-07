// GNSS receiver round trip: the simulated sky (8 satellites, 44 dB-Hz at the zenith, rounded to 8 bits like a HackRF) through the receiver for 40 s.
// The receiver must acquire and track the satellites, decode the navigation message bit-exactly, and solve a position that is near the truth but, with
// real pseudorange noise from its loops and an atmosphere that its models only approximate, not exactly on it.
#include "dect2/gnss_testkit.h"
#include <cstdlib>
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::gnsstest;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    GnssSimConfig cfg;
    GnssSim sim(cfg, 4e6);
    int nTx = 0;
    for (auto& s : sim.sats()) nTx += s.transmitted;
    Options o;
    o.rate = 4e6; o.secs = 40;
    Run r = run(sim, o);
    const GnssTelemetry& t = r.tel;
    printf("%s | %d simulated satellites\n", gnssSummary(t).c_str(), nTx);
    printf("receiver CPU %.1f s for %.0f s of signal: %.1fx real time\n", r.cpuSecs, o.secs, o.secs / r.cpuSecs);
    for (auto& l : r.log) if (l.find("first fix") != std::string::npos) printf("  log: %s\n", l.c_str());

    // tracking and message
    CHECK(t.nTracked >= nTx - 1, "tracked %d of %d satellites", t.nTracked, nTx);
    int withEph = 0;
    double cnErrSum = 0; int cnN = 0;
    for (auto& s : sim.sats()) {
        if (!s.transmitted) continue;
        if (!r.hasEph[s.prn]) continue;
        withEph++;
        const GpsEphemeris& e = r.eph[s.prn];
        const GpsEphemeris& q = s.eph;
        // bit exact: every transmitted integer came back (the decoded values are the integers times the scale factors)
        const bool same = e.sqrtA == q.sqrtA && e.e == q.e && e.m0 == q.m0 && e.omega0 == q.omega0 && e.i0 == q.i0 && e.omega == q.omega && e.dn == q.dn && e.omegaDot == q.omegaDot && e.idot == q.idot &&
                          e.cuc == q.cuc && e.cus == q.cus && e.crc == q.crc && e.crs == q.crs && e.cic == q.cic && e.cis == q.cis && e.af0 == q.af0 && e.af1 == q.af1 && e.af2 == q.af2 && e.tgd == q.tgd &&
                          e.toe == q.toe && e.toc == q.toc && e.iodc == q.iodc && e.iode2 == q.iode2 && e.health == q.health;
        CHECK(same, "PRN %d: the decoded ephemeris differs from the transmitted one", s.prn);
        CHECK(r.week[s.prn] == sim.week(), "PRN %d: week %d, sent %d", s.prn, r.week[s.prn], sim.week());
    }
    CHECK(withEph >= nTx - 1, "%d satellites with a decoded ephemeris of %d", withEph, nTx);
    CHECK(r.hasIono, "no ionosphere/UTC parameters");
    if (r.hasIono) {
        const GpsIono si = sim.iono();
        bool eq = true;
        for (int k = 0; k < 4; k++) eq = eq && r.iono.alpha[k] == si.alpha[k] && r.iono.beta[k] == si.beta[k];
        CHECK(eq, "ionosphere parameters differ");
        CHECK(r.utc.valid && r.utc.dtls == 18 && r.utc.a0 == sim.utc().a0 && r.utc.a1 == sim.utc().a1, "UTC parameters: leap seconds %d", r.utc.dtls);
    }
    int nAlm = 0;
    for (int p = 1; p <= 32; p++) if (r.alm[p].valid) nAlm++;
    printf("almanac entries decoded: %d (of the 30 sent: one page of subframe 5 and one of subframe 4 come every 30 s, the whole almanac takes 12.5 minutes)\n", nAlm);
    CHECK(nAlm >= 2, "only %d almanac entries", nAlm);
    for (auto& s : sim.sats()) {
        if (!r.alm[s.prn].valid) continue;
        const GpsAlmanac& a = r.alm[s.prn];
        CHECK(a.e == s.alm.e && a.sqrtA == s.alm.sqrtA && a.m0 == s.alm.m0 && a.omega0 == s.alm.omega0 && a.omega == s.alm.omega && a.toa == s.alm.toa, "PRN %d almanac differs", s.prn);
    }

    // C/N0 against the truth: the estimate sits about 1 dB under (the 8 bit quantisation and the band filter cost some of it)
    for (auto& c : t.channels) {
        if (c.state < GnssChLocked) continue;
        const double truth = sim.sats()[(size_t)c.prn - 1].cn0;
        cnErrSum += c.cn0 - truth; cnN++;
        CHECK(std::fabs(c.cn0 - truth) < 2.5, "PRN %d: C/N0 %.1f, true %.1f", c.prn, c.cn0, truth);
        CHECK(c.state >= GnssChFrameSync, "PRN %d is only at state %s", c.prn, gnssChStateName(c.state));
        CHECK(c.framesBad == 0, "PRN %d: %u subframes failed their parity", c.prn, c.framesBad);
    }
    printf("C/N0: mean error %+.2f dB over %d satellites\n", cnN ? cnErrSum / cnN : 0.0, cnN);

    // pseudoranges against the ideal ones
    double worstRms = 0, worstMean = 0, sumRms2 = 0; int nr = 0;
    for (auto& c : t.channels) {
        if (c.state < GnssChFrameSync) continue;
        const RangeStats st = rangeError(sim, r, c.prn, 28.0);
        if (st.n < 4) continue;
        printf("  PRN %2d  C/N0 %.1f (true %.1f)  pseudorange error mean %+5.2f m, rms %.2f m, max %.2f m (%d epochs)\n", c.prn, c.cn0, sim.sats()[(size_t)c.prn - 1].cn0, st.mean, st.rms, st.maxAbs, st.n);
        worstRms = std::fmax(worstRms, st.rms); worstMean = std::fmax(worstMean, std::fabs(st.mean));
        sumRms2 += st.rms * st.rms; nr++;
    }
    CHECK(nr >= nTx - 2, "only %d satellites with pseudorange statistics", nr);
    CHECK(worstRms < 6.0 && worstMean < 4.0, "pseudorange errors: worst rms %.2f m, worst mean %.2f m", worstRms, worstMean);
    CHECK(sumRms2 / std::max(nr, 1) > 0.3 * 0.3, "pseudorange noise is implausibly small");

    // the fix
    CHECK(t.fix.valid && t.state == 2 && t.dataValid, "no position fix");
    if (t.fix.valid) {
        double hz, vt;
        fixError(sim, t.fix, &hz, &vt);
        printf("fix: %s, horizontal error %.2f m, vertical %.2f m, HDOP %.2f, estimated horizontal error %.1f m, time to first fix %.1f s\n", t.fix.type.c_str(), hz, vt, t.fix.hdop, t.fix.hErrM, t.fix.firstFixSecs);
        CHECK(hz < 8.0 && std::fabs(vt) < 15.0, "position error %.2f m horizontal, %.2f m vertical", hz, vt);
        CHECK(hz > 0.05, "the horizontal error is zero (%.4f m): the test is not independent of the answer", hz);
        // the receiver's own error estimate should be about right (a single fix is a sample: allow a factor of 4 either way)
        CHECK(t.fix.hErrM > hz / 6 && t.fix.hErrM < hz * 6 + 5, "estimated error %.1f m against a real one of %.2f m", t.fix.hErrM, hz);
        CHECK(t.fix.firstFixSecs > 22 && t.fix.firstFixSecs < 32, "time to first fix %.1f s (the simulated ephemeris is complete after 26 s)", t.fix.firstFixSecs);
        CHECK(t.fix.nSats >= nTx - 1 && t.fix.nSatsPerSystem[GnssGps] == t.fix.nSats && t.fix.type == "GPS " + std::to_string(t.fix.nSats) + " satellites", "fix type '%s', %d satellites", t.fix.type.c_str(), t.fix.nSats);
        CHECK(t.fix.systemInFix[GnssGps] && !t.fix.systemInFix[GnssGlonass], "systems in the fix");
        // time: the fix's GPS time against the true time of the measurement; the leap seconds and the date
        CHECK(t.fix.gpsWeek == sim.week(), "week %d", t.fix.gpsWeek);
        const double tTrueNow = sim.trueTime(t.fix.fixCount ? r.meas.back().front().rxTime : 0);
        CHECK(std::fabs(t.fix.gpsTow - tTrueNow) < 5e-6, "GPS time of the fix %.7f, true %.7f (difference %.2e s)", t.fix.gpsTow, tTrueNow, t.fix.gpsTow - tTrueNow);
        CHECK(t.fix.leapSeconds == 18 && t.fix.timeValid, "leap seconds %d", t.fix.leapSeconds);
        int y, mo, d, h, mi; double sec;
        gpsTimeToCalendar(sim.week(), tTrueNow, 18, &y, &mo, &d, &h, &mi, &sec);
        CHECK(t.fix.year == y && t.fix.month == mo && t.fix.day == d && t.fix.hour == h && t.fix.minute == mi && std::fabs(t.fix.second - sec) < 1e-5, "UTC %04d-%02d-%02d %02d:%02d:%09.6f, expected %04d-%02d-%02d %02d:%02d:%09.6f", t.fix.year, t.fix.month,
              t.fix.day, t.fix.hour, t.fix.minute, t.fix.second, y, mo, d, h, mi, sec);
        // the receiver's carrier error (0 here) and the clock drift
        CHECK(std::fabs(t.cfoHz) < 3.0, "carrier error estimate %.2f Hz, true 0", t.cfoHz);
        // the sky: all the simulated satellites above the horizon are listed
        int inSky = 0;
        for (auto& sk : t.sky) for (auto& s : sim.sats()) if (s.prn == sk.prn && s.transmitted) inSky++;
        CHECK(inSky == nTx, "sky list has %d of the %d visible satellites", inSky, nTx);
        // the interface data
        CHECK(!t.scatterI.empty() && t.scatterI.size() == t.scatterQ.size() && t.scatterI.size() <= 400, "scatter %zu", t.scatterI.size());
        CHECK(t.acqCorr.size() == 256, "acquisition plot %zu points", t.acqCorr.size());
        CHECK(t.channels.size() <= 24 && t.nav.size() == t.channels.size(), "tables: %zu channels, %zu nav entries", t.channels.size(), t.nav.size());
        int used = 0;
        for (auto& c : t.channels) { used += c.used; CHECK(c.hasAzEl, "PRN %d has no azimuth/elevation", c.prn); }
        CHECK(used == t.fix.nSats, "%d channels marked as used, fix says %d", used, t.fix.nSats);
        // azimuth and elevation of the receiver against the simulation's
        for (auto& c : t.channels) {
            const GnssSimSat& s = sim.sats()[(size_t)c.prn - 1];
            if (!c.hasAzEl) continue;
            const double dAz = std::fabs(std::fmod(c.azDeg - s.azDeg + 540.0, 360.0) - 180.0);
            CHECK(dAz < 1.5 && std::fabs(c.elDeg - s.elDeg) < 1.5, "PRN %d: az/el %.1f/%.1f, true at the start %.1f/%.1f", c.prn, c.azDeg, c.elDeg, s.azDeg, s.elDeg);
        }
    }
    CHECK(t.blocksOk > 40 && t.blocksBad == 0, "subframes %llu good, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    CHECK(t.snrDb > 40 && t.snrDb < 46, "best C/N0 %.1f", t.snrDb);
    CHECK(t.levelDbfs > -20 && t.levelDbfs < -14, "input level %.1f dBFS", t.levelDbfs);
    if (!GNSS_SANITIZED && !std::getenv("CI")) CHECK(o.secs / r.cpuSecs > 3.0, "real-time factor %.1f", o.secs / r.cpuSecs);
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
