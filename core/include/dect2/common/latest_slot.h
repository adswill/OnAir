// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A one-place mailbox where the newest value wins: the UI posts tuning requests as fast as the mouse makes them, a worker takes them
// when it is free, and every request that was overtaken before the worker got to it is dropped (coalesced), never queued.
// Tested in tests/dsp/test_latest_slot.cpp.
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace dect2 {

template <class T>
class LatestSlot {
public:
    // Store v as the pending value. True when it replaced one the worker had not taken yet (that one is dropped).
    bool post(const T& v) {
        std::lock_guard<std::mutex> lk(mu_);
        const bool replaced = full_;
        val_ = v; full_ = true; posted_++;
        if (replaced) coalesced_++;
        cv_.notify_one();
        return replaced;
    }
    // Take the pending value: waits up to `wait` for one. False when there is none (or close() was called).
    template <class Rep, class Per>
    bool take(T& out, std::chrono::duration<Rep, Per> wait) {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, wait, [&] { return full_ || closed_; }) || !full_) return false;
        out = val_; full_ = false; busy_ = true; taken_++;
        return true;
    }
    bool tryTake(T& out) { return take(out, std::chrono::milliseconds(0)); }
    // the worker finished what it took
    void done() { std::lock_guard<std::mutex> lk(mu_); busy_ = false; idle_.notify_all(); }
    // drop a pending value (a stop or a start makes it stale)
    void drop() { std::lock_guard<std::mutex> lk(mu_); if (full_) { full_ = false; coalesced_++; } }
    // wait until nothing is pending or being worked on (at most `wait`); true when idle
    template <class Rep, class Per>
    bool waitIdle(std::chrono::duration<Rep, Per> wait) {
        std::unique_lock<std::mutex> lk(mu_);
        return idle_.wait_for(lk, wait, [&] { return !full_ && !busy_; });
    }
    void close() { std::lock_guard<std::mutex> lk(mu_); closed_ = true; cv_.notify_all(); }
    bool pending() const { std::lock_guard<std::mutex> lk(mu_); return full_; }
    uint64_t posted() const { std::lock_guard<std::mutex> lk(mu_); return posted_; }
    uint64_t taken() const { std::lock_guard<std::mutex> lk(mu_); return taken_; }
    uint64_t coalesced() const { std::lock_guard<std::mutex> lk(mu_); return coalesced_; }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_, idle_;
    T val_{};
    bool full_ = false, busy_ = false, closed_ = false;
    uint64_t posted_ = 0, taken_ = 0, coalesced_ = 0;
};

} // namespace dect2
