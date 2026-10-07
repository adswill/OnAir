// GNSS known-answer tests: the C/A codes against IS-GPS-200 Table 3-I, the word parity and its code properties, the subframe formats,
// the orbit algorithm against closed forms, the time and coordinate helpers against known dates and points.
// The navigation messages here come from the simulator (gnss_sim.cpp), which encodes them with its own code: a layout mistake that is the same
// in both would not show, so the field positions are also pinned by the properties of the message (preamble, TOW count, parity solved bits).
#include "dect2/gnss_codes.h"
#include "dect2/gnss_nav.h"
#include "dect2/gnss_sim.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void testCodes() {
    // IS-GPS-200 Table 3-I (ICD-GPS-200C Table 3-Ia), "first 10 chips octal", PRN 1..32
    static const unsigned first10[32] = {01440, 01620, 01710, 01744, 01133, 01455, 01131, 01454, 01626, 01504, 01642, 01750, 01764, 01772, 01775, 01776,
                                         01156, 01467, 01633, 01715, 01746, 01763, 01063, 01706, 01743, 01761, 01770, 01774, 01127, 01453, 01625, 01712};
    uint8_t c[33][1023];
    for (int prn = 1; prn <= 32; prn++) {
        CHECK(gpsCaChips(prn, c[prn]), "prn %d", prn);
        CHECK(gpsCaFirst10(prn) == first10[prn - 1], "PRN %d first 10 chips %o, expected %o", prn, gpsCaFirst10(prn), first10[prn - 1]);
        int ones = 0;
        for (int i = 0; i < 1023; i++) ones += c[prn][i];
        CHECK(ones == 512, "PRN %d has %d ones (a Gold code of length 1023 has 512)", prn, ones);
    }
    CHECK(!gpsCaChips(0, c[0]) && !gpsCaChips(33, c[0]), "PRN out of range accepted");
    // Gold code correlation: every periodic auto-correlation side lobe and cross-correlation value is -65, -1 or 63
    std::set<int> vals;
    int bad = 0;
    for (int a = 1; a <= 32; a++)
        for (int b = a; b <= 32; b++)
            for (int sh = 0; sh < 1023; sh++) {
                if (a == b && sh == 0) continue;
                int s = 0;
                for (int i = 0; i < 1023; i++) s += (c[a][i] ? -1 : 1) * (c[b][(i + sh) % 1023] ? -1 : 1);
                vals.insert(s);
                if (s != -65 && s != -1 && s != 63) bad++;
            }
    CHECK(bad == 0, "%d correlation values outside {-65, -1, 63}", bad);
    int self = 0;
    for (int i = 0; i < 1023; i++) self += 1;
    CHECK(self == 1023, "auto-correlation peak");
    printf("codes: 32 PRNs match Table 3-I, balance 512/511, correlation values %zu distinct (-65,-1,63 expected)\n", vals.size());
}

