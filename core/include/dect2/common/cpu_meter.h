// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CPU accounting: how much processor time each receiver (and each part of it) uses, for the status-bar CPU meter.
//
//   cpu::account("rx1 decoder")   an account id (created on first use; the same name gives the same id). 0 = nothing (not counted)
//   cpu::bindThread(id)           everything the calling thread does from now on is charged to id (a receiver's own thread)
//   cpu::Scope s(id);             the CPU time of this block on this thread is charged to id instead (and not to the thread's binding)
//   cpu::current()                the account the calling thread charges to now; the shared pool (threadpool.h) runs every job under
//                                 the account of the thread that queued it, so a decoder's parallel work counts as the decoder's
//   cpu::percent(id)              that account's CPU in percent of one core (100 = one core busy), averaged over about the last second
//   cpu::processPercent()         the whole process, same unit (it also counts what no account has: the GUI, drivers, audio ...)
//   cpu::usage()                  every account that has used CPU: name and percent, in the order they were created
//
// The time is the thread's own CPU clock (CLOCK_THREAD_CPUTIME_ID; GetThreadTimes on Windows, which counts in scheduler ticks: right on
// average over many blocks), read when a scope begins and ends: no sampling thread, no timer. A thread charges only at those points, so
// a bound thread that never enters a scope shows its time when it next does; the engine's threads enter one for every block.
// Cost: two clock reads per scope (well under a microsecond each); scopes are per block of samples, never per sample.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#include <time.h>
#endif

namespace dect2 {
namespace cpu {

// CPU time of the calling thread, nanoseconds
inline uint64_t threadNs() {
#if defined(_WIN32)
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    const uint64_t kk = ((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime, uu = ((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (kk + uu) * 100;
#else
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

// CPU time of the whole process (all threads, user + system), nanoseconds
inline uint64_t processNs() {
#if defined(_WIN32)
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    const uint64_t kk = ((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime, uu = ((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (kk + uu) * 100;
#else
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    return ((uint64_t)r.ru_utime.tv_sec + (uint64_t)r.ru_stime.tv_sec) * 1000000000ull + ((uint64_t)r.ru_utime.tv_usec + (uint64_t)r.ru_stime.tv_usec) * 1000ull;
#endif
}

namespace detail {
constexpr int kMaxAccounts = 256;
struct Account {
    std::string name;
    std::atomic<uint64_t> ns{0};
    // percent(): the last reading and the value it gave (guarded by Registry::m)
    uint64_t lastNs = 0;
    int64_t lastWall = 0;
    double pct = 0;
};
struct Registry {
    std::mutex m;
    Account acc[kMaxAccounts];
    std::atomic<int> count{1};   // id 0 is "nothing"
    uint64_t procLast = 0;
    int64_t procWall = 0;
    double procPct = 0;
};
inline Registry& reg() { static Registry r; return r; }
struct ThreadState {
    int bound = 0;     // bindThread()
    int cur = 0;       // the account being charged now (a scope's, else bound)
    uint64_t last = 0; // the thread clock when charging to cur began
};
inline ThreadState& ts() { thread_local ThreadState s; return s; }
inline int64_t wallNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
// charge the time since the last switch to the current account and switch to `to`
inline void switchTo(int to) {
    ThreadState& s = ts();
    if (s.cur == to) return;
    const uint64_t now = (s.cur || to) ? threadNs() : 0;
    if (s.cur > 0 && s.cur < kMaxAccounts && now > s.last) reg().acc[s.cur].ns.fetch_add(now - s.last, std::memory_order_relaxed);
    s.cur = to; s.last = now;
}
// percent from a running total: re-read once at least 0.5 s have passed, else the last value
inline double rate(uint64_t ns, uint64_t& lastNs, int64_t& lastWall, double& pct) {
    const int64_t w = wallNs();
    if (lastWall == 0) { lastWall = w; lastNs = ns; return pct; }
    if (w - lastWall >= 500000000) {
        pct = 100.0 * (double)(ns - lastNs) / (double)(w - lastWall);
        lastWall = w; lastNs = ns;
    }
    return pct;
}
} // namespace detail

// The id of the account with this name (created on first use). Returns 0 once 255 accounts exist (then nothing more is counted).
inline int account(const std::string& name) {
    detail::Registry& r = detail::reg();
    std::lock_guard<std::mutex> lk(r.m);
    const int n = r.count.load(std::memory_order_relaxed);
    for (int i = 1; i < n; i++) if (r.acc[i].name == name) return i;
    if (n >= detail::kMaxAccounts) return 0;
    r.acc[n].name = name;
    r.count.store(n + 1, std::memory_order_release);
    return n;
}

inline int current() { return detail::ts().cur; }

inline std::string name(int id) {
    if (id <= 0 || id >= detail::kMaxAccounts) return {};
    std::lock_guard<std::mutex> lk(detail::reg().m);
    return detail::reg().acc[id].name;
}

// The calling thread's time goes to id from now on (outside scopes)
inline void bindThread(int id) {
    detail::ThreadState& s = detail::ts();
    s.bound = id;
    detail::switchTo(id);
}

// Brings the calling thread's charge up to date (a bound thread's loop calls it once per block, so its figure stays current)
inline void tick() {
    const int c = detail::ts().cur;
    if (c) { detail::switchTo(0); detail::switchTo(c); }
}

// Charges this block to id. Nested scopes charge the inner one; the outer resumes after it.
class Scope {
public:
    explicit Scope(int id) : prev_(detail::ts().cur) { detail::switchTo(id); }
    ~Scope() { detail::switchTo(prev_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    int prev_;
};

// Total CPU seconds charged to id so far (for tests and logs)
inline double seconds(int id) {
    if (id <= 0 || id >= detail::kMaxAccounts) return 0;
    detail::ThreadState& s = detail::ts();
    if (s.cur == id) { const int c = s.cur; detail::switchTo(0); detail::switchTo(c); }   // include the running part of this thread
    return (double)detail::reg().acc[id].ns.load(std::memory_order_relaxed) * 1e-9;
}

// percent of one core over about the last second (the first call returns 0 and starts the window)
inline double percent(int id) {
    if (id <= 0 || id >= detail::kMaxAccounts) return 0;
    detail::Registry& r = detail::reg();
    std::lock_guard<std::mutex> lk(r.m);
    detail::Account& a = r.acc[id];
    return detail::rate(a.ns.load(std::memory_order_relaxed), a.lastNs, a.lastWall, a.pct);
}

inline double processPercent() {
    detail::Registry& r = detail::reg();
    const uint64_t ns = processNs();
    std::lock_guard<std::mutex> lk(r.m);
    return detail::rate(ns, r.procLast, r.procWall, r.procPct);
}

struct Usage { std::string name; double percent = 0; double seconds = 0; };
inline std::vector<Usage> usage() {
    detail::Registry& r = detail::reg();
    std::vector<Usage> out;
    std::lock_guard<std::mutex> lk(r.m);
    const int n = r.count.load(std::memory_order_acquire);
    for (int i = 1; i < n; i++) {
        detail::Account& a = r.acc[i];
        const uint64_t ns = a.ns.load(std::memory_order_relaxed);
        if (!ns) continue;
        out.push_back({a.name, detail::rate(ns, a.lastNs, a.lastWall, a.pct), (double)ns * 1e-9});
    }
    return out;
}

} // namespace cpu
} // namespace dect2
