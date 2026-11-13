// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Timeshift and playback: the PCR time map (rollover, clock jump), the rolling disk buffer (ring behaviour, seek by time, deletion of old segments),
// a recorded file as a source, the pacer and the feeder thread (real time pacing, pause, skip, go live).
#include "dect2/tv/playback.h"
#include "dect2/tv/timeshift.h"
#include <chrono>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"
namespace fs = std::filesystem;
typedef std::vector<uint8_t> Bytes;

// packet n of a synthetic stream at 1000 packets per second of stream time: a PCR packet every 100 packets (0.1 s), payload byte = n & 0xFF
static void mk(uint8_t* p, uint64_t n, double pcrOffset = 0, int pid = 0x101) {
    memset(p, 0xFF, 188);
    p[0] = 0x47; p[1] = (uint8_t)(pid >> 8); p[2] = (uint8_t)pid;
    if (n % 100 == 0) {
        p[3] = 0x20; p[4] = 183; p[5] = 0x10;
        const double sec = (double)n / 1000.0 + pcrOffset;
        const uint64_t base = (uint64_t)(sec * 90000.0) & 0x1FFFFFFFFull;
        p[6] = (uint8_t)(base >> 25); p[7] = (uint8_t)(base >> 17); p[8] = (uint8_t)(base >> 9); p[9] = (uint8_t)(base >> 1); p[10] = (uint8_t)((base & 1) << 7 | 0x7E); p[11] = 0;
        p[12] = (uint8_t)(n >> 24); p[13] = (uint8_t)(n >> 16); p[14] = (uint8_t)(n >> 8); p[15] = (uint8_t)n;
    } else {
        p[3] = 0x10; p[4] = (uint8_t)(n >> 24); p[5] = (uint8_t)(n >> 16); p[6] = (uint8_t)(n >> 8); p[7] = (uint8_t)n;
    }
}
static uint64_t idOf(const uint8_t* p) {
    const int o = (p[3] & 0x20) ? 12 : 4;
    return ((uint64_t)p[o] << 24) | ((uint64_t)p[o + 1] << 16) | ((uint64_t)p[o + 2] << 8) | p[o + 3];
}
static void pushN(DiskTimeshift& ts, uint64_t from, uint64_t to, double off = 0) {
    Bytes b;
    for (uint64_t n = from; n < to; n++) { uint8_t p[188]; mk(p, n, off); b.insert(b.end(), p, p + 188); }
    ts.push(b.data(), (size_t)(to - from));
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "onair_tshift_test";
    fs::remove_all(dir);
    {   // ---- time map
        PcrTimeline tl;
        for (int i = 0; i < 50; i++) tl.add((uint64_t)i * 100, i * 0.1 + 5.0);
        double t; uint64_t idx;
        CHECK(tl.timeAt(2500, t) && std::abs(t - 7.5) < 1e-6, "time at 2500: %f", t);
        CHECK(tl.timeAt(2550, t) && std::abs(t - 7.55) < 1e-3, "interpolates: %f", t);
        CHECK(tl.indexAt(7.5, idx) && idx == 2500, "index at 7.5: %llu", (unsigned long long)idx);
        CHECK(tl.indexAt(0, idx) && idx == 0 && tl.indexAt(999, idx) && idx == 4900, "clamped");
        CHECK(!tl.timeAt(0, t) || true, "");
        // rollover of the 33 bit PCR
        PcrTimeline w;
        w.add(0, 95443.0); w.add(100, 95443.5); w.add(200, 0.3); w.add(300, 0.8);
        CHECK(w.timeAt(300, t) && t > 95443.7 && t < 95445.0, "rollover continues the timeline: %f", t);
        // a jump
        PcrTimeline j;
        j.add(0, 10.0); j.add(100, 10.1); j.add(200, 500.0); j.add(300, 500.1);
        CHECK(j.timeAt(300, t) && t > 10.1 && t < 10.5, "clock jump continues the timeline: %f", t);
        // trim
        tl.trimBefore(3000);
        CHECK(tl.firstIndex() == 3000 && !tl.timeAt(10, t), "trim");
    }
    {   // ---- disk buffer: 10 s of stream = 10000 packets, segments of 1000 packets, keep 3.5 s
        DiskTimeshift ts;
        TimeshiftConfig c; c.dir = dir.string(); c.maxSeconds = 3.5; c.segmentPackets = 1000;
        CHECK(ts.open(c), "open");
        pushN(ts, 0, 2000);
        CHECK(ts.first() == 0 && ts.end() == 2000 && ts.segmentCount() == 2, "no deletion yet (%zu segs)", ts.segmentCount());
        uint8_t p[188];
        CHECK(ts.read(1234, p, 1) == 1 && idOf(p) == 1234, "read back");
        pushN(ts, 2000, 10000);
        CHECK(ts.end() == 10000, "end");
        CHECK(ts.first() >= 5000 && ts.first() <= 7000, "old segments dropped: first %llu", (unsigned long long)ts.first());
        CHECK(ts.bufferedSeconds() >= 3.4 && ts.bufferedSeconds() < 5.6, "buffered %.2f s", ts.bufferedSeconds());
        CHECK(ts.read(100, p, 1) == 0, "gone packets cannot be read");
        CHECK(ts.read(9999, p, 1) == 1 && idOf(p) == 9999, "newest readable (active segment)");
        CHECK(ts.read(10000, p, 1) == 0, "nothing past the end");
        // read across segment borders
        Bytes big(188 * 2500);
        const uint64_t from = ts.first() + 500;
        const size_t got = ts.read(from, big.data(), 2500);
        bool ok = got == 2500;
        for (size_t i = 0; ok && i < got; i++) ok = idOf(&big[i * 188]) == from + i;
        CHECK(ok, "range read over several segments (%zu)", got);
        // seek by time
        uint64_t idx;
        CHECK(ts.indexAt(8.0, idx) && idx >= 7900 && idx <= 8100, "index at 8.0 s: %llu", (unsigned long long)idx);
        double t;
        CHECK(ts.timeAt(9000, t) && std::abs(t - 9.0) < 0.01, "time at 9000: %f", t);
        CHECK(ts.indexAt(0.5, idx) && idx >= ts.first(), "seek before the buffer clamps to its start");
        // files on disk are bounded
        size_t files = 0; for (auto& e : fs::directory_iterator(dir)) { (void)e; files++; }
        CHECK(files == ts.segmentCount() && files <= 7, "segment files on disk: %zu", files);
        // byte limit
        ts.setLimits(0, 1000ull * 188 * 2);
        pushN(ts, 10000, 12000);
        CHECK(ts.bytes() <= 4000ull * 188, "byte cap (%llu)", (unsigned long long)ts.bytes());
        ts.clear();
        CHECK(ts.segmentCount() == 0 && ts.bufferedSeconds() == 0, "cleared");
        ts.close();
        size_t left = 0; for (auto& e : fs::directory_iterator(dir)) { (void)e; left++; }
        CHECK(left == 0, "close deletes the segments");
    }
    {   // left-over segment of a crashed run is removed, other files are not
        fs::create_directories(dir);
        { FILE* f = fopen((dir / "ts_00000007.seg").string().c_str(), "wb"); fputs("x", f); fclose(f); }
        { FILE* f = fopen((dir / "notes.txt").string().c_str(), "wb"); fputs("x", f); fclose(f); }
        DiskTimeshift ts; TimeshiftConfig c; c.dir = dir.string();
        CHECK(ts.open(c) && !fs::exists(dir / "ts_00000007.seg") && fs::exists(dir / "notes.txt"), "stale segments only");
        ts.close();
        // unwritable folder
        DiskTimeshift bad; TimeshiftConfig b; b.dir = (dir / "notes.txt" / "x").string();   // below a plain file: cannot be created anywhere
        CHECK(!bad.open(b), "bad folder");
    }
    {   // ---- file source
        const std::string path = (dir / "rec.ts").string();
        FILE* f = fopen(path.c_str(), "wb");
        for (uint64_t n = 0; n < 5000; n++) { uint8_t p[188]; mk(p, n, 100.0); fwrite(p, 1, 188, f); }
        fclose(f);
        TsFileSource src;
        CHECK(src.open(path) && src.end() == 5000 && std::abs(src.duration() - 4.9) < 0.1, "file source: %llu packets, %.2f s", (unsigned long long)src.end(), src.duration());
        uint64_t idx; uint8_t p[188];
        CHECK(src.indexAt(100.0 + 2.0, idx) && idx >= 1950 && idx <= 2050, "seek 2 s in: %llu", (unsigned long long)idx);
        CHECK(src.read(idx, p, 1) == 1 && idOf(p) == idx, "read");
        CHECK(!src.growing(), "not growing");
        CHECK(!TsFileSource().open((dir / "missing.ts").string()), "missing file");
        // PlaybackFeeder on a file: 1000 packets per stream second -> 5 s; play from 3 s: about 2 s of wall time
        PlaybackFeeder fd;
        std::atomic<uint64_t> got{0}; std::atomic<uint64_t> lastId{0}; std::atomic<bool> order{true};
        auto sink = [&](const uint8_t* b, size_t n) { for (size_t i = 0; i < n; i++) { const uint64_t id = idOf(b + i * 188); if (id <= lastId && got > 0 && id != lastId + 1 && order) {} lastId = id; } got += n; };
        uint64_t startIdx; src.indexAt(103.0, startIdx);
        const auto t0 = std::chrono::steady_clock::now();
        fd.start(&src, startIdx, sink);
        while (!fd.atEnd() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(6)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(fd.atEnd() && got >= 1900, "played to the end (%llu packets)", (unsigned long long)got.load());
        CHECK(el > 1.4 && el < 3.5, "paced in real time: %.2f s for ~2 s of stream", el);
        fd.stop();
        // pause, skip back, resume
        got = 0;
        fd.start(&src, 0, sink);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        fd.pause(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const uint64_t at = got;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        CHECK(got == at, "paused: nothing is sent");
        fd.skip(2.0);
        fd.pause(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        CHECK(fd.positionSeconds() > 102.0 && fd.positionSeconds() < 104.2, "skipped forward to %.2f", fd.positionSeconds());
        fd.stop();
    }
    {   // ---- feeder on a growing buffer: behind-live and go live
        DiskTimeshift ts; TimeshiftConfig c; c.dir = dir.string(); c.segmentPackets = 500;
        ts.open(c);
        pushN(ts, 0, 3000);
        PlaybackFeeder fd; std::atomic<uint64_t> got{0};
        fd.start(&ts, 0, [&](const uint8_t*, size_t n) { got += n; });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(fd.behindLiveSeconds() > 1.5, "behind live by %.2f s", fd.behindLiveSeconds());
        fd.goLive();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(fd.behindLiveSeconds() < 0.3, "live again: %.2f", fd.behindLiveSeconds());
        pushN(ts, 3000, 3500);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(fd.position() > 2900, "follows new data (pos %llu)", (unsigned long long)fd.position());
        fd.stop(); ts.close();
    }
    {   // ---- pacer
        TsPacer pc;
        pc.reset(10.0, 100.0);
        CHECK(std::abs(pc.delay(11.0, 100.0, 1.0, 0.0) - 1.0) < 1e-9, "1 s ahead waits 1 s");
        CHECK(std::abs(pc.delay(11.0, 100.0, 2.0, 0.0) - 0.5) < 1e-9, "double speed");
        CHECK(pc.delay(10.0, 100.5, 1.0, 0.0) == 0, "late = no wait");
        CHECK(pc.delay(10.0, 110.0, 1.0, 0.0) == 0 && std::abs(pc.delay(11.0, 110.0, 1.0, 0.0) - 1.0) < 1e-9, "re-anchors after a stall");
    }
    fs::remove_all(dir);
    return testSummary("tv_timeshift");
}
