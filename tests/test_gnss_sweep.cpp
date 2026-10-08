// Signal strength sweep: the same sky with the zenith C/N0 lowered. Lower satellites are weaker still (down to 9 dB under the zenith one), so each run shows
// how many satellites each level yields. The numbers are printed: where acquisition stops, where bits stop decoding, where the fix is lost.
#include "dect2/gnss_testkit.h"
#include "dect2/test_parallel.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::gnsstest;
using dect2::testpar::CaseOut;
// every case runs on its own thread and prints through `out`, so that the log keeps the order of the cases
#define CHECK(c, ...) do { if (!(c)) out.fail(__LINE__, __VA_ARGS__); } while (0)

static void runCase(int k, CaseOut& out) {
    const double tops[3] = {40.0, 36.0, 32.0};
    {
        GnssSimConfig cfg;
        cfg.cn0Top = tops[k];
        GnssSim sim(cfg, 4e6);
        Options o;
        o.rate = 4e6; o.secs = 34;
        Run r = run(sim, o);
        const GnssTelemetry& t = r.tel;
        double lowestTracked = 99, highestMissed = 0;
        int nTx = 0, tracked = 0, decoded = 0;
        for (auto& s : sim.sats()) {
            if (!s.transmitted) continue;
            nTx++;
            bool found = false;
            for (auto& c : t.channels) if (c.prn == s.prn && c.state >= GnssChLocked) { found = true; if (c.state >= GnssChFrameSync) decoded++; }
            if (found) { tracked++; lowestTracked = std::fmin(lowestTracked, s.cn0); }
            else highestMissed = std::fmax(highestMissed, s.cn0);
        }
        double hz = 0, vt = 0;
        if (t.fix.valid) fixError(sim, t.fix, &hz, &vt);
        out.print("zenith %.0f dB-Hz (satellites from %.1f to %.1f): %d of %d tracked (lowest %.1f dB-Hz, strongest missed %.1f dB-Hz), %d with the frame decoded, bad subframes %llu, %s", tops[k],
               tops[k] - 9.0, tops[k], tracked, nTx, lowestTracked, highestMissed, decoded, (unsigned long long)t.blocksBad, t.fix.valid ? "fix" : "no fix");
        if (t.fix.valid) out.print(" (%d satellites, error %.1f / %.1f m, HDOP %.1f, first fix %.1f s)", t.fix.nSats, hz, vt, t.fix.hdop, t.fix.firstFixSecs);
        out.print("\n");
        for (auto& c : t.channels) CHECK(c.cn0 - sim.sats()[(size_t)c.prn - 1].cn0 > -4.0 && c.cn0 - sim.sats()[(size_t)c.prn - 1].cn0 < 3.0, "PRN %d: C/N0 %.1f, true %.1f", c.prn, c.cn0, sim.sats()[(size_t)c.prn - 1].cn0);
        CHECK(t.blocksBad <= t.blocksOk / 20 + 1, "%llu bad subframes against %llu good", (unsigned long long)t.blocksBad, (unsigned long long)t.blocksOk);
        if (k == 0) { CHECK(t.fix.valid && t.fix.nSats >= 4 && hz < 40.0, "a 40 dB-Hz sky must give a fix"); CHECK(tracked >= 5, "only %d satellites tracked at 40 dB-Hz", tracked); }
        if (k == 1) { CHECK(tracked >= 3, "only %d satellites tracked at 36 dB-Hz", tracked); CHECK(decoded >= 3, "only %d decoded", decoded); }
        // nothing is claimed for the weakest sky: it is there to show the limit; the receiver must not invent satellites
        for (auto& c : t.channels) if (c.state >= GnssChLocked) CHECK(sim.sats()[(size_t)c.prn - 1].transmitted, "PRN %d tracked but not transmitted", c.prn);
    }
}

int main() {
    // the three skies share nothing: side by side, the log keeps the order
    const int fails = dect2::testpar::runCases(3, [](size_t i, CaseOut& out) { runCase((int)i, out); });
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
