// GNSS interference detector: a clean band, a tone, narrowband interference, a sawtooth chirp (range, period, rate) and a wideband noise jammer at several levels and two input rates,
// start and stop during a run, and 20 minutes of clean noise. Simulated signals only.
#include "x40_gen2.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::string factS(const X40Telemetry& t, const char* key) { for (auto& f : t.facts) if (f.first == key) return f.second; return ""; }
static double num(const std::string& s) { return atof(s.c_str()); }
static dect2::X40Telemetry go(const Jam& j, double fs, double sec) { JamGen g(j, fs); return runStream(x40::GnssJam, fs, sec, [&](size_t i0, size_t n, cf32* o) { g.render(i0, n, o); }); }
static int rowsOf(const X40Telemetry& t, const char* kind) { return countKind(t, kind); }

int main() {
    for (double fs : {4.0e6, 10.0e6}) {
        { Jam j; auto t = go(j, fs, 4); CHECK(factS(t, "state") == "clean", "clean fs %.0f: %s", fs, factS(t, "state").c_str()); }
        for (double db : (fs > 5e6 ? std::vector<double>{20.0} : std::vector<double>{30.0, 15.0, 8.0})) {
            { Jam j; j.kind = Jam::Cw; j.centerHz = -0.9e6; j.jamDb = db - 8; auto t = go(j, fs, 4);       // a tone concentrates its power in one bin, so it is far over the floor at a lower total power
              CHECK(factS(t, "state") == "continuous tone" && std::fabs(num(factS(t, "strongest line")) - -0.9) < 0.01, "cw %.0f dB fs %.0f: %s / %s", db, fs, factS(t, "state").c_str(), factS(t, "strongest line").c_str()); }
            { Jam j; j.kind = Jam::Narrow; j.centerHz = 0.5e6; j.bwHz = 150e3; j.jamDb = db; auto t = go(j, fs, 4);
              CHECK(factS(t, "state") == "narrowband interference", "narrow %.0f dB fs %.0f: %s", db, fs, factS(t, "state").c_str()); }
            { Jam j; j.kind = Jam::Chirp; j.centerHz = 0.3e6; j.spanHz = 0.7 * (fs > 5e6 ? 8e6 : 2.8e6); j.periodSec = 12e-6; j.jamDb = db; auto t = go(j, fs, 4);
              CHECK(factS(t, "state").rfind("swept", 0) == 0, "chirp %.0f dB fs %.0f: %s", db, fs, factS(t, "state").c_str());
              const auto sr = factS(t, "sweep range"), sp = factS(t, "sweep period");
              CHECK(std::fabs(num(sr) - j.spanHz / 1e6) < 0.2 * j.spanHz / 1e6 && std::fabs(num(sp) - 12.0) < 1.0, "chirp read: %s | %s", sr.c_str(), sp.c_str()); }
            { Jam j; j.kind = Jam::Wide; j.jamDb = db; j.fromSec = 2; auto t = go(j, fs, 6); CHECK(factS(t, "state") == "wideband noise-like interference", "wide %.0f dB fs %.0f: %s", db, fs, factS(t, "state").c_str()); }
        }
    }
    // starts at 4 s, stops at 9 s: two rows (the interference, then the clean state) after two seconds each
    { Jam j; j.kind = Jam::Chirp; j.centerHz = 0; j.spanHz = 2.8e6; j.fromSec = 4; j.toSec = 9; j.jamDb = 20; auto t = go(j, 4e6, 14); CHECK(rowsOf(t, "Interference") == 1 && rowsOf(t, "State") == 1, "events: %d interference %d state", rowsOf(t, "Interference"), rowsOf(t, "State")); }
    // detection limits: interference to noise power
    printf("level sweep at 4 Msps (interference to noise power, 4 s): class read\n");
    for (double db : {6.0, 3.0, 0.0, -3.0}) {
        std::string line;
        { Jam j; j.kind = Jam::Cw; j.centerHz = 0.4e6; j.jamDb = db; line += "tone: " + factS(go(j, 4e6, 4), "state") + "; "; }
        { Jam j; j.kind = Jam::Chirp; j.spanHz = 2.8e6; j.jamDb = db; line += "chirp: " + factS(go(j, 4e6, 4), "state") + "; "; }
        { Jam j; j.kind = Jam::Wide; j.jamDb = db; j.fromSec = 2; line += "wide: " + factS(go(j, 4e6, 6), "state"); }
        printf("  %+.0f dB: %s\n", db, line.c_str());
    }
    // 4 minutes of clean noise: no row (20 runs of 12 s at 2 Msps)
    { int falseRows = 0; for (int k = 0; k < 20; k++) { Jam j; j.seed = 100 + k; auto t = go(j, 2e6, 12); falseRows += (int)t.rows.size(); }
      printf("clean noise, 4 minutes: %d false rows\n", falseRows); CHECK(falseRows == 0, "false rows %d", falseRows); }
    printf(fails ? "x40 gnssjam: %d FAILED\n" : "x40 gnssjam: ok\n", fails);
    return fails ? 1 : 0;
}
