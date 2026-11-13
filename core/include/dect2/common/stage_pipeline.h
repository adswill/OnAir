// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "demod -> decode": a second thread that runs the heavy decode stage of a receiver, fed through a bounded queue (one mutex guards the queue and the busy flag, so flush/reset see a consistent state) (see docs/threading.md).
//
//   producer thread (the receiver's feed(), the analysis thread)  --push(item)-->  BoundedQueue  --> stage thread  --> handler(item)
//
// Who: one producer calls push/reset/flush; the stage thread is the only caller of the handler; the destructor (stop()) joins the stage.
// Full queue: DropOldest drops the oldest unprocessed item (a live source: the analysis thread never waits), Block waits (files, benchmarks).
// reset(): drops everything queued and waits until the handler that is running (if any) has returned, so after it returns no stale item
//   runs and the owner may clear the state the handler touches (a retune). Called from inside the handler it only drops the queue.
// flush(): waits until the queue is empty and the handler idle (end of a file, tests, before reading results).
// stop(): closes, drops what is queued, joins. The destructor does it, so declare the pipeline LAST in the receiver: it is destroyed first
//   and no handler can run on a half-destroyed receiver.
// DECT2_THREADS=0 (or threads=false): no thread; push() runs the handler on the caller, in order. Same output, easy to debug.
#pragma once
#include "dect2/common/queue.h"
#include "dect2/common/threadpool.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace dect2 {

template <class Item>
class StagePipeline {
public:
    using Handler = std::function<void(Item&&)>;
    StagePipeline(std::string name, Handler h, size_t capacity = 8, QueuePolicy policy = QueuePolicy::DropOldest, bool useThread = true)
        : name_(std::move(name)), handler_(std::move(h)), cap_(capacity ? capacity : 1), policy_(policy), threaded_(useThread && threadingEnabled()) {
        if (threaded_) th_ = std::thread([this] { loop(); });
    }
    ~StagePipeline() { stop(); }
    StagePipeline(const StagePipeline&) = delete;
    StagePipeline& operator=(const StagePipeline&) = delete;

    bool threaded() const { return threaded_; }

    // false when stopped (the item is not taken)
    bool push(Item&& it) {
        if (!threaded_) {
            if (stopped_.load(std::memory_order_acquire)) return false;
            handler_(std::move(it));
            return true;
        }
        std::unique_lock<std::mutex> lk(m_);
        if (policy_ == QueuePolicy::Block) space_.wait(lk, [&] { return stop_ || q_.size() < cap_; });
        if (stop_) return false;
        if (q_.size() >= cap_) { q_.pop_front(); dropped_++; }
        q_.push_back(std::move(it));
        lk.unlock();
        work_.notify_one();
        return true;
    }
    // Drops queued items and waits for the running one; see the header
    void reset() {
        if (!threaded_) return;
        std::unique_lock<std::mutex> lk(m_);
        q_.clear();
        space_.notify_all();
        if (std::this_thread::get_id() == th_.get_id()) return;
        idle_.wait(lk, [&] { return !busy_; });
    }
    // Waits until everything pushed so far has been handled (or dropped)
    void flush() {
        if (!threaded_) return;
        std::unique_lock<std::mutex> lk(m_);
        idle_.wait(lk, [&] { return stop_ || (q_.empty() && !busy_); });
    }
    void stop() {
        if (stopped_.exchange(true, std::memory_order_acq_rel)) return;
        if (!threaded_) return;
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; q_.clear(); }
        work_.notify_all();
        space_.notify_all();
        idle_.notify_all();
        if (th_.joinable() && std::this_thread::get_id() != th_.get_id()) th_.join();
    }
    uint64_t dropped() const { std::lock_guard<std::mutex> lk(m_); return dropped_; }
    size_t pending() const { std::lock_guard<std::mutex> lk(m_); return q_.size() + (busy_ ? 1 : 0); }

private:
    void loop() {   // the stage thread: the only caller of handler_
        setThreadName(name_.c_str());
        for (;;) {
            Item it;
            {
                std::unique_lock<std::mutex> lk(m_);
                work_.wait(lk, [&] { return stop_ || !q_.empty(); });
                if (stop_) return;
                it = std::move(q_.front());
                q_.pop_front();
                busy_ = true;
            }
            space_.notify_one();
            handler_(std::move(it));
            { std::lock_guard<std::mutex> lk(m_); busy_ = false; }
            idle_.notify_all();
        }
    }

    std::string name_;
    Handler handler_;
    const size_t cap_;
    const QueuePolicy policy_;
    const bool threaded_;
    std::atomic<bool> stopped_{false};
    mutable std::mutex m_;
    std::condition_variable work_, space_, idle_;
    std::deque<Item> q_;     // guarded by m_
    bool busy_ = false;      // guarded by m_: the handler is running
    bool stop_ = false;      // guarded by m_
    uint64_t dropped_ = 0;   // guarded by m_
    std::thread th_;
};

} // namespace dect2
