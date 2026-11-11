// Hardware timestamps of a radio that is disciplined by GPS (a PPS input or a GPSDO): a source publishes "sample srcIndex of this stream was taken
// at utcNs", the engine counts how many source samples it has passed on, and a receiver turns the two into time marks on its own sample clock
// (Lightning position, ltpos_rx.h). Nothing is published unless the radio has a PPS-type time source selected, so other modes never notice.
// One entry, keyed by the engine's ring: a process has one radio streaming hardware time at a time.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

namespace dect2::hwtime {

struct Mark {
    uint64_t srcIndex = 0;   // sample count of the stream, from its start
    int64_t utcNs = 0;       // when that sample was taken
    double srcRate = 0;
    uint64_t seq = 0;        // changes with each mark
};

class Board {
public:
    void arm(const void* key) { std::lock_guard<std::mutex> l(m_); mark_ = Mark(); seq_ = 0; consumed_.store(0, std::memory_order_relaxed); key_.store(key, std::memory_order_release); }
    void clear(const void* key) { std::lock_guard<std::mutex> l(m_); if (key_.load(std::memory_order_relaxed) == key) { key_.store(nullptr, std::memory_order_release); mark_ = Mark(); seq_ = 0; } }
    void publish(const void* key, uint64_t srcIndex, int64_t utcNs, double srcRate) {
        std::lock_guard<std::mutex> l(m_);
        if (key_.load(std::memory_order_relaxed) != key) return;
        mark_.srcIndex = srcIndex; mark_.utcNs = utcNs; mark_.srcRate = srcRate; mark_.seq = ++seq_;
    }
    // the engine's reading thread, one atomic load when nothing is armed
    void consumed(const void* key, uint64_t n) { if (key_.load(std::memory_order_acquire) == key) consumed_.fetch_add(n, std::memory_order_relaxed); }
    bool latest(const void* key, Mark& out, uint64_t& consumedTotal) {
        std::lock_guard<std::mutex> l(m_);
        if (key_.load(std::memory_order_relaxed) != key || mark_.seq == 0) return false;
        out = mark_;
        consumedTotal = consumed_.load(std::memory_order_relaxed);
        return true;
    }
private:
    std::mutex m_;
    std::atomic<const void*> key_{nullptr};
    std::atomic<uint64_t> consumed_{0};
    Mark mark_;
    uint64_t seq_ = 0;
};

inline Board& board() { static Board b; return b; }

// A radio's time register, set to zero at a PPS edge, counts seconds from that edge: the fraction is UTC's, the whole seconds come from the computer's clock (good to far under half a second)
inline int64_t ppsTimeToUtcNs(int64_t hwNs, int64_t systemNs) {
    const int64_t d = systemNs - hwNs;
    const int64_t whole = (d >= 0 ? (d + 500000000LL) / 1000000000LL : -((-d + 500000000LL) / 1000000000LL)) * 1000000000LL;
    return hwNs + whole;
}

} // namespace dect2::hwtime
