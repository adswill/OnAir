// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// IqRing: the C++ ring against the Rust ring (rust/onair_ring). (1) differential: the same random operation sequences on both, every result
// and counter identical; (2) two-thread stress on both (order, no loss, no duplicate, offered = accepted + dropped); (3) with --bench, ns/op.
// Under -DDECT2_SANITIZE_THREAD=ON ThreadSanitizer sees the C++ ring fully; the Rust code is not instrumented (its orderings are argued in ring.rs).
#include "dect2/common/ring.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>

using namespace dect2;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static bool same(const cf32* a, const cf32* b, size_t n) { return n == 0 || std::memcmp(a, b, n * sizeof(cf32)) == 0; }

static void differential(size_t cap, uint64_t seed, int steps, uint64_t& mismatches, uint64_t& ops) {
    IqRingCpp a(cap);
    IqRingRs b(cap);
    std::mt19937_64 rng(seed);
    std::vector<cf32> src, da, db;
    float v = 1;
    auto below = [&](size_t n) { return (size_t)(rng() % n); };
    for (int i = 0; i < steps; i++) {
        ops++;
        switch (below(10)) {
        case 0: case 1: case 2: case 3: {
            size_t n = below(cap * 2 + 2);
            src.resize(n);
            for (auto& s : src) { s = cf32(v, -v); v += 1; }
            if (a.write(src.data(), n) != b.write(src.data(), n)) mismatches++;
            break; }
        case 4: case 5: case 6: {
            size_t n = below(cap * 2 + 2);
            da.assign(n, cf32(-1, -1)); db.assign(n, cf32(-1, -1));
            size_t ma = a.read(da.data(), n), mb = b.read(db.data(), n);
            if (ma != mb || !same(da.data(), db.data(), n)) mismatches++;   // also compares the untouched tail
            break; }
        case 7: { size_t k = below(cap + 2); if (a.skipToNewest(k) != b.skipToNewest(k)) mismatches++; break; }
        case 8:
            switch (below(4)) {
            case 0: if (a.dropBacklog() != b.dropBacklog()) mismatches++; break;
            case 1: a.clear(); b.clear(); break;
            case 2: a.noteLost(7); b.noteLost(7); break;
            default: a.resetDropped(); b.resetDropped(); break;
            }
            break;
        default: break;
        }
        if (a.available() != b.available() || a.dropped() != b.dropped() || a.capacity() != b.capacity()) mismatches++;
        if (a.lostAtSource() != b.lostAtSource() || a.lostAtSource() > a.dropped()) mismatches++;   // 4.0: source losses apart, a part of dropped
        uint64_t oa = 0, ob = 0; int64_t na = 0, nb = 0; size_t la = 0, lb = 0;
        bool ra = a.lastWrite(oa, na, la), rb = b.lastWrite(ob, nb, lb);
        // the two stamps use the clock at different instants: compare everything but the time, and the time only for "monotonic and recent"
        if (ra != rb || oa != ob || la != lb || (ra && (nb < na - 1000000000LL || nb > na + 1000000000LL))) mismatches++;
    }
}

template <class R>
static void stress(const char* name, size_t cap, uint64_t total, size_t maxChunk, bool block) {
    R ring(cap);
    std::atomic<bool> done{false};
    std::atomic<uint64_t> accepted{0};
    std::thread prod([&] {
        std::mt19937_64 rng(1);
        std::vector<cf32> buf(maxChunk);
        uint64_t sent = 0, acc = 0;
        while (sent < total) {
            size_t n = std::min<uint64_t>(1 + rng() % maxChunk, total - sent);
            for (size_t i = 0; i < n; i++) buf[i] = cf32((float)((sent + i) & 0xFFFFFF), (float)((sent + i) >> 24));
            size_t off = 0;
            for (;;) {
                size_t m = ring.write(buf.data() + off, n - off);
                acc += m; off += m;
                if (!block || off == n) break;
                std::this_thread::yield();
            }
            sent += n;
        }
        accepted = acc;
        done.store(true, std::memory_order_release);
    });
    uint64_t seen = 0, gaps = 0, bad = 0;
    bool have = false; uint64_t prev = 0;
    std::mt19937_64 rng(2);
    std::vector<cf32> buf(maxChunk);
    for (;;) {
        bool fin = done.load(std::memory_order_acquire);
        size_t m = ring.read(buf.data(), 1 + rng() % maxChunk);
        if (m == 0 && fin && ring.available() == 0) break;
        for (size_t i = 0; i < m; i++) {
            uint64_t x = (uint64_t)buf[i].real() + ((uint64_t)buf[i].imag() << 24);
            if (have && x <= prev) bad++;
            if (have && x != prev + 1) gaps++;
            prev = x; have = true; seen++;
        }
    }
    prod.join();
    printf("  stress %-4s cap %-6zu %s: %llu offered, %llu read, %llu dropped, %llu gaps\n", name, cap, block ? "blocking" : "lossy   ",
           (unsigned long long)total, (unsigned long long)seen, (unsigned long long)ring.dropped(), (unsigned long long)gaps);
    CHECK(bad == 0);
    CHECK(seen == accepted.load());
    if (!block) CHECK(accepted.load() + ring.dropped() == total);
    if (block) CHECK(accepted.load() == total && gaps == 0);
}

