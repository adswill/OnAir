// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Bounded queue between threads, and the re-order buffer that makes parallel stages deterministic (see docs/threading.md).
//
// BoundedQueue<T>: any number of producers and consumers (one of each is the usual case). A mutex and two condition variables, so it is
// plain for ThreadSanitizer; the blocks it carries are large (milliseconds of samples), so a lock per block costs nothing.
//   full queue + Policy::DropOldest : push() removes the oldest item, counts it in dropped(), and never waits (a live radio path)
//   full queue + Policy::Block      : push() waits for room (a file / benchmark path: nothing may be lost)
//   close(): wakes everybody; push() returns false from then on, pop() drains what is left and then returns false
#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace dect2 {

enum class QueuePolicy { DropOldest, Block };

template <class T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity, QueuePolicy policy = QueuePolicy::DropOldest) : cap_(capacity ? capacity : 1), policy_(policy) {}

    // false when the queue is closed (the item is not taken)
    bool push(T&& v) {
        std::unique_lock<std::mutex> lk(m_);
        if (policy_ == QueuePolicy::Block) notFull_.wait(lk, [&] { return closed_ || q_.size() < cap_; });
        if (closed_) return false;
        if (q_.size() >= cap_) { q_.pop_front(); dropped_++; }
        q_.push_back(std::move(v));
        lk.unlock();
        notEmpty_.notify_one();
        return true;
    }
    // Waits for an item; false when closed and empty
    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(m_);
        notEmpty_.wait(lk, [&] { return closed_ || !q_.empty(); });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        lk.unlock();
        notFull_.notify_one();
        return true;
    }
    bool tryPop(T& out) {
        std::unique_lock<std::mutex> lk(m_);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        lk.unlock();
        notFull_.notify_one();
        return true;
    }
    // Drops everything queued (not what a consumer already took); returns how many
    size_t clear() {
        std::unique_lock<std::mutex> lk(m_);
        size_t n = q_.size();
        q_.clear();
        lk.unlock();
        notFull_.notify_all();
        return n;
    }
    void close() {
        { std::lock_guard<std::mutex> lk(m_); closed_ = true; }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }
    size_t size() const { std::lock_guard<std::mutex> lk(m_); return q_.size(); }
    uint64_t dropped() const { std::lock_guard<std::mutex> lk(m_); return dropped_; }

private:
    mutable std::mutex m_;
    std::condition_variable notEmpty_, notFull_;
    std::deque<T> q_;
    const size_t cap_;
    const QueuePolicy policy_;
    bool closed_ = false;
    uint64_t dropped_ = 0;
};

// Results of parallel jobs come back in any order; this hands them out in sequence order 0, 1, 2 ... (no gaps), so the output of a parallel
// stage is the same as that of a sequential one. put() from any thread; take() returns the next ready run of items, in order.
// skip(seq) marks a job that produced nothing (so later ones are not held back forever); reset() starts a new sequence at 0.
template <class T>
class ReorderBuffer {
public:
    void put(uint64_t seq, T&& v) {
        std::lock_guard<std::mutex> lk(m_);
        if (seq < next_) return;   // from before a reset
        pending_.emplace(seq, Slot{true, std::move(v)});
    }
    void skip(uint64_t seq) {
        std::lock_guard<std::mutex> lk(m_);
        if (seq < next_) return;
        pending_.emplace(seq, Slot{false, T()});
    }
    // Appends the items that are next in order to out; returns how many
    size_t take(std::vector<T>& out) {
        std::lock_guard<std::mutex> lk(m_);
        size_t n = 0;
        for (auto it = pending_.begin(); it != pending_.end() && it->first == next_;) {
            if (it->second.has) { out.push_back(std::move(it->second.v)); n++; }
            it = pending_.erase(it);
            next_++;
        }
        return n;
    }
    // Items waiting behind a gap
    size_t waiting() const { std::lock_guard<std::mutex> lk(m_); return pending_.size(); }
    void reset() { std::lock_guard<std::mutex> lk(m_); pending_.clear(); next_ = 0; }
    // Sequence numbers handed out by the producer side
    uint64_t nextSeq() { std::lock_guard<std::mutex> lk(m_); return issued_++; }

private:
    struct Slot { bool has; T v; };
    mutable std::mutex m_;
    std::map<uint64_t, Slot> pending_;
    uint64_t next_ = 0;
    uint64_t issued_ = 0;
};

} // namespace dect2
