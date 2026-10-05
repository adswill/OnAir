// ATSC 3.0 bootstrap: the generated signal is found in noise at an unknown position and every signalled field is read back.
#include "dect2/atsc3_bootstrap.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static bool run(const Bootstrap& b, long offset, double snrDb, unsigned seed) {
    auto sig = generateBootstrap(b);
    size_t n = sig.size() + 40000;
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, 1.f);
    double sigma = std::pow(10.0, -snrDb / 20.0) / std::sqrt(2.0);
    std::vector<cf32> x(n);
    for (auto& v : x) v = cf32(g(rng), g(rng)) * (float)sigma;
    cf32 rot(std::cos(0.7f), std::sin(0.7f));   // unknown carrier phase
    for (size_t i = 0; i < sig.size(); i++) x[offset + i] += sig[i] * rot;
    auto d = detectBootstrap(x.data(), n);
    char m[160];
    snprintf(m, sizeof m, "found (minor %d, offset %ld, %.0f dB, metric %.2f)", b.minorVersion, offset, snrDb, d.metric);
    CHECK(d.found, m);
    if (!d.found) return false;
    CHECK(d.start == offset, "position");
    CHECK(d.valid, "signalling consistent");
    CHECK(d.info == b, "fields");
    if (!(d.info == b))
        printf("  got minor %d symbols %d ea %d min %d bw %d bsr %d pre %d\n", d.info.minorVersion, d.info.numSymbols, d.info.eaWakeUp, d.info.minTimeToNext,
               d.info.systemBandwidth, d.info.bsrCoefficient, d.info.preambleStructure);
    return true;
}

int main() {
    // the helper tables
    CHECK(minTimeToNextMs(0) == 50 && minTimeToNextMs(10) == 700 && minTimeToNextMs(17) == 1500 && minTimeToNextMs(30) == 5300, "time to next table");
    Bootstrap b;
    b.bsrCoefficient = 8;
    CHECK(std::fabs(postBootstrapRate(b) - 9.216e6) < 1, "sample rate of the frame (24 * 0.384 MHz)");

    auto sig = generateBootstrap(b);
    CHECK(sig.size() == 4 * 3072, "length");
    double p = 0;
    for (auto& v : sig) p += std::norm(v);
    CHECK(std::fabs(p / sig.size() - 1.0) < 0.1, "unit power");

    b.minorVersion = 0; b.eaWakeUp = 2; b.minTimeToNext = 13; b.systemBandwidth = 0; b.bsrCoefficient = 8; b.preambleStructure = 0x35;
    run(b, 12345, 30, 1);
    run(b, 0, 30, 2);
    b.minorVersion = 3; b.eaWakeUp = 1; b.minTimeToNext = 30; b.systemBandwidth = 2; b.bsrCoefficient = 80; b.preambleStructure = 0xA7;
    run(b, 777, 10, 3);
    b.minorVersion = 7; b.eaWakeUp = 0; b.minTimeToNext = 0; b.systemBandwidth = 1; b.bsrCoefficient = 0; b.preambleStructure = 0xFF;
    run(b, 9999, 5, 4);
    b.numSymbols = 6;   // a later minor version may send more symbols; the end is found by the phase inversion
    b.minorVersion = 0; b.minTimeToNext = 21; b.bsrCoefficient = 24; b.preambleStructure = 0x01;
    run(b, 5000, 10, 5);
    printf(fails ? "atsc3 bootstrap: FAILED\n" : "atsc3 bootstrap: ok\n");
    return fails ? 1 : 0;
}
