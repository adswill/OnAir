// The frequency error of real radios at 1575.42 MHz: an Airspy or a dongle with a TCXO is within 1-2 ppm (1.6-3 kHz), a HackRF within 20 ppm (31 kHz),
// an RTL-SDR without a TCXO 50-100 ppm (80-160 kHz). The sample clock comes from the same crystal (the simulator makes it follow the oscillator), so the
// code runs up to 100 chips a second off as well. On top of that the satellites' own Doppler of up to +-5 kHz. The receiver must find the radio's offset by
// itself, lock every satellite, fix, and report the offset. A last case gives it the offset remembered from an earlier run (2 kHz stale): it must lock fast.
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

struct Case { const char* what; double cfoHz; double secs; double lockBy; double hintHz; };

static const Case cases[] = {
    {"TCXO 1 ppm", 1600.0, 34, 6, 0},
    {"HackRF +20 ppm", 31500.0, 70, 22, 0},
    {"HackRF -20 ppm", -31500.0, 70, 22, 0},
    {"RTL-SDR -80 ppm", 126000.0, 90, 45, 0},
    {"RTL-SDR +100 ppm", -157500.0, 90, 45, 0},
    {"RTL-SDR +100 ppm, offset remembered", -157500.0, 34, 6, -155500.0},
};

static void runCase(const Case& k, CaseOut& out) {
    GnssSimConfig cfg;
    cfg.cfoHz = k.cfoHz;
    GnssSim sim(cfg, 4e6);
    int nTx = 0;
    for (auto& s : sim.sats()) nTx += s.transmitted;
    Options o;
    o.rate = 4e6; o.secs = k.secs; o.reportEvery = 1.0;
    if (k.hintHz != 0) o.setup = [&](GnssReceiver& rx) { rx.setFrequencyHint(k.hintHz, true); };
    Run r = run(sim, o);
    const GnssTelemetry& t = r.tel;
    double hz = 0, vt = 0;
    if (t.fix.valid) fixError(sim, t.fix, &hz, &vt);
    const int locked = lockedCount(t);
    out.print("%-36s %+8.0f Hz: first lock %5.1f s, %d of %d locked, %s, error %.1f / %.1f m, offset estimate %+.0f Hz, first fix %.1f s, longest feed() %.0f ms\n", k.what, k.cfoHz, r.firstLock, locked, nTx,
              t.fix.valid ? "fix" : "NO FIX", hz, vt, t.cfoHz, t.fix.firstFixSecs, r.maxFeedSecs * 1e3);
    CHECK(r.firstLock >= 0 && r.firstLock <= k.lockBy, "%s: first lock at %.1f s (wanted by %.0f s)", k.what, r.firstLock, k.lockBy);
    CHECK(locked >= nTx - 1, "%s: %d of %d locked", k.what, locked, nTx);
    CHECK(t.fix.valid && t.fix.nSats >= nTx - 2, "%s: no fix or too few satellites (%d of %d)", k.what, t.fix.nSats, nTx);
    CHECK(hz < 12.0 && std::fabs(vt) < 20.0, "%s: position error %.1f / %.1f m", k.what, hz, vt);
    CHECK(std::fabs(t.cfoHz - k.cfoHz) < 20.0, "%s: offset estimate %.0f Hz, true %.0f", k.what, t.cfoHz, k.cfoHz);
    CHECK(r.maxFeedSecs < 0.25, "%s: one feed() took %.0f ms", k.what, r.maxFeedSecs * 1e3);
}

int main() {
    // the cases share nothing: four at a time, the longest first (the log keeps the order above)
    const int fails = dect2::testpar::runCases(std::size(cases), [](size_t i, CaseOut& out) { runCase(cases[i], out); }, {3, 4, 1, 2, 0, 5});
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
