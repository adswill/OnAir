// PACTOR-II / -III / -IV physical layer on the generator's signals, with the exact symbols known: the carriers, the clock, the phase steps, the frame
// period and the sync pattern, then the impairments one by one (tuning error, clock error, noise, fading, two paths) over all three modes, and
// noise or a tone alone (nothing may be found). The generator's frames are not real PACTOR frames (the coding is proprietary).
#include "dect2/pactorhf/pactorhf_dsp.h"
#include "dect2/pactorhf/pactorhf_gen.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<float> audio(PactorHfGenConfig c, double sec) {
    c.rate = pactorhf::kFs; c.analytic = false;
    PactorHfGenerator g(c);
    std::vector<cf32> o((size_t)(sec * c.rate));
    g.generate(o.data(), o.size());
    std::vector<float> x(o.size());
    for (size_t i = 0; i < x.size(); i++) x[i] = o[i].real();
    return x;
}

// best agreement of the recovered symbols of a carrier with the sent ones (any start shift of +-3, any constant step)
static double symAcc(const PactorHfGenerator& g, const pactorhf::Carrier& car, size_t kk, int order) {
    double best = 0;
    const auto& sent = g.sent()[kk];
    for (int sh = -3; sh <= 3; sh++) for (int add = 0; add < order; add++) {
        size_t m = 0, t = 0;
        for (size_t i = 10; i + 10 < car.sym.size() && i < 2500; i++) {
            const long j = (long)i + sh;
            if (j < 0) continue;
            t++;
            if ((sent[(size_t)j] + add) % order == car.sym[i]) m++;
        }
        if (t) best = std::max(best, (double)m / (double)t);
    }
    return best;
}

struct Case { const char* name; PactorHfGenConfig c; int wantCarriers; int wantLevel; double minAcc; bool frames; };

