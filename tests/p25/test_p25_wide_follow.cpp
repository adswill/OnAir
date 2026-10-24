// The whole-span bank in follow-only mode (WideModes::setFollowOnly), as the P25 trunking follower runs it without whole-span decoding: two P25
// carriers on the air; with an empty list no decoder starts, with one granted channel only that carrier gets one (and locks), and with the mode
// off the search finds both again.
#include "dect2/engine/modes.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/t2/t2rx.h"
#include "dect2/engine/wide_modes.h"
#include <cmath>
#include <cstdio>
#include <memory>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const double fs = 2.4e6, centre = 419.2e6;
    const int a = 51;   // P25 (active standard)
    const ModeTuning* mt = modeTuning(a + 1);
    const double off = mt ? -mt->tuneOffsetHz : 0;
    const double hz[2] = {-87500 + off, 62500 + off};
    std::unique_ptr<ModeSynth> g[2];
    for (int k = 0; k < 2; k++) {
        SynthConfig c; c.mode = a + 1; c.snrDb = 35; c.cfoHz = hz[k] - off;
        g[k] = makeModeSynth(a + 1, c, fs);
        if (!g[k]) { CHECK(false, "no P25 generator"); return testSummary("p25 wide follow"); }
    }
    WideModes wm;
    wm.configure(a, fs, off);
    wm.setCenterHz(centre);
    RxTelemetry t; uint64_t seq = 0;
    const size_t blk = (size_t)(fs * 0.1);
    std::vector<cf32> sum(blk), tmp(blk);
    auto run = [&](double secs) {
        size_t peak = 0;
        for (double s = 0; s < secs; s += 0.1) {
            std::fill(sum.begin(), sum.end(), cf32(0, 0));
            for (int k = 0; k < 2; k++) { g[k]->generate(tmp.data(), blk); for (size_t i = 0; i < blk; i++) sum[i] += tmp[i] * (k ? 0.6f : 1.f); }
            wm.feed(a, sum.data(), blk);
            peak = std::max(peak, wm.liveCount(a));
            wm.telemetry(a, t, seq);
        }
        return peak;
    };
    auto rowNear = [&](double f) -> const WideCarrier* { for (auto& r : t.wide) if (r.active && std::fabs(r.hz - f) < 2000) return &r; return nullptr; };

    wm.setFollowOnly(true, {});
    const size_t none = run(5);
    CHECK(none == 0, "no call granted: %zu decoders ran", none);

    wm.setFollowOnly(true, {centre + hz[1]});   // the call on the second carrier is granted
    const size_t one = run(10);
    const WideCarrier* r1 = rowNear(hz[1]);
    CHECK(one == 1, "one call granted: %zu decoders ran", one);
    CHECK(r1 && r1->state >= 1, "the granted carrier: %s", r1 ? "did not lock" : "no decoder");
    CHECK(!rowNear(hz[0]), "the other carrier got a decoder");

    wm.setFollowOnly(false, {});
    const size_t both = run(10);
    CHECK(both == 2 && rowNear(hz[0]) && rowNear(hz[1]), "follow-only off: %zu decoders", both);
    printf("follow only: none %zu, one %zu, off %zu\n", none, one, both);
    return testSummary("p25 wide follow");
}
