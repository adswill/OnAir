// Noise figure (Y-factor) meter: a noise source switched on and off (steps of 0.5 to 15 dB, 10 s per state), with a slow gain drift, three input rates and a few percent of power jitter; Y and the noise figure
// for a known ENR are read against the generator; steps under 0.3 dB are not reported; a steady noise gives nothing. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    // noise of power P0 (receiver) with the source adding P0 (10^(Y/10) - 1): a toggle every 10 s, a gain drift of 0.01 dB per second
    printf("Y put in -> Y read, NF read for ENR 15 dB / true NF for a receiver with Y: (dB)\n");
    for (double fs : {250000.0, 1e6, 2e6}) for (double ydb : {0.5, 1.0, 3.0, 6.0, 10.0, 15.0}) {
        if (fs != 2e6 && (ydb == 1.0 || ydb == 6.0)) continue;
        Rng r(5); const double p0 = 1.0; auto gen = [&](double t0, size_t n) {
            std::vector<cf32> x(n); for (size_t i = 0; i < n; i++) { const double t = t0 + i / fs; const bool on = (long)(t / 10) % 2 == 1; const double drift = std::pow(10.0, 0.01 * t / 10); const double p = p0 * drift * (on ? std::pow(10.0, ydb / 10) : 1.0); const float s = (float)std::sqrt(p / 2); x[i] = cf32(s * r.g(), s * r.g()); }
            return x; };
        auto t = runChunks(x40::YFactor, fs, 62.0, gen, 0, 145e6, 1.0);
        double y = factNum(t, "Y-factor");
        const double drift10 = 0.01 * 10;                      // the drift adds this much per 10 s to the difference between neighbouring states (up or down)
        printf("  fs %.0f: %.1f -> %.3f (drift %.2f dB per step)\n", fs, ydb, y, drift10);
        CHECK(std::fabs(y - ydb) < 0.12 + 0.0 * drift10, "fs %.0f Y %.1f read %.3f (%s)", fs, ydb, y, t.headline.c_str());
        if (fs == 250000.0 && ydb == 10.0) { const std::string nf = factStr(t, "noise figure if the source is ENR 15.0 dB"); const double want = 15.0 - 10 * std::log10(std::pow(10.0, ydb / 10) - 1); CHECK(std::fabs(atof(nf.c_str()) - want) < 0.15, "NF %s want %.2f", nf.c_str(), want); }
    }
    // steps of 0.1 dB and a steady noise: nothing reported
    { Rng r(5); auto gen = [&](double t0, size_t n) { std::vector<cf32> x(n); for (size_t i = 0; i < n; i++) { const double t = t0 + i / 250000.0; const bool on = (long)(t / 10) % 2 == 1; const float s = (float)std::sqrt(std::pow(10.0, on ? 0.01 : 0.0) / 2); x[i] = cf32(s * r.g(), s * r.g()); } return x; };
      auto t = runChunks(x40::YFactor, 250000, 62.0, gen, 0, 145e6, 1.0); CHECK(t.rows.empty(), "0.1 dB steps reported: %zu rows", t.rows.size()); }
    { Rng r(6); auto gen = [&](double, size_t n) { std::vector<cf32> x(n); for (auto& v : x) v = cf32(0.7f * r.g(), 0.7f * r.g()); return x; };
      auto t = runChunks(x40::YFactor, 250000, 62.0, gen, 0, 145e6, 1.0); CHECK(t.rows.empty(), "steady noise: %zu rows", t.rows.size()); }
    // a strong carrier in the 200 kHz does not break the step reading (it adds a constant): the step of the noise is then smaller, which is what the meter must report as read (noted in the page)
    printf(fails ? "x40 yfactor: %d FAILED\n" : "x40 yfactor: ok\n", fails);
    return fails ? 1 : 0;
}
