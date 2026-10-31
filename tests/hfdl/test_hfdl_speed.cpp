// HFDL receiver speed: 2 s of noise at the 16, 32 and 64 MHz capture rates, with every dial of the station table as the channel list.
// The wall-clock time per second of input is printed at every rate, pass or fail. The check fails only above real time on CI:
// 0.5 x ciSlack() is 1.0 on GitHub Actions (and 0.5 on a developer machine, where the receiver should run well under real time).
// Wide captures (above 16.5 MHz) use the shared FFT channeliser by the receiver's own rule; at 32 MHz the per-channel path is timed as well
// (printed, not limited) and the channeliser must be faster than it.
#include "dect2/hfdl/hfdl_rx.h"
#include "dect2/hfdl/hfdl_proto.h"
#include "../common/cpu_report.h"
#include "../common/cpu_time.h"
#include "../common/ci_slack.h"
#include <chrono>
#include <cstdio>
#include <ctime>
#include <set>
using namespace dect2;
#include "../common/test_util.h"

// mode: -1 the receiver's own rule, 0 the per-channel path, 1 the channeliser. Returns the wall time per second of input.
static double run(double rate, double secs, int mode = -1) {
    std::set<double> dials;
    for (const auto& s : hfdlStations()) for (int k : s.khz) dials.insert(k * 1e3);
    const std::vector<double> list(dials.begin(), dials.end());
    HfdlReceiver rx;
    rx.configure(rate);
    rx.setChanneliser(mode);
    rx.setChannels(list);
    // one block of noise, fed again and again (its generation is not timed)
    const size_t N = 65536;
    std::vector<cf32> buf(N);
    uint32_t s = 12345;
    auto uniform = [&s]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (float)s / 4294967296.0f - 0.5f; };
    for (auto& v : buf) v = cf32(0.2f * uniform(), 0.2f * uniform());
    const size_t chunks = (size_t)(secs * rate / (double)N);
    const double simSecs = (double)(chunks * N) / rate;
    const auto t0 = std::chrono::steady_clock::now();
    const double c0 = processCpuSecs();
    for (size_t i = 0; i < chunks; i++) rx.feed(buf.data(), N);
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / simSecs;
    const double cpu = (processCpuSecs() - c0) / simSecs;
    HfdlTelemetry t;
    rx.telemetry(t, ~0ull);
    int inBand = 0;
    for (const auto& c : t.channels) inBand += c.inBand ? 1 : 0;
    printf("HFDL %5.1f MHz capture, %2zu channels (%2d in band), %-11s: %.3f s wall per s of input (%.3f s CPU per s)\n",
           rate / 1e6, list.size(), inBand, mode == 0 ? "per-channel" : mode == 1 ? "channeliser" : "auto", wall, cpu);
    cpuCal("hfdl_speed", cpuCalMsps(rate) + (mode == 0 ? " per-channel" : mode == 1 ? " channeliser" : " auto"), wall, "wall-s/s", '<', 0.5 * ciSlack(), mode != 0);
    if (mode != 0) CHECK(wall < 0.5 * ciSlack(), "%.1f MHz capture: %.3f s per s of input, the limit is %.2f", rate / 1e6, wall, 0.5 * ciSlack());
    return wall;
}

int main() {
    run(16e6, 2);
    const double oldPath = run(32e6, 2, 0);
    const double newPath = run(32e6, 2, 1);
    printf("HFDL 32 MHz: the channeliser takes %.2f of the per-channel path's time\n", newPath / oldPath);
    CHECK(newPath < oldPath, "32 MHz: channeliser %.3f s per s, per-channel path %.3f", newPath, oldPath);
    run(32e6, 2);
    run(64e6, 2);
    return testSummary("hfdl_speed");
}
