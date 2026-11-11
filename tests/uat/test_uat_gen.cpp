#include "dect2/uat/uat_gen.h"
#include "dect2/uat/uat_rx.h"
#include "dect2/engine/modes.h"
#include "dect2/uat/uat_frame.h"
#include "dect2/uat/uat_demod.h"
#include "dect2/uat/uat_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static bool parseHex(const char* s, uint8_t* out, int n) {
    for (int i = 0; i < n; i++) {
        auto nib = [](char c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'A' && c <= 'F') return c - 'A' + 10; if (c >= 'a' && c <= 'f') return c - 'a' + 10; return -1; };
        const int hi = nib(s[i * 2]), lo = nib(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

int main() {
    SynthConfig cfg;
    cfg.mode = 85;
    cfg.snrDb = 28;
    CHECK(makeModeSynth(85, cfg, 2e6) != nullptr, "synth registry");
    CHECK(makeModeSynth(85, cfg, 1.5e6) == nullptr, "rate guard");
    const ModeTuning* mt = modeTuningById("uat");
    CHECK(mt && mt->stdMode == 85 && mt->defMhz == 978.0, "tuning");
    UatTx tx;
    const char* vec = "FF8196782DD44238C1453855F89980C7524F5970940ED83AD89CE7A9BEF8761BBCD9FCC817D82E2D1ACF90CA78DA3C49";
    CHECK(parseHex(vec, tx.frame, UAT_SHORT_FRAME_BYTES), "hex vector");
    tx.frameBytes = UAT_SHORT_FRAME_DATA_BYTES;
    const std::vector<cf32> burst = uatBurst(tx, 2e6);
    CHECK(burst.size() > 100, "burst length %zu", burst.size());
    auto synth = makeUatSynth(cfg, 2e6);
    std::vector<cf32> x(200000);
    synth->generate(x.data(), x.size());
    double pwr = 0;
    for (auto& v : x) pwr += std::norm(v);
    CHECK(pwr > 1e-6, "synth produces signal");
    // the round trip (QA sweep 3.9: the test signal had 6 kHz of deviation at 1 Mbit/s and the receiver found nothing): the test signal
    // through the demodulator and the frame decoder at several sample rates, with carrier offsets, a sample clock error and noise
    struct Case { double rate, cfo, ppm, snrDb; };
    for (const Case& c : {Case{2e6, 0, 0, 60}, Case{2e6, 2000, 50, 60}, Case{2e6, -2000, -50, 60}, Case{2083333.33, 0, 0, 60}, Case{2.4e6, 1500, 20, 60},
                          Case{4e6, -1000, 0, 60}, Case{2e6, 0, 0, 14}}) {
        SynthConfig sc; sc.mode = 85; sc.snrDb = 60;
        auto g = makeUatSynth(sc, c.rate);
        std::vector<cf32> x0((size_t)(2.2 * c.rate));
        g->generate(x0.data(), x0.size());
        std::vector<cf32> x;   // carrier offset and clock error
        double ph = 0;
        for (double t = 0; t + 1 < (double)x0.size(); t += 1 + c.ppm * 1e-6) {
            const size_t i = (size_t)t; const float f = (float)(t - (double)i);
            x.push_back((x0[i] * (1 - f) + x0[i + 1] * f) * cf32((float)std::cos(ph), (float)std::sin(ph)));
            ph += 2 * M_PI * c.cfo / c.rate;
        }
        if (c.snrDb < 50) {   // noise in the 1.2 MHz channel: the burst power over its own band
            std::mt19937 rng(5); std::normal_distribution<float> nd(0.f, 1.f);
            const float sigma = 0.08f / std::sqrt(2.f) * (float)std::pow(10.0, -c.snrDb / 20) * (float)std::sqrt(c.rate / 1.2e6);
            for (auto& v : x) v += cf32(nd(rng), nd(rng)) * sigma;
        }
        UatDemod d; d.configure(c.rate);
        int raw = 0, ok = 0;
        d.setSink([&](UatRawFrame& f) { raw++; UatDecoded u; if (uatDecodeFrame(f, u)) ok++; });
        for (size_t i = 0; i < x.size(); i += 16384) d.feed(x.data() + i, std::min<size_t>(16384, x.size() - i));
        printf("  %.4f Msps, %+.0f Hz, %+.0f ppm, %.0f dB: %d frames, %d decoded\n", c.rate / 1e6, c.cfo, c.ppm, c.snrDb, raw, ok);
        // 11 bursts in 2.2 s (four aircraft at 0, -6, -12, -18 dB): at 14 dB in the strongest one's band only that one decodes; a 50 ppm
        // clock (49 kHz at 978 MHz, more than a UAT radio is off) still gives most of them
        const int want = c.snrDb < 50 ? 2 : std::fabs(c.ppm) > 30 ? 6 : 10;
        CHECK(ok >= want, "round trip at %.3f Msps %+.0f Hz %+.0f ppm %.0f dB: %d decoded", c.rate / 1e6, c.cfo, c.ppm, c.snrDb, ok);
    }
    {   // noise alone: nothing
        std::mt19937 rng(7); std::normal_distribution<float> nd(0.f, 0.05f);
        std::vector<cf32> z((size_t)4e6); for (auto& v : z) v = cf32(nd(rng), nd(rng));
        UatDemod d; d.configure(2e6); int ok = 0;
        d.setSink([&](UatRawFrame& f) { UatDecoded u; if (uatDecodeFrame(f, u)) ok++; });
        for (size_t i = 0; i < z.size(); i += 16384) d.feed(z.data() + i, std::min<size_t>(16384, z.size() - i));
        CHECK(ok == 0, "noise: %d frames", ok);
    }
    if (fails) return 1;
    printf("uat gen OK\n");
    return 0;
}
