// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tests of the shared threading framework: pool, parallelFor, bounded queue, re-order buffer, stage pipeline.
// Run normally, with DECT2_THREADS=0 (everything inline) and under -DDECT2_SANITIZE_THREAD=ON.
#include "dect2/common/queue.h"
#include "dect2/common/stage_pipeline.h"
#include "dect2/common/threadpool.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static uint64_t work(uint64_t x) { for (int i = 0; i < 200; i++) x = x * 6364136223846793005ull + 1442695040888963407ull; return x; }

static void testDeterminism() {
    std::mt19937_64 rng(1);
    std::vector<uint64_t> in(10000), seq(10000), par(10000);
    for (auto& v : in) v = rng();
    for (size_t i = 0; i < in.size(); i++) seq[i] = work(in[i]);
    ThreadPool::shared().parallelFor(in.size(), [&](size_t i) { par[i] = work(in[i]); });
    CHECK(seq == par);
    ThreadPool p(3);
    std::vector<std::future<uint64_t>> f;
    for (size_t i = 0; i < in.size(); i++) f.push_back(p.submit([&, i] { return work(in[i]); }));
    bool same = true;
    for (size_t i = 0; i < in.size(); i++) same &= f[i].get() == seq[i];
    CHECK(same);
    // nested parallelFor from inside a task must not deadlock
    std::vector<uint64_t> outer(8, 0);
    ThreadPool::shared().parallelFor(8, [&](size_t i) {
        std::vector<uint64_t> inner(50);
        ThreadPool::shared().parallelFor(50, [&](size_t j) { inner[j] = work(i * 100 + j); });
        uint64_t s = 0; for (auto v : inner) s += v;
        outer[i] = s;
    });
    for (size_t i = 0; i < 8; i++) { uint64_t s = 0; for (size_t j = 0; j < 50; j++) s += work(i * 100 + j); CHECK(outer[i] == s); }
    // exception travels to the caller
    bool thrown = false;
    try { ThreadPool::shared().parallelFor(20, [&](size_t i) { if (i == 7) throw 1; }); } catch (int) { thrown = true; }
    CHECK(thrown);
}

static void testReorder() {
    ReorderBuffer<int> rb;
    std::vector<int> out;
    rb.put(2, 20); CHECK(rb.take(out) == 0);
    rb.put(0, 0); CHECK(rb.take(out) == 1);
    rb.skip(1); CHECK(rb.take(out) == 1);   // 1 skipped, 2 released
    CHECK(out.size() == 2 && out[0] == 0 && out[1] == 20);
    rb.reset();
    rb.put(0, 5);
    out.clear(); CHECK(rb.take(out) == 1 && out[0] == 5);
    // parallel producers, in-order consumer
    ReorderBuffer<uint64_t> rb2;
    const size_t n = 5000;
    std::vector<uint64_t> got;
    std::atomic<size_t> next{0};
    ThreadPool p(4);
    for (int t = 0; t < 4; t++) p.submit([&] { for (size_t i; (i = next++) < n;) { if (i % 7 == 0) std::this_thread::yield(); rb2.put(i, work(i)); } });
    while (got.size() < n) { if (rb2.take(got) == 0) std::this_thread::yield(); }
    bool ok = true;
    for (size_t i = 0; i < n; i++) ok &= got[i] == work(i);
    CHECK(ok);
}

static void testQueue() {
    BoundedQueue<int> q(3, QueuePolicy::DropOldest);
    for (int i = 0; i < 5; i++) CHECK(q.push(int(i)));
    CHECK(q.dropped() == 2);
    int v = -1; CHECK(q.pop(v) && v == 2);
    // Block policy: producer waits until the consumer takes
    BoundedQueue<int> b(2, QueuePolicy::Block);
    std::atomic<int> pushed{0};
    std::thread prod([&] { for (int i = 0; i < 100; i++) { b.push(int(i)); pushed++; } });
    sleepMs(30);
    CHECK(pushed.load() <= 3);   // two queued, one waiting
    int sum = 0;
    for (int i = 0; i < 100; i++) { int x; CHECK(b.pop(x) && x == i); sum += x; }
    prod.join();
    CHECK(b.dropped() == 0);
    // close wakes a blocked consumer
    BoundedQueue<int> c(2);
    std::thread cons([&] { int x; CHECK(!c.pop(x)); });
    sleepMs(20); c.close(); cons.join();
    CHECK(!c.push(1));
}

