// FLARM Chase decoding (3.9, docs/SENSITIVITY-3.9-C.md): near the threshold the receiver with the 6-bit Chase search must accept more packets
// than hard decisions alone, never make up an aircraft, and give nothing on noise. Simulated signals only (flarm_gen.h).
// ONAIR_TEST_LONG=1 prints the whole curve at 1 and 2.4 Msps.
#include "dect2/flarm/flarm_gen.h"
#include "dect2/flarm/flarm_rx.h"
#include "dect2/flarm/flarm_tel.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

constexpr double kRefLat = 54.70, kRefLon = 25.30;

static std::vector<FlarmTx> train(double u0, int n, uint32_t id, double cfo) {
    std::vector<FlarmTx> v;
    for (int i = 0; i < n; i++) {
        FlarmTx tx; const double t = 0.05 + 0.12 * i;
        tx.timeSec = t; tx.channel = i & 1; tx.keyTime = (uint32_t)(u0 + t); tx.cfoHz = cfo;
        flarm::V6& p = tx.pkt;
        p.addr = id; p.aircraftType = 1; p.alt = 900 + (uint32_t)(t * 3);
        flarm::packPosition(kRefLat + (1000 + 25.0 * t) / 111320.0, kRefLon + 0.01, p.lat19, p.lon20);
        flarm::packVelocity(25.0, 0.0, 0.5, p);
        v.push_back(tx);
    }
    return v;
}

static FlarmTelemetry decode(const std::vector<cf32>& x, double rate, double u0, bool chase) {
    FlarmReceiver::chaseOn = chase;
    FlarmReceiver r;
    r.configure(rate); r.setUnixTime(u0); r.setReference(kRefLat, kRefLon);
    FlarmTelemetry last; uint64_t seq = 0;
    for (size_t i = 0; i < x.size(); i += 16384) {
        r.feed(x.data() + i, std::min<size_t>(16384, x.size() - i));
        FlarmTelemetry t;
        if (r.telemetry(t, seq)) { seq = t.seq; last = std::move(t); }
    }
    FlarmReceiver::chaseOn = true;
    return last;
}

int main() {
    constexpr bool kCalibrate = true;   // WIP: print the whole curve and fail so the CI log shows it
    const bool full = kCalibrate || std::getenv("ONAIR_TEST_LONG") != nullptr;
    const double u0 = 1800000000.0 - std::fmod(1800000000.0, 64.0) + 20.0;
    const int N = full ? 60 : 30;
    uint64_t hard = 0, soft = 0;
    for (double rate : full ? std::vector<double>{1e6, 2.4e6} : std::vector<double>{1e6}) {
        for (double snr : full ? std::vector<double>{16, 14, 13, 12, 11, 10, 9, 8} : std::vector<double>{12, 11}) {
            for (unsigned seed = 1; seed <= (full ? 3u : 1u); seed++) {
                const auto x = flarmScene(rate, 0.12 * N + 0.3, train(u0, N, 0xDD1234, 3000), snr, 0, seed);
                const FlarmTelemetry h = decode(x, rate, u0, false), s = decode(x, rate, u0, true);
                printf("  %.1f Msps %4.1f dB seed %u: hard %3llu  chase %3llu of %d\n", rate / 1e6, snr, seed, (unsigned long long)h.blocksOk, (unsigned long long)s.blocksOk, N);
                CHECK(s.aircraftCount <= 1, "a made-up aircraft appeared (%u)", s.aircraftCount);
                hard += h.blocksOk; soft += s.blocksOk;
            }
        }
    }
    CHECK(soft > hard, "Chase decoding accepted no more than hard decisions: %llu vs %llu", (unsigned long long)soft, (unsigned long long)hard);
    {   // noise only: no packet, no aircraft
        const auto x = flarmScene(1e6, 3.0, {}, 10, 0, 9);
        const FlarmTelemetry t = decode(x, 1e6, u0, true);
        CHECK(t.blocksOk == 0 && t.aircraftCount == 0, "noise gave %llu packets", (unsigned long long)t.blocksOk);
    }
    if (kCalibrate) { printf("CALIBRATION run\n"); return 1; }
    return testSummary("flarm chase");
}
