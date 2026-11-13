// Exercise counter boundaries with continuous generated input. The test owns its receiver implementation so it can move the
// absolute timeline while preserving buffered samples, without adding a public testing API.
#include "dect2/dtmb/dtmb_defs.h"
#include "dect2/dtmb/dtmb_tel.h"
#include "dect2/dtmb/dtmb_tx.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/mode_tuning.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
// Include the standard-library dependencies first: only the DTMB classes below expose their counters to this test.
#define private public
#include "dect2/dtmb/dtmb_rx.h"
#include "dect2/dtmb/dtmb_front.h"
#include "dect2/dtmb/dtmb_gen.h"
#include "dect2/dtmb/dtmb_chain.h"
#undef private
#include "../../core/src/dtmb/dtmb_rx.cpp"

using namespace dect2;
using namespace dect2::dtmb;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void run(Header header, int carriers) {
    SignalConfig sc;
    sc.rate = 8e6; sc.snrDb = 30;
    sc.tx.header = header; sc.tx.carriers = carriers;
    sc.tx.phaseRotate = carriers != 1;
    sc.tx.profile.map = Mapping::Qam16; sc.tx.profile.rate = Rate::R06;
    Signal signal(sc, testPacketSource(17));
    DtmbReceiver rx;
    rx.configure(sc.rate);
    rx.setDecoderThreads(0);
    rx.setComputeMode(0);
    uint64_t packets = 0, bad = 0;
    uint32_t expected = UINT32_MAX;
    rx.setPacketCallback([&](const uint8_t* data, size_t count, double) {
        for (size_t i = 0; i < count; i++) {
            uint32_t seq = expected;
            if (checkTestPacket(data + 188 * i, 17, &seq)) packets++; else bad++;
            expected = seq + 1;
        }
    });
    std::vector<cf32> block(16384);
    auto feedFor = [&](double seconds) {
        const size_t total = (size_t)(seconds * sc.rate);
        for (size_t done = 0; done < total;) {
            const size_t n = std::min(block.size(), total - done);
            signal.generate(block.data(), n);
            rx.feed(block.data(), n);
            done += n;
        }
        rx.flush();
    };
    feedFor(0.8);
    CHECK(packets > 2000 && bad == 0, "initial lock: %llu packets, %llu bad", (unsigned long long)packets, (unsigned long long)bad);
    auto& s = *rx.p_;
    if (!s.chain) { CHECK(false, "no decoder before boundary"); return; }
    for (int64_t boundary : {int64_t(INT32_MAX), int64_t(UINT32_MAX)}) {
        const int64_t delta = boundary - 2500000 - s.produced();
        s.base += delta; s.S += delta; s.reportAt += delta;
        if (s.lastGoodAt >= 0) s.lastGoodAt += delta;
        s.front.dcCount_ = boundary - 65536;
        s.chain->frames_ = boundary - 64;
        // Preserve the PN phase while also taking the frame index across the boundary.
        const int sf = headerInfo(header).framesPerSuper;
        s.fnum += ((boundary - 64 - s.fnum) / sf) * sf;
        const int64_t genDelta = boundary - 2500000 - (int64_t)signal.tau_;
        signal.base_ += genDelta; signal.tau_ += (double)genDelta;
        signal.produced_ = boundary - 65536;
        const uint64_t before = packets;
        printf("  %s C=%d crossing %lld: %llu packets before\n", headerInfo(header).name, carriers, (long long)boundary, (unsigned long long)before);
        fflush(stdout);
        feedFor(0.8);
        CHECK(s.produced() > boundary && s.S > boundary && s.fnum > boundary && s.lastGoodAt > boundary,
              "receiver timeline did not cross %lld", (long long)boundary);
        CHECK((int64_t)s.front.dcCount_ > boundary && (int64_t)s.chain->frames_ > boundary,
              "input/frame counters did not cross %lld", (long long)boundary);
        CHECK(signal.tau_ > boundary && (int64_t)signal.produced_ > boundary,
              "generator timeline did not cross %lld", (long long)boundary);
        CHECK(packets > before + 2000 && bad == 0, "boundary: %llu -> %llu packets, %llu bad",
              (unsigned long long)before, (unsigned long long)packets, (unsigned long long)bad);
        const auto& tracker = s.body[carriers == 1 ? 1 : 0].tracker;
        CHECK(tracker.count >= 0 && tracker.count <= 30 && tracker.run >= 0 && tracker.run <= 3,
              "SI counters grew past their decision thresholds");
        printf("    %llu packets after, %llu bad\n", (unsigned long long)packets, (unsigned long long)bad);
    }
    // Reacquisition must also accept positions beyond 32 bits, not only the tracking loop.
    const uint64_t before = packets;
    s.loseLock("counter regression");
    expected = UINT32_MAX;   // packets during the acquisition gap are deliberately lost
    feedFor(0.8);
    CHECK(s.siOk && packets > before + 2000 && bad == 0, "reacquisition beyond 32 bits: %llu -> %llu packets, %llu bad",
          (unsigned long long)before, (unsigned long long)packets, (unsigned long long)bad);
}

int main() {
    run(Header::Pn595, 1);      // single-carrier body
    run(Header::Pn945, 3780);   // multi-carrier body with PN phase rotation
    printf(failures ? "dtmb_counters: %d FAILED\n" : "dtmb_counters: all passed\n", failures);
    return failures ? 1 : 0;
}
