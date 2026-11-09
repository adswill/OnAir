// HF ALE on every known channel of one capture: two generated ALE signals through the engine's bank. Simulated (ale_gen).
#include "dect2/ale/ale_gen.h"
#include "dect2/ale/ale_rx.h"
#include "dect2/ale/ale_tel.h"
#include "dect2/engine/wide_modes.h"
#include "dect2/engine/wide_bank.h"
#include "dect2/t2/t2rx.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const double rate = 192000, centre = 7120000;
    for (double cfo : {0.0, 25.0}) {
        const double secs = 26;
        const size_t n = (size_t)(rate * secs);
        std::vector<cf32> x(n, cf32(0, 0)), y(n);
        for (double hz : {7102000.0, 7150000.0}) {
            SynthConfig sc; sc.snrDb = 40; sc.cfoHz = cfo;
            auto g = makeAleSynth(sc, rate);
            g->generate(y.data(), n);
            const double w = 2 * M_PI * (hz - centre + aleTuning().tuneOffsetHz) / rate;   // the generator puts its dial at minus the tuning offset
            for (size_t i = 0; i < n; i++) x[i] += y[i] * cf32((float)std::cos(w * i), (float)std::sin(w * i));
        }
        WideModes w; w.setCenterHz(centre); w.setExtraPriors({7102000.0, 7150000.0}); w.configure(50, rate, 0);
        WideTuning tun; tun.cpuBudget = 0; w.setTuning(tun);
        for (size_t i = 0; i < n; i += 8192) w.feed(50, x.data() + i, std::min<size_t>(8192, n - i));
        RxTelemetry t; uint64_t seq = 0;
        CHECK(w.telemetry(50, t, seq), "telemetry");
        CHECK(w.liveCount(50) == 2, "two channels, got %zu", w.liveCount(50));
        printf("CFO %.0f: %zu channels, %zu events, %zu stations\n", cfo, t.wide.size(), t.ale.events.size(), t.ale.stations.size());
        CHECK(t.ale.events.size() >= 8, "events from both channels %zu", t.ale.events.size());
        CHECK(!t.ale.stations.empty(), "stations");
    }
    return testSummary("ale_wide");
}
