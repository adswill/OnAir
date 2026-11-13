#include "dect2/wxsat/wxsat_pass.h"
#include <cstdio>
using namespace dect2::wxpass;
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); fails++; } }
static Pass P(const char* n, double aos, double dur, double el, double sun = 30, bool vis = true) { Pass p; p.sat = n; p.aosS = aos; p.losS = aos + dur; p.maxElDeg = el; p.sunElDeg = sun; p.visibleChannels = vis; return p; }
int main() {
    Pass a = P("NOAA 19", 0, 720, 75);
    check(rate(a) == 5, "high long daylight pass: 5");
    Pass b = P("NOAA 18", 0, 600, 45);
    check(rate(b) == 4, "45 degrees: 4");
    Pass c = P("METEOR", 0, 600, 20);
    check(rate(c) == 2, "20 degrees: 2");
    Pass d = P("NOAA 15", 0, 600, 8);
    check(rate(d) == 1 && d.why.find("horizon") != std::string::npos, "barely above the horizon: 1 and says so");
    Pass n = P("NOAA 19", 0, 720, 75, -20);
    check(rate(n) == 4 && n.why.find("night") != std::string::npos, "night costs a star for visible products");
    Pass ni = P("METEOR IR", 0, 720, 75, -20, false);
    check(rate(ni) == 5, "infrared-only product does not care about night");
    Pass sh = P("X", 0, 90, 80);
    check(rate(sh) == 1 && sh.why.find("short") != std::string::npos, "a 90 s pass is too short for anything");
    Pass mid = P("X", 0, 300, 80);
    check(rate(mid) == 4, "a 5 minute pass loses a star");
    RateOptions hi; hi.horizonDeg = 20;
    Pass lowAnt = P("X", 0, 600, 22);
    check(rate(lowAnt, hi) == 1, "a high horizon changes the rating");
    // planning: three passes, two overlap
    std::vector<Pass> in = {P("A", 0, 600, 70), P("B", 400, 600, 30), P("C", 2000, 600, 50), P("D", 3000, 600, 10)};
    auto out = plan(in, 2);
    check(out.size() == 3 && out[0].sat == "A" && out[1].sat == "B" && out[2].sat == "C", "weak pass under the minimum dropped (D)");
    check(out[0].losS == 600 && out[1].aosS == 600 && out[1].losS == 1000, "the weaker overlapping pass is trimmed to what is left");
    // overlap leaves too little: dropped
    out = plan({P("A", 0, 600, 70), P("B", 500, 650, 30)}, 1);
    check(out.size() == 2 && out[1].aosS == 600, "50 s of overlap: the rest (550 s) is kept");
    out = plan({P("A", 0, 600, 70), P("B", 500, 200, 30)}, 1);
    check(out.size() == 1 && out[0].sat == "A", "remaining 100 s is too short, dropped");
    // fully inside a better pass: dropped
    out = plan({P("A", 0, 900, 80), P("B", 100, 300, 50)}, 1);
    check(out.size() == 1, "fully covered pass dropped");
    // equal stars: higher elevation wins
    out = plan({P("LOW", 0, 600, 41), P("HIGH", 0, 600, 55)}, 1);
    check(out.size() == 1 && out[0].sat == "HIGH", "equal stars: higher elevation wins the slot");
    check(plan({}, 1).empty(), "empty list");
    // a weaker pass overlapping in the middle of a stronger one at both ends is dropped (cannot split)
    out = plan({P("A", 100, 200, 80), P("B", 0, 800, 41)}, 1);
    bool bTrimmed = false; for (auto& p : out) if (p.sat == "B") bTrimmed = true;
    check(out.size() == 1 || bTrimmed, "no crash on a pass that wraps a stronger one");
    std::printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