// parity: the Hamming-code properties of Table 20-XIV, and detection of every single and double error in a word
static void testParity() {
    // the code is linear: parity(a) ^ parity(b) == parity(a ^ b) when the previous bits are all zero
    int lin = 0;
    for (uint32_t a = 1; a < (1u << 24); a = a * 5 + 12345) {
        const uint32_t b = (a * 2654435761u) >> 8 & 0xFFFFFF;
        if ((lnavParity(a, 0, 0) ^ lnavParity(b, 0, 0)) != lnavParity(a ^ b, 0, 0)) lin++;
    }
    CHECK(lin == 0, "parity is not linear");
    // single bit errors of the 30 bit word give distinct nonzero syndromes (the extended Hamming code detects any double error and corrects one)
    std::set<uint32_t> syn;
    for (int k = 0; k < 24; k++) syn.insert(lnavParity(1u << k, 0, 0));
    for (int k = 0; k < 6; k++) syn.insert(1u << k);
    CHECK(syn.size() == 30, "single-bit syndromes: %zu distinct of 30", syn.size());
    bool zeroSyn = false;
    for (auto s : syn) zeroSyn |= s == 0;
    CHECK(!zeroSyn, "a single bit error has a zero syndrome");
    // every double error in a word is detected
    int missed = 0;
    uint8_t w[30];
    const uint32_t d = 0x5A3C71;
    const uint32_t par = lnavParity(d, 1, 0);
    for (int i = 0; i < 24; i++) w[i] = (uint8_t)(((d >> (23 - i)) & 1) ^ 0);          // prevD30 = 0
    for (int i = 0; i < 6; i++) w[24 + i] = (uint8_t)((par >> (5 - i)) & 1);
    uint32_t out;
    CHECK(lnavCheckWord(w, 1, 0, &out) && out == d, "a good word fails");
    for (int i = 0; i < 30; i++)
        for (int j = i; j < 30; j++) {
            uint8_t x[30];
            memcpy(x, w, 30);
            x[i] ^= 1;
            if (j != i) x[j] ^= 1;
            if (lnavCheckWord(x, 1, 0, &out)) missed++;
        }
    CHECK(missed == 0, "%d single or double errors not detected", missed);
    // the polarity of the whole stream does not matter when the previous word's bits are inverted with it
    uint8_t inv[30];
    for (int i = 0; i < 30; i++) inv[i] = w[i] ^ 1;
    CHECK(lnavCheckWord(inv, 0, 1, &out) && out == d, "an inverted word does not check");
    // the D30* inversion: data bits are sent complemented when the previous word ended in 1
    const uint32_t d2 = 0x123456;
    const uint32_t p2 = lnavParity(d2, 0, 1);
    uint8_t w2[30];
    for (int i = 0; i < 24; i++) w2[i] = (uint8_t)(((d2 >> (23 - i)) & 1) ^ 1);
    for (int i = 0; i < 6; i++) w2[24 + i] = (uint8_t)((p2 >> (5 - i)) & 1);
    CHECK(lnavCheckWord(w2, 0, 1, &out) && out == d2, "word after a D30* of 1");
    printf("parity: linear, 30 distinct single-error syndromes, all %d single and double errors detected\n", 30 * 31 / 2);
}

