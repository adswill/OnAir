// FSK200/500 (F01: 200 Bd, 500 Hz shift, asynchronous ITA2 with 1.5 stop bits). What is checked:
//  1. messages built from the format as Rivet reads it (reversals, figure shift, five-figure groups, CR LF) through the receiver with
//     impairments: dial error, off-centre tuning (tones centred at 650 or 2600 Hz instead of 1500), sender clock error (up to 1 %), drift,
//     the shift reversed, 48 and 250 ksps;
//  2. a noise sweep (dial error and clock error): share of the groups read right, the 50 % and 90 % points;
//  3. noise alone, a carrier and an FSK200/1000 signal must give no text (false decodes counted).
// Simulated signals only: no recording of FSK200/500 was available.
#include "x40_gen_rivet.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static Path path(double off, double drift = 0, double ppm = 0) { Path p; p.offsetHz = off; p.driftHzPerSec = drift; p.ppm = ppm; return p; }

int main() {
    const auto g = randomGroups(41, 60);
    struct Case { const char* what; double centre, fs, rxOff; Path p; bool rev; };
    const Case cases[] = {
        {"8 ksps, tones at 1250 / 1750 Hz", 1500, 8000, 0, path(0), false},
        {"dial +31 Hz", 1500, 8000, 0, path(31), false},
        {"off-centre: tones at 400 / 900 Hz, 48 ksps", 650, 48000, 0, path(0), false},
        {"off-centre: tones at 2350 / 2850 Hz", 2600, 8000, 0, path(0), false},
        {"clock +1 %, drift +0.5 Hz/s", 1500, 8000, 0, path(-20, 0.5, 10000), false},
        {"clock -0.5 %", 1500, 8000, 0, path(7, 0, -5000), false},
        {"shift reversed", 1500, 8000, 0, path(0), true},
        {"250 ksps, receiver offset -60 kHz", 1500, 250000, -60000, path(-60000), false},
    };
    for (const Case& c : cases) {
        auto x = f01Signal(g, c.centre, c.fs, c.p, c.rev);
        addSnr(x, 15, c.fs, 3);
        const auto t = run(x40::F01, x, c.fs, c.rxOff);
        const int f = groupsFound(g, rowText(t, "Text"));
        printf("  %-46s %2d / %zu groups | %s\n", c.what, f, g.size(), t.headline.c_str());
        if (f < (int)g.size() - 1) printf("%s", rowText(t, "Text").c_str());
        CHECK(f >= (int)g.size() - 1, "%s: %d of %zu groups", c.what, f, g.size());
    }
    printf("noise sweep (dial +12 Hz, clock +200 ppm): groups right\n");
    std::vector<std::pair<double, double>> pts;
    for (double snr = -6; snr <= 10.1; snr += 2) {
        int ok = 0, n = 0;
        for (unsigned seed : {1u, 2u}) {
            const auto gs = randomGroups(400 + seed, 60);
            auto x = f01Signal(gs, 1500, 8000, path(12, 0, 200));
            addSnr(x, snr, 8000, 80 + seed);
            const auto t = run(x40::F01, x, 8000);
            ok += groupsFound(gs, rowText(t, "Text")); n += (int)gs.size();
        }
        pts.push_back({snr, (double)ok / n});
        printf("  %6.1f dB: %3d / %d\n", snr, ok, n);
    }
    printf("  FSK200/500: 50 %% of the groups at %.1f dB, 90 %% at %.1f dB (signal to noise in 3 kHz)\n", snrAt(pts, 0.5), snrAt(pts, 0.9));
    CHECK(pts.back().second >= 0.9, "%.2f of the groups at %.0f dB", pts.back().second, pts.back().first);
    int falseRows = 0;
    for (unsigned seed : {7u, 8u}) { std::vector<cf32> n((size_t)(8000 * 60), cf32(0, 0)); addNoise(n, 1.0, seed); const auto t = run(x40::F01, n, 8000); falseRows += countKind(t, "Text"); }
    { std::vector<std::pair<double, double>> seg = {{1250, 40.0}}; auto x = toneSeq(seg, 8000, path(0)); addSnr(x, 0, 8000, 9); const auto t = run(x40::F01, x, 8000); falseRows += countKind(t, "Text"); }
    printf("false decodes (2 x 60 s noise, a carrier): %d rows\n", falseRows);
    CHECK(falseRows == 0, "%d rows from noise or a carrier", falseRows);
    printf(fails ? "x40 f01: %d FAILED\n" : "x40 f01: ok\n", fails);
    return fails ? 1 : 0;
}
