// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Time machine": a rolling memory of the last seconds of the radio's raw IQ, so the signal from just BEFORE an event can be saved too.
// The analysis thread (Engine::ingest) only converts a block to 8-bit and copies it into a pre-allocated ring: no lock held while copying, no
// allocation, never waits (so the live path loses nothing). A trigger (button, squelch opening, an automation rule) names how much to keep
// before it and how much to wait after it; a worker thread then copies that window out of the ring and writes it as one .cs8 file with
// SigMF metadata. Memory only. The window is limited by a memory budget; "decimate" (boxcar average by 2, 4, 8, 16) stretches it.
// A retune throws the history away (it would be at another centre frequency).
#pragma once
#include "source.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace dect2 {

struct TimeMachineStats {
    bool enabled = false;
    double storedRateHz = 0;       // the ring's rate: the radio's rate divided by the decimation
    double windowSeconds = 0;      // how far back the ring reaches when full (after the memory budget)
    double filledSeconds = 0;      // history in the ring now
    uint64_t bytes = 0;            // memory the ring holds
    bool saving = false;           // a trigger is waiting for its "after" part, or the file is being written
    double savePct = 0;            // progress of the pending save, 0..100
    int saves = 0;                 // files written since the ring was set up
    std::string lastPath;          // the last file written
    double lastSeconds = 0;        // its length
    uint64_t lastMissing = 0;      // samples of the requested window that had already been overwritten (a very slow disk)
    std::string error;             // why the last save failed
};

class TimeMachine {
public:
    TimeMachine() = default;
    ~TimeMachine() { off(); }
    TimeMachine(const TimeMachine&) = delete;
    TimeMachine& operator=(const TimeMachine&) = delete;

    static constexpr uint64_t kMaxFileBytes = 4ull << 30;   // a saved file never gets bigger than this

    // UI thread. wantSeconds <= 0 (or rate <= 0) switches it off and frees the ring. decimate: 1, 2, 4, 8 or 16. memBudgetMB limits the ring;
    // the window is shortened to fit. Returns the window in seconds (0 when off).
    double configure(double wantSeconds, double radioRateHz, int decimate, int memBudgetMB);
    void off() { configure(0, 0, 1, 0); }
    bool enabled() const { return on_.load(std::memory_order_relaxed); }

    // Analysis thread: every block of the radio's samples, before anything touches them. Cheap when off.
    void push(const cf32* x, size_t n) {
        if (on_.load(std::memory_order_relaxed)) pushOn(x, n);
    }
    // The history is not contiguous with what comes next (a retune): forget it. Any thread.
    void discard();

    // Save the last preSeconds and the next postSeconds around now to path (.cs8; a .sigmf-meta is written beside it when sigmf is on).
    // Returns at once; false when off, a save is already running, there is not enough free disk space or the file would be too big.
    bool trigger(const std::string& path, double preSeconds, double postSeconds, double centerHz, bool sigmf, std::string& err);
    TimeMachineStats stats() const;
    double storedRate() const;   // 0 when off

    // Test hook: the file size that a window of this many stored samples makes
    static uint64_t fileBytes(uint64_t samples) { return samples * 2; }

private:
    struct Ring;
    void pushOn(const cf32* x, size_t n);
    void saveLoop(std::shared_ptr<Ring> r, std::string path, uint64_t startPos, uint64_t endPos, double centerHz, bool sigmf);
    std::shared_ptr<Ring> get() const;

    std::atomic<bool> on_{false};
    mutable std::mutex mu_;           // guards ring_ (the pointer only), worker_, and the result fields
    std::shared_ptr<Ring> ring_;
    std::thread worker_;
    std::atomic<bool> busy_{false};
    std::atomic<uint64_t> savePos_{0}, saveSpan_{0};   // progress of the pending save
    int saves_ = 0;
    std::string lastPath_, error_;
    double lastSeconds_ = 0;
    uint64_t lastMissing_ = 0;
};

}  // namespace dect2
