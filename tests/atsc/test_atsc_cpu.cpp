// CPU budget of the two ATSC receivers: ATSC 1.0 (8-VSB, radio at 8 and 10 Msps) and ATSC 3.0 (radio at 10 Msps), each alone and both at once,
// from the app's own test signals (the synthetic source), fed as fast as the receivers take them. The figure is CPU seconds of the whole process
// (all threads) per second of signal: together the two must stay under one core on a developer machine (twice that on the shared CI runners, see
// ci_slack.h). The same runs check that nothing was lost to the speed: ATSC 1.0 delivers nearly every packet (one per data segment) with the
// sync byte and no error flag, and ATSC 3.0 run beside it delivers as many packets as it does alone.
// docs/ATSC-PERF-3.9.md has the per-stage figures (tests/bench/bench_dvb --case atsc,atsc-10m,atsc3).
#include "../common/cpu_report.h"
#include "../common/cpu_time.h"
#include "dect2/atsc/atsc_rx.h"
#include "dect2/atsc3/atsc3_rx.h"
#include "dect2/common/ring.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/source/source.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ATSC_CPU_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define ATSC_CPU_SANITIZED 1
#endif
#endif
#ifndef ATSC_CPU_SANITIZED
#define ATSC_CPU_SANITIZED 0
#endif

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static double processCpu() { return processCpuSecs(); }

// the app's test signal for a standard (3: ATSC 1.0, 5: ATSC 3.0) at `rate`, `secs` long, as 8-bit I/Q like a radio's
static bool generate(int stdMode, double rate, double secs, std::vector<cf32>& out) {
    setenv("DECT2_NOPACE", "1", 1);
    TuneSettings t;
    t.bandwidthMhz = 6;
    t.sampleRate = rate;
    t.centerHz = 600e6;
    if (const ModeTuning* m = modeTuning(stdMode)) t.centerHz = m->defMhz * 1e6;
    t.synth.mode = stdMode >= 4 ? stdMode : 0;
    t.synth.atsc = stdMode == 3;
    t.synth.snrDb = 30;
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "synthetic";
    auto src = makeSource(dev);
    IqRing ring(1u << 22);
    std::string err;
    if (!src || !src->start(t, ring, err)) { printf("synthetic source: %s\n", err.c_str()); return false; }
    const double r = src->sampleRate();
    ExactResampler rs;
    const bool resample = std::fabs(r - rate) > 1;
    if (resample) rs.configure(r, rate, 24);
    const size_t total = (size_t)(rate * secs);
    out.clear(); out.reserve(total);
    std::vector<cf32> buf(1 << 16), conv;
    auto last = std::chrono::steady_clock::now();
    while (out.size() < total) {
        const size_t n = ring.read(buf.data(), buf.size());
        if (!n) {
            if (std::chrono::steady_clock::now() - last > std::chrono::seconds(60)) { printf("no samples for 60 s\n"); src->stop(); return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        last = std::chrono::steady_clock::now();
        const cf32* p = buf.data();
        size_t m = n;
        if (resample) { conv.clear(); rs.process(buf.data(), n, conv); p = conv.data(); m = conv.size(); }
        for (size_t i = 0; i < m && out.size() < total; i++) {   // 8-bit, as a radio delivers it
            auto q = [](float v) { return std::lround(std::min(127.f, std::max(-128.f, v * 128.f))) / 128.f; };
            out.push_back(cf32(q(p[i].real()), q(p[i].imag())));
        }
    }
    src->stop();
    return true;
}

struct Counts { std::atomic<uint64_t> packets{0}, bad{0}; };

// feeds `sig` in radio-sized blocks; returns when the receiver has taken everything (both receivers block instead of dropping)
static void feedAtsc1(const std::vector<cf32>& sig, double rate, Counts& c) {
    AtscReceiver rx;
    rx.configure(rate);
    rx.setBlocking(true);
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) {
        for (size_t i = 0; i < n; i++) { const uint8_t* q = p + i * 188; if (q[0] != 0x47 || (q[1] & 0x80)) c.bad++; }
        c.packets += n;
    });
    const size_t B = 1 << 16;
    for (size_t i = 0; i < sig.size(); i += B) rx.feed(sig.data() + i, std::min(B, sig.size() - i));
    rx.flush();
}

static void feedAtsc3(const std::vector<cf32>& sig, double rate, Counts& c) {
    Atsc3Rx rx;
    rx.setPaced(false);
    rx.configure(rate);
    rx.setBlocking(true);
    rx.setPacketCallback([&](const uint8_t*, size_t n, double) { c.packets += n; });
    const size_t B = 1 << 16;
    for (size_t i = 0; i < sig.size(); i += B) rx.feed(sig.data() + i, std::min(B, sig.size() - i));
    // the frames still queued: wait until the process is idle, then stop
    double prev = processCpu();
    for (int i = 0; i < 300; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const double now = processCpu();
        if (now - prev < 0.005) break;
        prev = now;
    }
    rx.stop();
}

