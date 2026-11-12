// Why the input ring overflowed: samples the radio had delivered, thrown away because the sample thread did not read them in time.
// 3.9 called every overflow "this computer is too slow", and fast machines with the CPU nearly idle saw it (a network radio's reconnect
// was counted as an overflow too, and a one-off hold-up of the sample thread looked the same as a receiver that cannot keep up). The
// engine (engine_loops.cpp) keeps about a second of the sample thread's own figures and, at each overflow, asks classify() what it was:
//   Starting  in the first seconds after a start or a retune (tables and plans are built on the first blocks): once is normal
//   Cpu       the receiver needs more than real time and is on the CPU for it: one core is full (other cores may idle: the receiver
//             of most modes is one thread). The only case where "lower the sample rate or the bandwidth" is the right advice
//   Waiting   the receiver needs more than real time but is mostly not on the CPU: it waits for its worker threads, a lock, or the
//             operating system gives its core to other programs (power saving, a busy machine)
//   Stalled   the receiver itself keeps up easily, but the sample thread was held up outside it for a moment (or the radio delivered a
//             burst after a pause): not the CPU
// Pure logic, no engine: tests/engine/test_overflow_cause.cpp.
#pragma once
#include <cstdint>

namespace dect2 {

enum class OverflowCause { None = 0, Starting, Cpu, Waiting, Stalled };

struct OverflowWindow {
    double wallSec = 0;        // wall time the window covers
    double signalSec = 0;      // signal time the sample thread processed in it
    double busyWallSec = 0;    // wall time inside the receive path (ingest)
    double busyCpuSec = 0;     // the sample thread's CPU time inside it
    double sinceStartSec = 1e9;   // seconds since the receiver started or was reset
};

inline const char* overflowCauseName(OverflowCause c) {
    switch (c) {
    case OverflowCause::Starting: return "starting";
    case OverflowCause::Cpu: return "cpu";
    case OverflowCause::Waiting: return "waiting";
    case OverflowCause::Stalled: return "stalled";
    default: return "none";
    }
}

constexpr double kOverflowStartSec = 5.0;   // overflows this soon after a start or reset are the start's own
constexpr double kBehindLoad = 0.9;         // the receive path needs at least this share of real time: it is the limit
constexpr double kOnCpu = 0.6;              // of that time at least this share on the CPU: the CPU is the limit

inline OverflowCause classifyOverflow(const OverflowWindow& w) {
    if (w.sinceStartSec < kOverflowStartSec) return OverflowCause::Starting;
    if (w.wallSec <= 0) return OverflowCause::Stalled;
    // the receive path's time against the signal it handled (however little: right after a hold-up the window is one short pass of one
    // block); with no signal handled at all, against the window
    const double load = w.busyWallSec / (w.signalSec > 0 ? w.signalSec : w.wallSec);
    if (load >= kBehindLoad) return w.busyWallSec > 0 && w.busyCpuSec / w.busyWallSec >= kOnCpu ? OverflowCause::Cpu : OverflowCause::Waiting;
    return OverflowCause::Stalled;
}

// The sample thread's running totals, one mark per pass of its loop (about 30 a second), and the window over the last two seconds.
struct LoopMark { int64_t ns = 0; double rx = 0, rxCpu = 0, samp = 0; };   // steady ns; seconds in the receive path (wall, CPU); samples processed

class LoopMarks {
public:
    static constexpr unsigned kN = 64;
    void clear() { n_ = 0; }
    void push(int64_t ns, double rx, double rxCpu, double samp) { LoopMark& m = m_[n_++ % kN]; m.ns = ns; m.rx = rx; m.rxCpu = rxCpu; m.samp = samp; }
    // from the newest mark back to the oldest one within spanSec, and at least to the one before it (a pass that took longer than the
    // span: a hold-up inside or outside the receiver, which the window must contain to tell which)
    OverflowWindow window(double rate, int64_t startNs, int64_t now, double spanSec = 2.0) const {
        OverflowWindow w;
        w.sinceStartSec = (double)(now - startNs) * 1e-9;
        if (!n_) return w;
        const unsigned n = n_ < kN ? n_ : kN;
        const LoopMark& cur = m_[(n_ - 1) % kN];
        const LoopMark* old = &cur;
        for (unsigned k = 1; k < n; k++) {
            const LoopMark& m = m_[(n_ - 1 - k) % kN];
            if (k > 1 && (double)(cur.ns - m.ns) * 1e-9 > spanSec) break;
            old = &m;
        }
        w.wallSec = (double)(cur.ns - old->ns) * 1e-9;
        w.signalSec = (cur.samp - old->samp) / (rate > 1 ? rate : 1);
        w.busyWallSec = cur.rx - old->rx;
        w.busyCpuSec = cur.rxCpu - old->rxCpu;
        return w;
    }

private:
    LoopMark m_[kN];
    unsigned n_ = 0;
};

}  // namespace dect2
