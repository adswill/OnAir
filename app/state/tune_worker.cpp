// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// tune_worker.h: one tuning thread per engine, fed through a LatestSlot.
#include "tune_worker.h"
#include "dect2/common/latest_slot.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace tunework {
namespace {

struct Job { dect2::TuneSettings t; bool reset = false; };

struct Worker {
    dect2::Engine* e = nullptr;
    dect2::LatestSlot<Job> slot;
    std::atomic<bool> stop{false};
    std::thread th;
};

std::mutex gMu;                                         // the map of workers
std::map<const dect2::Engine*, std::unique_ptr<Worker>> gW;
std::mutex gStatMu;
Stats gStats;
double gCallMsSum = 0;

void run(Worker* w) {
    Job j;
    while (!w->stop.load()) {
        if (!w->slot.take(j, std::chrono::milliseconds(50))) continue;
        const auto t0 = std::chrono::steady_clock::now();
        if (w->e->running()) {
            if (j.reset) w->e->retuneReset(j.t); else w->e->retune(j.t);
            char b[120];
            snprintf(b, sizeof b, "retuned: radio centre %.6f MHz", j.t.centerHz / 1e6);
            w->e->log(b);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        {
            std::lock_guard<std::mutex> lk(gStatMu);
            gStats.done++; gCallMsSum += ms; if (ms > gStats.callMsMax) gStats.callMsMax = ms;
        }
        w->slot.done();
    }
}

Worker* workerFor(dect2::Engine& e) {
    std::lock_guard<std::mutex> lk(gMu);
    auto& p = gW[&e];
    if (!p) { p = std::make_unique<Worker>(); p->e = &e; p->th = std::thread(run, p.get()); }
    return p.get();
}

} // namespace

void post(dect2::Engine& e, const dect2::TuneSettings& t, bool reset) {
    const auto t0 = std::chrono::steady_clock::now();
    Worker* w = workerFor(e);
    Job j{t, reset};
    const bool replaced = w->slot.post(j);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::lock_guard<std::mutex> lk(gStatMu);
    gStats.posted++; if (replaced) gStats.coalesced++;
    if (ms > gStats.postMsMax) gStats.postMsMax = ms;
}

bool busy(const dect2::Engine& e) {
    std::lock_guard<std::mutex> lk(gMu);
    auto it = gW.find(&e);
    return it != gW.end() && !it->second->slot.waitIdle(std::chrono::milliseconds(0));
}

void flush(dect2::Engine& e, int maxMs) {
    Worker* w = nullptr;
    { std::lock_guard<std::mutex> lk(gMu); auto it = gW.find(&e); if (it != gW.end()) w = it->second.get(); }
    if (w) w->slot.waitIdle(std::chrono::milliseconds(maxMs));
}

void forget(dect2::Engine& e) {
    std::unique_ptr<Worker> w;
    { std::lock_guard<std::mutex> lk(gMu); auto it = gW.find(&e); if (it != gW.end()) { w = std::move(it->second); gW.erase(it); } }
    if (!w) return;
    w->stop = true; w->slot.close();
    if (w->th.joinable()) w->th.join();
}

void shutdown() {
    std::map<const dect2::Engine*, std::unique_ptr<Worker>> all;
    { std::lock_guard<std::mutex> lk(gMu); all.swap(gW); }
    for (auto& kv : all) { kv.second->stop = true; kv.second->slot.close(); }
    for (auto& kv : all) if (kv.second->th.joinable()) kv.second->th.join();
}

Stats stats() {
    std::lock_guard<std::mutex> lk(gStatMu);
    Stats s = gStats;
    s.callMsMean = s.done ? gCallMsSum / (double)s.done : 0;
    return s;
}

} // namespace tunework
