// A receiver that is a little too slow must lose whole stretches of signal, not scatter tiny gaps over every frame.
// A producer replays a generated DVB-T2 signal (16K extended, GI 1/4) in real time into a ring; the consumer feeds the receiver and is slowed
// down to 92% of real time. A frame carries data only if none of its samples were lost, so the test counts frames that arrived intact.
// With the old behaviour (the producer drops what does not fit) a quarter or more are ruined; with dropBacklog() almost none.
#include "dect2/t2gen.h"
#include "dect2/t2rx.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
using namespace dect2;
using Clock = std::chrono::steady_clock;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Result { uint64_t intact = 0, frames = 0; double lostSec = 0, busy = 0; };

static Result run(const std::vector<std::vector<cf32>>& frames, size_t frameLen, double secs, double consumerSpeed, bool catchUp) {
    const double fn = 64e6 / 7;
    IqRing ring(1u << 23); // 0.92 s (the real one is 1.8 s): overload shows up within seconds
    T2Receiver rx;
    rx.configure(fn, 8);
    std::atomic<bool> stop{false};
    // what the producer managed to write, so that positions in the ring can be turned back into positions in the signal
    struct Chunk { uint64_t stream, ringPos, accepted; };
    std::vector<Chunk> chunks;
    std::vector<std::pair<uint64_t, uint64_t>> lost;   // lost stretches of the signal
    std::mutex mu;
    std::thread prod([&] {
        const size_t chunk = 1 << 16;
        size_t fi = 0, pos = 0, sent = 0;
        uint64_t ringPos = 0;
        const auto t0 = Clock::now();
        std::vector<cf32> buf(chunk);
        while (!stop) {
            for (size_t i = 0; i < chunk; i++) {
                buf[i] = frames[fi][pos];
                if (++pos == frames[fi].size()) { pos = 0; fi = (fi + 1) % frames.size(); }
            }
            const size_t m = ring.write(buf.data(), chunk);
            {
                std::lock_guard<std::mutex> lk(mu);
                chunks.push_back({sent, ringPos, m});
                if (m < chunk) lost.push_back({sent + m, sent + chunk});
            }
            ringPos += m;
            sent += chunk;
            std::this_thread::sleep_until(t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(sent / fn)));
        }
    });
    std::vector<std::pair<uint64_t, uint64_t>> skips;   // ring positions the consumer threw away
    double busy = 0;
    uint64_t consumed = 0;   // ring positions read or skipped
    uint64_t fed = 0;        // samples given to the receiver: what the slow-down schedule is based on
    const auto t0 = Clock::now();
    std::vector<cf32> buf(1 << 16);
    while (std::chrono::duration<double>(Clock::now() - t0).count() < secs) {
        if (catchUp) if (const size_t skipped = ring.dropBacklog()) { skips.push_back({consumed, consumed + skipped}); consumed += skipped; rx.markGap(skipped); }
        const size_t n = ring.read(buf.data(), buf.size());
        if (!n) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        const auto a = Clock::now();
        rx.feed(buf.data(), n);
        busy += std::chrono::duration<double>(Clock::now() - a).count();
        consumed += n;
        fed += n;
        // slow the consumer down to `consumerSpeed` times real time, on an absolute schedule so that sleep overshoot does not add up
        std::this_thread::sleep_until(t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(fed / (fn * consumerSpeed))));
    }
    stop = true;
    prod.join();
    // ring positions skipped by the consumer -> stretches of the signal
    for (auto& s : skips)
        for (auto& c : chunks) {
            const uint64_t a = std::max(s.first, c.ringPos), b = std::min(s.second, c.ringPos + c.accepted);
            if (a < b) lost.push_back({c.stream + (a - c.ringPos), c.stream + (b - c.ringPos)});
        }
    // frames up to the end of what the consumer reached
    const uint64_t end = chunks.empty() ? 0 : chunks.back().stream;
    Result r;
    uint64_t lostTotal = 0;
    for (auto& l : lost) lostTotal += l.second - l.first;
    for (uint64_t f = 0; (f + 1) * frameLen <= end; f++) {
        r.frames++;
        bool hit = false;
        for (auto& l : lost) if (l.first < (f + 1) * frameLen && l.second > f * frameLen) { hit = true; break; }
        if (!hit) r.intact++;
    }
    r.lostSec = lostTotal / fn;
    r.busy = busy / secs;
    return r;
}

int main() {
    TxParams tp;
    tp.s2field1 = 4; tp.ext = true; tp.giIdx = 3; tp.dataSymbols = 0;
    T2Generator gen(tp);
    std::vector<std::vector<cf32>> frames(12);
    for (auto& f : frames) gen.nextFrame(f);
    const size_t frameLen = gen.frameLength();
    const double secs = 30;
    Result fast = run(frames, frameLen, secs, 1.0e9, false);   // consumer as fast as it can: the reference
    printf("fast consumer:                    %3llu of %3llu frames intact, lost %5.2f s, receiver busy %.0f%%\n", (unsigned long long)fast.intact, (unsigned long long)fast.frames, fast.lostSec, 100 * fast.busy);
    Result slowOld = run(frames, frameLen, secs, 0.92, false);
    printf("92%% speed, drops scattered:       %3llu of %3llu frames intact, lost %5.2f s\n", (unsigned long long)slowOld.intact, (unsigned long long)slowOld.frames, slowOld.lostSec);
    Result slowNew = run(frames, frameLen, secs, 0.92, true);
    printf("92%% speed, backlog dropped:       %3llu of %3llu frames intact, lost %5.2f s\n", (unsigned long long)slowNew.intact, (unsigned long long)slowNew.frames, slowNew.lostSec);
    CHECK(fast.intact == fast.frames, "the fast consumer must lose nothing");
    CHECK(slowNew.intact > 0.85 * slowNew.frames, "dropping the backlog should keep most frames intact, got %llu of %llu", (unsigned long long)slowNew.intact, (unsigned long long)slowNew.frames);
    CHECK(slowNew.intact > slowOld.intact + slowOld.frames / 10, "dropping the backlog (%llu) should clearly beat scattered drops (%llu)", (unsigned long long)slowNew.intact, (unsigned long long)slowOld.intact);
    printf(fails ? "overload test FAILED\n" : "overload test passed\n");
    return fails ? 1 : 0;
}
