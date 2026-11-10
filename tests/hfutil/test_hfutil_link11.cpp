// Link 11 CLEW frame-structure reader (hfutil/hfu_link11.h): generated messages at both frame rates and both phase-change constellations
// with a Doppler offset, noise in 3 kHz, a sample clock error, an echo, chunk sizes, two messages and noise only. The generator and the
// receiver follow the same published summary, so this checks the reader, not the summary. Simulated signals only.
#include "dect2/hfutil/hfu_link11.h"
#include "dect2/hfutil/hfutil_rx.h"
#include "dect2/hfutil/hfutil_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/impair.h"

namespace {

using Frames = std::vector<std::array<uint8_t, 15>>;

Frames randomFrames(int n, unsigned seed) {
    Frames f((size_t)n);
    std::mt19937 r(seed);
    for (auto& a : f) for (auto& q : a) q = (uint8_t)(r() & 3);
    return f;
}

void noise3k(std::vector<cf32>& x, double snrDb, double p, unsigned seed) {
    const double n0 = p / std::pow(10.0, snrDb / 10) * (24000.0 / 3000.0);
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, (float)std::sqrt(n0 / 2));
    for (auto& v : x) v += cf32(g(rng), g(rng));
}

bool gTrace = false;
std::vector<hfu::Link11Msg> run(const std::vector<cf32>& x, size_t chunk = 4096) {
    std::vector<hfu::Link11Msg> out;
    hfu::Link11Rx rx;
    if (gTrace) rx.setEventSink([](const std::string& s) { printf("  event: %s\n", s.c_str()); });
    rx.setMessageSink([&](const hfu::Link11Msg& m) { out.push_back(m); });
    for (size_t i = 0; i < x.size(); i += chunk) rx.push(x.data() + i, std::min(chunk, x.size() - i));
    return out;
}

// phase changes are relative: the reader may see the whole constellation turned by a quarter when the offset is near 45 degrees
int rightPhases(const hfu::Link11Msg& m, const Frames& f) {
    int best = 0;
    for (int rot = 0; rot < 4; rot++) {
        int ok = 0;
        for (size_t i = 0; i < std::min(m.phases.size(), f.size()); i++) for (int k = 0; k < 15; k++) ok += ((m.phases[i][(size_t)k] + rot) & 3) == f[i][(size_t)k];
        best = std::max(best, ok);
    }
    return best;
}

struct Case { bool fast = true; double off = 45, cfo = 0, snr = 99, ppm = 0, echoDb = -99; int frames = 40; size_t chunk = 4096; };

void one(const char* name, const Case& c, double needFrac = 1.0) {
    const Frames f = randomFrames(c.frames, (unsigned)c.frames * 7 + (c.fast ? 1 : 2));
    hfu::Link11GenCfg g; g.fast = c.fast; g.offsetDeg = c.off;
    std::vector<cf32> x = hfu::genLink11(g, f);
    double p = 0; for (auto& v : x) p += std::norm(v); p /= (double)x.size();
    if (c.echoDb > -90) impair::echo(x, 24, c.echoDb);
    if (c.ppm != 0) x = impair::clock(x, c.ppm);
    if (c.cfo != 0) impair::shift(x, c.cfo, 24000);
    if (c.snr < 90) noise3k(x, c.snr, p, 3);
    const auto v = run(x, c.chunk);
    const int total = 15 * c.frames;
    const int ok = v.size() == 1 ? rightPhases(v[0], f) : 0;
    printf("%-30s %s: %zu msg, %d frames, Doppler %+.1f Hz, offset %.0f deg, %d/%d phases\n", name, c.fast ? "fast" : "slow", v.size(), v.empty() ? 0 : v[0].frames,
           v.empty() ? 0 : v[0].dopplerHz, v.empty() ? 0 : v[0].offsetDeg, ok, total);
    CHECK(v.size() == 1, "%s: %zu messages", name, v.size());
    if (v.size() != 1) return;
    CHECK(v[0].fast == c.fast, "%s: frame rate", name);
    CHECK(v[0].frames == c.frames, "%s: %d frames, want %d", name, v[0].frames, c.frames);
    CHECK(std::fabs(v[0].dopplerHz - c.cfo) < 2, "%s: Doppler %.1f", name, v[0].dopplerHz);
    CHECK(ok >= (int)std::ceil(needFrac * total), "%s: %d of %d phases", name, ok, total);
}

}  // namespace

