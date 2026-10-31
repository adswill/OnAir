// Coast-mode collision cancellation: a weaker message under (or partly under) a stronger one in a busy port is found by cancelling the stronger
// burst. Real-world impairments: carrier offsets of both transmitters, bit clock error, noise, DC offset, 8-bit quantisation, different levels
// and overlaps. Also: clean traffic costs nothing (the same messages, nothing invented), and switching it off gives the old behaviour.
#include "dect2/ais/ais_gen.h"
#include "dect2/ais/ais_rx.h"
#include "dect2/ais/ais_testutil.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <set>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Run { int sentMsgs = 0, gotSent = 0, invented = 0; uint64_t cancelled = 0, recovered = 0, collisionRecovered = 0; };

static Run run(const std::vector<AisBurstSpec>& sp, const AisRenderConfig& rc, bool cancel) {
    std::set<uint32_t> sent;
    for (const auto& s : sp) sent.insert(ais::getU(s.payload, 8, 30));
    const std::vector<cf32> x = renderAisBursts(sp, rc);
    AisReceiver rx;
    rx.configure(rc.rate);
    rx.setCollisionCancel(cancel);
    aistest::Collector col;
    const AisTelemetry t = aistest::runReceiver(rx, x, 16384, &col);
    Run r;
    r.sentMsgs = (int)sent.size();
    std::set<uint32_t> got;
    for (const auto& g : col.got) {
        const uint32_t m = ais::getU(g.bits, 8, 30);
        if (sent.count(m)) got.insert(m); else r.invented++;
    }
    r.gotSent = (int)got.size();
    r.cancelled = t.cancelled; r.recovered = t.recovered; r.collisionRecovered = t.collisionRecovered;
    return r;
}

// pairs of bursts on AIS 1 and AIS 2: the strong one and a weaker one starting 'lag' later, with their own offsets and levels
static std::vector<AisBurstSpec> pairs(std::mt19937& rng, int n, double lvLo, double lvHi, double lagLo, double lagHi, double cfoLo, double cfoHi) {
    std::vector<AisBurstSpec> sp;
    for (int i = 0; i < n; i++) {
        const double t0 = 0.1 + 0.2 * i;
        auto u = [&](double a, double b) { return a + (b - a) * (double)(rng() % 100000) / 100000.0; };
        AisBurstSpec s; s.payload = aistest::randomPosition(rng); s.channel = i & 1 ? 'B' : 'A'; s.startSec = t0; s.level = 1.0; s.cfoHz = u(-300, 300); s.phase = u(0, 6.28);
        AisBurstSpec w; w.payload = aistest::randomPosition(rng); w.channel = s.channel; w.startSec = t0 + u(lagLo, lagHi); w.level = u(lvLo, lvHi);
        const double d = u(cfoLo, cfoHi);
        w.cfoHz = s.cfoHz + (rng() & 1 ? d : -d); w.phase = u(0, 6.28);
        sp.push_back(s); sp.push_back(w);
    }
    return sp;
}

