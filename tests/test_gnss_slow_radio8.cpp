// What an RTL-SDR really delivers: 2.048, 2.4 or 2.56 Msps (asked for 4, the dongle stops at 2.56), unsigned 8-bit samples at a low level (the noise covers
// only a few steps), a DC offset, I/Q gain and phase imbalance, and an oscillator that is 1.5 kHz off with a TCXO and warms up (drifts). The last case is a
// dongle without a TCXO, 60 ppm off, at 2.048 Msps. The receiver must lock, decode and fix in every case.
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

struct Case { double rate; double noiseSteps; double cfoHz; double drift; double secs; };
static const Case cases[] = {
    {2.048e6, 4.0, 1500, 2.0, 40},
    {2.4e6, 3.0, -1500, -2.0, 40},
    {2.56e6, 2.0, 1500, 2.0, 40},
    {2.048e6, 4.0, -94500, 5.0, 90},
};

static void runCase(const Case& k, CaseOut& out) {
    GnssSimConfig cfg;
    cfg.noiseRms = k.noiseSteps / 127.5;        // per component, in converter steps
    cfg.cfoHz = k.cfoHz;
    GnssSim sim(cfg, k.rate);
    int nTx = 0;
    for (auto& s : sim.sats()) nTx += s.transmitted;
    Radio8 rd;
    rd.driftHzPerS = k.drift;
    Options o;
    o.rate = k.rate; o.secs = k.secs; o.quantise8 = false; o.reportEvery = 1.0;
    o.hook = radio8Hook(rd, k.rate);
    Run r = run(sim, o);
    const GnssTelemetry& t = r.tel;
    double hz = 0, vt = 0;
    if (t.fix.valid) fixError(sim, t.fix, &hz, &vt);
    const int locked = lockedCount(t);
    out.print("%.3f Msps, noise %.0f steps, %+.0f Hz drifting %+.0f Hz/s: first lock %.1f s, %d of %d locked, %s %d satellites, error %.1f / %.1f m, first fix %.1f s, DC seen %+.4f %+.4f, %.1fx real time\n",
              k.rate / 1e6, k.noiseSteps, k.cfoHz, k.drift, r.firstLock, locked, nTx, t.fix.valid ? "fix with" : "no fix,", t.fix.nSats, hz, vt, t.fix.firstFixSecs, t.dcI, t.dcQ, o.secs / r.cpuSecs);
    CHECK(locked >= nTx - 2, "%.3f Msps: %d of %d locked", k.rate / 1e6, locked, nTx);
    CHECK(t.fix.valid && t.fix.nSats >= nTx - 3, "%.3f Msps: no fix or too few satellites (%d of %d)", k.rate / 1e6, t.fix.nSats, nTx);
    CHECK(hz < 15.0 && std::fabs(vt) < 25.0, "%.3f Msps: position error %.1f / %.1f m", k.rate / 1e6, hz, vt);
}

int main() {
    // the cases share nothing: all four at once, the log keeps the order above
    const int fails = dect2::testpar::runCases(std::size(cases), [](size_t i, CaseOut& out) { runCase(cases[i], out); }, {3, 0, 1, 2});
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
