// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared worker pool, thread naming and DECT2_THREADS (threadpool.h).
#include "dect2/common/threadpool.h"
#include "dect2/common/platform.h"
#include <algorithm>
#include <cstdlib>
#include <exception>
#ifndef _WIN32
#include <pthread.h>
#endif

namespace dect2 {

int configuredThreads() {
    static const int v = [] {
        const char* e = std::getenv("DECT2_THREADS");
        if (!e || !*e) return -1;
        char* end = nullptr;
        long n = std::strtol(e, &end, 10);
        if (end == e || n < 0) return -1;
        return (int)std::min(n, 64L);
    }();
    return v;
}

ThreadPool::ThreadPool(int workers, const char* prefix, bool background) {
    std::string base = prefix ? prefix : "onair-w";
    for (int i = 0; i < workers; i++) {
        std::string name = base + std::to_string(i);
        threads_.emplace_back([this, name, background] { workerLoop(name, background); });
    }
}

ThreadPool::~ThreadPool() {
    { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
}

void ThreadPool::workerLoop(const std::string& name, bool background) {
    setThreadName(name.c_str());
    setThreadPriority(background ? ThreadPriority::Background : ThreadPriority::Decode);
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
            if (q_.empty()) return;   // stop_ and drained
            job = std::move(q_.front());
            q_.pop_front();
        }
        job();
    }
}

void ThreadPool::enqueue(std::function<void()> job) {
    { std::lock_guard<std::mutex> lk(m_); q_.push_back(std::move(job)); }
    cv_.notify_one();
}

namespace {
int autoWorkers(bool decode) {
    const int c = configuredThreads();
    if (c >= 0) return c;
    int hw = (int)std::thread::hardware_concurrency();
#if defined(__linux__)
    if (decode && coreSets().hetero) hw = CPU_COUNT(&coreSets().big);   // the decode workers are held to the big cores (platform.h): no more of them than those
#endif
    return std::max(1, std::min(8, hw - 1));
}
} // namespace

ThreadPool& ThreadPool::shared() {
    static ThreadPool pool(autoWorkers(true), "onair-w", false);
    return pool;
}

ThreadPool& ThreadPool::background() {
    static ThreadPool pool(autoWorkers(false), "onair-bg", true);
    return pool;
}

void ThreadPool::parallelFor(size_t n, const std::function<void(size_t)>& fn) { parallelFor(n, fn, (size_t)-1); }

void ThreadPool::parallelFor(size_t n, const std::function<void(size_t)>& fn, size_t maxHelpers) {
    if (n == 0) return;
    if (n == 1 || threads_.empty() || maxHelpers == 0) { for (size_t i = 0; i < n; i++) fn(i); return; }
    // The loop state lives in a shared_ptr: a helper task that is still queued when the loop is done finds next >= n and returns at once.
    struct State {
        std::atomic<size_t> next{0};
        size_t n = 0;
        const std::function<void(size_t)>* fn = nullptr;
        std::mutex m;
        std::condition_variable cv;
        size_t done = 0;                 // jobs finished (guarded by m)
        std::exception_ptr err;          // first exception (guarded by m)
    };
    auto st = std::make_shared<State>();
    st->n = n;
    st->fn = &fn;
    const int acc = cpu::current();   // the helpers' time counts for the caller's account
    auto run = [st, acc] {
        cpu::Scope scope(acc);
        for (;;) {
            size_t i = st->next.fetch_add(1, std::memory_order_acq_rel);
            if (i >= st->n) return;
            std::exception_ptr e;
            try { (*st->fn)(i); } catch (...) { e = std::current_exception(); }
            if (acc) cpu::tick();   // charged before the caller can see the job as done
            bool last;
            {
                std::lock_guard<std::mutex> lk(st->m);
                if (e && !st->err) st->err = e;
                last = ++st->done == st->n;
            }
            if (last) st->cv.notify_all();   // only the caller waits, and only for the last one (no wake-up per index)
        }
    };
    const size_t helpers = std::min({n - 1, threads_.size(), maxHelpers});
    for (size_t h = 0; h < helpers; h++) enqueue(run);
    run();   // the caller helps; when it returns every index is claimed (not yet finished: a worker may still run its index)
    std::unique_lock<std::mutex> lk(st->m);
    st->cv.wait(lk, [&] { return st->done == st->n; });
    if (st->err) std::rethrow_exception(st->err);
}

} // namespace dect2
