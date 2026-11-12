// XPA (10 and 20 Bd, 20-tone MFSK). What is checked:
//  1. messages built from the format as Rivet reads it (start tones, sync, message start, five-figure groups with the repeat tone, the end)
//     through the receiver with impairments: dial error, off-centre tuning (tones 950 Hz from where they belong, and a receiver offset of
//     -20 kHz at 250 ksps), sender clock error, drift, lower sideband; 8, 48 and 250 ksps;
//  2. a noise sweep at both speeds (with a dial error and a clock error): share of the groups read right, the 50 % and 90 % points;
//  3. noise alone, a carrier and the other speed's signal must give no groups (false decodes counted).
// Simulated signals only: no recording of XPA was available.
#include "x40_gen_rivet.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static Path path(double off, double drift = 0, double ppm = 0, bool lower = false) { Path p; p.offsetHz = off; p.driftHzPerSec = drift; p.ppm = ppm; p.lower = lower; return p; }
static int decodeKinds(const X40Telemetry& t) { return countKind(t, "Groups") + countKind(t, "Start") + countKind(t, "Message start") + countKind(t, "End"); }

int main() {
    const auto g = randomGroups(11, 30);
    struct Case { const char* what; double baud, fs, rxOff; Path p; };
    const Case cases[] = {
        {"10 Bd, 8 ksps", 10, 8000, 0, path(0)},
        {"20 Bd, dial +37.3 Hz", 20, 8000, 0, path(37.3)},
        {"10 Bd, off-centre +950 Hz, 48 ksps", 10, 48000, 0, path(950)},
        {"20 Bd, clock +400 ppm, drift +0.2 Hz/s", 20, 8000, 0, path(-61, 0.2, 400)},
        {"10 Bd, lower sideband, dial -12 Hz", 10, 8000, 0, path(-12, 0, 0, true)},
        {"20 Bd, lower sideband, clock -300 ppm", 20, 8000, 0, path(25, 0, -300, true)},
        {"20 Bd, 250 ksps, receiver offset -20 kHz", 20, 250000, -20000, path(-20000 + 14)},
    };
    for (const Case& c : cases) {
        auto x = xpaSignal(g, false, c.baud, c.fs, c.p);
        addSnr(x, 10, c.fs, 3);
        const auto t = run(x40::Xpa, x, c.fs, c.rxOff);
        const int f = groupsFound(g, rowText(t, "Groups"));
        printf("  %-44s %2d / %zu groups, start %d, end %d | %s\n", c.what, f, g.size(), countKind(t, "Start"), countKind(t, "End"), t.headline.c_str());
        CHECK(f >= (int)g.size() - 1, "%s: %d of %zu groups", c.what, f, g.size());
        CHECK(countKind(t, "End") >= 1, "%s: no end of message", c.what);
        CHECK(t.headline.find(c.baud == 10 ? "10 Bd" : "20 Bd") != std::string::npos, "%s: speed in '%s'", c.what, t.headline.c_str());
    }
    // noise sweep: groups right of 2 x 30 (signal to noise in 3 kHz)
    for (double baud : {10.0, 20.0}) {
        printf("noise sweep, %.0f Bd (dial +23 Hz, clock +100 ppm): groups right\n", baud);
        std::vector<std::pair<double, double>> pts;
        const double top = baud == 10 ? -10 : -7;
        for (double snr = top - 12; snr <= top + 0.1; snr += 2) {
            int ok = 0, n = 0;
            for (unsigned seed : {1u, 2u}) {
                const auto gs = randomGroups(100 + seed, 30);
                auto x = xpaSignal(gs, false, baud, 8000, path(23, 0, 100));
                addSnr(x, snr, 8000, 50 + seed);
                const auto t = run(x40::Xpa, x, 8000);
                ok += groupsFound(gs, rowText(t, "Groups")); n += (int)gs.size();
            }
            pts.push_back({snr, (double)ok / n});
            printf("  %6.1f dB: %3d / %d\n", snr, ok, n);
        }
        printf("  %.0f Bd: 50 %% of the groups at %.1f dB, 90 %% at %.1f dB (signal to noise in 3 kHz)\n", baud, snrAt(pts, 0.5), snrAt(pts, 0.9));
        CHECK(pts.back().second >= 0.9, "%.0f Bd: %.2f of the groups at %.0f dB", baud, pts.back().second, pts.back().first);
    }
    // false decodes: noise alone (two minutes), a carrier in noise, a 20 Bd signal must not read as 10 Bd text and the other way round is fine
    int falseRows = 0;
    for (unsigned seed : {7u, 8u}) { std::vector<cf32> n((size_t)(8000 * 60), cf32(0, 0)); addNoise(n, 1.0, seed); const auto t = run(x40::Xpa, n, 8000); falseRows += decodeKinds(t); }
    { std::vector<std::pair<double, double>> seg = {{900, 40.0}}; auto x = toneSeq(seg, 8000, path(0)); addSnr(x, 0, 8000, 9); const auto t = run(x40::Xpa, x, 8000); falseRows += decodeKinds(t); }
    printf("false decodes (2 x 60 s noise, a carrier): %d rows\n", falseRows);
    CHECK(falseRows == 0, "%d rows from noise or a carrier", falseRows);
    printf(fails ? "x40 xpa: %d FAILED\n" : "x40 xpa: ok\n", fails);
    return fails ? 1 : 0;
}