int main() {
    // 1. colliding pairs: the weaker one starts 2 - 12 ms after the stronger, 6 - 14 dB lower, 300 - 900 Hz apart; a real receiver's impairments
    {
        std::mt19937 rng(11);
        const auto sp = pairs(rng, 24, 0.2, 0.5, 0.002, 0.012, 300, 900);
        AisRenderConfig rc; rc.rate = 1e6; rc.durationSec = 0.2 * 24 + 0.3; rc.snrDb = 30; rc.sroPpm = 15; rc.dcOffset = 0.002; rc.quantBits = 8; rc.cfoHz = 120; rc.seed = 3;
        const Run off = run(sp, rc, false), on = run(sp, rc, true);
        printf("pairs: %d messages, off %d, on %d (%llu cancelled, %llu recovered), invented %d\n", off.sentMsgs, off.gotSent, on.gotSent,
               (unsigned long long)on.cancelled, (unsigned long long)on.recovered, on.invented);
        CHECK(on.gotSent >= off.gotSent + 4, "cancellation finds more of the weaker messages: %d vs %d", on.gotSent, off.gotSent);
        CHECK(on.invented == 0 && off.invented == 0, "nothing invented: %d / %d", on.invented, off.invented);
        CHECK(on.collisionRecovered > 0 && off.collisionRecovered == 0, "counted: %llu / %llu", (unsigned long long)on.collisionRecovered, (unsigned long long)off.collisionRecovered);
    }
    // 2. different parameter combinations: closer levels, nearly the same frequency, heavier noise; it never does worse than without
    {
        struct Combo { double lvLo, lvHi, lagLo, lagHi, cfoLo, cfoHi, snr; } combos[] = {
            {0.15, 0.35, 0.003, 0.010, 600, 1200, 35}, {0.3, 0.6, 0.008, 0.020, 200, 700, 28}, {0.2, 0.4, 0.0, 0.004, 400, 1000, 32}, {0.1, 0.25, 0.005, 0.015, 100, 500, 40}};
        int ci = 0;
        for (const Combo& cb : combos) {
            std::mt19937 rng(100 + ci);
            const auto sp = pairs(rng, 16, cb.lvLo, cb.lvHi, cb.lagLo, cb.lagHi, cb.cfoLo, cb.cfoHi);
            AisRenderConfig rc; rc.rate = 2e6; rc.durationSec = 0.2 * 16 + 0.3; rc.snrDb = cb.snr; rc.sroPpm = -20; rc.cfoHz = -200; rc.seed = 5 + (uint32_t)ci;
            const Run off = run(sp, rc, false), on = run(sp, rc, true);
            printf("combo %d: off %d on %d of %d\n", ci, off.gotSent, on.gotSent, off.sentMsgs);
            CHECK(on.gotSent >= off.gotSent, "combo %d: not worse: %d vs %d", ci, on.gotSent, off.gotSent);
            CHECK(on.invented == 0, "combo %d: invented %d", ci, on.invented);
            ci++;
        }
    }
    // 3. clean traffic: single bursts, spread over both channels, with offsets and noise: identical result on and off
    {
        std::mt19937 rng(77);
        std::vector<AisBurstSpec> sp;
        for (int i = 0; i < 30; i++) {
            AisBurstSpec s; s.payload = aistest::randomPosition(rng); s.channel = i & 1 ? 'B' : 'A'; s.startSec = 0.1 + 0.1 * i; s.level = 0.5 + 0.5 * (double)(rng() % 100) / 100;
            s.cfoHz = (double)((int)(rng() % 1200) - 600);
            sp.push_back(s);
        }
        AisRenderConfig rc; rc.rate = 1e6; rc.durationSec = 3.3; rc.snrDb = 18; rc.seed = 9;
        const Run off = run(sp, rc, false), on = run(sp, rc, true);
        printf("clean: off %d on %d of %d\n", off.gotSent, on.gotSent, off.sentMsgs);
        CHECK(on.gotSent == off.gotSent && on.invented == 0, "clean traffic unchanged: %d vs %d, invented %d", on.gotSent, off.gotSent, on.invented);
        CHECK(on.gotSent >= 26, "clean traffic decoded: %d", on.gotSent);
    }
    // 4. noise only: no message appears out of a noisy channel (false positives of the extra attempts)
    {
        std::vector<AisBurstSpec> none;
        AisRenderConfig rc; rc.rate = 1e6; rc.durationSec = 4; rc.snrDb = 5; rc.seed = 21;
        const std::vector<cf32> x = renderAisBursts(none, rc);
        AisReceiver rx; rx.configure(rc.rate);
        const AisTelemetry t = aistest::runReceiver(rx, x);
        CHECK(t.blocksOk == 0, "noise decodes %llu messages", (unsigned long long)t.blocksOk);
    }
    // 5. a burst that is only half under another and is the stronger of the two still decodes (the strong one hides nothing)
    {
        std::mt19937 rng(31);
        std::vector<AisBurstSpec> sp;
        for (int i = 0; i < 10; i++) {
            AisBurstSpec s; s.payload = aistest::randomPosition(rng); s.channel = 'A'; s.startSec = 0.1 + 0.2 * i; s.level = 0.35; s.cfoHz = -400;
            AisBurstSpec w = s; w.payload = aistest::randomPosition(rng); w.startSec += 0.012; w.level = 1.0; w.cfoHz = 350;
            sp.push_back(s); sp.push_back(w);
        }
        AisRenderConfig rc; rc.rate = 1e6; rc.durationSec = 2.4; rc.snrDb = 30; rc.seed = 13;
        const Run off = run(sp, rc, false), on = run(sp, rc, true);
        printf("late strong: off %d on %d of %d\n", off.gotSent, on.gotSent, off.sentMsgs);
        CHECK(on.gotSent >= off.gotSent && on.invented == 0, "late strong burst: %d vs %d", on.gotSent, off.gotSent);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ais collision OK\n");
    return 0;
}
