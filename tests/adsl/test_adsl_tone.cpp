// ADS-L M-band tone-energy chips (3.9, docs/SENSITIVITY-3.9-C.md): near the threshold the receiver with the tone detector decodes more Traffic packets
// than with the FM discriminator, with a carrier offset as well, and noise gives nothing. Simulated signals only (adsl_gen.h).
// ONAIR_TEST_LONG=1 prints the whole curve.
#include "dect2/adsl/adsl_gen.h"
#include "dect2/adsl/adsl_rx.h"
#include "dect2/adsl/adsl_tel.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace dect2;
using namespace dect2::adsl;
#include "../common/test_util.h"

static constexpr double kCentre = 868.875e6, kRate = 2e6;

static uint64_t good(Band band, double snr, double cfo, int n, unsigned seed, bool tone) {
    std::vector<AdslTx> v;
    for (int i = 0; i < n; i++) {
        Traffic t; t.amt = AmtFlarm; t.addr = 0x400000u + (uint32_t)i; t.category = 4; t.flightState = 2;
        t.hasPos = true; t.lat = 54.7; t.lon = 25.3; t.hasAlt = true; t.altM = 1234;
        AdslTx x; x.timeSec = 0.05 + i * 0.05; x.band = band; x.net = adslTrafficNet(t); x.cfoHz = cfo;
        v.push_back(x);
    }
    const auto iq = adslScene(kRate, kCentre, 0.1 + n * 0.05, v, snr, 0, seed);
    AdslReceiver::toneDetect = tone;
    AdslReceiver rx;
    rx.configure(kRate); rx.setCenterHz(kCentre);
    for (size_t i = 0; i < iq.size(); i += 16384) rx.feed(iq.data() + i, std::min<size_t>(16384, iq.size() - i));
    const std::vector<cf32> tail((size_t)(kRate * 0.3), cf32(0, 0));
    rx.feed(tail.data(), tail.size());
    AdslTelemetry t; rx.telemetry(t, 0);
    AdslReceiver::toneDetect = true;
    return t.blocksOk;
}

int main() {
    const bool full = std::getenv("ONAIR_TEST_LONG") != nullptr;
    const int n = 30;
    uint64_t dis = 0, ton = 0;
    for (Band b : {BandM0, BandM1}) {
        CHECK(good(b, 30, 0, n, 1, true) == (uint64_t)n, "band %d clean: not every packet", (int)b);
        CHECK(good(b, 20, 15e3, n, 2, true) == (uint64_t)n, "band %d +15 kHz: not every packet", (int)b);
        CHECK(good(b, 20, -15e3, n, 3, true) == (uint64_t)n, "band %d -15 kHz: not every packet", (int)b);
        for (double snr : full ? std::vector<double>{12, 10, 8, 7, 6, 5, 4, 3, 2} : std::vector<double>{5}) {
            const uint64_t d = good(b, snr, 2e3, n, 10, false), t = good(b, snr, 2e3, n, 10, true);
            printf("band %d %4.1f dB: discriminator %3llu  tone chips %3llu of %d\n", (int)b, snr, (unsigned long long)d, (unsigned long long)t, n);
            dis += d; ton += t;
        }
    }
    CHECK(ton > dis, "the tone chips decoded no more: %llu vs %llu", (unsigned long long)ton, (unsigned long long)dis);
    CHECK(good(BandM0, -40, 0, 20, 9, true) == 0, "noise gave packets");
    return testSummary("adsl tone");
}
