// Whole-span TV, phase 1 regression (docs/WHOLESPAN-TV-DESIGN.md): the multi-channel TV path must deliver the same transport stream after the
// refactor into SpanTv + TvAdapter as before it. A fixed capture (two DVB-T2 channels, then two DVB-T channels) goes through MultiTv with a blocking
// feed; the first packets that reach the sink (the selected channel) are hashed and compared with the digest the code before the refactor gave.
// Set SPANTV_GOLDEN_PRINT=1 to print the digests without checking.
#include "dect2/engine/multitv.h"
#include "multitv_testkit.h"
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
using namespace dect2;
using namespace multitvtest;
#include "../common/test_util.h"

struct Digest { uint64_t h = 1469598103934665603ull; size_t packets = 0; int slot = -1000; };

template <class Mt = MultiTv, class Opt = MultiTvOptions>
static Digest run(const std::vector<ChanSpec>& ch, double rate, bool dvbt, double secs, size_t want) {
    Capture cap(ch, rate, 28);
    Mt mt;
    Opt o; o.dvbt = dvbt; o.spanRate = rate; o.widthMhz = 8; o.maxChannels = 3; o.autoGrid = true; o.blocking = true;
    CHECK(mt.configure(o), "configure");
    Digest d;
    std::mutex mu;
    mt.setSink([&](int slot, const uint8_t* pk, size_t n, double, const TsSnapshot&) {
        std::lock_guard<std::mutex> lk(mu);
        if (d.slot == -1000) d.slot = slot;
        for (size_t i = 0; i < n * 188 && d.packets < want; i++) {
            d.h = (d.h ^ pk[i]) * 1099511628211ull;
            if (i % 188 == 187) d.packets++;
        }
    });
    std::vector<cf32> blk;
    RxTelemetry t;
    for (size_t done = 0; done < (size_t)(rate * secs); done += 1 << 16) { cap.pull(1 << 16, blk); mt.feed(blk.data(), blk.size()); mt.poll(t); }
    mt.wait();
    for (int i = 0; i < 200; i++) { { std::lock_guard<std::mutex> lk(mu); if (d.packets >= want) break; } std::this_thread::sleep_for(std::chrono::milliseconds(25)); }
    mt.reset();
    return d;
}

int main() {
    const bool print = getenv("SPANTV_GOLDEN_PRINT") != nullptr;
    const Digest a = run({{false, -4e6, 8, 101, "Alpha TV", 0, 0, 0x1001, 1}, {false, 4e6, 8, 202, "Beta TV", -3, 3, 0x1002, 2}}, 20e6, false, 2.5, 1500);
    const Digest b = run({{true, -4e6, 8, 5, "Terra One", 0, 0, 0x1001, 1}, {true, 4e6, 8, 6, "Terra Two", -2, 0, 0x1002, 2}}, 20e6, true, 2.0, 1500);
    printf("DVB-T2: slot %d, %zu packets, digest %016llx\n", a.slot, a.packets, (unsigned long long)a.h);
    printf("DVB-T:  slot %d, %zu packets, digest %016llx\n", b.slot, b.packets, (unsigned long long)b.h);
    if (!print) {
        CHECK(a.packets == 1500 && a.h == 0x0ull, "DVB-T2 digest changed");
        CHECK(b.packets == 1500 && b.h == 0x0ull, "DVB-T digest changed");
    }
    return testSummary("spantv_golden");
}
