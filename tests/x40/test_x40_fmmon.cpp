// FM modulation monitor: the deviation, pilot, stereo band, RDS and SCA readings against what the generator put in, at three input rates, with carrier offset and drift,
// changes during the run, over-deviation, a noise sweep (the reading and the false pilot / RDS / SCA reports on a noise-only carrier). Simulated signals only.
#include "x40_gen2.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// a number after "label" in the facts
static double fact(const X40Telemetry& t, const char* key) { for (auto& f : t.facts) if (f.first == key) return atof(f.second.c_str()); return -999; }
static std::string factS(const X40Telemetry& t, const char* key) { for (auto& f : t.facts) if (f.first == key) return f.second; return ""; }
static int rows(const X40Telemetry& t, const char* kind) { return countKind(t, kind); }

int main() {
    for (double fs : {250000.0, 500000.0, 1000000.0}) for (double off : {0.0, 12000.0, -35000.0}) {
        Fm f; f.sca67 = 4000; f.sca92 = 3000; f.offsetHz = off; f.driftHzPerSec = off == 0 ? 400 : 0;
        auto x = fmStation(f, fs, 6.0);
        auto t = run(x40::FmMon, x, fs);
        const double pk = fact(t, "peak deviation") * 1000;
        CHECK(std::fabs(pk - f.peakHz) < 0.15 * f.peakHz,   // the 100 kHz channel filter takes a little off the 92 kHz SCA and the third-largest rule a little off the top
           "fs %.0f off %.0f: peak %.0f want %.0f", fs, off, pk, f.peakHz);
        const double pil = fact(t, "pilot 19 kHz") * 1000; CHECK(std::fabs(pil - 7500) < 400, "pilot %.0f", pil);
        const double rds = fact(t, "RDS 57 kHz") * 1000; CHECK(std::fabs(rds - 2000) < 350, "rds %.0f", rds);
        const double lmr = fact(t, "stereo (L-R) band") * 1000; CHECK(std::fabs(lmr - 15000 / std::sqrt(2.0)) < 900, "L-R %.0f", lmr);   // a DSB-SC band: its power as a sine is 1/sqrt(2) of the tone peak
        const double aud = fact(t, "audio (L+R)") * 1000; CHECK(std::fabs(aud - std::hypot(30000, 12000) * 1.0) < 6000 && aud > 25000, "audio %.0f", aud);
        const double o = fact(t, "carrier offset") * 1000; CHECK(std::fabs(o - (off + (off == 0 ? 400 * 3.0 : 0))) < 1200, "offset %.0f want %.0f", o, off);
        CHECK(factS(t, "RDS 57 kHz").find("present") != std::string::npos, "RDS present: %s", factS(t, "RDS 57 kHz").c_str());
        CHECK(factS(t, "SCA 67 / 92 kHz").find("absent") == std::string::npos, "SCA: %s", factS(t, "SCA 67 / 92 kHz").c_str());
        CHECK(t.headline.rfind("stereo", 0) == 0, "headline %s", t.headline.c_str());
    }
    // no pilot, no RDS, no SCA: mono
    { Fm f; f.pilotTo = 0; f.rdsTo = 0; auto t = run(x40::FmMon, fmStation(f, 250000, 5.0), 250000);
      CHECK(t.headline.rfind("mono", 0) == 0, "mono: %s", t.headline.c_str()); CHECK(factS(t, "SCA 67 / 92 kHz") == "absent / absent", "no SCA: %s", factS(t, "SCA 67 / 92 kHz").c_str()); CHECK(factS(t, "RDS 57 kHz").find("absent") != std::string::npos, "no RDS"); }
    // over-deviation (scaled to 95 kHz) is counted and listed; a normal one is not
    { Fm f; f.scale = 1.6; auto t = run(x40::FmMon, fmStation(f, 250000, 6.0), 250000); CHECK(rows(t, "Over-deviation") >= 4, "over: %d rows", rows(t, "Over-deviation"));
      Fm g; g.scale = 1.0; auto u = run(x40::FmMon, fmStation(g, 250000, 6.0), 250000); CHECK(rows(u, "Over-deviation") == 0, "no over: %d", rows(u, "Over-deviation")); }
    // the pilot and the RDS stop at 6 s, SCA starts at 3 s: three changes are logged
    { Fm f; f.pilotTo = 6; f.rdsTo = 6; f.sca67 = 4000; f.scaFrom = 3; auto t = run(x40::FmMon, fmStation(f, 250000, 11.0), 250000);
      CHECK(rows(t, "Pilot") == 2 && rows(t, "RDS") == 1 && rows(t, "SCA") == 1, "changes: pilot %d rds %d sca %d", rows(t, "Pilot"), rows(t, "RDS"), rows(t, "SCA")); }
    // noise: carrier to noise in 200 kHz; the readings and the false reports
    printf("noise sweep (carrier to noise in 200 kHz): peak deviation read / true, pilot, RDS, SCA 67 reports\n");
    for (double cnr : {40.0, 30.0, 25.0, 20.0, 15.0}) {
        Fm f; f.sca67 = 4000; auto x = fmStation(f, 250000, 6.0); addNoise(x, noiseFor(cnr, 200000, 250000), 5);
        auto t = run(x40::FmMon, x, 250000);
        printf("  %.0f dB: peak %.1f / %.1f kHz, pilot %.2f, RDS %s, SCA %s\n", cnr, fact(t, "peak deviation"), f.peakHz / 1000, fact(t, "pilot 19 kHz"), factS(t, "RDS 57 kHz").c_str(), factS(t, "SCA 67 / 92 kHz").c_str());
        if (cnr >= 30) { CHECK(std::fabs(fact(t, "pilot 19 kHz") - 7.5) < 0.6, "pilot at %.0f dB", cnr); CHECK(factS(t, "RDS 57 kHz").find("present") != std::string::npos, "RDS at %.0f dB", cnr); }
    }
    // an unmodulated carrier in noise: no pilot, no RDS, no SCA, no over-deviation reports in 10 minutes
    {
        int falseRows = 0;
        for (int k = 0; k < 10; k++) {
            std::vector<cf32> x((size_t)(250000 * 60), cf32(1, 0)); addNoise(x, noiseFor(k < 5 ? 35.0 : 22.0, 200000, 250000), 30 + k);
            auto t = run(x40::FmMon, x, 250000);
            for (auto& r : t.rows) if (r.kind != "Pilot" || r.id != "absent") falseRows++;
        }
        printf("plain carrier in noise (35 and 22 dB), 10 minutes: %d false reports\n", falseRows);
        CHECK(falseRows == 0, "false reports %d", falseRows);
    }
    {   // white noise alone: no carrier, no pilot, no buzz (3.9 of the sweep: the AM monitor "found a carrier", the FM monitor a 4.5 kHz pilot, UVB-76 a buzz)
        for (double sg : {0.02, 0.1, 0.5}) {
            std::vector<cf32> x((size_t)(250000 * 8), cf32(0, 0)); addNoise(x, (float)sg, 5);
            auto t = run(x40::FmMon, x, 250000);
            CHECK(t.blocksOk == 0, "FM monitor on noise (sigma %.2f): %llu decodes", sg, (unsigned long long)t.blocksOk);
        }
    }

    printf(fails ? "x40 fmmon: %d FAILED\n" : "x40 fmmon: ok\n", fails);
    return fails ? 1 : 0;
}