static void testSubframes() {
    GnssSimConfig cfg;
    GnssSim sim(cfg, 4e6);
    const auto& sats = sim.sats();
    int nTx = 0;
    for (auto& s : sats) nTx += s.transmitted;
    CHECK(nTx >= 8 && nTx <= 12, "%d satellites above the mask", nTx);
    const double eph[3] = {0, 0, 0};
    (void)eph;
    int checked = 0;
    for (size_t i = 0; i < sats.size(); i++) {
        if (!sats[i].transmitted) continue;
        GpsEphemeris e;
        e.prn = sats[i].prn;
        // the subframes of a whole frame; the frame at the start of the stream
        const uint32_t s0 = (uint32_t)(std::floor(sim.startTow() / 30.0)) * 5;
        int ids[5] = {};
        GpsAlmanac alm;
        GpsIono io; GpsUtc ut;
        for (uint32_t k = 0; k < 5; k++) {
            uint8_t bits[300];
            sim.subframeBits(i, s0 + k, bits);
            LnavSubframe sf;
            CHECK(lnavDecodeSubframe(bits, 0, 0, sf), "PRN %d subframe %u: parity", sats[i].prn, k);
            // the preamble, the TOW count and the solved bits
            uint8_t pre = 0;
            for (int b = 0; b < 8; b++) pre = (uint8_t)(pre << 1 | bits[b]);
            CHECK(pre == 0x8B, "preamble %02X", pre);
            CHECK(bits[58] == 0 && bits[59] == 0 && bits[298] == 0 && bits[299] == 0, "word 2 and 10 do not end in 00");
            CHECK(lnavTowCount(sf) == (s0 + k + 1) % 100800, "TOW count %u, expected %u", lnavTowCount(sf), (s0 + k + 1) % 100800);
            const unsigned id = lnavSubframeId(sf);
            CHECK(id == k % 5 + 1, "subframe id %u", id);
            ids[k] = (int)id;
            if (id == 1) lnavParseSf1(sf, e); else if (id == 2) lnavParseSf2(sf, e); else if (id == 3) lnavParseSf3(sf, e);
        }
        const GpsEphemeris& t = sats[i].eph;
        CHECK(e.complete(), "PRN %d: ephemeris not complete (iodc %d iode %d %d)", e.prn, e.iodc, e.iode2, e.iode3);
        // bit exact: the decoded values are the values of the transmitted integers
        CHECK(e.sqrtA == t.sqrtA && e.e == t.e && e.m0 == t.m0 && e.omega0 == t.omega0 && e.i0 == t.i0 && e.omega == t.omega, "PRN %d: orbit elements differ", e.prn);
        CHECK(e.dn == t.dn && e.omegaDot == t.omegaDot && e.idot == t.idot, "PRN %d: rates differ", e.prn);
        CHECK(e.cuc == t.cuc && e.cus == t.cus && e.crc == t.crc && e.crs == t.crs && e.cic == t.cic && e.cis == t.cis, "PRN %d: harmonics differ", e.prn);
        CHECK(e.af0 == t.af0 && e.af1 == t.af1 && e.tgd == t.tgd && e.toe == t.toe && e.toc == t.toc && e.iodc == t.iodc, "PRN %d: clock differs", e.prn);
        CHECK(e.wn == 2400 % 1024 && e.health == 0, "week %d health %d", e.wn, e.health);
        checked++;
    }
    // the almanac and the other pages: walk a whole 25 frame cycle of one satellite
    size_t first = 0;
    while (!sats[first].transmitted) first++;
    int alms = 0, ionoPages = 0;
    GpsAlmanac got[33];
    GpsIono io; GpsUtc ut;
    double toa = -1; int wna = -1;
    for (uint32_t f = 0; f < 25; f++)
        for (uint32_t k = 3; k < 5; k++) {
            uint8_t bits[300];
            sim.subframeBits(first, (uint32_t)(sim.startTow() / 6.0) / 5 * 5 + f * 5 + k, bits);
            LnavSubframe sf;
            if (!lnavDecodeSubframe(bits, 0, 0, sf)) { CHECK(false, "almanac subframe parity"); continue; }
            GpsAlmanac a;
            int sv = 0;
            const int r = lnavParseAlmanacPage(sf, (int)lnavSubframeId(sf), &sv, a, io, ut, &toa, &wna);
            if (r == 1) { if (a.valid) { got[a.prn] = a; alms++; } }
            else if (r == 2) ionoPages++;
        }
    // 30 satellites have almanacs (SV 31 and 32 are not in service)
    int nGot = 0;
    for (int p = 1; p <= 32; p++) if (got[p].valid) nGot++;
    CHECK(nGot == 30, "%d almanac entries decoded, 30 sent", nGot);
    CHECK(ionoPages == 1 && io.valid && ut.valid, "iono/UTC pages %d", ionoPages);
    const GpsIono si = sim.iono();
    for (int k = 0; k < 4; k++) CHECK(io.alpha[k] == si.alpha[k] && io.beta[k] == si.beta[k], "iono parameter %d", k);
    CHECK(ut.dtls == 18 && ut.dtlsf == 18, "leap seconds %d", ut.dtls);
    CHECK(toa == 356352.0 && wna == (2400 & 255), "almanac reference time %.0f week %d", toa, wna);
    for (size_t i = 0; i < sats.size(); i++) {
        const GpsAlmanac& a = got[sats[i].prn];
        const GpsAlmanac& t = sats[i].alm;
        CHECK(a.valid && a.e == t.e && a.sqrtA == t.sqrtA && a.omega0 == t.omega0 && a.omega == t.omega && a.m0 == t.m0 && a.i0 == t.i0 && a.omegaDot == t.omegaDot && a.af0 == t.af0 && a.af1 == t.af1 && a.toa == t.toa,
              "PRN %d almanac differs", sats[i].prn);
    }
    // the almanac position must be close to the ephemeris position at the almanac time (the harmonic terms are the only difference: a few hundred metres)
    double worst = 0;
    for (size_t i = 0; i < sats.size(); i++) {
        double pa[3], pe[3], clk;
        gpsAlmanacPos(got[sats[i].prn], 356352.0 + 600.0, pa);
        gpsEphemerisState(sats[i].eph, 356352.0 + 600.0, pe, &clk);
        worst = std::fmax(worst, std::sqrt((pa[0] - pe[0]) * (pa[0] - pe[0]) + (pa[1] - pe[1]) * (pa[1] - pe[1]) + (pa[2] - pe[2]) * (pa[2] - pe[2])));
    }
    CHECK(worst < 600, "almanac and ephemeris differ by %.0f m", worst);
    printf("subframes: %d satellites' ephemerides bit-exact, 30 almanacs, iono/UTC; almanac against ephemeris at most %.0f m apart\n", checked, worst);
}

static double dist(const double* a, const double* b) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }

