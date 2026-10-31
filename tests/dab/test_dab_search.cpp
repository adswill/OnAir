// DAB sync search on channels with no DAB: silence and signals with deep power dips look like a null symbol, and the phase reference search that
// follows is the costly part. It must not run on every block that comes in (that took ten times real time), and a DAB signal that starts after
// them must still be found.
#include "dect2/dab/dab.h"
#include "../common/cpu_report.h"
#include "../common/ci_slack.h"
#include "dect2/common/stable_random.h"
#include "dect2/dab/dab_gen.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;

#include "../common/test_util.h"

static double feedAll(DabReceiver& rx, const std::vector<cf32>& x, double rate) {
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < x.size(); i += 16384) rx.feed(x.data() + i, std::min<size_t>(16384, x.size() - i));
    return 100.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / ((double)x.size() / rate);
}

int main() {
    const double rate = 2.1e6;
    const size_t N = (size_t)(4 * rate);
    std::mt19937 g(1);
    dect2::normal_distribution<float> nd(0, 1.f);
    std::vector<cf32> x(N);
    {
        DabReceiver rx;
        rx.configure(rate);
        rx.audio().setSilent(true);
        const double zeros = feedAll(rx, x, rate);
        cpuCal("dab_search", "silence", zeros, "%-load", '<', 60 * ciSlack());
        CHECK(zeros < 60 * ciSlack(), "silence: load %.0f %%", zeros);
        for (size_t period : {5000, 50000}) {
            for (size_t i = 0; i < N; i++) x[i] = cf32{nd(g), nd(g)} * (((i / period) & 1) ? 1.f : 0.001f);
            const double l = feedAll(rx, x, rate);
            cpuCal("dab_search", "power dips every " + std::to_string(period) + " samples", l, "%-load", '<', 60 * ciSlack());
            CHECK(l < 60 * ciSlack(), "power dips every %zu samples: load %.0f %%", period, l);
        }
    }
    {   // the DAB signal after a channel with nothing: found within a second or two
        DabReceiver rx;
        rx.configure(rate);
        rx.audio().setSilent(true);
        for (size_t i = 0; i < N; i++) x[i] = cf32{nd(g), nd(g)} * (((i / 5000) & 1) ? 1.f : 0.001f);
        feedAll(rx, x, rate);
        dabgen::TxConfig tc;
        tc.utcSeconds = 1700000000;
        SynthConfig sc;
        sc.snrDb = 15;
        auto syn = makeDabSynth(tc, sc, rate);
        std::vector<cf32> y((size_t)(3 * rate));
        syn->generate(y.data(), y.size());
        feedAll(rx, y, rate);
        DabTelemetry t;
        rx.telemetry(t, 0);
        CHECK(t.state == 2 && rx.ensemble().valid, "no lock after the DAB signal started: state %d", t.state);
    }
    {   // the scan's verdict "no DAB here": noise alone is quiet (no null symbol) within a second; a weak DAB signal never is
        auto quiet = [&](double snr, bool dab) {
            DabReceiver rx;
            rx.configure(rate);
            rx.audio().setSilent(true);
            std::vector<cf32> y((size_t)(1.0 * rate));
            if (dab) {
                dabgen::TxConfig tc;
                tc.utcSeconds = 1700000000;
                SynthConfig sc;
                sc.snrDb = snr;
                auto syn = makeDabSynth(tc, sc, rate);
                syn->generate(y.data(), y.size());
            } else for (auto& v : y) v = cf32{nd(g), nd(g)};
            double worst = 0;
            for (size_t i = 0; i < y.size(); i += 8192) {
                rx.feed(y.data() + i, std::min<size_t>(8192, y.size() - i));
                DabTelemetry t;
                rx.telemetry(t, 0);
                worst = std::max(worst, t.quietMs);
            }
            return worst;
        };
        const double n = quiet(0, false), d0 = quiet(0, true), d3 = quiet(3, true);
        printf("quiet input: noise %.0f ms, DAB at 0 dB %.0f ms, at 3 dB %.0f ms\n", n, d0, d3);
        CHECK(n > 700, "noise: only %.0f ms quiet in 1 s", n);
        CHECK(d0 < 300 && d3 < 300, "DAB taken for quiet: %.0f / %.0f ms", d0, d3);
    }
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("test_dab_search: all passed\n");
    return 0;
}
