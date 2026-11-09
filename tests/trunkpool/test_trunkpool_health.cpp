// Control channel health: rolling decode-quality score per channel, the move to a clearly better one with hysteresis (no flapping), and the site book.
#include "dect2/trunkpool/cc_health.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2::tpool;
#include "../common/test_util.h"

int main() {
    const double A = 851.0125e6, B = 852.2375e6;
    std::mt19937 g(3);
    auto feed = [&](ControlHealth& h, double hz, double t0, double t1, double errRate, double rate = 2.0) {
        for (double t = t0; t < t1; t += 1.0 / rate) { if ((g() % 10000) / 10000.0 < errRate) h.onBad(hz, t); else h.onGood(hz, t); }
    };
    // 1. a channel that degrades: the radio moves to the good one after the hold time, once
    {
        ControlHealth h;
        feed(h, A, 0, 600, 0.02); feed(h, B, 0, 600, 0.03, 0.5);   // both known (B was visited earlier, now heard on the side at 0.5 msg/s)
        CHECK(h.advise(A, 600) == 0, "healthy: stay");
        double moved = 0;
        for (double t = 600; t < 1200; t += 1.0) {
            feed(h, A, t, t + 1, 0.6); feed(h, B, t, t + 1, 0.03, 0.5);
            const double a = h.advise(A, t);
            if (a > 0 && moved == 0) { moved = t; CHECK(std::fabs(a - B) < 1, "moves to B"); }
        }
        printf("channel A degraded (60%% bad) at 600 s: advice to move after %.0f s\n", moved - 600);
        CHECK(moved > 0 && moved - 600 < 200, "moved within %.0f s", moved - 600);
    }
    // 2. no flapping: two channels of equal, noisy quality over a simulated hour: no advice at all
    {
        ControlHealth h;
        int advice = 0;
        for (double t = 0; t < 3600; t += 1.0) {
            feed(h, A, t, t + 1, 0.05 + 0.15 * std::sin(t / 300.0) * std::sin(t / 300.0), 2.0);
            feed(h, B, t, t + 1, 0.05 + 0.15 * std::cos(t / 300.0) * std::cos(t / 300.0), 2.0);
            if (h.advise(A, t) > 0) advice++;
        }
        printf("equal noisy channels over one hour: %d advices\n", advice);
        CHECK(advice == 0, "no flapping (%d)", advice);
    }
    // 3. a channel that was never heard well is not a target; a silent current channel is left for a live one
    {
        ControlHealth h;
        feed(h, A, 0, 300, 0.01);
        h.onBad(B, 100);                                 // B: one failure only
        double a = 0;
        for (double t = 300; t < 600; t += 1.0) { a = h.advise(A, t); if (a > 0) break; }   // A goes silent
        CHECK(a == 0, "no target");
        feed(h, B, 600, 900, 0.01);
        for (double t = 600; t < 900; t += 1.0) { feed(h, B, t, t + 1, 0.01); a = h.advise(A, t); if (a > 0) break; }
        CHECK(std::fabs(a - B) < 1, "silent A, live B: move");
    }
    // 4. the site book
    {
        ControlHealth h;
        h.learnSite(0x123, 1, 4, A, 10); h.learnSite(0x123, 1, 5, B, 20); h.learnSite(0x123, 1, 4, A, 30);
        CHECK(h.sites().size() == 2 && h.sites()[0].seen == 2, "two sites, one seen twice");
        ControlHealth o;
        CHECK(o.importCsv(h.exportCsv()) == 2 && o.sites().size() == 2 && std::fabs(o.sites()[1].ccHz - B) < 1, "csv round trip");
    }
    return testSummary("trunkpool health");
}
