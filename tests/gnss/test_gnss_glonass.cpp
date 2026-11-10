// GLONASS L1OF: the ranging code, the Hamming check of the strings and the orbit integration checked against the ICD; then the simulated sky on
// its own tune (1602 MHz, 10 Msps): the fourteen frequency channels cut out of the band, the strings found by their time mark, the ephemeris compared
// with what was sent, the orbit integrated from tb, the position fixed with GLONASS alone; the outermost channels k = -7 and +6 among those in view;
// and GPS with GLONASS in one 40 Msps tune at 1590.4 MHz, GLONASS with a clock unknown of its own and tau_GPS applied; the "All systems" tune
// (1582 MHz, 50 Msps) holds the L1, B1I and all fourteen GLONASS bands. The faults of tests/common/impair.h:
// +-5 kHz, a sample clock 20 ppm off, low C/N0, a start in the middle of a string.
#include "dect2/gnss/gnss_codes.h"
#include "dect2/gnss/gnss_glo.h"
#include "dect2/gnss/gnss_testkit.h"
#include "dect2/engine/test_parallel.h"
#include "gnss_faults.h"
#include <cmath>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "dect2/common/mathconst.h"
using namespace dect2;
using namespace dect2::gnsstest;
using dect2::testpar::CaseOut;
#define CHECK(c, ...) do { if (!(c)) out.fail(__LINE__, __VA_ARGS__); } while (0)

static int unitChecks() {
    int fails = 0;
    auto fail = [&](const char* what) { printf("FAIL: %s\n", what); fails++; };
    // ICD 3.3.2.1: the code starts with the group 111111100
    uint8_t c[kGloLen];
    gloCaChips(c);
    const char* head = "111111100";
    for (int i = 0; i < 9; i++) if (c[i] != head[i] - '0') { fail("code start"); break; }
    int ones = 0;
    for (int i = 0; i < kGloLen; i++) ones += c[i];
    if (ones != 256) fail("an m-sequence of 511 has 256 ones");
    // the Hamming code: a string with its check bits passes; every single error is corrected; two errors are erased
    uint8_t s[86] = {};
    unsigned seed = 99;
    for (int i = 9; i <= 84; i++) { seed = seed * 1103515245u + 12345u; s[i] = (uint8_t)((seed >> 16) & 1u); }
    s[85] = 0;
    gloSetHamming(s);
    uint8_t t[86];
    std::memcpy(t, s, sizeof t);
    if (gloCheckString(t) != 0) fail("a good string fails the check");
    for (int e = 1; e <= 85; e++) {
        if (e == 8) continue;          // the overall check bit alone: rule c erases the string (all C1..C7 zero, the sum one)
        std::memcpy(t, s, sizeof t);
        t[e] ^= 1;
        const int r = gloCheckString(t);
        // an error in a check bit leaves the data alone (rule a: one checksum and the overall sum); a data bit is corrected
        if (r < 0 || std::memcmp(t + 9, s + 9, 77) != 0) { fail("single error not corrected"); break; }
    }
    int erased = 0;
    for (int e = 9; e <= 84; e++) { std::memcpy(t, s, sizeof t); t[e] ^= 1; t[e + 1] ^= 1; erased += gloCheckString(t) < 0; }
    if (erased != 76) fail("double errors not erased");
    // the orbit: integrated 15 minutes out and back lands where it started; a circular orbit keeps its radius
    GloEphemeris e;
    e.tb = 3600; e.pos[0] = 25510e3; e.vel[1] = std::sqrt(kGloMu / 25510e3) - kGloOmega * 25510e3;
    double p1[3], v1[3], clk;
    gloEphemerisState(e, 3600 + 900, p1, v1, &clk);
    GloEphemeris back = e;
    back.tb = 4500; for (int q = 0; q < 3; q++) { back.pos[q] = p1[q]; back.vel[q] = v1[q]; }
    double p0[3];
    gloEphemerisState(back, 3600, p0, nullptr, nullptr);
    if (std::hypot(p0[0] - e.pos[0], p0[1] - e.pos[1], p0[2] - e.pos[2]) > 0.05) fail("orbit integration is not reversible");
    const double r1 = std::sqrt(p1[0] * p1[0] + p1[1] * p1[1] + p1[2] * p1[2]);
    if (std::fabs(r1 - 25510e3) > 30e3) fail("orbit radius");
    // the All systems tune: every band fits at 50 Msps around 1582 MHz; on L1 at 4 Msps only the L1 band
    {
        std::vector<cf32> z(200000);
        for (size_t i = 0; i < z.size(); i++) z[i] = cf32(0.01f * (float)((i * 7919) % 13) - 0.06f, 0.01f * (float)((i * 104729) % 11) - 0.05f);
        GnssTelemetry t;
        GnssReceiver rx;
        rx.configure(50e6);
        rx.setCenterMhz(1582.0);
        for (int n = 0; n < 200 && !rx.telemetry(t, 0); n++) rx.feed(z.data(), z.size());     // the first report comes after 0.25 s of signal
        const unsigned all = gnssSystemBit(GnssGps) | gnssSystemBit(GnssBeidou) | gnssSystemBit(GnssGlonass);
        if ((t.activeMask & all) != all) fail("All systems: not every band active at 1582 MHz, 50 Msps");
        GnssReceiver l1;
        l1.configure(4e6);
        for (int n = 0; n < 200 && !l1.telemetry(t, 0); n++) l1.feed(z.data(), z.size());
        if (t.activeMask != gnssSystemBit(GnssGps)) fail("L1 at 4 Msps: only the L1 band");
    }
    return fails;
}

