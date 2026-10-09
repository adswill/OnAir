// DeliveryCheck (delivery_check.h): radios simulated as the engine sees them, with USB transfers that arrive late and in bursts, a clock off
// its nominal rate and a sample thread that looks only now and then. Nothing lost must count nothing; lost transfers must count what was lost.
#include "dect2/delivery_check.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Lcg { uint64_t s; double u() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return (double)(s >> 11) / 9007199254740992.0; } };

struct Radio {
    double rate;            // nominal
    double ppm = 0;         // the radio's clock against the computer's
    size_t block;           // samples per USB transfer
    double jitterMs = 10;   // a transfer arrives up to this late
    double hiccupEvery = 3; // seconds between OS hiccups that hold back the transfers this long, then let them through in a burst
    double hiccupMs = 150;
    std::vector<std::pair<double, int>> drops;   // at this second, lose this many transfers
    double dropFrac = 0;    // or lose this share of the transfers at random
};

struct Result { double lostSec; int events; double trueLostSec; };

static Result run(const Radio& r, double seconds, uint64_t seed) {
    Lcg g{seed};
    DeliveryCheck dc;
    const int64_t t0 = 1000000000000LL;
    dc.restart(t0 + 1000000000LL);   // as the engine at a start: one second of grace
    const double real = r.rate * (1 + r.ppm * 1e-6);
    const double blockSec = (double)r.block / real;
    // the transfers: when each completes in the radio, and when the host gets it
    struct D { int64_t ns; uint64_t off; };
    std::vector<D> del;
    uint64_t off = 0;
    double held = 0;   // the end of the current hiccup
    double nextHiccup = r.hiccupEvery;
    size_t di = 0;
    double trueLost = 0;
    for (double t = blockSec; t < seconds; t += blockSec) {
        bool lose = r.dropFrac > 0 && g.u() < r.dropFrac;
        for (; di < r.drops.size() && t >= r.drops[di].first; di++) {}
        for (auto& d : r.drops) if (t >= d.first && t < d.first + d.second * blockSec) lose = true;
        if (lose) { trueLost += blockSec; continue; }
        off += r.block;
        double at = t + g.u() * r.jitterMs * 1e-3;
        if (r.hiccupEvery > 0 && t >= nextHiccup) { held = t + r.hiccupMs * 1e-3; nextHiccup += r.hiccupEvery; }
        if (at < held) at = held + g.u() * 1e-3;
        if (!del.empty() && at * 1e9 < (double)(del.back().ns - t0)) at = (double)(del.back().ns - t0) * 1e-9;   // in order
        del.push_back({t0 + (int64_t)(at * 1e9), off});
    }
    // the sample thread looks every 33 ms, now and then only after a 300 ms stall, and sees the latest delivery
    double lost = 0; int events = 0;
    size_t k = 0;
    for (double t = 0; t < seconds + 1; t += (g.u() < 0.05 ? 0.3 : 0.033)) {
        const int64_t now = t0 + (int64_t)(t * 1e9);
        while (k < del.size() && del[k].ns <= now) k++;
        if (!k) continue;
        const double s = dc.observe(del[k - 1].off, del[k - 1].ns, r.rate, (double)r.block);
        if (s > 0) { lost += s / real; events++; if (getenv("DC_TRACE")) printf("    at %.2f s: %.1f ms\n", t, s / real * 1e3); }
    }
    return {lost, events, trueLost};
}

int main() {
    // ---- nothing lost: a HackRF (20 Msps, 6.5 ms transfers) with jitter, hiccups and a clock 50 ppm off; an RTL-SDR on macOS (100 ms
    // blocks, 100 ppm); a radio whose clock is 300 ppm off
    {
        Radio h{20e6, 50, 131072};
        Radio rtl{2.4e6, -100, 240000, 30, 4, 200};
        Radio off{10e6, 300, 65536};
        for (uint64_t seed = 1; seed <= 5; seed++) {
            const Result a = run(h, 120, seed), b = run(rtl, 120, seed), c = run(off, 120, seed);
            CHECK(a.events == 0, "HackRF without loss: %d gaps, %.1f ms (seed %llu)", a.events, a.lostSec * 1e3, (unsigned long long)seed);
            CHECK(b.events == 0, "RTL-SDR without loss: %d gaps, %.1f ms (seed %llu)", b.events, b.lostSec * 1e3, (unsigned long long)seed);
            CHECK(c.events == 0, "300 ppm clock error: %d gaps, %.1f ms (seed %llu)", c.events, c.lostSec * 1e3, (unsigned long long)seed);
        }
        printf("  no loss: 0 gaps in 3 x 5 x 120 s\n");
    }
    // ---- lost transfers: every gap found (a long one can be counted in two pieces), the time counted to within 10 ms or 2 %
    {
        Radio h{20e6, 50, 131072};
        h.drops = {{10.0, 1}, {30.0, 4}, {50.0, 16}, {70.0, 150}, {72.0, 2}};   // 6.5 ms, 26 ms, 105 ms, 983 ms, and 13 ms soon after the long one
        for (uint64_t seed = 1; seed <= 5; seed++) {
            const Result a = run(h, 90, seed);
            if (seed == 1) printf("  HackRF, 5 gaps: %d found, %.1f ms counted, %.1f ms lost\n", a.events, a.lostSec * 1e3, a.trueLostSec * 1e3);
            CHECK(a.events >= 5 && a.events <= 7, "HackRF: %d gaps found, 5 lost (seed %llu)", a.events, (unsigned long long)seed);
            CHECK(std::fabs(a.lostSec - a.trueLostSec) < std::max(0.01, 0.02 * a.trueLostSec), "HackRF: %.1f ms counted, %.1f ms lost (seed %llu)", a.lostSec * 1e3, a.trueLostSec * 1e3, (unsigned long long)seed);
        }
        Radio rtl{2.4e6, -100, 240000, 30, 4, 200};
        rtl.drops = {{20.0, 1}, {60.0, 3}};
        const Result b = run(rtl, 90, 7);
        printf("  RTL-SDR, 2 gaps: %d found, %.1f ms counted, %.1f ms lost\n", b.events, b.lostSec * 1e3, b.trueLostSec * 1e3);
        CHECK(b.events >= 2 && b.events <= 3 && std::fabs(b.lostSec - b.trueLostSec) < 0.01, "RTL-SDR: %d gaps, %.1f ms counted, %.1f ms lost", b.events, b.lostSec * 1e3, b.trueLostSec * 1e3);
    }
    // ---- a steady trickle: 2 % of the transfers lost at random; the total is what matters (within 15 %)
    {
        Radio h{10e6, 0, 65536};
        h.dropFrac = 0.02;
        const Result a = run(h, 60, 3);
        printf("  2 %% of transfers lost: %.0f ms counted in %d steps, %.0f ms lost\n", a.lostSec * 1e3, a.events, a.trueLostSec * 1e3);
        CHECK(std::fabs(a.lostSec - a.trueLostSec) < 0.15 * a.trueLostSec, "trickle: %.0f ms counted, %.0f ms lost", a.lostSec * 1e3, a.trueLostSec * 1e3);
    }
    printf(fails ? "delivery check: FAILED\n" : "delivery check: ok\n");
    return fails ? 1 : 0;
}
