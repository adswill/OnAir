// VDL2 tables have no small wall: 300 aircraft and 90 ground stations all stay in the lists (they used to stop at 200 and 64), and the
// processing stays far below real time. The bursts are rendered in one second pieces so the clip never sits in memory whole.
#include "dect2/limits.h"
#include "dect2/vdl2/vdl2_gen.h"
#include "dect2/vdl2/vdl2_proto.h"
#include "dect2/vdl2/vdl2_rx.h"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <random>
#include "dect2/common/stable_random.h"
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const double rate = 1e6, center = 136.85e6, f = 136.975e6;
    const int nAir = 300, nGs = 90;
    std::vector<Vdl2Wave> waves;
    std::vector<Vdl2Frames> frames;
    for (int i = 0; i < nAir + nGs; i++) {
        Vdl2Frames fr;
        if (i < nAir) fr.push_back(vdl2Frame({0x10A000u + (uint32_t)(i % 7), 4, false}, {0x400000u + (uint32_t)i * 17, 1, false}, (uint8_t)(kVdl2CtrlXID | 0x10), vdl2XidAircraftLe(51.0 + 0.01 * i, 0.5, 30000, "EGLL")));
        else fr.push_back(vdl2Frame({0xFFFFFF, 7, false}, {0x10B000u + (uint32_t)(i - nAir) * 5, 4, false}, kVdl2CtrlXID, vdl2XidGsif(51.4, -0.4, "EGLL")));
        Vdl2Wave w = vdl2MakeWave(fr);
        w.start = (0.2 + 0.12 * i) * rate;
        w.carHz = f - center;
        w.amp = 0.08;
        w.phase = 0.7 + i;
        waves.push_back(w);
    }
    const double secs = 0.2 + 0.12 * (nAir + nGs) + 0.5;
    Vdl2Receiver rx;
    rx.configure(rate);
    rx.setCenterHz(center);
    std::mt19937 rng(4);
    dect2::normal_distribution<float> nz(0.f, 0.004f);
    const size_t chunk = (size_t)rate;
    std::vector<cf32> buf(chunk);
    const auto t0 = std::chrono::steady_clock::now();
    double rendering = 0;
    for (size_t at = 0; at < (size_t)(secs * rate); at += chunk) {
        for (auto& v : buf) v = cf32{nz(rng), nz(rng)};
        const auto r0 = std::chrono::steady_clock::now();
        for (const auto& w : waves) if (w.start + 0.2 * rate > (double)at && w.start < (double)(at + chunk)) vdl2Render(w, buf.data(), (int64_t)at, chunk, rate);
        rendering += std::chrono::duration<double>(std::chrono::steady_clock::now() - r0).count();
        for (size_t o = 0; o < chunk; o += 16384) rx.feed(buf.data() + o, std::min<size_t>(16384, chunk - o));
    }
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    Vdl2Telemetry t;
    const bool got = rx.telemetry(t, 0);
    printf("telemetry %d, frames ok %llu bad %llu\n", got, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    std::set<uint32_t> air, gs;
    for (const auto& a : t.aircraft) air.insert(a.icao);
    for (const auto& g : t.stations) if (g.addr >= 0x10B000u) gs.insert(g.addr);
    printf("%.1f s of signal: %zu aircraft, %zu ground stations of the %d + %d sent; receiver %.1f s (rendering %.1f s)\n", secs, air.size(), gs.size(), nAir, nGs, total - rendering, rendering);
    CHECK(air.size() >= 270, "aircraft kept: %zu of %d (the old wall was 200)", air.size(), nAir);
    CHECK(gs.size() >= 80, "ground stations kept: %zu of %d (the old wall was 64)", gs.size(), nGs);
    CHECK(t.aircraft.size() == air.size(), "no duplicates in the aircraft list");
    CHECK(total - rendering < secs * 0.9, "processing %.1f s for %.1f s of signal", total - rendering, secs);
    // the cost of one table update with a full table, measured without a signal: 10000 aircraft are bounded by the age and the safety cap
    // "remove after" (Settings > Decoding): with a 20 s limit, 30 s of quiet on the receiver's own sample clock empties both tables
    limits::setStale(limits::Family::Vdl2, 20);
    for (int i = 0; i < 30; i++) {
        for (auto& v : buf) v = cf32{nz(rng), nz(rng)};
        for (size_t o = 0; o < chunk; o += 16384) rx.feed(buf.data() + o, std::min<size_t>(16384, chunk - o));
    }
    Vdl2Telemetry t2;
    rx.telemetry(t2, 0);
    CHECK(t2.aircraft.empty() && t2.stations.empty(), "after 30 s with a 20 s limit: %zu aircraft, %zu stations left", t2.aircraft.size(), t2.stations.size());
    limits::setStale(limits::Family::Vdl2, -1);
    CHECK(limits::kAircraft >= 10000 && limits::kGroundStations >= 4096, "caps are large");
    return testSummary("vdl2_caps");
}