struct Case { const char* name; gnssfault::Faults f; bool warm; double secs; bool combined = false; };

static void runCase(const Case& k, CaseOut& out) {
    GnssSimConfig cfg;
    cfg.systems = gnssSystemBit(GnssGlonass) | (k.combined ? gnssSystemBit(GnssGps) : 0u);
    cfg.warmStart = k.warm;
    const double center = k.combined ? 1590.4e6 : kGloL1Hz;
    const double rate = k.combined ? 40e6 : 10e6;
    cfg.centerHz = center;
    GnssSim sim(cfg, rate);
    int nGps = 0;
    std::vector<size_t> glo;
    bool outer[2] = {false, false};
    for (size_t i = 0; i < sim.sats().size(); i++) {
        const GnssSimSat& s = sim.sats()[i];
        if (!s.transmitted) continue;
        if (s.sys == GnssGps) nGps++;
        if (s.sys == GnssGlonass) { glo.push_back(i); if (s.fcn == -7) outer[0] = true; if (s.fcn == 6) outer[1] = true; }
    }
    Options o;
    o.rate = rate; o.secs = k.secs; o.chunk = k.combined ? 64000 : 50000;
    o.centerMhz = center / 1e6;
    o.hook = gnssfault::hook(k.f, o.rate);
    GloEphemeris eph[kGloSlots];
    bool hasEph[kGloSlots] = {};
    o.after = [&](GnssReceiver& rx, size_t done) {
        if (done + o.chunk < (size_t)(k.secs * o.rate)) return;
        for (int i = 0; i < kGloSlots; i++) hasEph[i] = rx.getGloEphemeris(i + kGloMinK, eph[i]);
    };
    Run r = run(sim, o);
    const GnssTelemetry& t = r.tel;
    double hz = 0, vt = 0;
    if (t.fix.valid) fixError(sim, t.fix, &hz, &vt);
    out.print("%s: %s, error %.2f / %.2f m, %d GPS + %d GLONASS in the fix (%d + %zu in view), tau_GPS %s %.1f ns\n", k.name, gnssSummary(t).c_str(), hz, vt,
              t.fix.nSatsPerSystem[GnssGps], t.fix.nSatsPerSystem[GnssGlonass], nGps, glo.size(), t.fix.gloTimeValid ? "received" : "not received", t.fix.gloTauGpsNs);
    const bool weak = k.f.noiseSnrDb < 1e8;
    CHECK(glo.size() >= 6, "only %zu GLONASS satellites in view", glo.size());
    CHECK(outer[0] && outer[1], "the outermost channels k = -7 and +6 are not in view");
    CHECK(t.fix.valid, "no fix");
    CHECK(hz < 15.0 && std::fabs(vt) < 25.0, "position error %.1f / %.1f m", hz, vt);
    CHECK((t.decodeMask & gnssSystemBit(GnssGlonass)) != 0, "GLONASS not in the decode mask");
    int high = 0;
    for (size_t i : glo) high += sim.sats()[i].elDeg >= 10.0;
    CHECK(t.fix.nSatsPerSystem[GnssGlonass] >= high - (weak ? 3 : 1), "%d GLONASS satellites in the fix, %d above 10 degrees", t.fix.nSatsPerSystem[GnssGlonass], high);
    CHECK(t.fix.gloTimeValid && std::fabs(t.fix.gloTauGpsNs - 20.0) < 1.0, "tau_GPS %.2f ns", t.fix.gloTauGpsNs);     // 21 steps of 2^-30 s
    if (k.combined) {
        CHECK(t.fix.nSatsPerSystem[GnssGps] >= 4, "GPS left the combined fix");
        // tau_GPS is applied and the simulation has no other offset: the GLONASS clock unknown lands on the GPS one
        const double isb = t.fix.clockBiasM[GnssGlonass] - t.fix.clockBiasM[GnssGps];
        out.print("  receiver clock against GLONASS minus against GPS: %+.2f m (expected 0)\n", isb);
        CHECK(t.fix.systemInFix[GnssGlonass] && std::fabs(isb) < 6.0, "inter-system clock %.2f m", isb);
    }
    int ephOk = 0, outerOk = 0;
    for (size_t i : glo) {
        const GnssSimSat& s = sim.sats()[i];
        const int slot = s.fcn - kGloMinK;
        int n = 0; double sum = 0, sum2 = 0;
        for (auto& ep : r.meas) for (auto& m : ep) {
            if (m.sys != GnssGlonass || m.fcn != s.fcn) continue;
            const double tSim = m.rxTime / (1.0 + k.f.clockPpm * 1e-6) + (double)k.f.skip / o.rate;
            double truth = std::fmod(sim.svTransmitTime(i, sim.trueTime(tSim)) + 10800.0 - 18.0, 86400.0);
            const double e = gloWrapDay(m.txTow - truth) * dect2::math::kSpeedOfLight;

            sum += e; sum2 += e * e; n++;
        }
        const GnssChannel* ch = nullptr;
        for (auto& c : t.channels) if (c.sys == GnssGlonass && c.fcn == s.fcn) ch = &c;
        out.print("  R%02d k%+d el %4.1f: %s, C/N0 %.1f, strings %u good %u bad, range error %+.2f m rms %.2f m (%d s)%s\n", s.prn, s.fcn, s.elDeg,
                  ch ? gnssChStateName(ch->state) : "not tracked", ch ? ch->cn0 : 0.f, ch ? ch->framesOk : 0u, ch ? ch->framesBad : 0u, n ? sum / n : 0.0,
                  n ? std::sqrt(sum2 / n) : 0.0, n, hasEph[slot] ? ", ephemeris" : "");
        // a GLONASS chip is 587 m: twice the code noise of GPS at the same C/N0 (and twice again with the noise doubled)
        if (n >= 10) CHECK(std::sqrt(sum2 / n) < (weak ? 20.0 : 12.0) && std::fabs(sum / n) < 6.0, "R%02d: range error %.2f m rms %.2f", s.prn, sum / n, std::sqrt(sum2 / n));
        if (ch && !weak) CHECK(ch->framesBad <= 2, "R%02d: %u strings failed", s.prn, ch->framesBad);
        if (hasEph[slot]) {
            // the orbit the receiver integrates against the simulation's: the geometric range at one instant
            const double tTrue = sim.trueTime(k.secs - 5.0);
            const double tau = sim.geometricDelay(i, tTrue);
            double pos[3];
            gloEphemerisState(eph[slot], std::fmod(tTrue - tau + 10800.0 - 18.0, 86400.0), pos, nullptr, nullptr);
            const double a = 7.292115e-5 * tau, x = pos[0] * std::cos(a) + pos[1] * std::sin(a), y = -pos[0] * std::sin(a) + pos[1] * std::cos(a);
            const double* rx = sim.receiverEcef();
            const double rr = std::sqrt((x - rx[0]) * (x - rx[0]) + (y - rx[1]) * (y - rx[1]) + (pos[2] - rx[2]) * (pos[2] - rx[2]));
            CHECK(std::fabs(rr - tau * dect2::math::kSpeedOfLight) < 0.05, "R%02d: the orbit from the ephemeris is %.3f m off in range", s.prn, rr - tau * dect2::math::kSpeedOfLight);
        }
        if (hasEph[slot]) {
            const GloEphemeris& a = eph[slot];
            const GloEphemeris& b = s.glo;
            bool same = a.n == b.n && a.tb == b.tb && a.tauN == b.tauN && a.gammaN == b.gammaN && a.NT > 0 && a.k == s.fcn;
            for (int q = 0; q < 3; q++) same = same && a.pos[q] == b.pos[q] && a.vel[q] == b.vel[q] && a.acc[q] == b.acc[q];
            CHECK(same, "R%02d: decoded ephemeris differs from the one sent", s.prn);
            ephOk += same;
            if (same && (s.fcn == -7 || s.fcn == 6)) outerOk++;
        }
    }
    CHECK(ephOk >= high - (weak ? 3 : 1), "%d GLONASS ephemerides, %d satellites above 10 degrees", ephOk, high);
    CHECK(outerOk == 2, "the outermost channels k = -7 and +6: %d of 2 decoded", outerOk);
    int sky = 0;
    for (auto& s : t.sky) sky += s.sys == GnssGlonass && s.fcn >= kGloMinK && s.fcn <= kGloMaxK;
    CHECK(sky >= ephOk, "%d GLONASS satellites in the sky plot, %d ephemerides", sky, ephOk);
}

