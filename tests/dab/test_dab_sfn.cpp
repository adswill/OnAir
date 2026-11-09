// DAB network paths and transmitter matching on synthetic impulse responses: delays from distances, a static offset that fits nothing, a path beyond the guard interval.
#include "dect2/dab/dab_sfn.h"
#include <cstdio>
#include <random>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<float> cir(std::initializer_list<std::pair<double, double>> p, double base = 500) {
    std::vector<float> v(2048, 0.f); std::mt19937 r(1); std::uniform_real_distribution<float> u(0, 0.01f);
    for (auto& c : v) c = u(r);
    for (auto& q : p) { const int b = (int)std::lround((base + q.first) * 2.048); v[(size_t)((b + 2048) % 2048)] += (float)std::pow(10.0, q.second / 20); }
    return v;
}

int main() {
    auto paths = dabCirPaths(cir({{0, 0}, {80, -6}, {-40, -10}}));
    CHECK(paths.size() == 3, "%zu paths", paths.size());
    CHECK(std::fabs(paths[0].delayUs) < 0.1 && std::fabs(paths[1].delayUs - 80) < 1 && std::fabs(paths[2].delayUs + 40) < 1 && std::fabs(paths[1].levelDb + 6) < 1, "delays");
    // A at 30 km, B at 54 km (80 us later), C at 18 km (40 us earlier)
    std::vector<DabSfnTx> t = {{1, 1, 30, 10, 20}, {2, 1, 54, 100, 14}, {3, 1, 18, 250, 10}, {4, 1, -1, 0, 8}};
    auto s = dabSfnSolve(paths, t);
    CHECK(s.refTx == 0, "the strongest path is A: %d", s.refTx);
    CHECK(s.rows[0].matched && s.rows[1].matched && s.rows[2].matched && !s.rows[3].matched, "three matched");
    CHECK(std::fabs(s.rows[1].expectedUs - 80) < 1, "expected %.1f", s.rows[1].expectedUs);
    CHECK(!s.outsideGuard, "inside");
    // the same with the delays cyclic around the buffer end, and a far path
    auto far = dabCirPaths(cir({{0, 0}, {300, -8}}, 1000));
    CHECK(far.size() == 2 && std::fabs(far[1].delayUs - 300) < 1, "far path %.1f", far.size() > 1 ? far[1].delayUs : 0);
    CHECK(dabSfnSolve(far, t).outsideGuard, "beyond the guard interval");
    // a path that no transmitter explains stays unmatched; no positions at all
    auto q = dabSfnSolve(dabCirPaths(cir({{0, 0}, {123, -5}})), {{1, 1, 30, 0, 20}, {2, 1, 40, 0, 10}});
    CHECK(q.rows[0].matched && !q.rows[1].matched, "the unexplained path");
    CHECK(dabSfnSolve(paths, {{1, 1, -1, 0, 3}}).refTx == -1, "no position");
    return testSummary("dab_sfn");
}
