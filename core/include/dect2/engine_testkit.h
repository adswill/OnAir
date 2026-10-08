// Helpers for the engine tests: let the synthetic source play as fast as the receiver takes samples, and count the signal instead of the wall clock.
// (A real radio cannot be hurried, so the source normally plays at the radio's speed. The tests that check what the receiver decodes, not that it
// keeps up in real time, do not need that: the source waits for a slow receiver, so no sample is lost however slow the machine is.)
#pragma once
#include "engine.h"
#include <chrono>
#include <cstdio>
#include <thread>

namespace dect2 {
namespace enginetest {

// Speed of the synthetic source relative to a radio. Fast, but not unlimited: when the receiver is never idle the engine publishes a report only about
// every 250 ms of wall time, which is pace x 0.25 s of signal. So the faster the source, the coarser the clock of the reports: a test that checks
// "locked within N seconds" picks a pace for which that step is well below N.
constexpr double kFastPace = 4.0;

// Seconds of signal the engine's analysis thread has processed since the engine was created (the engine's own diagnostics; the counter is not
// atomic, a torn read of a double does not happen on the machines OnAir runs on)
inline double signalSecs(const Engine& e) {
    double rx = 0, sig = 0;
    sscanf(e.loadProfile().c_str(), "analysis thread: receiver %lf s for %lf s of signal", &rx, &sig);
    return sig;
}

// Polls pred() every 2 ms until it is true or `timeoutSecs` of wall time have gone; returns pred() (a guard against a hang, not a clock the checks use)
template <class P>
bool waitFor(double timeoutSecs, P pred) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        if (pred()) return true;
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > timeoutSecs) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace enginetest
} // namespace dect2