int main() {
    const int uf = unitChecks();
    if (uf) { printf("%d unit failure(s)\n", uf); return 1; }
    // 50000 samples at 10 Msps: 5 kHz is 25 whole cycles in a chunk
    const Case cases[] = {
        {"clean", {}, true, 70},
        {"+5 kHz offset", {5000.0, 0, 1e9, 0}, true, 70},
        {"-5 kHz offset, sample clock +20 ppm", {-5000.0, 20, 1e9, 0}, true, 70},
        {"low C/N0 (noise doubled), sample clock -20 ppm", {0.0, -20, 0.0, 0}, true, 85},
        {"start 1.3 s into the stream (in a string), cold", {0.0, 0, 1e9, 13000000}, false, 90},
        {"GPS + GLONASS at 1590.4 MHz, 40 Msps", {}, true, 75, true},
    };
    if (getenv("GNSS_CASE")) { CaseOut out; runCase(cases[atoi(getenv("GNSS_CASE"))], out); fputs(out.text.c_str(), stdout); return out.fails ? 1 : 0; }
    // GNSS_PART=0 / 1: half of the cases each (ctest gnss_glonass and gnss_glonass_b, so a shared runner gives each half its own time limit);
    // unset: all six, the longest first
    std::vector<size_t> idx = {5, 4, 3, 0, 1, 2};
    if (const char* pt = getenv("GNSS_PART")) idx = atoi(pt) == 1 ? std::vector<size_t>{4, 3, 2} : std::vector<size_t>{5, 0, 1};
    const int fails = dect2::testpar::runCases(idx.size(), [&](size_t i, CaseOut& out) { runCase(cases[idx[i]], out); });
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("gnss glonass: ok\n");
    return 0;
}