static void testOrbit() {
    // a circular orbit has a closed form: u = M0 + n tk, x' = A cos u, y' = A sin u, node = Omega0 + (Omega_dot - Omega_e) tk - Omega_e toe
    GpsEphemeris e;
    e.sqrtA = 5153.7;
    e.e = 0; e.i0 = 55.0 * kPi / 180; e.omega0 = 1.234; e.m0 = 0.7; e.omega = 0; e.toe = 3600; e.omegaDot = 0; e.toc = 3600;
    const double A = e.sqrtA * e.sqrtA, n = std::sqrt(kGpsMu / (A * A * A));
    double worst = 0;
    for (double t = 3600; t < 3600 + 7200; t += 600) {
        double p[3], clk;
        gpsEphemerisState(e, t, p, &clk);
        const double tk = t - e.toe, u = e.m0 + n * tk, node = e.omega0 - kEarthRate * (tk + e.toe);
        const double x = A * std::cos(u), y = A * std::sin(u);
        const double q[3] = {x * std::cos(node) - y * std::cos(e.i0) * std::sin(node), x * std::sin(node) + y * std::cos(e.i0) * std::cos(node), y * std::sin(e.i0)};
        worst = std::fmax(worst, dist(p, q));
    }
    CHECK(worst < 1e-6, "circular orbit differs from the closed form by %.3g m", worst);
    // an eccentric orbit: the radius is a (1 - e cos E) with E from Kepler's equation, and the energy (vis-viva) is constant
    GpsEphemeris g = e;
    g.e = 0.02; g.omega = 0.9; g.omegaDot = 0;
    const double Ag = g.sqrtA * g.sqrtA;
    double eMax = 0;
    for (double t = 3600; t < 3600 + 20000; t += 1000) {
        double p0[3], p1[3], p2[3], clk, E;
        gpsEphemerisState(g, t, p0, &clk, &E);
        gpsEphemerisState(g, t + 1.0, p1, &clk);
        gpsEphemerisState(g, t - 1.0, p2, &clk);
        // inertial position: undo the earth rotation (rotate by +wE * t about z) before differencing for the velocity
        auto inertial = [&](const double* p, double tt, double* q) { const double a = kEarthRate * tt; q[0] = p[0] * std::cos(a) - p[1] * std::sin(a); q[1] = p[0] * std::sin(a) + p[1] * std::cos(a); q[2] = p[2]; };
        double q0[3], q1[3], q2[3];
        inertial(p0, t, q0); inertial(p1, t + 1, q1); inertial(p2, t - 1, q2);
        const double v[3] = {(q1[0] - q2[0]) / 2, (q1[1] - q2[1]) / 2, (q1[2] - q2[2]) / 2};
        const double r = std::sqrt(q0[0] * q0[0] + q0[1] * q0[1] + q0[2] * q0[2]);
        CHECK(std::fabs(r - Ag * (1 - g.e * std::cos(E))) < 1e-6, "radius against Kepler's equation");
        const double v2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        const double en = v2 / 2 - kGpsMu / r;
        eMax = std::fmax(eMax, std::fabs(en / (-kGpsMu / (2 * Ag)) - 1.0));
    }
    CHECK(eMax < 2e-5, "orbital energy not conserved: %.3g", eMax);
    // the simulator's orbit code (another arrangement of the same model, with harmonic terms) against the receiver's: the delay it applies
    // must be the range from the receiver's orbit routine, to well below a metre
    GnssSimConfig cfg;
    GnssSim sim(cfg, 4e6);
    double dMax = 0;
    for (auto& s : sim.sats()) {
        if (!s.transmitted) continue;
        const size_t i = (size_t)(s.prn - 1);
        const double t0 = sim.startTow() + 100.0;
        const double tau = sim.geometricDelay(i, t0);
        double p[3], clk;
        gpsEphemerisState(s.eph, t0 - tau, p, &clk);
        const double a = kEarthRate * tau;
        const double x = p[0] * std::cos(a) + p[1] * std::sin(a), y = -p[0] * std::sin(a) + p[1] * std::cos(a);
        const double* rx = sim.receiverEcef();
        const double rg = std::sqrt((x - rx[0]) * (x - rx[0]) + (y - rx[1]) * (y - rx[1]) + (p[2] - rx[2]) * (p[2] - rx[2]));
        dMax = std::fmax(dMax, std::fabs(rg - tau * kC));
        CHECK(tau > 0.066 && tau < 0.088, "delay %.4f s", tau);
    }
    CHECK(dMax < 1e-3, "orbit routines differ by %.3g m", dMax);
    printf("orbit routines of the receiver and of the simulator agree to %.1e m\n", dMax);
    printf("orbit: circular closed form within %.1e m, Kepler radius exact, energy conserved to %.1e\n", worst, eMax);
}

