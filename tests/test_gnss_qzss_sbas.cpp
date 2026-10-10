// QZSS L1 C/A and SBAS L1 next to GPS in the simulated sky: the QZSS satellites are tracked, their ephemeris decoded and used in the fix with GPS (one
// clock), the SBAS satellites are tracked and their messages decoded (Viterbi, preamble, CRC; the type and PRN reported, the corrections not applied).
// The same with the faults of tests/impair.h: +-5 kHz frequency offset, a sample clock 20 ppm off, low C/N0, and a start in the middle of a frame.
#include "dect2/gnss_testkit.h"
#include "dect2/test_parallel.h"
#include "gnss_faults.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
using namespace dect2;
using namespace dect2::gnsstest;
using dect2::testpar::CaseOut;
#define CHECK(c, ...) do { if (!(c)) out.fail(__LINE__, __VA_ARGS__); } while (0)

struct Case { const char* name; gnssfault::Faults f; double cn0Top; bool warm; double secs; };

static void runCase(const Case& k, CaseOut& out) {
    GnssSimConfig cfg;
    cfg.systems = gnssSystemBit(GnssGps) | gnssSystemBit(GnssQzss) | gnssSystemBit(GnssSbas);
    cfg.cn0Top = k.cn0Top;
    cfg.warmStart = k.warm;
    GnssSim sim(cfg, 4e6);
    int nGps = 0;
    std::vector<int> qzss, sbas;
    for (auto& s : sim.sats()) {
        if (!s.transmitted) continue;
        if (s.sys == GnssGps) nGps++;
        if (s.sys == GnssQzss) qzss.push_back(s.prn);
        if (s.sys == GnssSbas) sbas.push_back(s.prn);
    }
    Options o;
    o.rate = 4e6; o.secs = k.secs; o.chunk = 64000;
    o.hook = gnssfault::hook(k.f, o.rate);
    Run r = run(sim, o);
    const GnssTelemetry& t = r.tel;
    double hz = 0, vt = 0;
    if (t.fix.valid) fixError(sim, t.fix, &hz, &vt);
    out.print("%s: %s, error %.2f / %.2f m, %d GPS + %d QZSS in the fix (%d + %zu in view), SBAS messages %llu\n", k.name, gnssSummary(t).c_str(), hz, vt,
              t.fix.nSatsPerSystem[GnssGps], t.fix.nSatsPerSystem[GnssQzss], nGps, qzss.size(), (unsigned long long)t.sbasMessages);
    {
        // when the satellites of each system were first locked and first gave an ephemeris (QZSS) or a message (SBAS)
        double lockQ = -1, lockS = -1, ephQ = -1, msgS = -1;
        for (auto& rep : r.reports) {
            for (auto& c : rep.channels) {
                if (c.state >= GnssChLocked && c.sys == GnssQzss && lockQ < 0) lockQ = rep.signalSecs;
                if (c.state >= GnssChLocked && c.sys == GnssSbas && lockS < 0) lockS = rep.signalSecs;
            }
            for (auto& n : rep.nav) {
                if (n.sys == GnssQzss && n.hasEphemeris && ephQ < 0) ephQ = rep.signalSecs;
                if (n.sys == GnssSbas && n.sbasMessages > 0 && msgS < 0) msgS = rep.signalSecs;
            }
        }
        out.print("  first QZSS lock %.0f s, ephemeris %.0f s; first SBAS lock %.0f s, message %.0f s\n", lockQ, ephQ, lockS, msgS);
    }
    CHECK(!qzss.empty() && sbas.size() == 3, "the sky should hold QZSS and three SBAS satellites (%zu, %zu)", qzss.size(), sbas.size());
    CHECK(t.fix.valid, "no fix");
    CHECK(hz < 12.0 && std::fabs(vt) < 20.0, "position error %.1f / %.1f m", hz, vt);
    CHECK(t.fix.nSatsPerSystem[GnssQzss] >= (int)qzss.size() - (k.f.noiseSnrDb < 1e8 ? 1 : 0) && t.fix.nSatsPerSystem[GnssQzss] >= 1, "%d QZSS satellites in the fix of %zu", t.fix.nSatsPerSystem[GnssQzss], qzss.size());
    CHECK(t.fix.type.find("QZSS") != std::string::npos, "fix type '%s'", t.fix.type.c_str());
    // the QZSS transmit times the receiver read against the truth (the pseudoranges before any model)
    for (size_t i = 0; i < sim.sats().size(); i++) {
        const GnssSimSat& x = sim.sats()[i];
        if (x.sys != GnssQzss || !x.transmitted) continue;
        int n = 0; double sum = 0, sum2 = 0;
        for (auto& ep : r.meas) for (auto& m : ep) {
            if (m.sys != GnssQzss || m.prn != x.prn || m.rxTime < 20) continue;
            // the receiver's sample clock: the clock fault makes 1 + ppm samples of each, and the skipped start is missing
            const double tSim = m.rxTime / (1.0 + k.f.clockPpm * 1e-6) + (double)k.f.skip / o.rate;
            const double e = (m.txTow - sim.svTransmitTime(i, sim.trueTime(tSim))) * 299792458.0;
            sum += e; sum2 += e * e; n++;
        }
        if (n) out.print("  %s range error: mean %+.2f m, rms %.2f m over %d s\n", gnssSatName(GnssQzss, x.prn).c_str(), sum / n, std::sqrt(sum2 / n), n);
        if (n < 10 && k.f.noiseSnrDb < 1e8) continue;         // a weak one may be tracked late or not at all at the low C/N0
        CHECK(n > 10 && std::sqrt(sum2 / n) < 8.0, "%s: range error rms %.1f m over %d", gnssSatName(GnssQzss, x.prn).c_str(), n ? std::sqrt(sum2 / n) : 0.0, n);
    }
    for (int p : qzss) {
        GpsEphemeris e;
        const GnssSimSat* s = nullptr;
        for (auto& x : sim.sats()) if (x.sys == GnssQzss && x.prn == p) s = &x;
        bool ok = false;
        for (auto& n : t.nav) if (n.sys == GnssQzss && n.prn == p) ok = n.hasEphemeris;
        if (k.f.noiseSnrDb > 1e8) CHECK(ok, "no ephemeris of %s", gnssSatName(GnssQzss, p).c_str());
        (void)e; (void)s;
    }
    // the channel list names the systems
    int sbasRows = 0;
    for (size_t i = 0; i < t.channels.size(); i++) {
        const GnssChannel& c = t.channels[i];
        if (c.sys != GnssSbas) continue;
        sbasRows++;
        const GnssNavInfo* n = nullptr;
        for (auto& x : t.nav) if (x.sys == GnssSbas && x.prn == c.prn) n = &x;
        int types = 0;
        if (n) for (int b = 0; b < 32; b++) types += (n->sbasTypesSeen >> b) & 1;
        out.print("  %s: %s, C/N0 %.1f, %u messages (%u bad), last type %d, %d types seen\n", gnssSatName(c.sys, c.prn).c_str(), gnssChStateName(c.state), c.cn0,
                  n ? n->sbasMessages : 0u, c.framesBad, n ? n->sbasLastType : -1, types);
        // the simulation sends a message a second in a rotation of 16 types; the decoder starts after a few seconds of tracking
        CHECK(n && n->sbasMessages >= (uint32_t)(k.f.noiseSnrDb < 1e8 ? 10 : k.secs * 0.5), "%s: %u SBAS messages in %.0f s", gnssSatName(c.sys, c.prn).c_str(), n ? n->sbasMessages : 0u, k.secs);
        CHECK(types >= 12, "%s: only %d message types", gnssSatName(c.sys, c.prn).c_str(), types);
        CHECK(c.framesBad <= 2, "%s: %u missed messages", gnssSatName(c.sys, c.prn).c_str(), c.framesBad);
        CHECK(n && (n->sbasTypesSeen & ~((1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 7) | (1u << 9) | (1u << 10) | (1u << 12) | (1u << 17) | (1u << 18) | (1u << 24) |
                                         (1u << 25) | (1u << 26) | (1u << 27) | (1u << 28))) == 0, "a message type that was not sent");
    }
    CHECK(sbasRows == (int)sbas.size(), "%d SBAS channels of %zu", sbasRows, sbas.size());
}

int main() {
    // chunks of 64000 samples: 5 kHz is 80 whole cycles in one, so impair::shift keeps the phase from chunk to chunk
    const Case cases[] = {
        {"clean", {}, 44, true, 55},
        {"+5 kHz offset", {5000.0, 0, 1e9, 0}, 44, true, 55},
        {"-5 kHz offset, sample clock +20 ppm", {-5000.0, 20, 1e9, 0}, 44, true, 55},
        {"low C/N0 (noise doubled: 41 dB-Hz at the zenith, 32 low in the sky), sample clock -20 ppm", {0.0, -20, 0.0, 0}, 44, true, 60},
        {"start 3.3 s into the stream, cold", {0.0, 0, 1e9, 13200000}, 44, false, 55},
    };
    if (getenv("GNSS_CASE")) { CaseOut out; runCase(cases[atoi(getenv("GNSS_CASE"))], out); fputs(out.text.c_str(), stdout); return 0; }
    const int fails = dect2::testpar::runCases(5, [&](size_t i, CaseOut& out) { runCase(cases[i], out); }, {4, 3, 0, 1, 2});
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("gnss qzss/sbas: ok\n");
    return 0;
}
