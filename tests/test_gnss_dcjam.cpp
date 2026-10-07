// A radio's DC spike (0.05 of full scale, a quarter of the noise rms) and a continuous-wave jammer 400 kHz above the centre (amplitude 0.3, a little above the
// noise rms): the receiver removes the DC, cuts the line out of the spectrum, and still acquires, decodes and fixes. (Without the excision the jammer makes
// false satellites: the code's line spectrum has a line at every kilohertz that a jammer on a multiple of 1 kHz correlates with.)
#include "dect2/gnss_testkit.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::gnsstest;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    GnssSimConfig cfg;
    cfg.jammer = true; cfg.dcOffset = 0.05;
    GnssSim sim(cfg, 4e6);
    int nTx = 0;
    for (auto& s : sim.sats()) nTx += s.transmitted;
    Options o;
    o.rate = 4e6; o.secs = 32;
    Run r = run(sim, o);
    const GnssTelemetry& t = r.tel;
    double hz = 0, vt = 0;
    if (t.fix.valid) fixError(sim, t.fix, &hz, &vt);
    printf("DC 0.05 and a jammer: %s, error %.2f / %.2f m, input level %.1f dBFS, DC estimate %+.4f %+.4f, first fix %.1f s\n", gnssSummary(t).c_str(), hz, vt, t.levelDbfs, t.dcI, t.dcQ, t.fix.firstFixSecs);
    CHECK(t.fix.valid, "no fix");
    CHECK(t.fix.nSats >= nTx - 2, "%d satellites in the fix of %d", t.fix.nSats, nTx);
    CHECK(hz < 12.0 && std::fabs(vt) < 20.0, "position error %.1f / %.1f m", hz, vt);
    CHECK(std::fabs(t.dcI - 0.05) < 0.005 && std::fabs(t.dcQ - 0.05) < 0.005, "DC estimate %.4f %.4f", t.dcI, t.dcQ);
    CHECK(t.channels.size() <= (size_t)nTx + 2, "%zu channels for %d satellites: false satellites from the jammer", t.channels.size(), nTx);
    for (auto& c : t.channels) CHECK(sim.sats()[(size_t)c.prn - 1].transmitted, "PRN %d is tracked but is not in the signal", c.prn);
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
