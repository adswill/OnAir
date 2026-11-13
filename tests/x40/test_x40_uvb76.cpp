// UVB-76 buzzer monitor: buzz rate and length, a break in the train, a long transmission, with carrier offset, noise and period drift; noise alone logs nothing.
#include "x40_gen.h"
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static double fact(const X40Telemetry& t, const char* k) { for (auto& f : t.facts) if (f.first == k) return atof(f.second.c_str()); return -1; }

int main() {
    const double fs = 96000;
    // rate sweep, offsets, noise (carrier to noise in the 4.5 kHz audio band), period drift
    for (double period : {2.0, 2.4, 3.0}) for (double off : {0.0, 700.0, -2500.0}) for (double snr : {40.0, 25.0, 15.0}) for (double ppm : {0.0, 20000.0}) {
        Buzz b; b.period = period; b.buzzLen = period / 2; b.ppm = ppm;
        auto x = buzzer(b, fs, 40.0); mix(x, off, 3.0, fs); addNoise(x, std::pow(10.0, -snr / 10) * fs / 4500.0 / 2, 11);
        auto t = run(x40::Uvb76, x, fs);
        const double want = 60.0 / (period * (1 + ppm * 1e-6));
        CHECK(std::fabs(fact(t, "buzz rate") - want) < 0.6, "period %.1f off %.0f snr %.0f ppm %.0f: rate %.2f want %.2f", period, off, snr, ppm, fact(t, "buzz rate"), want);
        CHECK(std::fabs(fact(t, "buzz length") - period / 2) < 0.12, "buzz length %.2f want %.2f", fact(t, "buzz length"), period / 2);
    }
    // a break, then the train again; a long transmission in between
    {
        Buzz b; b.breakAt = 30; b.breakLen = 20; b.voiceAt = 60; b.voiceLen = 9;
        auto t = run(x40::Uvb76, buzzer(b, fs, 100.0), fs);
        CHECK(countKind(t, "Train break") == 1, "break rows %d", countKind(t, "Train break"));
        CHECK(countKind(t, "Long transmission") == 1, "long transmission rows %d", countKind(t, "Long transmission"));
        CHECK(countKind(t, "Buzz train") >= 1, "train rows %d", countKind(t, "Buzz train"));
    }
    // false decodes: noise only, and a steady carrier
    {
        std::vector<cf32> z(96000 * 60); addNoise(z, 1.0, 3);
        auto t = run(x40::Uvb76, z, fs); CHECK(t.rows.empty(), "noise only: %zu rows", t.rows.size());
        std::vector<cf32> c(96000 * 60, cf32(1, 0)); addNoise(c, 0.001, 3);
        t = run(x40::Uvb76, c, fs); CHECK(t.rows.empty(), "carrier only: %zu rows", t.rows.size());
    }
    {   // white noise alone: no carrier, no pilot, no buzz (3.9 of the sweep: the AM monitor "found a carrier", the FM monitor a 4.5 kHz pilot, UVB-76 a buzz)
        for (double sg : {0.02, 0.1, 0.5}) {
            std::vector<cf32> x((size_t)(250000 * 8), cf32(0, 0)); addNoise(x, (float)sg, 5);
            auto t = run(x40::Uvb76, x, 250000);
            CHECK(t.blocksOk == 0, "UVB-76 on noise (sigma %.2f): %llu decodes", sg, (unsigned long long)t.blocksOk);
        }
    }

    printf(fails ? "x40 uvb76: %d FAILED\n" : "x40 uvb76: ok\n", fails);
    return fails ? 1 : 0;
}