int main() {
    // 1. PACTOR-II and PACTOR-III, every constellation order of the standard, clean
    {
        for (int order : {2, 4}) {
            PactorHfGenConfig c; c.variant = 2; c.order = order;
            PactorHfGenerator g(c);
            const auto x = audio(c, 40);
            const pactorhf::Analysis a = pactorhf::analyse(x.data(), x.size(), 0);
            CHECK(a.found && a.variant == 2 && a.nCarriers == 2, "PACTOR-II order %d found %d variant %d carriers %d", order, a.found, a.variant, a.nCarriers);
            CHECK(std::fabs(a.spacingHz - 200) < 3 && std::fabs(a.centreHz - 885) < 5, "PACTOR-II spacing %.1f centre %.1f", a.spacingHz, a.centreHz);
            CHECK(std::fabs(a.baud - 100) < 0.05, "baud %.3f", a.baud);
            CHECK(a.car.size() == 2 && a.car[0].order == order && a.car[1].order == order, "PACTOR-II phase steps per symbol %d", a.car.empty() ? 0 : a.car[0].order);
            if (a.car.size() == 2) CHECK(symAcc(g, a.car[0], 0, order) > 0.995 && symAcc(g, a.car[1], 1, order) > 0.995, "PACTOR-II symbols %.3f %.3f", symAcc(g, a.car[0], 0, order), symAcc(g, a.car[1], 1, order));
            CHECK(a.frame.found && a.frame.period == 362 && a.frame.syncLen >= 38 && a.frame.syncLen <= 42, "PACTOR-II frame %d period %d sync %d", a.frame.found, a.frame.period, a.frame.syncLen);
        }
    }
    {   // PACTOR-III: the carrier counts of the speed levels, DBPSK (levels 1 .. 3) and DQPSK (4 .. 6)
        const struct { int level, order, carriers; } L[] = {{2, 2, 6}, {3, 2, 14}, {4, 4, 14}, {5, 4, 16}, {6, 4, 18}};
        for (const auto& l : L) {
            PactorHfGenConfig c; c.variant = 3; c.level = l.level; c.order = l.order; c.amp = 0.25 / std::sqrt((double)l.carriers / 2);
            PactorHfGenerator g(c);
            const auto x = audio(c, 40);
            const pactorhf::Analysis a = pactorhf::analyse(x.data(), x.size(), 0);
            CHECK(a.found && a.variant == 3 && a.nCarriers == l.carriers, "PACTOR-III level %d: variant %d carriers %d (want %d)", l.level, a.variant, a.nCarriers, l.carriers);
            CHECK(a.level == l.level, "PACTOR-III level %d reported as %d", l.level, a.level);
            CHECK(std::fabs(a.spacingHz - 120) < 2 && std::fabs(a.centreHz - 1500) < 4, "PACTOR-III spacing %.1f centre %.1f", a.spacingHz, a.centreHz);
            double worst = 1;
            for (size_t k = 0; k < a.car.size() && k < g.sent().size(); k++) worst = std::min(worst, symAcc(g, a.car[k], k, l.order));
            CHECK(worst > 0.99, "PACTOR-III level %d worst carrier symbol accuracy %.3f", l.level, worst);
            CHECK(a.frame.found && a.frame.period == 362, "PACTOR-III frame period %d", a.frame.period);
        }
    }
    // 2. impairments, PACTOR-II and -III
    {
        const Case cs[] = {
            {"II noise 12 dB", [] { PactorHfGenConfig c; c.snrDb = 12; return c; }(), 2, 0, 0.99, true},
            {"II tuning +12 Hz", [] { PactorHfGenConfig c; c.snrDb = 15; c.cfoHz = 12; return c; }(), 2, 0, 0.99, true},
            {"II tuning -20 Hz", [] { PactorHfGenConfig c; c.snrDb = 15; c.cfoHz = -20; return c; }(), 2, 0, 0.99, true},
            {"II clock +200 ppm", [] { PactorHfGenConfig c; c.snrDb = 15; c.clockPpm = 200; return c; }(), 2, 0, 0.99, true},
            {"II clock -300 ppm, tuning 8 Hz", [] { PactorHfGenConfig c; c.snrDb = 15; c.clockPpm = -300; c.cfoHz = 8; return c; }(), 2, 0, 0.99, true},
            {"II fading 0.5 Hz", [] { PactorHfGenConfig c; c.snrDb = 25; c.fadeHz = 0.5; return c; }(), 2, 0, 0.98, true},
            {"III level 6 noise 15 dB", [] { PactorHfGenConfig c; c.variant = 3; c.level = 6; c.snrDb = 15; c.amp = 0.08; return c; }(), 18, 6, 0.98, true},
            {"III level 4 tuning 7 Hz clock 100 ppm", [] { PactorHfGenConfig c; c.variant = 3; c.level = 4; c.snrDb = 20; c.cfoHz = 7; c.clockPpm = 100; c.amp = 0.1; return c; }(), 14, 4, 0.98, true},
            {"III level 3 DBPSK tuning -15 Hz", [] { PactorHfGenConfig c; c.variant = 3; c.level = 3; c.order = 2; c.snrDb = 20; c.cfoHz = -15; c.amp = 0.1; return c; }(), 14, 3, 0.98, true},
        };
        for (const auto& k : cs) {
            PactorHfGenerator g(k.c);
            const auto x = audio(k.c, 40);
            const pactorhf::Analysis a = pactorhf::analyse(x.data(), x.size(), 0);
            double worst = 1;
            for (size_t i = 0; i < a.car.size() && i < g.sent().size(); i++) worst = std::min(worst, symAcc(g, a.car[i], i, k.c.order));
            CHECK(a.found && a.nCarriers == k.wantCarriers && (!k.wantLevel || a.level == k.wantLevel), "%s: found %d carriers %d level %d", k.name, a.found, a.nCarriers, a.level);
            CHECK(worst >= k.minAcc, "%s: worst symbol accuracy %.3f", k.name, worst);
            CHECK(!k.frames || (a.frame.found && a.frame.period == 362), "%s: frame period %d", k.name, a.frame.period);
            if (k.c.clockPpm != 0) CHECK(std::fabs(a.clockPpm - k.c.clockPpm) < 40, "%s: clock %.0f ppm (sent %.0f)", k.name, a.clockPpm, k.c.clockPpm);
        }
    }
    // 3. PACTOR-IV: the single carrier at 1800 baud and its constellation
    {
        const struct { int order; double snr; double cfo; double ppm; } L[] = {{2, 25, 0, 0}, {4, 25, 0, 0}, {8, 28, 0, 0}, {16, 30, 0, 0}, {4, 22, 30, 0}, {2, 25, -40, 100}, {8, 30, 20, 50}};
        for (const auto& l : L) {
            PactorHfGenConfig c; c.variant = 4; c.order = l.order; c.snrDb = l.snr; c.cfoHz = l.cfo; c.clockPpm = l.ppm; c.amp = 0.3;
            const auto x = audio(c, 12);
            const pactorhf::Analysis a = pactorhf::analyse(x.data(), x.size(), 0);
            CHECK(a.found && a.variant == 4 && std::fabs(a.baud - 1800) < 1, "PACTOR-IV order %d: found %d variant %d", l.order, a.found, a.variant);
            CHECK(std::fabs(a.centreHz - (1500 + l.cfo)) < 12, "PACTOR-IV order %d: centre %.1f (want %.1f)", l.order, a.centreHz, 1500 + l.cfo);
            CHECK(a.constellation == l.order, "PACTOR-IV order %d: estimated %d (%s)", l.order, a.constellation, a.note.c_str());
        }
    }
    // 4. nothing there: noise, one tone, a tone pair of the wrong spacing, speech-like noise
    {
        std::mt19937 rng(7);
        std::normal_distribution<float> nd(0.f, 0.1f);
        std::vector<float> x((size_t)(30 * pactorhf::kFs));
        for (auto& v : x) v = nd(rng);
        CHECK(!pactorhf::analyse(x.data(), x.size(), 0).found, "noise alone");
        CHECK(!pactorhf::analyse(x.data(), x.size(), 4).found && !pactorhf::analyse(x.data(), x.size(), 2).found && !pactorhf::analyse(x.data(), x.size(), 3).found, "noise alone, every mode");
        for (size_t i = 0; i < x.size(); i++) x[i] += 0.2f * (float)std::sin(2 * 3.14159265358979 * 1000.0 * (double)i / pactorhf::kFs);
        CHECK(!pactorhf::analyse(x.data(), x.size(), 0).found, "a tone in noise");
        const auto sh = audio([] { PactorHfGenConfig c; c.variant = 2; return c; }(), 1.0);
        CHECK(!pactorhf::analyse(sh.data(), sh.size(), 0).found, "one second is too short");
    }
    CHECK(pactorhf::levelForCarriers(2) == 1 && pactorhf::levelForCarriers(6) == 2 && pactorhf::levelForCarriers(18) == 6 && pactorhf::levelForCarriers(5) == 0, "levels of the carrier counts");
    return testSummary("pactorhf dsp");
}
