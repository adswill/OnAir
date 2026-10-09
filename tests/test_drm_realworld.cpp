// DRM against the faults of REAL_WORLD_CHECKLIST.md, applied with tests/impair.h (written independently of the receiver) to the generator's clean
// signal: swapped I/Q, NaN samples, every robustness mode with an HF tuning error and a 100 ppm clock, and one combined case
// (1.5 kHz = 50 ppm at 30 MHz, +80 ppm, an echo inside the guard and 8-bit clipping).
#include "data/drm/testkit.h"
#include "impair.h"
#include "jobs.h"
#include <atomic>
#include <cstdio>
#include <limits>
using namespace drmtest;
using testjobs::jprintf;
static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

static const double kFs = 192000;

static std::shared_ptr<std::vector<cf32>> render(const Sig& s, double secs, double rate = kFs) {
    auto x = std::make_shared<std::vector<cf32>>((size_t)(secs * rate));
    synthGen(s, rate)(x->data(), x->size());
    return x;
}

static Gen play(std::shared_ptr<std::vector<cf32>> x) {
    auto pos = std::make_shared<size_t>(0);
    return [x, pos](cf32* out, size_t n) {
        for (size_t i = 0; i < n; i++) out[i] = *pos < x->size() ? (*x)[(*pos)++] : cf32(0, 0);
    };
}

static void check(const char* what, std::shared_ptr<std::vector<cf32>> x, double secs, double minShare, double rate = kFs) {
    RunOpt o; o.secs = secs;
    const Result r = runRx(play(x), rate, o);
    const double share = (double)r.exact / std::max(1.0, (secs - 6.0) / 0.4);
    jprintf("%-46s state %d, exact %llu bad %llu (%.0f%%), CFO %+.1f Hz, SRO %+.1f ppm\n", what, r.tel.state, (unsigned long long)r.exact, (unsigned long long)r.bad, 100 * share, r.tel.cfoHz,
            r.tel.sroPpm);
    CHECK(r.tel.state == 2 && share >= minShare && r.bad <= 1, "%s", what);
}

int main() {
    testjobs::Jobs jobs;
    jobs.add([] {
        Sig s; s.snrDb = 30;
        auto x = render(s, 20); impair::shift(*x, 700, kFs); impair::swapIq(*x);
        check("swapped I/Q", x, 20, 0.8);
    });
    // the channel away from the middle of the sample band (a recording made at an offset)
    jobs.add([] {
        Sig s; s.snrDb = 30;
        // (not 192 kHz: the generator leaves images 48 kHz from the signal, and one of them would land in the middle)
        auto x = render(s, 24, 250000); impair::shift(*x, 81500, 250000);
        check("off centre +81.5 kHz at 250 kHz", x, 24, 0.7, 250000);
    });
    jobs.add([] {
        Sig s; s.snrDb = 30;
        auto x = render(s, 20, 2e6); impair::shift(*x, -420000, 2e6);
        check("off centre -420 kHz at 2 Msps", x, 20, 0.6, 2e6);
    });
    jobs.add([] {
        Sig s; s.snrDb = 30;
        auto x = render(s, 20);
        for (size_t i = 0; i < 2000; i++) (*x)[(size_t)(8 * kFs) + i] = cf32(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
        check("NaN and infinite samples for 10 ms", x, 20, 0.7);
    });
    for (int mode : {1, 2, 3, 4}) for (double ppm : {-100.0, 100.0}) jobs.add([=] {
        Sig s; s.snrDb = 30; s.mode = mode;
        auto x = render(s, 24);
        auto y = std::make_shared<std::vector<cf32>>(impair::clock(*x, ppm));
        impair::shift(*y, ppm > 0 ? 1500 : -1500, kFs);
        char b[80]; snprintf(b, sizeof b, "mode %c, %+.0f ppm, %+d Hz", "?ABCD"[mode], ppm, ppm > 0 ? 1500 : -1500);
        check(b, y, 24, 0.8);
    });
    jobs.add([] {
        Sig s; s.snrDb = 30;
        auto x = render(s, 24);
        auto y = std::make_shared<std::vector<cf32>>(impair::clock(*x, 80));
        impair::shift(*y, 1500, kFs); impair::echo(*y, 120, -6, 2.0); impair::clip8(*y, 2.0);
        check("combined: +1.5 kHz, +80 ppm, echo, 8 bit", y, 24, 0.8);
    });
    jobs.run();
    if (fails) { printf("%d check(s) failed\n", fails.load()); return 1; }
    printf("OK\n");
    return 0;
}
