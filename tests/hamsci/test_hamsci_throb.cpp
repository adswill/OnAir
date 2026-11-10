// Throb and ThrobX on simulated signals: all six variants (text, centre frequency, variant detection), noise sweep, noise alone, other signals, other
// sample rates and the streaming decoder. Simulated only: no recording of a real Throb transmission was available.
#include "dect2/hamsci/hamsci_dsp.h"
#include "hamsci_testutil.h"
using namespace dect2::hamsci;

static std::string squash(const std::string& s) { std::string t; for (char c : s) if (c != ' ' && c != '\n') t += c; return t; }

int main() {
    const double fs = 12000;
    const std::string text = "CQ CQ DE W1AW 73";
    for (int v = 0; v < 6; v++)
        for (double fc : {900.0, 1500.0, 2700.0}) {
            auto x = throbGenerate(text, v, fs, fc, 0.5, 4);
            // pad with the same preamble so even the slow modes have the 14 s the decoder wants
            std::vector<float> pad(x.size() < (size_t)(16 * fs) ? (size_t)(16 * fs) - x.size() : 0, 0.f);
            x.insert(x.end(), pad.begin(), pad.end());
            ThrobResult r;
            const bool ok = throbDecode(x.data(), x.size(), fs, r);
            CHECK(ok && squash(r.text).find(squash(text)) != std::string::npos, "%s at %.0f Hz: ok=%d got '%s' (variant %s)", throbName(v), fc, ok, ok ? r.text.c_str() : "", ok ? throbName(r.variant) : "-");
            if (ok) {
                CHECK(r.variant == v, "%s at %.0f Hz read as %s", throbName(v), fc, throbName(r.variant));
                CHECK(std::fabs(r.centreHz - fc) < 3.0, "%s centre %.1f want %.0f", throbName(v), r.centreHz, fc);
            }
        }
    // shifted characters of Throb
    {
        auto x = throbGenerate("TEST? A@B=C", 1, fs, 1200, 0.5, 4);
        x.resize(std::max(x.size(), (size_t)(16 * fs)), 0.f);
        ThrobResult r;
        const bool ok = throbDecode(x.data(), x.size(), fs, r);
        CHECK(ok && squash(r.text).find("TEST?A@B=C") != std::string::npos, "shifted characters: '%s'", ok ? r.text.c_str() : "");
    }
    // noise sweep for three variants
    for (int v : {0, 2, 3}) {
        const double snrs[] = {10, 0, -6, -12};
        int okAt[4] = {};
        for (int si = 0; si < 4; si++)
            for (uint32_t seed = 1; seed <= 4; seed++) {
                auto x = throbGenerate(text, v, fs, 1400, 0.5, 4);
                x.resize(std::max(x.size(), (size_t)(16 * fs)), 0.f);
                addNoiseSnr(x, fs, snrs[si], 2500, seed, 0.125 * 0.5);
                ThrobResult r;
                if (throbDecode(x.data(), x.size(), fs, r) && squash(r.text).find(squash(text)) != std::string::npos) okAt[si]++;
            }
        std::printf("%s noise sweep (SNR in 2500 Hz, exact of 4):", throbName(v));
        for (int i = 0; i < 4; i++) std::printf(" %+.0f dB: %d", snrs[i], okAt[i]);
        std::printf("\n");
        CHECK(okAt[0] == 4, "%s: strong signal %d of 4", throbName(v), okAt[0]);
    }
    // noise alone and other signals
    {
        int falses = 0;
        for (uint32_t seed = 1; seed <= 10; seed++) {
            auto x = noiseOnly((size_t)(20 * fs), 0.1, seed);
            ThrobResult r;
            if (throbDecode(x.data(), x.size(), fs, r)) falses++;
        }
        CHECK(falses == 0, "noise alone: %d false decodes of 10", falses);
        auto y = fsk441Generate("W9XYZ K1JT 26", fs, 150);
        y.resize((size_t)(20 * fs));
        ThrobResult r;
        CHECK(!throbDecode(y.data(), y.size(), fs, r), "FSK441 read as Throb '%s'", r.text.c_str());
        std::vector<float> cw((size_t)(20 * fs));
        for (size_t i = 0; i < cw.size(); i++) cw[i] = (float)(0.3 * std::sin(6.2831853 * 1500.0 * (double)i / fs));
        CHECK(!throbDecode(cw.data(), cw.size(), fs, r), "carrier read as Throb");
    }
    for (double f2 : {8000.0, 11025.0, 24000.0}) {
        auto x = throbGenerate(text, 2, f2, 1300, 0.5, 4);
        x.resize(std::max(x.size(), (size_t)(16 * f2)), 0.f);
        ThrobResult r;
        const bool ok = throbDecode(x.data(), x.size(), f2, r);
        CHECK(ok && squash(r.text).find(squash(text)) != std::string::npos, "rate %.0f: '%s'", f2, ok ? r.text.c_str() : "");
    }
    // streaming: 60 s of Throb4 with noise, the text must come out once and in order
    {
        std::string long_ = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789 THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG";
        auto x = throbGenerate(long_, 2, fs, 1000, 0.5, 4);
        addNoiseSnr(x, fs, 6, 2500, 11, 0.125 * 0.5);
        { auto tail = noiseOnly((size_t)(16 * fs), 0.05, 5); x.insert(x.end(), tail.begin(), tail.end()); }
        ThrobDecoder d(fs);
        std::string got;
        for (size_t i = 0; i < x.size(); i += 4096) { d.feed(x.data() + i, std::min<size_t>(4096, x.size() - i)); for (auto& r : d.take()) got += r.text; }
        std::printf("streaming text: '%s'\n", got.c_str());
        CHECK(squash(got).find("THEQUICKBROWNFOXJUMPSOVERTHELAZYDOG") != std::string::npos, "streaming text '%s'", got.c_str());
    }
    std::printf(g_fails ? "Throb: %d failures\n" : "Throb: all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
