// Per-stage CPU accounting for the receivers (the DVB family first): where does a second of signal go?
//
//   DECT2_STAGE("dvbt.fft");      // at the top of a block: the CPU time this thread spends until the end of the block goes to "dvbt.fft"
//   DECT2_STAGE_VAR(sc, "dvbt.chanest"); ... sc.stop();   // the same, ended early by hand
//
// Off by default: a scope then costs one relaxed atomic load. stageProfEnable(true) (tests/bench/bench_dvb.cpp, or DECT2_STAGE_PROF=1 in the
// environment) turns the accounting on: each scope reads the thread's own CPU clock at both ends, so the figures are CPU seconds, not wall
// time, and stay right on a busy machine; work a stage hands to other threads is counted where those threads wrap it in the same name.
// Scopes nest: a stage's time is its own, without the stages opened inside it (exclusive time), so the figures of one run add up. Scopes end
// in the reverse order they began (stop() ends the innermost one early). Sites are static objects that link themselves into one list on first
// use; stageProfSnapshot() reads them all.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#include <time.h>

namespace dect2::prof {

struct Stage {
    const char* name;
    std::atomic<uint64_t> ns{0}, calls{0};
    Stage* next = nullptr;
    explicit Stage(const char* n);
};

inline std::atomic<Stage*>& stageList() { static std::atomic<Stage*> head{nullptr}; return head; }
inline std::atomic<int>& stageFlag() {
    static std::atomic<int> f{[] { const char* e = getenv("DECT2_STAGE_PROF"); return e && *e && *e != '0' ? 1 : 0; }()};
    return f;
}
inline Stage::Stage(const char* n) : name(n) {
    Stage* h = stageList().load(std::memory_order_relaxed);
    do next = h; while (!stageList().compare_exchange_weak(h, this, std::memory_order_release, std::memory_order_relaxed));
}
inline bool stageProfEnabled() { return stageFlag().load(std::memory_order_relaxed) != 0; }
inline void stageProfEnable(bool on) { stageFlag().store(on ? 1 : 0); }
inline uint64_t threadCpuNs() {
#if defined(CLOCK_THREAD_CPUTIME_ID) && !defined(_WIN32)
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#else   // no per-thread CPU clock (Windows): wall time, only meaningful on an idle machine
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
// every stage seen so far: name, CPU seconds, calls (the same name at two sites is summed)
inline std::vector<std::pair<std::string, std::pair<double, uint64_t>>> stageProfSnapshot() {
    std::vector<std::pair<std::string, std::pair<double, uint64_t>>> v;
    for (Stage* s = stageList().load(std::memory_order_acquire); s; s = s->next) {
        const double sec = (double)s->ns.load(std::memory_order_relaxed) * 1e-9;
        const uint64_t c = s->calls.load(std::memory_order_relaxed);
        bool merged = false;
        for (auto& e : v) if (e.first == s->name) { e.second.first += sec; e.second.second += c; merged = true; break; }
        if (!merged) v.push_back({s->name, {sec, c}});
    }
    return v;
}
inline void stageProfReset() {
    for (Stage* s = stageList().load(std::memory_order_acquire); s; s = s->next) { s->ns.store(0); s->calls.store(0); }
}

class Scope {
public:
    explicit Scope(Stage& s) : s_(stageProfEnabled() ? &s : nullptr) {
        if (!s_) return;
        const uint64_t now = threadCpuNs();
        parent_ = top();
        if (parent_) parent_->acc_ += now - parent_->t0_;   // the enclosing stage pauses
        top() = this;
        t0_ = now;
    }
    ~Scope() { stop(); }
    void stop() {   // ends the scope early (the innermost one)
        if (!s_) return;
        const uint64_t now = threadCpuNs();
        s_->ns.fetch_add(acc_ + (now - t0_), std::memory_order_relaxed);
        s_->calls.fetch_add(1, std::memory_order_relaxed);
        s_ = nullptr;
        top() = parent_;
        if (parent_) parent_->t0_ = now;                    // and goes on
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    static Scope*& top() { static thread_local Scope* t = nullptr; return t; }
    Stage* s_;
    Scope* parent_ = nullptr;
    uint64_t t0_ = 0, acc_ = 0;
};

} // namespace dect2::prof

#define DECT2_STAGE_CAT2(a, b) a##b
#define DECT2_STAGE_CAT(a, b) DECT2_STAGE_CAT2(a, b)
#define DECT2_STAGE(name)                                                         \
    static ::dect2::prof::Stage DECT2_STAGE_CAT(dect2StageSite_, __LINE__){name}; \
    ::dect2::prof::Scope DECT2_STAGE_CAT(dect2StageScope_, __LINE__) { DECT2_STAGE_CAT(dect2StageSite_, __LINE__) }
#define DECT2_STAGE_VAR(var, name)                                                \
    static ::dect2::prof::Stage DECT2_STAGE_CAT(dect2StageSite_, __LINE__){name}; \
    ::dect2::prof::Scope var { DECT2_STAGE_CAT(dect2StageSite_, __LINE__) }