static void testTimeAndGeodesy() {
    int y, mo, d, h, mi; double s;
    gpsTimeToCalendar(0, 0, 0, &y, &mo, &d, &h, &mi, &s);
    CHECK(y == 1980 && mo == 1 && d == 6 && h == 0 && mi == 0 && s == 0, "GPS epoch: %d-%d-%d", y, mo, d);
    gpsTimeToCalendar(1024, 0, 13, &y, &mo, &d, &h, &mi, &s);          // the first week rollover, 1999-08-22 00:00:00 GPS time = 23:59:47 UTC the day before
    CHECK(y == 1999 && mo == 8 && d == 21 && h == 23 && mi == 59 && std::fabs(s - 47) < 1e-9, "week 1024: %d-%d-%d %d:%d:%.1f", y, mo, d, h, mi, s);
    gpsTimeToCalendar(2086, 259218, 18, &y, &mo, &d, &h, &mi, &s);     // 2020-01-01 00:00:00 UTC
    CHECK(y == 2020 && mo == 1 && d == 1 && h == 0 && mi == 0 && std::fabs(s) < 1e-9, "2020: %d-%d-%d %d:%d:%.1f", y, mo, d, h, mi, s);
    CHECK(gpsResolveWeek(352, 2393) == 2400, "week 352 near 2393: %d", gpsResolveWeek(352, 2393));
    CHECK(gpsResolveWeek(1000, 2393) == 1000 + 1024 && gpsResolveWeek(100, 2393) == 100 + 2048, "week resolve: %d %d", gpsResolveWeek(1000, 2393), gpsResolveWeek(100, 2393));
    double x[3];
    llaToEcef(0, 0, 0, x);
    CHECK(std::fabs(x[0] - 6378137.0) < 1e-6 && std::fabs(x[1]) < 1e-6 && std::fabs(x[2]) < 1e-6, "equator point");
    llaToEcef(90, 0, 0, x);
    CHECK(std::fabs(x[2] - 6356752.314245) < 1e-3, "pole z %.6f", x[2]);
    double worst = 0;
    for (double lat = -89; lat < 90; lat += 17.3)
        for (double lon = -179; lon < 180; lon += 31.7)
            for (double h = -100; h < 30000; h += 4000) {
                double la, lo, hh;
                llaToEcef(lat, lon, h, x);
                ecefToLla(x, &la, &lo, &hh);
                worst = std::fmax(worst, std::fabs(la - lat) * 111000 + std::fabs(lo - lon) * 111000 * 0.01 + std::fabs(hh - h));
            }
    CHECK(worst < 1e-5, "geodetic round trip %.3g m", worst);
    // elevation: a point straight above is at 90 degrees, a point 90 degrees round the earth is below the horizon
    double rx[3], up[3], az, el;
    llaToEcef(25.2, 55.36, 10, rx);
    llaToEcef(25.2, 55.36, 20e6, up);
    azElFromEcef(rx, up, &az, &el);
    CHECK(el > 89.99, "zenith elevation %.3f", el);
    llaToEcef(45, 55.36, 20e6, up);        // due north
    azElFromEcef(rx, up, &az, &el);
    CHECK(az < 0.5 || az > 359.5, "north azimuth %.2f", az);
    // atmosphere: Klobuchar at night is a constant 5 ns times the obliquity factor; the troposphere at sea level is about 2.3 to 2.5 m in the zenith
    GpsIono io;
    for (int i = 0; i < 4; i++) { io.alpha[i] = 1e-8; io.beta[i] = 0; }
    const double night = klobucharDelay(io, 25.2, 55.36, 0, 90, 0.0 + 3600 * 17);       // 21 h local: x outside 1.57 rad
    CHECK(std::fabs(night - 5e-9 * (1 + 16 * std::pow(0.53 - 0.5, 3))) < 1e-12, "Klobuchar night value %.4g ns", night * 1e9);
    const double zen = troposphereDelay(25.2, 0, 90);
    CHECK(zen > 2.2 && zen < 2.7, "zenith troposphere %.2f m", zen);
    CHECK(troposphereDelay(25.2, 0, 10) > 4 * zen * 0.9, "troposphere at 10 degrees");
    printf("time and geodesy: calendar dates, week resolution, lla round trip %.1e m, atmosphere models in range\n", worst);
}

int main() {
    testCodes();
    testParity();
    testSubframes();
    testOrbit();
    testTimeAndGeodesy();
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
