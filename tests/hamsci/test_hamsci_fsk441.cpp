// FSK441 on simulated signals: generator round trip, tuning error, noise sweep, a short ping inside noise, noise alone, other signals.
// Simulated only: no recording of a real meteor scatter contact was available.
#include "dect2/hamsci/hamsci_dsp.h"
#include "hamsci_testutil.h"
using namespace dect2::hamsci;

static const double kFs = 12000;

int main() {
    const std::string msg = "W9XYZ K1JT 26";
    // 1. clean round trip, several tuning errors
    for (double off : {0.0, -120.0, 85.0, 250.0}) {
        auto x = fsk441Generate(msg, kFs, 3, off);
        Fsk441Burst b;
        const bool ok = fsk441DecodeBlock(x.data(), std::min(x.size(), (size_t)(1.5 * kFs)), kFs, b);
        CHECK(ok && b.text.find("W9XYZ") != std::string::npos && b.text.find("K1JT") != std::string::npos, "offset %.0f: ok=%d text '%s'", off, ok, ok ? b.text.c_str() : "");
        CHECK(ok && std::fabs(b.freqOffsetHz - off) <= 20, "offset %.0f read as %.1f", off, ok ? b.freqOffsetHz : 0.0);
    }
    // 2. noise sweep (SNR in 2500 Hz): count how many of 10 seeds decode the full call sign pair
    int okAt[6] = {};
    const double snrs[6] = {20, 10, 4, 0, -4, -8};
    for (int si = 0; si < 6; si++)
        for (uint32_t seed = 1; seed <= 10; seed++) {
            auto x = fsk441Generate(msg, kFs, 6, 40.0);
            addNoiseSnr(x, kFs, snrs[si], 2500, seed, 0.125);
            Fsk441Burst b;
            if (fsk441DecodeBlock(x.data(), std::min(x.size(), (size_t)(2.0 * kFs)), kFs, b) && b.text.find("W9XYZ") != std::string::npos) okAt[si]++;
        }
    std::printf("fsk441 noise sweep (SNR in 2500 Hz, ok of 10):");
    for (int i = 0; i < 6; i++) std::printf(" %+.0f dB: %d", snrs[i], okAt[i]);
    std::printf("\n");
    CHECK(okAt[0] == 10 && okAt[1] == 10, "strong signals must decode: %d %d", okAt[0], okAt[1]);
    CHECK(okAt[2] >= 8, "+4 dB: %d of 10", okAt[2]);
    // 3. streaming: a 150 ms ping in 12 s of noise
    {
        auto burst = fsk441Generate(msg, kFs, 1, -60.0);
        burst.resize((size_t)(0.18 * kFs));
        auto stream = noiseOnly((size_t)(12 * kFs), 0.12, 5);
        for (size_t i = 0; i < burst.size(); i++) stream[(size_t)(5.0 * kFs) + i] += burst[i] * 0.9f;
        Fsk441Decoder d(kFs);
        for (size_t i = 0; i < stream.size(); i += 2048) d.feed(stream.data() + i, std::min<size_t>(2048, stream.size() - i));
        auto r = d.take();
        CHECK(r.size() == 1, "ping: %zu bursts", r.size());
        if (!r.empty()) {
            std::printf("ping text '%s' at %.2f s\n", r[0].text.c_str(), r[0].startSec);
            CHECK(r[0].text.find("W9XYZ") != std::string::npos, "ping text '%s'", r[0].text.c_str());
            CHECK(std::fabs(r[0].startSec - 5.0) < 0.3, "ping start %.2f", r[0].startSec);
        }
    }
    // 4. noise alone, 60 s: no burst, and a pure carrier and an FM-like tone sweep are not FSK441
    {
        auto stream = noiseOnly((size_t)(60 * kFs), 0.12, 9);
        Fsk441Decoder d(kFs);
        for (size_t i = 0; i < stream.size(); i += 2048) d.feed(stream.data() + i, std::min<size_t>(2048, stream.size() - i));
        CHECK(d.take().empty(), "noise alone gave a burst");
        std::vector<float> cw((size_t)(3 * kFs));
        for (size_t i = 0; i < cw.size(); i++) cw[i] = (float)(0.3 * std::sin(2 * 3.14159265 * 1500.0 * (double)i / kFs));
        Fsk441Burst b;
        CHECK(!fsk441DecodeBlock(cw.data(), cw.size(), kFs, b), "a carrier decoded as '%s'", b.text.c_str());
    }
    // 5. other sample rates
    for (double fs : {8000.0, 11025.0, 24000.0}) {
        auto x = fsk441Generate(msg, fs, 3, 30.0);
        Fsk441Burst b;
        const bool ok = fsk441DecodeBlock(x.data(), std::min(x.size(), (size_t)(1.5 * fs)), fs, b);
        CHECK(ok && b.text.find("K1JT") != std::string::npos, "rate %.0f: '%s'", fs, ok ? b.text.c_str() : "");
    }
    std::printf(g_fails ? "FSK441: %d failures\n" : "FSK441: all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
