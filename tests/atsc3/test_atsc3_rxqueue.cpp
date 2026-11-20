// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The live input queue of the ATSC 3.0 receiver (Atsc3Rx without blocking, as the engine runs it for a radio). The test signal is fed at
// its real rate; twice the decoder is made to fall behind by more than the queue holds (a stall: three seconds of samples arrive at
// once). Samples must be dropped, and the receiver must go on from the next whole frame: no frame cut by a gap reaches the decoder, no
// baseband packet is bad, and the transport stream carries on after each stall.
#include "atsc3_sim.h"
#include "dect2/atsc3/atsc3_rx.h"
#include "dect2/atsc3/atsc3_synth.h"
#include <chrono>
#include <cstdio>
#include <thread>

using namespace dect2;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main() {
    const double rate = 6.144e6, frameSec = 0.1025;
    SynthConfig cfg;
    cfg.snrDb = 25;
    auto synth = makeAtsc3Synth(cfg, rate);
    if (!synth) { printf("FAIL: no generator\n"); return 1; }
    Atsc3Rx rx;
    std::vector<uint8_t> ts;
    std::mutex tsMu;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) { std::lock_guard<std::mutex> lk(tsMu); ts.insert(ts.end(), p, p + n * 188); });
    rx.setBlocking(false);
    rx.setQueueSeconds(1.0);
    rx.configure(rate);
    auto tsSize = [&] { std::lock_guard<std::mutex> lk(tsMu); return ts.size(); };

    // signal seconds: 4 paced, stall of 3, 4 paced, stall of 3, 4 paced
    const double paced = 4.0, stall = 3.0;
    const size_t chunk = 65536;
    std::vector<cf32> buf;
    double fed = 0;
    const auto t0 = std::chrono::steady_clock::now();
    double ahead = 0;   // signal seconds fed ahead of the wall clock (the stalls)
    auto feedPaced = [&](double secs) {
        const size_t total = (size_t)(secs * rate);
        buf.resize(chunk);
        for (size_t done = 0; done < total; done += chunk) {
            const size_t n = std::min(chunk, total - done);
            synth->generate(buf.data(), n);
            fed += (double)n / rate;
            std::this_thread::sleep_until(t0 + std::chrono::duration<double>(fed - ahead));
            rx.feed(buf.data(), n);
        }
    };
    auto feedStall = [&](double secs) {
        // generated first, then handed over at once: the receiver sees what a decoder that stopped for `secs` would see
        std::vector<cf32> all((size_t)(secs * rate));
        synth->generate(all.data(), all.size());
        for (size_t off = 0; off < all.size(); off += chunk) rx.feed(all.data() + off, std::min(chunk, all.size() - off));
        fed += secs;
        ahead += secs;
    };
    feedPaced(paced);
    feedStall(stall);
    const size_t ts1 = tsSize();
    feedPaced(paced);
    const size_t ts1end = tsSize();
    feedStall(stall);
    const size_t ts2 = tsSize();
    feedPaced(paced);
    const size_t ts2end = tsSize();
    Atsc3Telemetry t;
    uint64_t seq = 0;
    long lastFrames = -1;
    for (int i = 0; i < 50; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (rx.telemetry(t, seq)) seq = t.seq;
        if (i > 3 && t.frames == lastFrames) break;
        lastFrames = t.frames;
    }
    rx.telemetry(t, 0);
    rx.stop();
    long video = 0, audio = 0;
    bool hevc = false, aac = false;
    const bool opened = sim::countTs(ts, video, audio, hevc, aac);
    const double kept = fed - t.droppedSec;
    printf("  %.1f s fed, %.2f s dropped in %ld blocks: frames %ld (%ld failed), bb %ld (%ld bad), TS %zu B (%zu in the %.0f s after the first stall, %zu after the second) %s: %ld video + %ld audio\n",
           fed, t.droppedSec, t.droppedBlocks, t.frames, t.framesFailed, t.bbPackets, t.bbBad, ts.size(), ts1end - ts1, paced, ts2end - ts2, opened ? "" : "(unreadable)", video, audio);
    CHECK(t.droppedBlocks > 0 && t.droppedSec > 2.0, "the stalls overflowed the queue");
    // each overflow of the 1 s queue drops 0.75 s: the two stalls take 4.5 s, a slow runner a few more overflows (5.3 s seen without a
    // GPU). Dropping most of the signal means the queue drops without need; the frame and video counts below follow what was kept.
    CHECK(t.droppedSec < 0.5 * fed, "most of the signal is kept");
    CHECK(t.framesFailed == 0, "no frame cut by a gap is decoded");
    CHECK(t.bbBad == 0, "every baseband packet is good");
    CHECK(t.frames >= (long)(kept / frameSec) - 6, "the frames after each gap are found");
    CHECK(ts1end - ts1 > 100000 && ts2end - ts2 > 100000, "the transport stream carries on after each stall");
    CHECK(opened && video >= (long)((kept - 6.0) * 25), "video frames");
    printf("atsc3 rx queue: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
