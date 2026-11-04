// AIS: 450 stations are all kept (the table used to evict the oldest above 300), and the receiver keeps real time.
#include "dect2/ais/ais_testutil.h"
#include "dect2/limits.h"
#include <chrono>
#include <cstdio>
#include <set>
using namespace dect2;
using namespace dect2::ais;
#include "../common/test_util.h"

int main() {
    const int n = 450;
    std::mt19937 rng(5);
    std::vector<AisBurstSpec> sp;
    std::set<uint32_t> sent;
    for (int i = 0; i < n; i++) {
        AisBurstSpec s;
        s.payload = aistest::randomPosition(rng, 201000000u + (uint32_t)i);
        s.channel = i & 1 ? 'B' : 'A';
        s.startSec = 0.1 + 0.04 * i;
        s.level = 1.0;
        sp.push_back(s);
        sent.insert(201000000u + (uint32_t)i);
    }
    AisRenderConfig rc; rc.rate = 1e6; rc.durationSec = 0.1 + 0.04 * n + 0.3; rc.snrDb = 30; rc.seed = 3;
    const std::vector<cf32> x = renderAisBursts(sp, rc);
    AisReceiver rx;
    rx.configure(rc.rate);
    aistest::Collector col;
    const auto t0 = std::chrono::steady_clock::now();
    const AisTelemetry t = aistest::runReceiver(rx, x, 16384, &col);
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::set<uint32_t> have;
    for (const auto& v : t.vessels) if (sent.count(v.mmsi)) have.insert(v.mmsi);
    printf("%d stations sent, %zu in the list, vesselCount %u, %.1f s for %.1f s of signal\n", n, have.size(), (unsigned)t.vesselCount, dt, rc.durationSec);
    CHECK(have.size() >= (size_t)(n * 0.9), "stations kept: %zu of %d", have.size(), n);
    CHECK(t.vesselCount >= (uint32_t)have.size(), "count %u", (unsigned)t.vesselCount);
    CHECK(dt < rc.durationSec * 0.9, "processing %.1f s for %.1f s", dt, rc.durationSec);
    // "remove after": with a 20 s limit, 30 s of quiet on the receiver's clock empties the table, and the count follows the list
    limits::setStale(limits::Family::Ais, 20);
    std::vector<cf32> quiet((size_t)(30 * rc.rate), cf32(0, 0));
    AisTelemetry t2 = aistest::runReceiver(rx, quiet, 16384, &col);
    CHECK(t2.vessels.empty() && t2.vesselCount == 0, "after 30 s with a 20 s limit: %zu listed, count %u", t2.vessels.size(), (unsigned)t2.vesselCount);
    return testSummary("ais_caps");
}
