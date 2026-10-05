// Finding the bootstrap in a continuous signal with a carrier frequency offset, and the guard interval offset estimator.
#include "dect2/atsc3_sync.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main() {
    std::mt19937 rng(101);
    std::normal_distribution<float> g(0.f, 1.f);
    Bootstrap bs;
    bs.minorVersion = 0; bs.eaWakeUp = 1; bs.minTimeToNext = 4; bs.systemBandwidth = 0; bs.bsrCoefficient = 8; bs.preambleStructure = 11;
    auto sig = generateBootstrap(bs);

    struct Case { double cfo, snr; long offset; } cases[] = {
        {0, 20, 30000}, {500, 20, 41111}, {-1300, 15, 25000}, {2900, 15, 50000}, {7300, 12, 33333}, {-12500, 12, 47000}, {16000, 10, 36000}, {-4200, 5, 29000}};
    for (auto& cs : cases) {
        std::vector<cf32> x(120000);
        double sg = std::sqrt(std::pow(10.0, -cs.snr / 10.0) / 2.0);
        for (auto& v : x) v = cf32(g(rng), g(rng)) * (float)sg;
        for (size_t i = 0; i < sig.size(); i++) x[cs.offset + i] += sig[i];
        // the carrier offset: the whole signal is shifted by cfo
        for (size_t i = 0; i < x.size(); i++) x[i] *= cf32((float)std::cos(2 * M_PI * cs.cfo * i / kBootstrapRate), (float)std::sin(2 * M_PI * cs.cfo * i / kBootstrapRate));
        auto r = findBootstrap(x.data(), x.size());
        char m[120];
        snprintf(m, sizeof m, "found (offset %.0f Hz, %.0f dB)", cs.cfo, cs.snr);
        CHECK(r.found && r.det.valid, m);
        if (!r.found) continue;
        printf("  cfo %+7.0f Hz  %2.0f dB: found at %.1f (true %ld), estimated %+8.1f Hz, metric %.2f, fields %s\n", cs.cfo, cs.snr, r.start, cs.offset, r.cfoHz, r.metric, r.det.info == bs ? "ok" : "WRONG");
        snprintf(m, sizeof m, "position (offset %.0f Hz)", cs.cfo);
        CHECK(std::fabs(r.start - cs.offset) <= 2.0, m);
        snprintf(m, sizeof m, "carrier offset (%.0f Hz)", cs.cfo);
        CHECK(std::fabs(r.cfoHz - cs.cfo) < 150.0, m);
        snprintf(m, sizeof m, "fields (offset %.0f Hz)", cs.cfo);
        CHECK(r.det.info == bs, m);
    }
    // nothing but noise
    {
        std::vector<cf32> x(100000);
        for (auto& v : x) v = cf32(g(rng), g(rng));
        auto r = findBootstrap(x.data(), x.size());
        CHECK(!r.found || !r.det.valid, "noise only: no bootstrap");
    }
    // guard interval estimator: 4 OFDM symbols of random data with a cyclic prefix and a known offset
    {
        const int fft = 1024, guard = 128;
        std::vector<cf32> x;
        for (int s = 0; s < 6; s++) {
            std::vector<cf32> u(fft);
            for (auto& v : u) v = cf32(g(rng), g(rng));
            for (int i = 0; i < guard; i++) x.push_back(u[fft - guard + i]);
            for (int i = 0; i < fft; i++) x.push_back(u[i]);
        }
        const double rate = 9.216e6, f = 123.0;
        for (size_t i = 0; i < x.size(); i++) x[i] *= cf32((float)std::cos(2 * M_PI * f * i / rate), (float)std::sin(2 * M_PI * f * i / rate));
        double e = estimateCfoGuard(x.data(), x.size(), fft, guard, 6, rate);
        printf("  guard interval estimate: %.1f Hz (true %.1f)\n", e, f);
        CHECK(std::fabs(e - f) < 5.0, "guard interval offset estimate");
    }
    // the exact resampler: a tone keeps its frequency and phase, for ratios that need large numerators
    for (double ratio : {0.9216, 0.6144, 1.4745, 0.3072}) {
        const double in = 10.0e6, f = 0.9e6;
        std::vector<cf32> x(40000);
        for (size_t i = 0; i < x.size(); i++) x[i] = cf32((float)std::cos(2 * M_PI * f * i / in), (float)std::sin(2 * M_PI * f * i / in));
        std::vector<cf32> y;
        resampleExact(x.data(), x.size(), in, in * ratio, y);
        double err = 0; int cnt = 0;
        for (size_t k = 400; k + 400 < y.size(); k++) {
            double t = k / (in * ratio);
            cf32 e((float)std::cos(2 * M_PI * f * t), (float)std::sin(2 * M_PI * f * t));
            err += std::norm(y[k] - e); cnt++;
        }
        double db = 10 * std::log10(err / cnt + 1e-30);
        printf("  resample x%.4f: error %.1f dB, %zu samples\n", ratio, db, y.size());
        char m[80]; snprintf(m, sizeof m, "resampler accuracy, ratio %.4f", ratio);
        CHECK(db < -50.0 && std::fabs((double)y.size() - 40000.0 * ratio) < 3, m);
    }
    printf(fails ? "atsc3 sync: FAILED\n" : "atsc3 sync: ok\n");
    return fails ? 1 : 0;
}