int main() {
    CHECK(hfu::l11DataTone(0) == 935 && hfu::l11DataTone(13) == 2365 && hfu::l11DataTone(14) == 2915, "tones");
    gTrace = true;
    for (bool fast : {true, false}) for (double off : {45.0, 0.0}) { Case c; c.fast = fast; c.off = off; one("clean", c); }
    gTrace = false;
    { Case c; c.cfo = 40; c.snr = 25; one("Doppler +40 Hz", c); }
    { Case c; c.fast = false; c.cfo = -55; c.snr = 25; one("Doppler -55 Hz", c); }
    { Case c; c.snr = 15; one("SNR 15 dB", c); }
    { Case c; c.fast = false; c.snr = 10; one("SNR 10 dB", c, 0.97); }
    { Case c; c.ppm = 60; c.snr = 25; c.frames = 150; one("clock +60 ppm, 150 frames", c); }
    { Case c; c.echoDb = -10; c.snr = 25; one("echo 1 ms -10 dB", c, 0.98); }
    { Case c; c.chunk = 77; c.snr = 25; one("chunks of 77", c); }
    // two messages, one fast and one slow
    {
        hfu::Link11GenCfg g;
        const Frames a = randomFrames(20, 1), b = randomFrames(30, 2);
        std::vector<cf32> x = hfu::genLink11(g, a);
        g.fast = false;
        const auto y = hfu::genLink11(g, b);
        x.insert(x.end(), y.begin(), y.end());
        noise3k(x, 25, 0.03, 4);
        const auto v = run(x);
        CHECK(v.size() == 2 && v[0].fast && !v[1].fast && v[0].frames == 20 && v[1].frames == 30, "two messages: %zu", v.size());
        if (v.size() == 2) CHECK(rightPhases(v[0], a) == 300 && rightPhases(v[1], b) == 450, "two messages: phases");
    }
    // noise only
    {
        std::vector<cf32> x(24000 * 8, cf32(0, 0));
        noise3k(x, 0, 0.03, 8);
        CHECK(run(x).empty(), "noise: a message");
    }
    // through the HF utility receiver at 250 ksps
    {
        hfu::Link11GenCfg g;
        const auto b = hfu::genLink11(g, randomFrames(25, 5));
        const double rate = 250000;
        std::vector<cf32> x((size_t)((double)b.size() * rate / 24000.0));
        for (size_t i = 0; i < x.size(); i++) {
            const double q = (double)i * 24000.0 / rate;
            const size_t k = (size_t)q;
            const float fr = (float)(q - (double)k);
            const cf32 v = k + 1 < b.size() ? b[k] * (1 - fr) + b[k + 1] * fr : cf32(0, 0);
            const double a = 2 * M_PI * -20000.0 * (double)i / rate;
            x[i] = v * cf32((float)std::cos(a), (float)std::sin(a));
        }
        HfUtilReceiver rx(kHfuKindLink11);
        rx.configure(rate);
        rx.setSignalOffset(-20000);
        for (size_t i = 0; i < x.size(); i += 65536) rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
        HfUtilTelemetry t;
        rx.telemetry(t, 0);
        printf("receiver: %s\n%s", t.mode.c_str(), t.text.c_str());
        CHECK(t.blocksOk == 1 && t.text.find("25 data frames") != std::string::npos, "receiver: %llu messages", (unsigned long long)t.blocksOk);
    }
    return testSummary("hfutil_link11");
}
