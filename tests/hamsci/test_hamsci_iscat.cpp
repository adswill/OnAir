// ISCAT-A and ISCAT-B on simulated signals: both variants, tuning errors, message lengths, a noise sweep, noise alone, several sample rates, and the
// streaming decoder. Simulated only: no recording of a real ISCAT transmission was available.
#include "dect2/hamsci/hamsci_dsp.h"
#include "hamsci_testutil.h"
using namespace dect2::hamsci;

int main() {
    const double fs = 12000;
    const char* msgs[] = {"CQ DX K1JT FN20", "TEST", "W9XYZ 73", "A", "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"};
    for (int v = 0; v < 2; v++)
        for (const char* m : msgs)
            for (double off : {0.0, -70.0, 45.0}) {
                auto x = iscatGenerate(m, v, fs, 14.0, off, 0.5, 0.37);
                IscatResult r;
                const bool ok = iscatDecode(x.data(), x.size(), fs, v, r);
                CHECK(ok && r.text == m, "ISCAT-%c '%s' offset %.0f: ok=%d got '%s'", v ? 'B' : 'A', m, off, ok, ok ? r.text.c_str() : "");
                if (ok && r.text == m) CHECK(std::fabs(r.freqOffsetHz - off) <= 11.0, "ISCAT-%c offset %.0f read %.1f", v ? 'B' : 'A', off, r.freqOffsetHz);
            }
    // noise sweep
    for (int v = 0; v < 2; v++) {
        const double snrs[] = {10, 0, -5, -10, -14};
        int okAt[5] = {};
        for (int si = 0; si < 5; si++)
            for (uint32_t seed = 1; seed <= 6; seed++) {
                auto x = iscatGenerate("CQ DX K1JT FN20", v, fs, 14.0, 20.0, 0.5, 0.0);
                addNoiseSnr(x, fs, snrs[si], 2500, seed, 0.125);
                IscatResult r;
                if (iscatDecode(x.data(), x.size(), fs, v, r) && r.text == "CQ DX K1JT FN20") okAt[si]++;
            }
        std::printf("ISCAT-%c noise sweep (SNR in 2500 Hz, exact text of 6):", v ? 'B' : 'A');
        for (int i = 0; i < 5; i++) std::printf(" %+.0f dB: %d", snrs[i], okAt[i]);
        std::printf("\n");
        CHECK(okAt[0] == 6 && okAt[1] == 6, "ISCAT-%c: strong signals %d %d", v ? 'B' : 'A', okAt[0], okAt[1]);
    }
    // noise alone: neither variant
    {
        int falses = 0;
        for (uint32_t seed = 1; seed <= 12; seed++) {
            auto x = noiseOnly((size_t)(16 * fs), 0.1, seed);
            IscatResult r;
            for (int v = 0; v < 2; v++) if (iscatDecode(x.data(), x.size(), fs, v, r)) falses++;
        }
        CHECK(falses == 0, "noise alone gave %d false decodes of 24", falses);
    }
    // other tones: a carrier and an FSK441 signal are not ISCAT
    {
        auto x = std::vector<float>((size_t)(14 * fs));
        for (size_t i = 0; i < x.size(); i++) x[i] = (float)(0.3 * std::sin(6.2831853 * 1300.0 * (double)i / fs));
        IscatResult r;
        CHECK(!iscatDecode(x.data(), x.size(), fs, 0, r) && !iscatDecode(x.data(), x.size(), fs, 1, r), "carrier decoded");
        auto y = fsk441Generate("W9XYZ K1JT 26", fs, 40);
        CHECK(!iscatDecode(y.data(), std::min(y.size(), (size_t)(14 * fs)), fs, 0, r), "FSK441 read as ISCAT-A '%s'", r.text.c_str());
    }
    // other sample rates
    for (double f2 : {8000.0, 11025.0, 24000.0}) {
        auto x = iscatGenerate("CQ DX K1JT FN20", 0, f2, 12.0, 10.0, 0.5, 0.0);
        IscatResult r;
        const bool ok = iscatDecode(x.data(), x.size(), f2, 0, r);
        CHECK(ok && r.text == "CQ DX K1JT FN20", "rate %.0f: '%s'", f2, ok ? r.text.c_str() : "");
    }
    // streaming, automatic variant, with 5 s of noise in front
    {
        auto lead = noiseOnly((size_t)(5 * fs), 0.1, 3);
        auto sig = iscatGenerate("TEST 123", 1, fs, 20.0, -30.0, 0.5, 0.0);
        addNoiseSnr(sig, fs, 5, 2500, 4, 0.125);
        std::vector<float> all(lead);
        all.insert(all.end(), sig.begin(), sig.end());
        IscatDecoder d(fs, 2);
        for (size_t i = 0; i < all.size(); i += 4096) d.feed(all.data() + i, std::min<size_t>(4096, all.size() - i));
        auto r = d.take();
        CHECK(!r.empty() && r.back().text == "TEST 123" && r.back().variant == 1, "streaming: %zu results, last '%s'", r.size(), r.empty() ? "" : r.back().text.c_str());
    }
    std::printf(g_fails ? "ISCAT: %d failures\n" : "ISCAT: all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