// ---- benchmark: ns per call and per sample, in one thread (write then read of the same block: the API cost) and across two threads
template <class R>
static double benchSingle(size_t batch, bool bulk, size_t iters) {
    R ring(1u << 20);
    std::vector<cf32> a(batch, cf32(1, 2)), b(batch);
    auto t0 = std::chrono::steady_clock::now();
    size_t sink = 0;
    for (size_t i = 0; i < iters; i++) {
        if (bulk || batch == 1) { sink += ring.write(a.data(), batch); sink += ring.read(b.data(), batch); }
    }
    auto t1 = std::chrono::steady_clock::now();
    if (sink == 1) printf(" ");
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / (double)iters / 2.0;   // per write or read call
}
template <class R>
static double benchThreads(size_t batch, size_t total) {
    R ring(1u << 20);
    std::atomic<bool> go{false};
    std::thread prod([&] {
        std::vector<cf32> a(batch, cf32(1, 2));
        while (!go.load(std::memory_order_acquire)) {}
        uint64_t sent = 0;
        while (sent < total) sent += ring.write(a.data(), batch) ? batch : (std::this_thread::yield(), 0);
    });
    std::vector<cf32> b(batch);
    auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    uint64_t got = 0;
    while (got < total) got += ring.read(b.data(), batch);
    auto t1 = std::chrono::steady_clock::now();
    prod.join();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / (double)(total / batch);   // per batch moved through
}

int main(int argc, char** argv) {
    bool bench = argc > 1 && std::strcmp(argv[1], "--bench") == 0;
    if (bench) {
        printf("%-10s %-12s %12s %12s   (ns per call, one thread: write+read same block)\n", "batch", "", "C++", "Rust");
        for (size_t batch : {(size_t)1, (size_t)4096, (size_t)65536}) {
            size_t iters = batch == 1 ? 20000000 : batch == 4096 ? 400000 : 30000;
            double c = 1e9, r = 1e9;
            for (int k = 0; k < 5; k++) { c = std::min(c, benchSingle<IqRingCpp>(batch, true, iters)); r = std::min(r, benchSingle<IqRingRs>(batch, true, iters)); }
            printf("%-10zu %-12s %12.1f %12.1f\n", batch, "per call", c, r);
        }
        printf("%-10s %-12s %12s %12s   (ns per batch handed over, producer and consumer threads)\n", "batch", "", "C++", "Rust");
        for (size_t batch : {(size_t)1, (size_t)4096, (size_t)65536}) {
            size_t total = batch == 1 ? 20000000 : batch == 4096 ? 400000000 : 400000000;
            double c = 1e9, r = 1e9;
            for (int k = 0; k < 3; k++) { c = std::min(c, benchThreads<IqRingCpp>(batch, total)); r = std::min(r, benchThreads<IqRingRs>(batch, total)); }
            printf("%-10zu %-12s %12.1f %12.1f\n", batch, "per batch", c, r);
        }
        return 0;
    }
    uint64_t mismatches = 0, ops = 0;
    for (size_t cap : {(size_t)1, (size_t)2, (size_t)4, (size_t)16, (size_t)64, (size_t)1024, (size_t)65536})
        for (uint64_t seed = 1; seed <= 60; seed++) differential(cap, seed * 104729 + cap, 1500, mismatches, ops);
    printf("  differential: %llu operations, %llu mismatches\n", (unsigned long long)ops, (unsigned long long)mismatches);
    CHECK(mismatches == 0);
    const bool tsan =
#if defined(__SANITIZE_THREAD__)
        true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
        true;
#else
        false;
#endif
#else
        false;
#endif
    const uint64_t n = tsan ? 400000 : 4000000;
    stress<IqRingCpp>("C++", 64, n, 17, true);
    stress<IqRingRs>("Rust", 64, n, 17, true);
    stress<IqRingCpp>("C++", 4096, n, 1000, true);
    stress<IqRingRs>("Rust", 4096, n, 1000, true);
    stress<IqRingCpp>("C++", 256, n, 200, false);
    stress<IqRingRs>("Rust", 256, n, 200, false);
    printf(fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
}
