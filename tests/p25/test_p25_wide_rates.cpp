// P25 on a wide capture (3.4): a HackRF at 5, 10 and 20 Msps. Three P25 carriers sit far from the centre (each with its own tuning error, 6 dB
// apart, in noise); the whole-span bank must find and lock every one, and in follow-only mode (the trunking follower) a call granted on the farthest
// carrier must start exactly one decoder that locks. The rate is the user's: nothing here clamps it.
#include "dect2/engine/modes.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/wide_modes.h"
#include <cmath>
#include <cstdio>
#include <memory>
using namespace dect2;
#include "../common/test_util.h"

static void scene(int a, double fs) {
    const double centre = 419.2e6;
    const ModeTuning* mt = modeTuning(a + 1);
    const double off = mt ? -mt->tuneOffsetHz : 0;
    // carriers at -0.36, +0.05 and +0.33 of the rate, each with a small error of its own (inside the search window)
    const double pos[3] = {-0.36 * fs, 0.05 * fs, 0.33 * fs}, err[3] = {1800, -2300, 900};
    const float lvl[3] = {1.f, 0.5f, 0.25f};
    std::unique_ptr<ModeSynth> g[3];
    double hz[3];
    for (int k = 0; k < 3; k++) {
        hz[k] = pos[k] + off;
        SynthConfig c; c.mode = a + 1; c.snrDb = 30; c.cfoHz = hz[k] + err[k] - off;
        g[k] = makeModeSynth(a + 1, c, fs);
        if (!g[k]) { CHECK(false, "no P25 generator"); return; }
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
            for (int k = 0; k < 3; k++) { g[k]->generate(tmp.data(), blk); for (size_t i = 0; i < blk; i++) sum[i] += tmp[i] * lvl[k]; }
            wm.feed(a, sum.data(), blk);
            peak = std::max(peak, wm.liveCount(a));
            wm.telemetry(a, t, seq);
        }
        return peak;
    };
    auto rowNear = [&](double f) -> const WideCarrier* { for (auto& r : t.wide) if (r.active && std::fabs(r.hz - f) < 6000) return &r; return nullptr; };

    if (a == 51) {
    wm.setFollowOnly(true, {centre + hz[2]});   // the call is on the farthest carrier
    const size_t one = run(10);
    const WideCarrier* r = rowNear(hz[2]);
    CHECK(one == 1, "%.0f Msps: one call granted: %zu decoders ran", fs / 1e6, one);
    CHECK(r && r->state >= 1, "%.0f Msps: the granted far carrier: %s", fs / 1e6, r ? "did not lock" : "no decoder");
    CHECK(!rowNear(hz[0]) && !rowNear(hz[1]), "%.0f Msps: another carrier got a decoder", fs / 1e6);

    wm.setFollowOnly(false, {});
    }
    const size_t all = run(12);
    int locked = 0;
    for (int k = 0; k < 3; k++) { const WideCarrier* w = rowNear(hz[k]); if (w && w->state >= 1) locked++; }
    CHECK(all >= 3 && locked == 3, "mode %d %.0f Msps: whole span: %zu decoders, %d of 3 carriers locked", a, fs / 1e6, all, locked);
    printf("mode %d, %.0f Msps: whole span %zu decoders, %d/3 locked\n", a, fs / 1e6, all, locked);
}

int main() {
    for (int a : {51, 10, 39, 46}) for (double fs : {5e6, 10e6, 20e6}) scene(a, fs);
    return testSummary("p25 wide rates");
}
