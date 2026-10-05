// Single-producer / single-consumer lock-free ring of complex float samples.
#pragma once
#include <atomic>
#include <complex>
#include <cstddef>
#include <vector>
#include <cstdint>

namespace dect2 {

using cf32 = std::complex<float>;

class IqRing {
public:
    explicit IqRing(size_t capacityPow2 = 1u << 24) : buf_(capacityPow2), mask_(capacityPow2 - 1) {}

    // Producer. Returns number written; samples that do not fit are counted as dropped.
    size_t write(const cf32* src, size_t n) {
        size_t w = w_.load(std::memory_order_relaxed);
        size_t r = r_.load(std::memory_order_acquire);
        size_t freeN = buf_.size() - (w - r);
        size_t m = n < freeN ? n : freeN;
        for (size_t i = 0; i < m; i++) buf_[(w + i) & mask_] = src[i];
        w_.store(w + m, std::memory_order_release);
        if (m < n) dropped_.fetch_add(n - m, std::memory_order_relaxed);
        return m;
    }

    size_t capacity() const { return buf_.size(); }

    // Consumer.
    size_t read(cf32* dst, size_t n) {
        size_t r = r_.load(std::memory_order_relaxed);
        size_t w = w_.load(std::memory_order_acquire);
        size_t avail = w - r;
        size_t m = n < avail ? n : avail;
        for (size_t i = 0; i < m; i++) dst[i] = buf_[(r + i) & mask_];
        r_.store(r + m, std::memory_order_release);
        return m;
    }

    // Consumer. When the consumer is too slow the ring fills and the producer can only write the few samples that happen to be free,
    // which spreads tiny gaps over every frame and nothing decodes. Throwing the whole backlog away in one go leaves one clean gap and
    // intact signal in between. Keeps the newest `keep` samples and returns how many were discarded (counted as dropped).
    size_t skipToNewest(size_t keep) {
        size_t r = r_.load(std::memory_order_relaxed);
        size_t w = w_.load(std::memory_order_acquire);
        size_t avail = w - r;
        if (avail <= keep) return 0;
        size_t skip = avail - keep;
        r_.store(r + skip, std::memory_order_release);
        dropped_.fetch_add(skip, std::memory_order_relaxed);
        return skip;
    }

    // The policy built on skipToNewest(): when more than half of the ring is backlog, jump to the newest tenth of it.
    size_t dropBacklog() { return available() > buf_.size() / 2 ? skipToNewest(buf_.size() / 16) : 0; }

    size_t available() const { return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire); }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    void clear() { r_.store(w_.load(std::memory_order_acquire), std::memory_order_release); }

private:
    std::vector<cf32> buf_;
    size_t mask_;
    std::atomic<size_t> w_{0}, r_{0};
    std::atomic<uint64_t> dropped_{0};
};

} // namespace dect2