// CPU per second of signal of one run, the process idle again at the end
template <class F> static double cpuOf(double secs, F f) {
    const double c0 = processCpu();
    f();
    double prev = processCpu();
    for (int i = 0; i < 100; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const double now = processCpu();
        if (now - prev < 0.002) break;
        prev = now;
    }
    return (processCpu() - c0) / secs;
}

int main(int argc, char** argv) {
    const double secs = argc > 1 ? atof(argv[1]) : 4.0;
    const bool checked = !ATSC_CPU_SANITIZED && !getenv("ATSC_CPU_NOLIMIT");
    const double slack = ciSlack();
    std::vector<cf32> a8, a10, a3;
    if (!generate(3, 8e6, secs, a8) || !generate(3, 10e6, secs, a10) || !generate(5, 10e6, secs, a3)) { printf("FAIL: no test signal\n"); return 1; }
    // one packet per data segment: 312 of every 313 segments of 832 symbols at 10.762 MBd; the first fields go to the lock
    const double perSec = 10.762238e6 / 832.0 * 312.0 / 313.0;

    Counts c8, c10, c3, c1t, c3t;
    const double u8 = cpuOf(secs, [&] { feedAtsc1(a8, 8e6, c8); });
    const double u10 = cpuOf(secs, [&] { feedAtsc1(a10, 10e6, c10); });
    const double u3 = cpuOf(secs, [&] { feedAtsc3(a3, 10e6, c3); });
    const double ub = cpuOf(secs, [&] {   // both at once, each fed by its own thread as two radios would
        std::thread t1([&] { feedAtsc1(a8, 8e6, c1t); });
        std::thread t3([&] { feedAtsc3(a3, 10e6, c3t); });
        t1.join(); t3.join();
    });
    printf("ATSC 1.0 8 Msps: %.3f CPU s per s, %llu packets (%llu bad)\n", u8, (unsigned long long)c8.packets.load(), (unsigned long long)c8.bad.load());
    printf("ATSC 1.0 10 Msps: %.3f CPU s per s, %llu packets (%llu bad)\n", u10, (unsigned long long)c10.packets.load(), (unsigned long long)c10.bad.load());
    printf("ATSC 3.0 10 Msps: %.3f CPU s per s, %llu packets\n", u3, (unsigned long long)c3.packets.load());
    printf("both (ATSC 1.0 8 Msps + ATSC 3.0 10 Msps): %.3f CPU s per s, %llu + %llu packets\n", ub, (unsigned long long)c1t.packets.load(), (unsigned long long)c3t.packets.load());

    // quality: no packet lost or damaged to the speed
    const double want = 0.9 * perSec * secs;
    CHECK((double)c8.packets >= want && c8.bad == 0, "ATSC 1.0 at 8 Msps: %llu packets (%llu bad), want at least %.0f and none bad", (unsigned long long)c8.packets.load(), (unsigned long long)c8.bad.load(), want);
    CHECK((double)c10.packets >= want && c10.bad == 0, "ATSC 1.0 at 10 Msps: %llu packets (%llu bad), want at least %.0f and none bad", (unsigned long long)c10.packets.load(), (unsigned long long)c10.bad.load(), want);
    CHECK((double)c1t.packets >= want && c1t.bad == 0, "ATSC 1.0 beside ATSC 3.0: %llu packets (%llu bad), want at least %.0f and none bad", (unsigned long long)c1t.packets.load(), (unsigned long long)c1t.bad.load(), want);
    CHECK(c3.packets > 0 && (double)c3t.packets >= 0.9 * (double)c3.packets, "ATSC 3.0 beside ATSC 1.0: %llu packets, alone %llu", (unsigned long long)c3t.packets.load(), (unsigned long long)c3.packets.load());

    // the budget: under one core for both together; each alone well under it
    const double l1 = 0.75 * slack, l3 = 0.5 * slack, lb = 1.0 * slack;
    cpuCal("atsc_cpu", "ATSC 1.0 " + cpuCalMsps(8e6), u8, "cpu-s/s", '<', l1, checked);
    cpuCal("atsc_cpu", "ATSC 1.0 " + cpuCalMsps(10e6), u10, "cpu-s/s", '<', l1, checked);
    cpuCal("atsc_cpu", "ATSC 3.0 " + cpuCalMsps(10e6), u3, "cpu-s/s", '<', l3, checked);
    cpuCal("atsc_cpu", "ATSC 1.0 + ATSC 3.0", ub, "cpu-s/s", '<', lb, checked);
    if (checked) {
        CHECK(u8 < l1, "ATSC 1.0 at 8 Msps: %.3f CPU s per s of signal, the limit is %.2f", u8, l1);
        CHECK(u10 < l1, "ATSC 1.0 at 10 Msps: %.3f CPU s per s of signal, the limit is %.2f", u10, l1);
        CHECK(u3 < l3, "ATSC 3.0 at 10 Msps: %.3f CPU s per s of signal, the limit is %.2f", u3, l3);
        CHECK(ub < lb, "both receivers: %.3f CPU s per s of signal, the limit is %.2f", ub, lb);
    }
    printf("%s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails ? 1 : 0;
}
