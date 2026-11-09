// AM census: carriers with a known modulation at known places in one wide capture: each must be found on the raster and classified, a fading one flagged,
// an interfered sideband named; a bare spur must not read as a station.
#include "dect2/general/am_census.h"
#include "dect2/dsp/fftutil.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::mt19937 rng(5);
static std::normal_distribution<float> gn(0, 1);

// band-limited noise: white noise through a simple FIR low-pass (voice stand-in), complex
static std::vector<cf32> voice(size_t n, double rate, double bwHz) {
    std::vector<cf32> o(n);
    const int taps = std::max(3, (int)(rate / bwHz * 2));
    cf32 sum(0, 0); std::vector<cf32> h((size_t)taps, cf32(0, 0)); size_t p = 0;
    for (size_t i = 0; i < n; i++) { const cf32 v(gn(rng), gn(rng)); sum += v - h[p]; h[p] = v; p = (p + 1) % (size_t)taps; o[i] = sum / (float)std::sqrt((double)taps); }
    return o;
}

static const AmCensusRow* find(const std::vector<AmCensusRow>& r, double khz) { for (const auto& x : r) if (std::fabs(x.khz - khz) < 0.5) return &x; return nullptr; }

int main() {
    const double rate = 2e6, centre = 7.3e6;
    const size_t n = (size_t)(rate * 36);
    std::vector<cf32> x(n, cf32(0, 0));
    auto osc = [&](double f, size_t i) { const double w = 2 * M_PI * f * (double)i / rate; return cf32((float)std::cos(w), (float)std::sin(w)); };
    // 1) strong clean AM on 7205 kHz
    { auto m = voice(n, rate, 4000); for (size_t i = 0; i < n; i++) x[i] += osc(7205e3 - centre, i) * (0.1f * (1.f + 0.5f * std::min(1.f, std::max(-1.f, m[i].real())))); }
    // 2) fading AM on 7250 (a 0.2 Hz deep fade of carrier and sidebands)
    { auto m = voice(n, rate, 4000); for (size_t i = 0; i < n; i++) { const float f = 0.55f + 0.45f * (float)std::cos(2 * M_PI * 0.2 * (double)i / rate); x[i] += osc(7250e3 - centre, i) * (0.04f * f * (1.f + 0.5f * std::min(1.f, std::max(-1.f, m[i].real())))); } }
    // 3) AM on 7300 with interference on the lower sideband (a narrow carrier + noise 3 kHz below)
    { auto m = voice(n, rate, 4000), q = voice(n, rate, 1500); for (size_t i = 0; i < n; i++) { x[i] += osc(7300e3 - centre, i) * (0.03f * (1.f + 0.3f * std::min(1.f, std::max(-1.f, m[i].real())))); x[i] += osc(7300e3 - centre - 3000, i) * (0.02f * q[i]); } }
    // 4) DRM-like: flat +-4.5 kHz with no carrier, on 7350
    { auto m = voice(n, rate, 9000); for (size_t i = 0; i < n; i++) x[i] += osc(7350e3 - centre, i) * (0.012f * m[i]); }
    // 5) a bare spur on 7400
    for (size_t i = 0; i < n; i++) x[i] += osc(7400e3 - centre, i) * 0.05f;
    // 6) USB voice on 7145 (upper side only), no carrier
    { auto m = voice(n, rate, 2500); for (size_t i = 0; i < n; i++) x[i] += osc(7145e3 + 1500 - centre, i) * (0.02f * m[i]); }
    // noise
    { const float s = (float)std::pow(10.0, -80.0 / 20) * 0.7071f; for (auto& v : x) v += cf32(gn(rng) * s, gn(rng) * s); }
    AmCensus c; c.configure(rate, centre);
    for (size_t i = 0; i < n; i += 40000) c.feed(x.data() + i, std::min<size_t>(40000, n - i));
    const auto r = c.rows();
    CHECK(c.stepHz() == 5e3, "raster %.0f", c.stepHz());
    printf("rows %zu\n", r.size());
    for (const auto& w : r) printf("  %8.1f kHz %-7s snr %5.1f fade %4.1f sel %d ecss %+d score %.1f\n", w.khz, amKindName(w.kind), w.snrDb, w.fadeDb, w.selective, w.ecss, w.score);
    auto a = find(r, 7205), b = find(r, 7250), d = find(r, 7300), e = find(r, 7350), f = find(r, 7400), g = find(r, 7145);
    CHECK(a && a->kind == AmKind::Am && !a->fading, "clean AM");
    CHECK(b && b->kind == AmKind::Am && b->fading && b->fadeDb > 8, "fading AM");
    CHECK(d && d->kind == AmKind::Am && d->ecss == 1, "ECSS: interference on the lower side, advice upper (got %d)", d ? d->ecss : 99);
    CHECK(e && e->kind == AmKind::Drm, "DRM");
    CHECK(f && f->kind == AmKind::Carrier, "spur");
    CHECK(g && g->kind == AmKind::Ssb, "SSB");
    CHECK(!r.empty() && std::fabs(r[0].khz - 7205) < 0.5, "best first: %.1f", r.empty() ? 0 : r[0].khz);
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
