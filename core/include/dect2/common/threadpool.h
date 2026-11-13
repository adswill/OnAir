// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The shared worker pool and the thread helpers of the receivers (see docs/threading.md).
//
//   ThreadPool::shared()   one pool for the whole process, created on first use, N = hardware_concurrency - 1 (1..8) workers, named "onair-wN"
//   parallelFor(n, fn)     runs fn(0..n-1) on the pool; the calling thread helps and returns when all n are done (safe from inside a pool task)
//   submit(fn) -> future   runs fn on a worker
//
// Scheduling: the shared pool's workers run at ThreadPriority::Decode (platform.h: macOS QoS user-initiated, so they stay on the performance
// cores; the big cores of a big.LITTLE Linux board), the background pool's at Background (the efficiency cores). One pool for the whole process,
// so many receivers share its N workers instead of each starting its own: no more busy threads than cores. Every job is charged to the CPU
// account of the thread that queued it (cpu_meter.h), so a receiver's parallel work shows in its own CPU figure.
//
// DECT2_THREADS in the environment: unset = automatic, a number = that many workers, 0 = no workers at all: parallelFor and submit run
// on the calling thread in index order (debugging, determinism checks). Every user of the pool must give the same output for any N:
// jobs write only their own slot, results are merged in index order by the caller.
#pragma once
#include "dect2/common/cpu_meter.h"
#include "dect2/common/thread_name.h"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace dect2 {

// DECT2_THREADS: -1 when unset, else the number (0 = single thread mode). Read once.
int configuredThreads();

// 0 when DECT2_THREADS=0, else true: stages (StagePipeline) and the pool use real threads
inline bool threadingEnabled() { return configuredThreads() != 0; }

class ThreadPool {
public:
    // workers: number of threads (0 = every call runs inline); prefix: thread name; background: lowest QoS (bulk work that must not disturb the receivers)
    explicit ThreadPool(int workers, const char* prefix = "onair-w", bool background = false);
    ~ThreadPool();   // finishes the queued tasks, joins every worker
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // The process-wide pool for decoding work (created once, lives to the end of the process)
    static ThreadPool& shared();
    // The process-wide pool for bulk background work (the LDPC decoders): same size, background priority
    static ThreadPool& background();

    int workers() const { return (int)threads_.size(); }

    // Runs fn(i) for every i in [0, n). The caller works too; returns when all are finished. fn must only touch the data of its own i.
    // Exceptions from fn are caught on the worker; the first one is rethrown here after all jobs ended.
    // maxHelpers: at most that many workers join the caller (a job that is cheap per index does not need to wake every worker)
    void parallelFor(size_t n, const std::function<void(size_t)>& fn, size_t maxHelpers);
    void parallelFor(size_t n, const std::function<void(size_t)>& fn);   // every worker may join

    // Runs fn on a worker (inline when there are none). The future carries the result or the exception.
    template <class F>
    auto submit(F&& fn) -> std::future<std::invoke_result_t<F>> {
        using R = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        std::future<R> fut = task->get_future();
        if (threads_.empty()) { (*task)(); return fut; }
        enqueue([task, acc = cpu::current()] { cpu::Scope s(acc); (*task)(); });   // the CPU counts for whoever queued it (cpu_meter.h)
        return fut;
    }

private:
    void enqueue(std::function<void()> job);
    void workerLoop(const std::string& name, bool background);
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;   // guarded by m_; producers: any thread; consumers: the workers
    bool stop_ = false;                     // guarded by m_
    std::vector<std::thread> threads_;      // joined by the destructor
};

} // namespace dect2
