// The widest capture the HackRF makes, 20 Msps: the front end mixes, filters and resamples to 4.096 Msps, and the rest is as at 4 Msps. A full fix, and the
// speed of feed() at this rate (the number is printed). A second check with the signal 6 MHz off the centre (the tuned centre set to 1581.42 MHz): the
// receiver mixes the band down itself.
#include "dect2/gnss_testkit.h"
#include "dect2/test_parallel.h"
#include <cstdlib>
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::gnsstest;
using dect2::testpar::CaseOut;
// every case runs on its own thread and prints through `out`, so that the log keeps the order of the cases
#define CHECK(c, ...) do { if (!(c)) out.fail(__LINE__, __VA_ARGS__); } while (0)

// the full run at the centre
static void centred(CaseOut& out) {
    GnssSimConfig cfg;
    GnssSim sim(cfg, 20e6);
    int nTx = 0;
    for (auto& s : sim.sats()) nTx += s.transmitted;
    Options o;
    o.rate = 20e6; o.secs = 30;
    Run r = run(sim, o);
    const GnssTelemetry& t = r.tel;
    double hz = 0, vt = 0;
    if (t.fix.valid) fixError(sim, t.fix, &hz, &vt);
    out.print("20 Msps: %s, error %.2f / %.2f m, first fix %.1f s; feed() took %.1f s for %.0f s of signal: %.1fx real time\n", gnssSummary(t).c_str(), hz, vt, t.fix.firstFixSecs, r.cpuSecs, o.secs, o.secs / r.cpuSecs);
    CHECK(t.fix.valid && t.fix.nSats >= nTx - 2, "no fix, or too few satellites (%d of %d)", t.fix.nSats, nTx);
    CHECK(hz < 12.0 && std::fabs(vt) < 20.0, "position error %.1f / %.1f m", hz, vt);
    if (!GNSS_SANITIZED && !std::getenv("CI")) CHECK(o.secs / r.cpuSecs > 2.0, "real-time factor %.2f", o.secs / r.cpuSecs);
    CHECK(t.activeMask == 1u && t.inputRate == 20e6, "set-up fields");
}

static void offCentre(CaseOut& out) {
    // an off-centre signal: the receiver is told the tuning (the engine's tuned frequency is not passed on to it yet: setCenterMhz)
    {
        GnssSimConfig c2;
        GnssSim s2(c2, 20e6);
        int nTx = 0;
        for (auto& s : s2.sats()) nTx += s.transmitted;
        // the simulated signal is at the centre of its baseband; shift it by 6 MHz in the test, as an off-centre tuning would
        Options o2;
        o2.rate = 20e6; o2.secs = 5; o2.centerMhz = 1575.42 - 6.0;
        double ph = 0;
        o2.hook = [&](std::vector<cf32>& b, size_t) -> size_t {
            const double w = 2 * 3.14159265358979 * 6e6 / 20e6;
            for (auto& v : b) { v *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph += w; if (ph > 6.2831853) ph -= 6.2831853; }
            return b.size();
        };
        Run r2 = run(s2, o2);
        int locked = 0;
        for (auto& c : r2.tel.channels) if (c.state >= GnssChLocked) locked++;
        out.print("signal 6 MHz above the tuned centre: %d satellites locked after 5 s\n", locked);
        CHECK(locked >= nTx - 2, "only %d locked with an off-centre signal", locked);
    }
}

int main() {
    // the two runs share nothing: side by side, the log keeps the order
    const int fails = dect2::testpar::runCases(2, [](size_t i, CaseOut& out) { if (i == 0) centred(out); else offCentre(out); });
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
