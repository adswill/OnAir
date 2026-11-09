// PI4 on simulated signals: packing, the code and the interleaver through a clean round trip, tuning range, start times, a noise sweep, noise alone,
// other sample rates, and the streaming decoder. Simulated only: the layout comes from MSHV's decoder source, no transmission of a real PI4 beacon was available.
#include "dect2/hamsci/hamsci_dsp.h"
#include "hamsci_testutil.h"
using namespace dect2::hamsci;

int main() {
    const double fs = 12000;
    {   // packing
        uint8_t b[6];
        CHECK(pi4Pack("PI4ABC/P", b) && pi4Unpack(b) == "PI4ABC/P", "pack/unpack");
        CHECK(pi4Pack("k1jt", b) && pi4Unpack(b) == "K1JT    ", "pack pads and upper-cases: '%s'", pi4Unpack(b).c_str());
        CHECK(!pi4Pack("TOOLONGMSG", b) && !pi4Pack("A-B", b), "illegal messages must be refused");
    }
    const char* msgs[] = {"PI4ABC", "K1JT", "DB0KO/B", "12345678", "A"};
    for (const char* m : msgs)
        for (double f0 : {700.0, 1500.0, 2200.0})
            for (double start : {0.0, 1.3, 2.9}) {
                auto x = pi4Generate(m, fs, f0, 0.5, start, 32.0);
                Pi4Result r;
                const bool ok = pi4Decode(x.data(), x.size(), fs, r);
                std::string want = m;
                CHECK(ok && r.text == want, "'%s' at %.0f Hz start %.1f: ok=%d got '%s'", m, f0, start, ok, ok ? r.text.c_str() : "");
                if (ok) CHECK(std::fabs(r.freqHz - f0) < 3.0 && std::fabs(r.startSec - start) < 0.1, "'%s' f %.1f (want %.0f) start %.2f (want %.1f)", m, r.freqHz, f0, r.startSec, start);
            }
    {
        const double snrs[] = {5, 0, -8, -12, -15, -18};
        int okAt[6] = {};
        for (int si = 0; si < 6; si++)
            for (uint32_t seed = 1; seed <= 6; seed++) {
                auto x = pi4Generate("PI4ABC", fs, 1234.5, 0.5, 1.7, 32.0);
                // the signal exists only during 27 s: compute noise from the signal power during it
                addNoiseSnr(x, fs, snrs[si], 2500, seed, 0.125);
                Pi4Result r;
                if (pi4Decode(x.data(), x.size(), fs, r) && r.text == "PI4ABC") okAt[si]++;
            }
        std::printf("PI4 noise sweep (SNR in 2500 Hz, exact of 6):");
        for (int i = 0; i < 6; i++) std::printf(" %+.0f dB: %d", snrs[i], okAt[i]);
        std::printf("\n");
        CHECK(okAt[0] == 6 && okAt[1] == 6, "strong signals: %d %d", okAt[0], okAt[1]);
    }
    {   // noise alone
        int falses = 0;
        for (uint32_t seed = 1; seed <= 8; seed++) {
            auto x = noiseOnly((size_t)(32 * fs), 0.1, seed);
            Pi4Result r;
            if (pi4Decode(x.data(), x.size(), fs, r)) falses++;
        }
        CHECK(falses == 0, "noise alone: %d false decodes of 8", falses);
    }
    for (double f2 : {8000.0, 11025.0, 24000.0}) {
        auto x = pi4Generate("PI4ABC", f2, 1000.0, 0.5, 0.5, 32.0);
        Pi4Result r;
        const bool ok = pi4Decode(x.data(), x.size(), f2, r);
        CHECK(ok && r.text == "PI4ABC", "rate %.0f: '%s'", f2, ok ? r.text.c_str() : "");
    }
    {   // streaming
        auto lead = noiseOnly((size_t)(7 * fs), 0.1, 3);
        auto sig = pi4Generate("K1JT", fs, 1800, 0.5, 0.0, 40.0);
        addNoiseSnr(sig, fs, 0, 2500, 4, 0.125);
        std::vector<float> all(lead);
        all.insert(all.end(), sig.begin(), sig.end());
        Pi4Decoder d(fs);
        for (size_t i = 0; i < all.size(); i += 4096) d.feed(all.data() + i, std::min<size_t>(4096, all.size() - i));
        auto r = d.take();
        CHECK(r.size() == 1 && r[0].text == "K1JT", "streaming: %zu results '%s'", r.size(), r.empty() ? "" : r[0].text.c_str());
    }
    std::printf(g_fails ? "PI4: %d failures\n" : "PI4: all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
