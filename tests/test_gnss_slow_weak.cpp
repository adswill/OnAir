// Real signal levels: GPS L1 arrives at about -130 dBm, C/N0 35-45 dB-Hz with a good active antenna and 25-35 with a poor one or a partly blocked sky.
// The simulated satellites are weaker away from the zenith (about 9 dB at the horizon), so a top C/N0 of 40 gives 31-40, 36 gives 27-36, 33 gives 24-33.
// The receiver must find the weak ones too (a longer search when the first one finds too few), make no false satellites, and still fix.
#include "dect2/gnss_testkit.h"
#include "dect2/test_parallel.h"
#include <cmath>
#include <cstdio>
#include <iterator>
using namespace dect2;
using namespace dect2::gnsstest;
using dect2::testpar::CaseOut;
// every case runs on its own thread and prints through `out`, so that the log keeps the order of the cases
#define CHECK(c, ...) do { if (!(c)) out.fail(__LINE__, __VA_ARGS__); } while (0)

struct Case { double top; double secs; int minFound30; bool wantFix; };
// minFound30: of the satellites at 30 dB-Hz or more, all but this many must be locked by the end
static const Case cases[] = {{40, 60, 1, true}, {36, 75, 1, true}, {33, 75, 1, false}};

static void runCase(const Case& k, CaseOut& out) {
    GnssSimConfig cfg;
    cfg.cn0Top = k.top;
    cfg.cfoHz = 2500;            // a radio with a TCXO
    GnssSim sim(cfg, 4e6);
    int nTx = 0, n30 = 0, n35 = 0;
    for (auto& s : sim.sats()) if (s.transmitted) { nTx++; n30 += s.cn0 >= 30; n35 += s.cn0 >= 35; }
    Options o;
    o.rate = 4e6; o.secs = k.secs; o.reportEvery = 1.0;
    Run r = run(sim, o);
    const GnssTelemetry& t = r.tel;
    int locked = 0, locked30 = 0, falseSats = 0;
    for (auto& c : t.channels) {
        const GnssSimSat& s = sim.sats()[(size_t)c.prn - 1];
        if (!s.transmitted) { falseSats++; continue; }
        if (c.state >= GnssChLocked) { locked++; locked30 += s.cn0 >= 30; }
    }
    double hz = 0, vt = 0;
    if (t.fix.valid) fixError(sim, t.fix, &hz, &vt);
    out.print("top C/N0 %.0f dB-Hz (%d satellites, %d at 30+, %d at 35+): first lock %.1f s, %d locked (%d of the 30+), %d false, %s %d satellites, error %.1f / %.1f m, first fix %.1f s, %.1fx real time\n",
              k.top, nTx, n30, n35, r.firstLock, locked, locked30, falseSats, t.fix.valid ? "fix with" : "no fix,", t.fix.nSats, hz, vt, t.fix.firstFixSecs, o.secs / r.cpuSecs);
    CHECK(locked30 >= n30 - k.minFound30, "top %.0f: %d of the %d satellites at 30 dB-Hz or more locked", k.top, locked30, n30);
    CHECK(falseSats == 0, "top %.0f: %d satellites tracked that are not in the signal", k.top, falseSats);
    if (k.wantFix) {
        CHECK(t.fix.valid && t.fix.nSats >= 4, "top %.0f: no fix", k.top);
        CHECK(hz < 25.0 && std::fabs(vt) < 40.0, "top %.0f: position error %.1f / %.1f m", k.top, hz, vt);
    }
}

int main() {
    // the cases share nothing: all three at once, the log keeps the order above
    const int fails = dect2::testpar::runCases(std::size(cases), [](size_t i, CaseOut& out) { runCase(cases[i], out); }, {1, 2, 0});
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
