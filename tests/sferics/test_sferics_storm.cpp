// Storm trend from strike rate and strength. Simulated strike series with noise.
#include "dect2/sferics/sferics_storm.h"
#include <cstdio>
#include <random>
using namespace dect2;
#include "../common/test_util.h"

static StormPhase run(double rate0, double rate1, double db0, double db1, unsigned seed, double jitter = 2.0) {
    StormTrend t; std::mt19937 r(seed); std::normal_distribution<double> g(0, jitter);
    double now = 0; std::uniform_real_distribution<double> u(0, 1);
    for (double s = 0; s < 600; s += 1.0) {
        const double f = s / 600, rate = (rate0 + (rate1 - rate0) * f) / 60.0, db = db0 + (db1 - db0) * f;
        if (u(r) < rate) t.add(s, (float)(db + g(r)));
        now = s;
    }
    return t.update(now);
}

int main() {
    CHECK(run(0, 0, 0, 0, 1) == StormPhase::None, "nothing");
    int ok = 0, n = 0;
    for (unsigned s = 1; s <= 30; s++) { n++; if (run(4, 12, 15, 30, s) == StormPhase::Approaching) ok++; }
    printf("approaching recognised %d/%d\n", ok, n); CHECK(ok >= 26, "approaching %d", ok);
    ok = 0;
    for (unsigned s = 1; s <= 30; s++) if (run(12, 3, 30, 15, s) == StormPhase::Receding) ok++;
    printf("receding recognised %d/%d\n", ok, n); CHECK(ok >= 26, "receding %d", ok);
    ok = 0; int wrong = 0;
    for (unsigned s = 1; s <= 30; s++) { auto p = run(6, 6, 20, 20, s, 1.0); if (p == StormPhase::Steady) ok++; if (p == StormPhase::Approaching || p == StormPhase::Receding) wrong++; }
    printf("steady %d/30, called moving %d\n", ok, wrong); CHECK(wrong <= 6, "steady storm called moving %d", wrong);
    ok = 0; for (unsigned s = 1; s <= 10; s++) if (run(30, 30, 35, 35, s) == StormPhase::Overhead) ok++;
    CHECK(ok >= 8, "overhead %d", ok);
    { StormTrend t; for (int i = 0; i < 8; i++) t.add(i * 70.0, 12); CHECK(t.update(560) == StormPhase::Distant || t.last == StormPhase::Steady, "a few strikes"); }
    return testSummary("sferics_storm");
}
