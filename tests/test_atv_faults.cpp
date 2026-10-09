// Analog TV receiver against real-world faults from tests/impair.h (REAL_WORLD_CHECKLIST.md), applied to the whole recording independently of the
// receiver: NaN samples, a mirrored spectrum, a ghost, an overdriven 8-bit radio, lost samples, and all of them at once with the worst UHF tuning error.
#include "dect2/atv_testkit.h"
#include "impair.h"
#include <cmath>
#include <cstdio>
#include <functional>

using namespace dect2;
using namespace dect2::atvkit;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static double barError(const Run& r, const AtvFormat& f) {
    if (!r.frame) return 9;
    AtvCard card(f);
    double c[8][3], worst = 0;
    barColours(*r.frame, card, c);
    for (int i = 0; i < 8; i++) {
        float rgb[3]; atvEbuBar(i, rgb);
        for (int k = 0; k < 3; k++) worst = std::max(worst, std::fabs(c[i][k] - rgb[k]));
    }
    return worst;
}

static Run runVec(const AtvGenConfig& cfg, double secs, const std::function<void(std::vector<cf32>&)>& fault) {
    std::vector<cf32> x((size_t)(secs * cfg.rate));
    {
        AtvGenerator g(cfg);
        g.generate(x.data(), x.size());
    }
    for (auto& v : x) v *= 0.3f;     // a radio's level
    if (fault) fault(x);
    Run r;
    AtvReceiver rx;
    rx.setSilent(true);
    rx.setAudioTap([&](const float* l, const float*, size_t n) { r.audio.insert(r.audio.end(), l, l + n); });
    rx.configure(cfg.rate);
    uint64_t seq = 0, fseq = 0;
    for (size_t i = 0; i < x.size(); i += 65536) {
        rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
        AtvTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; r.tel = t; }
        while (auto f = rx.frame(fseq)) r.frame = f;
    }
    return r;
}

static void check(const char* what, const Run& r, const AtvFormat& f, double maxBars, bool sound = true) {
    const double w = barError(r, f);
    const size_t n = r.audio.size();
    const double snd = n > 48000 ? toneAmp(r.audio, 1000, n - 24000, n) : 0;
    printf("%-36s state %d %-16s colour %d bars %.3f carrier %+.0f Hz line %+.1f ppm sound %.3f\n", what, r.tel.state, r.tel.system.c_str(), (int)r.tel.colour, w,
           r.tel.cfoHz, r.tel.lineErrPpm, snd);
    CHECK(r.tel.state == 2 && r.tel.colour && w < maxBars, "%s: state %d colour %d bars %.3f", what, r.tel.state, (int)r.tel.colour, w);
    if (sound) CHECK(std::fabs(snd - 0.5) < 0.08, "%s: sound tone %.3f", what, snd);
}

int main() {
    AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.cnrDb = 40; c.rate = 10e6; c.sound = 4;
    AtvFormat gp; atvMakeFormat(kAtvG, kAtvPal, gp);
    const double fs = c.rate;

    check("NaN samples", runVec(c, 2.5, [](std::vector<cf32>& x) { for (size_t i = 10000000; i < 10000050; i++) x[i] = cf32(NAN, INFINITY); }), gp, 0.06);
    check("I/Q swapped", runVec(c, 2.5, [](std::vector<cf32>& x) { impair::swapIq(x); }), gp, 0.06);
    check("ghost 1.5 us -12 dB", runVec(c, 2.5, [](std::vector<cf32>& x) { impair::echo(x, 15, -12, 2.0); }), gp, 0.12);
    check("overdriven 8-bit radio", runVec(c, 2.5, [](std::vector<cf32>& x) { impair::clip8(x, 3.0); }), gp, 0.10);
    check("skip and USB drop", runVec(c, 2.5, [](std::vector<cf32>& x) { impair::skip(x, 333333); impair::drop(x, 9000000, 41234); }), gp, 0.06);
    // combined: 50 ppm of 860 MHz (UHF channel 69), the radio's clock +80 ppm, a ghost, clipping
    check("combined", runVec(c, 3.0, [fs](std::vector<cf32>& x) {
        impair::shift(x, 43e3, fs); x = impair::clock(x, 80); impair::echo(x, 15, -12, 2.0); impair::clip8(x, 3.0); }), gp, 0.12);
    printf(fails ? "atv faults: FAILED (%d)\n" : "atv faults: ok\n", fails);
    return fails ? 1 : 0;
}
