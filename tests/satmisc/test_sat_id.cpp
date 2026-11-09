// Satellite identification by Doppler curve: simulated tracks only (no real recording of a pass goes through this test).
#include "dect2/satmisc/sat_id.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::satmisc;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static Tle makeTle(int norad, double incl, double raan, double m, double nm) {
    Tle t; t.name = "SAT " + std::to_string(norad); t.norad = norad; t.epochJd = 2461000.5; t.bstar = 1e-5; t.inclDeg = incl; t.raanDeg = raan; t.ecc = 0.001; t.argpDeg = 90; t.meanAnomDeg = m; t.meanMotion = nm;
    return t;
}
static std::vector<DopplerSample> track(const Sgp4& s, const Observer& o, double t0, double t1, double f, double off, double noise, uint32_t seed, const SatIdOptions& opt) {
    std::mt19937 g(seed); std::normal_distribution<double> n(0, 1);
    std::vector<DopplerSample> v;
    for (double t = t0; t < t1; t += 2) { double h = 0, el = 0; if (predictDopplerHz(s, o, t, opt, h, el)) v.push_back({t, h + off + noise * n(g), 0}); }
    return v;
}
// find a visible window of 300 s
static bool window(const Sgp4& s, const Observer& o, double start, double& t0, double& t1) {
    auto p = findPasses(s, o, start, start + 86400, 20);
    for (const auto& x : p) if (x.los - x.aos > 300) { t0 = x.tca - 150; t1 = x.tca + 150; return true; }
    return false;
}

int main() {
    const Observer rx{54.7, 25.3, 0.1};
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(0, 360), inc(50, 100), nm(13.5, 15.6);
    std::vector<Tle> cat;
    for (int i = 0; i < 1500; i++) cat.push_back(makeTle(10000 + i, inc(rng), u(rng), u(rng), nm(rng)));
    const double start = jdToUnix(2461000.5) + 3600;
    int tested = 0, right = 0, confident = 0;
    SatIdOptions opt; opt.refHz = 437.5e6; opt.refine = 1;
    for (int i = 0; i < 1500 && tested < 12; i += 37) {
        Sgp4 s; if (!s.init(cat[(size_t)i])) continue;
        double t0, t1; if (!window(s, rx, start, t0, t1)) continue;
        tested++;
        const double off = (tested % 2 ? 1 : -1) * 3000.0 * tested / 4;   // transmit frequency and radio error unknown
        const auto v = track(s, rx, t0, t1, opt.refHz, off, 15, (uint32_t)i, opt);
        SatIdResult r = identifySatellite(cat, rx, v, opt);
        if (r.ok && r.ranked[0].tle.norad == cat[(size_t)i].norad) right++;
        if (r.confident) confident++;
        CHECK(r.ok, "not identified, why: %s", r.why.c_str());
        if (r.ok) CHECK(std::fabs(r.ranked[0].offsetHz - off) < 40, "offset %.0f, true %.0f", r.ranked[0].offsetHz, off);
    }
    printf("identified %d of %d simulated tracks among %zu satellites, %d confident\n", right, tested, cat.size(), confident);
    CHECK(tested >= 8, "only %d tracks", tested);
    CHECK(right == tested, "%d of %d", right, tested);
    CHECK(confident >= tested - 1, "confident %d", confident);

    // Noise sweep: a wrong answer must not be confident
    Sgp4 s0; s0.init(cat[37]);
    double t0 = 0, t1 = 0; CHECK(window(s0, rx, start, t0, t1), "no window");
    for (double noise : {5.0, 30.0, 60.0, 150.0, 400.0}) {
        const auto v = track(s0, rx, t0, t1, opt.refHz, 1200, noise, 3, opt);
        SatIdResult r = identifySatellite(cat, rx, v, opt);
        const bool got = r.ok && r.ranked[0].tle.norad == cat[37].norad;
        printf("  noise %.0f Hz rms: %s rms %.1f confident %d\n", noise, got ? "right" : (r.ok ? "WRONG" : "none"), r.ranked.empty() ? -1.0 : r.ranked[0].rmsHz, r.confident);
        if (r.ok && r.confident) CHECK(got, "confident wrong answer at noise %.0f", noise);
        if (noise <= 30) CHECK(got, "missed at %.0f", noise);
    }
    // No Doppler in the track (a fixed interferer) gives no answer
    { std::vector<DopplerSample> v; for (double t = t0; t < t1; t += 2) v.push_back({t, 500.0, 0}); SatIdResult r = identifySatellite(cat, rx, v, opt); CHECK(!r.ok, "fixed line identified as %s", r.ranked.empty() ? "?" : r.ranked[0].tle.name.c_str()); }
    // Two satellites of a launch train: ambiguous, not a pick
    { std::vector<Tle> two = cat; Tle c = cat[37]; c.norad = 99999; c.name = "TWIN"; c.meanAnomDeg += 0.003; two.push_back(c);
      const auto v = track(s0, rx, t0, t1, opt.refHz, 0, 15, 4, opt); SatIdResult r = identifySatellite(two, rx, v, opt); CHECK(r.ok && r.ambiguous && !r.confident, "train: ok %d ambiguous %d confident %d", r.ok, r.ambiguous, r.confident); }
    // Bistatic: GRAVES geometry, known carrier
    { SatIdOptions b; b.refHz = 143.05e6; b.bistatic = true; b.tx = Observer{47.348, 5.515, 0.2}; b.fitOffset = false; b.goodRmsHz = 25; b.refine = 0;
      int ok = 0, n = 0;
      for (int i = 0; i < 1500 && n < 8; i += 11) {
          Sgp4 s; if (!s.init(cat[(size_t)i])) continue; double a, e; if (!window(s, rx, start, a, e)) continue;
          const auto v = track(s, rx, a + 120, a + 150, b.refHz, 0, 4, (uint32_t)i, b); if (v.size() < 10) continue;
          n++; SatIdResult r = identifySatellite(cat, rx, v, b); if (r.ok && r.ranked[0].tle.norad == cat[(size_t)i].norad) ok++;
      }
      printf("bistatic: %d of %d\n", ok, n); CHECK(n >= 4 && ok >= n - 1, "bistatic %d of %d", ok, n); }
    // Empty catalogue and short track
    { SatIdResult r = identifySatellite({}, rx, track(s0, rx, t0, t1, opt.refHz, 0, 5, 1, opt), opt); CHECK(!r.ok, "empty catalogue"); SatIdResult q = identifySatellite(cat, rx, {}, opt); CHECK(!q.ok, "no track"); }
    printf(fails ? "FAILED %d\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