static void testPipeline() {
    // order and flush
    std::vector<int> seen;
    {
        StagePipeline<int> sp("t-order", [&](int&& x) { seen.push_back(x); }, 16, QueuePolicy::Block);
        for (int i = 0; i < 1000; i++) sp.push(int(i));
        sp.flush();
        CHECK(seen.size() == 1000);
        bool ord = true; for (int i = 0; i < (int)seen.size(); i++) ord &= seen[i] == i;
        CHECK(ord);
    }
    // drop oldest when behind: the newest always arrives
    {
        std::atomic<int> last{-1}; std::atomic<int> n{0};
        StagePipeline<int> sp("t-drop", [&](int&& x) { sleepMs(1); last = x; n++; }, 2, QueuePolicy::DropOldest);
        for (int i = 0; i < 200; i++) sp.push(int(i));
        sp.flush();
        CHECK(last.load() == 199);
        CHECK(n.load() + (int)sp.dropped() == 200);
    }
    // reset in flight: nothing stale runs after reset returns, the handler is not running
    {
        std::atomic<bool> inHandler{false}; std::atomic<int> stale{0}; std::atomic<int> gen{0};
        StagePipeline<int> sp("t-reset", [&](int&& x) { inHandler = true; sleepMs(5); if (x != gen.load()) stale++; inHandler = false; }, 64, QueuePolicy::Block);
        for (int round = 0; round < 20; round++) {
            for (int i = 0; i < 30; i++) sp.push(int(gen.load()));
            sleepMs(round % 4);
            sp.reset();
            CHECK(!inHandler.load());
            gen++;   // a handler of the old generation still queued would count as stale
            sleepMs(1);
        }
        sp.flush();
        CHECK(stale.load() == 0);
    }
    // shutdown while busy and while the producer blocks: the destructor joins, no hang
    for (int k = 0; k < 20; k++) {
        std::atomic<int> n{0};
        std::thread* prod = nullptr;
        {
            StagePipeline<int> sp("t-busy", [&](int&&) { sleepMs(2); n++; }, 2, QueuePolicy::Block);
            prod = new std::thread([&] { for (int i = 0; i < 1000; i++) if (!sp.push(int(i))) break; });
            sleepMs(5 + k % 3);
            sp.stop();   // wakes the blocked producer, joins the stage
            prod->join();
        }
        delete prod;
        CHECK(n.load() < 1000);
    }
    // many start/stop cycles
    for (int k = 0; k < 300; k++) { StagePipeline<int> sp("t-cycle", [](int&&) {}, 4); sp.push(1); if (k % 2) sp.flush(); }
    // handler may call reset() on its own pipeline without a deadlock
    {
        StagePipeline<int>* sp = nullptr;
        std::atomic<int> n{0};
        StagePipeline<int> s("t-self", [&](int&&) { sp->reset(); n++; }, 8);
        sp = &s;
        for (int i = 0; i < 10; i++) s.push(int(i));
        s.flush();
        CHECK(n.load() >= 1);
    }
    // inline mode gives the same results
    {
        std::vector<int> a;
        StagePipeline<int> sp("t-inline", [&](int&& x) { a.push_back(x); }, 4, QueuePolicy::Block, false);
        CHECK(!sp.threaded());
        for (int i = 0; i < 10; i++) sp.push(int(i));
        CHECK(a.size() == 10);
    }
}

static void testStress() {
    // many tasks, many parallelFor callers at once, pools created and destroyed while busy
    std::atomic<uint64_t> total{0};
    std::vector<std::thread> callers;
    for (int c = 0; c < 6; c++) callers.emplace_back([&] {
        for (int r = 0; r < 200; r++) ThreadPool::shared().parallelFor(37, [&](size_t i) { total += work(i) & 1; });
    });
    for (auto& t : callers) t.join();
    uint64_t one = 0; for (size_t i = 0; i < 37; i++) one += work(i) & 1;
    CHECK(total.load() == one * 6 * 200);
    for (int k = 0; k < 50; k++) {
        ThreadPool p(1 + k % 4);
        std::atomic<int> n{0};
        for (int i = 0; i < 100; i++) p.submit([&] { sleepMs(0); n++; });
        if (k % 2) { p.parallelFor(20, [&](size_t) { n++; }); }
        // the destructor finishes the queued tasks
    }
}

// cpu_meter.h: a scope charges its block, the pool charges its jobs to the account of the thread that queued them
static void spin(double secs) {
    const uint64_t t0 = cpu::threadNs();
    volatile double x = 1;
    while ((double)(cpu::threadNs() - t0) * 1e-9 < secs) x = x * 1.0000001 + 1e-9;
}
static void testCpuMeter() {
    const int a = cpu::account("test a"), b = cpu::account("test b");
    CHECK(a > 0 && b > 0 && a != b && cpu::account("test a") == a && cpu::name(b) == "test b");
    {
        cpu::Scope s(a);
        CHECK(cpu::current() == a);
        spin(0.05);
        {
            cpu::Scope s2(b);
            spin(0.03);
        }
        CHECK(cpu::current() == a);
    }
    CHECK(cpu::current() == 0);
    const double sa = cpu::seconds(a), sb = cpu::seconds(b);
    CHECK(sa >= 0.045 && sa < 0.5);
    CHECK(sb >= 0.025 && sb < 0.5);
    const int c = cpu::account("test pool");
    {
        cpu::Scope s(c);
        ThreadPool::shared().parallelFor(4, [](size_t) { spin(0.02); });
    }
    CHECK(cpu::seconds(c) >= 0.075);   // all four jobs, wherever they ran
    CHECK(cpu::processPercent() >= 0);
}

int main() {
    printf("DECT2_THREADS=%d pool workers=%d\n", configuredThreads(), ThreadPool::shared().workers());
    testCpuMeter();
    testDeterminism();
    testReorder();
    testQueue();
    testPipeline();
    testStress();
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
