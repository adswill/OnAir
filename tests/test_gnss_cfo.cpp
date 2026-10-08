// A radio whose oscillator is 3 kHz off at L1 (1.9 ppm, a poor TCXO) and whose sample clock is a further 20 ppm off: every signal appears shifted by 3 kHz
// on top of its Doppler, the code runs 20 chips a second off against the samples. The receiver must still acquire, decode and fix, and report the shift.
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
    const double cfos[2] = {3000.0, -3000.0}, sros[2] = {20.0, -20.0};
    GnssSimConfig cfg;
    cfg.cfoHz = cfos[k]; cfg.sroPpm = sros[k];
    GnssSim sim(cfg, 4e6);
    int nTx = 0;
    for (auto& s : sim.sats()) nTx += s.transmitted;
    Options o;
    o.rate = 4e6; o.secs = 32;
    Run r = run(sim, o);
    const GnssTelemetry& t = r.tel;
    double hz = 0, vt = 0;
    if (t.fix.valid) fixError(sim, t.fix, &hz, &vt);
    out.print("oscillator %+.0f Hz, sample clock %+.0f ppm: %s, error %.2f m horizontal %.2f m vertical, receiver's carrier error estimate %+.1f Hz, first fix %.1f s, %d of %d satellites\n", cfos[k], sros[k],
              gnssSummary(t).c_str(), hz, vt, t.cfoHz, t.fix.firstFixSecs, t.fix.nSats, nTx);
    CHECK(t.fix.valid, "no fix with an oscillator error of %.0f Hz", cfos[k]);
    CHECK(t.fix.nSats >= nTx - 2, "only %d satellites in the fix of %d", t.fix.nSats, nTx);
    CHECK(hz < 12.0 && std::fabs(vt) < 20.0, "position error %.1f m / %.1f m", hz, vt);
    CHECK(std::fabs(t.cfoHz - cfos[k]) < 8.0, "carrier error estimate %.1f Hz, true %.0f", t.cfoHz, cfos[k]);
    CHECK(t.fix.firstFixSecs > 22 && t.fix.firstFixSecs < 31, "first fix %.1f s", t.fix.firstFixSecs);
}

int main() {
    // the two cases share nothing: side by side, the log keeps the order
    const int fails = dect2::testpar::runCases(2, [](size_t i, CaseOut& out) { runCase((int)i, out); });
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
