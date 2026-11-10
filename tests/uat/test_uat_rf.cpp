// UAT through the radio: the DO-282 test frame as binary CPFSK at the real 1.041667 Mbit/s (uat_gen.cpp), at the sample rates radios give,
// with carrier offsets, a weak input level and white noise, into UatDemod. Simulated signals only.
// Checks: every rate from 2 to 10 Msps decodes (3.8 needed exactly 2 samples a bit); a signal at 1e-3 of full scale decodes (the 8-bit phase
// table lost it); the soft erasures decode more frames at low SNR than hard decisions; noise alone gives nothing.
// ONAIR_TEST_LONG=1 prints the success curve with and without erasures.
#include "dect2/uat/uat_demod.h"
#include "dect2/uat/uat_gen.h"
#include "dect2/uat/uat_frame.h"
#include "dect2/dsp/gen_util.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char* kVec = "007E6987D2D74238C1453855F89980C7524F5970940ED83AD89CE7A9BEF8";   // a valid basic frame (dump978 fec_tests, corrected)

static void hex(const char* s, uint8_t* out, int n) {
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'A' + 10; };
    for (int i = 0; i < n; i++) out[i] = (uint8_t)(nib(s[2 * i]) << 4 | nib(s[2 * i + 1]));
}

// frames decoded from `bursts` bursts 2 ms apart; snrDb is the burst power over the noise in 1.3 MHz (inf: no noise)
static int run(double rate, int bursts, double cfoHz, double snrDb, float level, bool soft, unsigned seed, bool noiseOnly = false) {
    std::vector<UatTx> txs;
    for (int i = 0; i < bursts; i++) {
        UatTx t; hex(kVec, t.frame, UAT_SHORT_FRAME_BYTES); t.frameBytes = UAT_SHORT_FRAME_DATA_BYTES;
        t.timeSec = 0.001 + 0.002 * i + 1.3e-7 * (i % 5); t.cfoHz = cfoHz; t.gainDb = 0;
        txs.push_back(t);
    }
    std::vector<cf32> x = uatScene(rate, 0.004 + 0.002 * bursts, txs, 200, seed);
    const float amp = 0.08f;   // uatBurst's amplitude at 0 dB
    const float g = level / amp;
    for (auto& v : x) v = noiseOnly ? cf32(0, 0) : v * g;
    if (std::isfinite(snrDb)) {
        const double sig2 = (double)level * level, n2 = sig2 / std::pow(10.0, snrDb / 10) * (rate / 1.3e6);
        std::mt19937 rng(seed);
        std::normal_distribution<float> N(0.f, (float)std::sqrt(n2 / 2));
        for (auto& v : x) v += cf32(N(rng), N(rng));
    }
    UatDemod d;
    d.configure(rate);
    d.setSoft(soft);
    int got = 0;
    d.setSink([&](UatRawFrame& f) { if (noiseOnly ? true : (!f.uplink && std::memcmp(f.data, txs[0].frame, UAT_SHORT_FRAME_DATA_BYTES) == 0)) got++; });
    for (size_t o = 0; o < x.size(); o += 40000) d.feed(x.data() + o, std::min<size_t>(40000, x.size() - o));
    std::vector<cf32> z((size_t)(rate * 0.01));
    d.feed(z.data(), z.size());
    return got;
}

int main() {
    const int n = 20;
    for (double rate : {2.0e6, 2.048e6, 2.083333e6, 2.4e6, 3.2e6, 4e6, 6e6, 8e6, 10e6}) {
        for (double cfo : {0.0, 50e3, -50e3}) {
            const int got = run(rate, n, cfo, INFINITY, 0.3f, true, 1);
            if (got != n) printf("%.3f Msps, %+.0f kHz: %d of %d\n", rate / 1e6, cfo / 1e3, got, n);
            CHECK(got == n, "%.3f Msps carrier %+.0f kHz: %d of %d frames", rate / 1e6, cfo / 1e3, got, n);
        }
    }
    {   // a weak input: 1e-3 of full scale (the old 8-bit phase table had about 1/128 per step), 20 dB SNR
        const int got = run(2e6, n, 10e3, 20, 1e-3f, true, 2);
        CHECK(got >= n - 1, "weak input level 1e-3: %d of %d", got, n);
    }
    for (bool soft : {false, true}) {   // noise only, 2 x 0.2 s: any frame at all is a false decode
        const int f = run(2.4e6, 100, 0, -10, 0.1f, soft, 3, true) + run(2.083333e6, 100, 0, -10, 0.1f, soft, 4, true);
        printf("noise only, %s: %d frames\n", soft ? "soft erasures" : "hard", f);
        CHECK(f == 0, "noise only gave %d frames (%s)", f, soft ? "soft" : "hard");
    }
    // soft erasures against hard decisions, near the threshold (summed over seeds)
    constexpr bool kCalibrate = true;   // WIP: print the whole curve and fail so the CI log shows it
    const bool full = kCalibrate || std::getenv("ONAIR_TEST_LONG") != nullptr;
    int hardAt = 0, softAt = 0;
    for (double snr = full ? 16 : 9; snr >= (full ? 4 : 9); snr -= 1) {
        int h = 0, s = 0;
        const int seeds = full ? 10 : 4;
        for (int k = 0; k < seeds; k++) { h += run(2.4e6, n, 20e3, snr, 0.1f, false, 100 + k); s += run(2.4e6, n, 20e3, snr, 0.1f, true, 100 + k); }
        printf("SNR %4.1f dB (1.3 MHz): hard %5.1f %%  soft erasures %5.1f %%\n", snr, 100.0 * h / (n * seeds), 100.0 * s / (n * seeds));
        hardAt += h; softAt += s;
    }
    CHECK(softAt > hardAt, "erasures did not help: %d vs %d", softAt, hardAt);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    if (kCalibrate) { printf("CALIBRATION run\n"); return 1; }
    return fails ? 1 : 0;
}
