// JT6M on simulated signals: tone plan against WSJT's gentone.f, generator round trip over tuning errors and drift, noise sweep, a short ping
// inside noise (streaming), a continuous weak signal averaged by folding, noise alone, a carrier, other sample rates.
// Simulated only: no recording of a real JT6M contact was available.
#include "dect2/hamsci/hamsci_dsp.h"
#include "hamsci_testutil.h"
#include <algorithm>
using namespace dect2::hamsci;

static const double kFs = 12000;

static bool contains(const std::string& text, const std::string& want) { return text.find(want) != std::string::npos; }
// the message repeats: look for it in two copies of a folded period
static bool cyclicContains(const std::string& period, const std::string& want) { return contains(period + period, want); }

int main() {
    // 0. tone plan: f = (n + 51) * 11025 / 512, sync n = -1; character order of gen6m.f
    CHECK(std::fabs(jt6mToneHz(-1) - 1076.66) < 0.01 && std::fabs(jt6mToneHz(0) - 1098.19) < 0.01 && std::fabs(jt6mToneHz(42) - 2002.59) < 0.01, "tone plan");
    CHECK(jt6mCharIndex('0') == 0 && jt6mCharIndex('.') == 10 && jt6mCharIndex(',') == 11 && jt6mCharIndex(' ') == 12 && jt6mCharIndex('/') == 13 &&
          jt6mCharIndex('#') == 14 && jt6mCharIndex('?') == 15 && jt6mCharIndex('$') == 16 && jt6mCharIndex('A') == 17 && jt6mCharIndex('z') == 42 &&
          jt6mCharIndex('@') == -1, "character set");
    CHECK(jt6mMessage("K1JT W9XYZ 26") == "K1JT W9XYZ 26 " && jt6mMessage("AB") == "AB  " && jt6mMessage("abc") == "ABC ", "message padding");

    const std::string msg = "K1JT W9XYZ 26";
    // 1. clean round trip, tuning errors and drift
    struct Case { double off, drift; } cases[] = {{0, 0}, {-120, 0}, {85, 0}, {180, 0}, {-60, 4.0}, {40, -6.0}};
    for (const auto& c : cases) {
        auto x = jt6mGenerate(msg, kFs, 4.0, c.off, 0.5, 0.0, c.drift);
        addNoiseSnr(x, kFs, 15, 2500, 3, 0.125);
        Jt6mBlock b;
        const bool ok = jt6mDecodeBlock(x.data(), x.size(), kFs, b);
        const std::string t = ok && !b.pings.empty() ? b.pings[0].text : "";
        CHECK(ok && contains(t, "W9XYZ") && contains(t, "K1JT"), "offset %.0f drift %.1f: ok=%d text '%s'", c.off, c.drift, ok, t.c_str());
        CHECK(ok && std::fabs(b.freqOffsetHz - (c.off + c.drift * 2.0)) <= 10, "offset %.0f read as %.1f", c.off, ok ? b.freqOffsetHz : 0.0);
        CHECK(ok && b.period == 14 && cyclicContains(b.averaged, "K1JT W9XYZ 26 "), "fold: period %d '%s'", b.period, b.averaged.c_str());
    }
    // 2. noise sweep (SNR in 2500 Hz): 3 s of signal, how many of 10 seeds give both call signs
    const double snrs[6] = {10, 4, 0, -3, -6, -10};
    int okAt[6] = {};
    for (int si = 0; si < 6; si++)
        for (uint32_t seed = 1; seed <= 10; seed++) {
            auto x = jt6mGenerate(msg, kFs, 3.0, 25.0);
            addNoiseSnr(x, kFs, snrs[si], 2500, seed, 0.125);
            Jt6mBlock b;
            if (!jt6mDecodeBlock(x.data(), x.size(), kFs, b)) continue;
            std::string all;
            for (const auto& p : b.pings) all += p.text + "|";
            if ((contains(all, "W9XYZ") && contains(all, "K1JT")) || (b.period == 14 && cyclicContains(b.averaged, "K1JT W9XYZ"))) okAt[si]++;
        }
    std::printf("jt6m noise sweep (SNR in 2500 Hz, ok of 10):");
    for (int i = 0; i < 6; i++) std::printf(" %+.0f dB: %d", snrs[i], okAt[i]);
    std::printf("\n");
    CHECK(okAt[0] == 10 && okAt[1] == 10, "strong signals must decode: %d %d", okAt[0], okAt[1]);
    CHECK(okAt[2] >= 8, "0 dB: %d of 10", okAt[2]);
    // 3. streaming: a 0.6 s ping inside 20 s of noise, at a position that straddles a window edge
    for (double at : {5.0, 7.3, 10.6}) {
        auto ping = jt6mGenerate(msg, kFs, 0.65, -50.0);
        auto stream = noiseOnly((size_t)(20 * kFs), 0.10, 5);
        for (size_t i = 0; i < ping.size(); i++) stream[(size_t)(at * kFs) + i] += ping[i];
        Jt6mDecoder d(kFs);
        for (size_t i = 0; i < stream.size(); i += 1500) d.feed(stream.data() + i, std::min<size_t>(1500, stream.size() - i));
        d.flush();
        auto r = d.take();
        CHECK(r.size() == 1, "ping at %.1f: %zu pings", at, r.size());
        if (!r.empty()) {
            std::printf("ping at %.1f: '%s' at %.2f s, offset %.1f Hz\n", at, r[0].text.c_str(), r[0].startSec, r[0].freqOffsetHz);
            CHECK(contains(r[0].text, "W9XYZ"), "ping text '%s'", r[0].text.c_str());
            CHECK(std::fabs(r[0].startSec - at) < 0.2, "ping start %.2f", r[0].startSec);
            CHECK(std::fabs(r[0].freqOffsetHz + 50.0) < 10, "ping offset %.1f", r[0].freqOffsetHz);
        }
    }
    // 4. a continuous weak signal over 30 s (ionospheric scatter): the folded message comes through
    {
        auto x = jt6mGenerate("SP2XYZ LY2AB KO24", kFs, 28.0, 60.0);
        addNoiseSnr(x, kFs, -6, 2500, 11, 0.125);
        Jt6mBlock b;
        const bool ok = jt6mDecodeBlock(x.data(), x.size(), kFs, b);
        std::printf("weak continuous: ok=%d period %d '%s' good frames %d\n", ok, b.period, b.averaged.c_str(), b.goodFrames);
        CHECK(ok && b.period == 18 && cyclicContains(b.averaged, "LY2AB"), "weak continuous fold: period %d '%s'", b.period, b.averaged.c_str());
    }
    // 5. noise alone (60 s), a carrier on the sync tone, a carrier between the tones: nothing
    {
        auto stream = noiseOnly((size_t)(60 * kFs), 0.12, 9);
        Jt6mDecoder d(kFs);
        for (size_t i = 0; i < stream.size(); i += 2048) d.feed(stream.data() + i, std::min<size_t>(2048, stream.size() - i));
        d.flush();
        auto r = d.take();
        CHECK(r.empty(), "noise alone gave %zu pings ('%s')", r.size(), r.empty() ? "" : r[0].text.c_str());
        for (double f : {jt6mToneHz(-1), 1500.0}) {
            std::vector<float> cw((size_t)(4 * kFs));
            for (size_t i = 0; i < cw.size(); i++) cw[i] = (float)(0.3 * std::sin(2 * 3.14159265 * f * (double)i / kFs));
            addNoiseSnr(cw, kFs, 20, 2500, 4, 0.045);
            Jt6mBlock b;
            CHECK(!jt6mDecodeBlock(cw.data(), cw.size(), kFs, b), "a carrier at %.0f Hz decoded as '%s'", f, b.pings.empty() ? "" : b.pings[0].text.c_str());
        }
    }
    // 6. other sample rates
    for (double fs : {8000.0, 11025.0, 24000.0, 48000.0}) {
        auto x = jt6mGenerate(msg, fs, 3.0, 30.0);
        Jt6mBlock b;
        const bool ok = jt6mDecodeBlock(x.data(), x.size(), fs, b);
        CHECK(ok && !b.pings.empty() && contains(b.pings[0].text, "K1JT"), "rate %.0f: '%s'", fs, ok && !b.pings.empty() ? b.pings[0].text.c_str() : "");
    }
    // 7. two overlapping signals 400 Hz apart: the stronger one wins inside its tolerance window, the other one is outside it
    {
        auto a = jt6mGenerate("AAA1 BBB2", kFs, 4.0, 0.0, 0.5);
        auto b2 = jt6mGenerate("CCC3 DDD4", kFs, 4.0, 400.0, 0.3, 0.37);
        for (size_t i = 0; i < a.size(); i++) a[i] += b2[i];
        addNoiseSnr(a, kFs, 10, 2500, 21, 0.125);
        Jt6mBlock b;
        const bool ok = jt6mDecodeBlock(a.data(), a.size(), kFs, b);
        CHECK(ok && !b.pings.empty() && contains(b.pings[0].text, "BBB2"), "overlap: '%s'", ok && !b.pings.empty() ? b.pings[0].text.c_str() : "");
    }
    std::printf(g_fails ? "JT6M: %d failures\n" : "JT6M: all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
