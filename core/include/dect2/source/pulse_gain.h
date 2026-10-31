// Auto gain for pulsed message modes (ADS-B / Mode S), after readsb's adaptive gain: what counts is the number of good messages per second and
// the share of LOUD ones (a pulse peak within about 3 dB of full scale), not the headroom of a servo. A pulse is under 0.1 % of the samples, so the headroom
// of the 99.99 % point says nothing about it; a message does. More gain brings in the weak far aircraft until the near ones start to clip the
// front end: the loud share rises, the rate stops growing (clipped messages fail the CRC) and the climb turns back.
// PulseJudge keeps the counters of one gain setting (cumulative counters in, rate and share out); the controller restarts it at every gain change.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dect2::ag {

struct PulseJudge {
    // thresholds (readsb's defaults are of the same order: it lowers the gain when more than a few percent of the messages are loud)
    static constexpr double kLoudHigh = 0.10;      // above this share of loud messages (with enough messages) the gain goes down at once
    static constexpr double kLoudOk = 0.03;        // up to this share costs nothing; above it the objective is docked
    static constexpr double kPenaltyDbPerShare = 60.0;   // objective dB lost per 1.0 of loud share above kLoudOk (10 % loud: 4.2 dB)
    static constexpr double kMinSecs = 4.0;        // shortest window that gives a rate
    static constexpr double kMaxWaitSecs = 20.0;   // ... with few aircraft the window may take this long to reach kMinMsgs
    static constexpr double kMinMsgs = 30;         // messages for a rate that is worth a decision
    static constexpr double kLoudMinMsgs = 40;     // messages before the share is trusted for the hard "too loud" rule

    void clear() { *this = PulseJudge(); }
    // A new gain: the window starts here with the counters as they are
    void restart(double now, uint64_t msgs, uint64_t loud) { t0_ = now; m0_ = msgs; l0_ = loud; t_ = now; m_ = msgs; l_ = loud; on_ = true; }
    // The newest cumulative counters; a counter that went backwards (a reset of the receiver) restarts the window
    void update(double now, uint64_t msgs, uint64_t loud) {
        if (!on_ || msgs < m0_ || loud < l0_ || now < t0_) { restart(now, msgs, loud); return; }
        t_ = now; m_ = msgs; l_ = loud;
    }
    bool started() const { return on_; }
    double secs() const { return on_ ? t_ - t0_ : 0; }
    double msgs() const { return on_ ? (double)(m_ - m0_) : 0; }
    double loud() const { return on_ ? (double)(l_ - l0_) : 0; }
    double rate() const { return secs() > 0.2 ? msgs() / secs() : 0; }
    double loudShare() const { return msgs() > 0 ? loud() / msgs() : 0; }
    // enough of a window to decide on: kMinSecs and kMinMsgs, or kMaxWaitSecs of a quiet sky
    bool ready() const { return on_ && ((secs() >= kMinSecs && msgs() >= kMinMsgs) || secs() >= kMaxWaitSecs); }
    bool tooLoud() const { return on_ && secs() >= 2.0 && msgs() >= kLoudMinMsgs && loudShare() > kLoudHigh; }
    // The number the climb maximises, in dB: messages per second on a log scale, docked for a high share of loud messages
    double objective() const {
        return 10.0 * std::log10(rate() + 0.2) - kPenaltyDbPerShare * std::max(0.0, loudShare() - kLoudOk);
    }
private:
    bool on_ = false;
    double t0_ = 0, t_ = 0;
    uint64_t m0_ = 0, l0_ = 0, m_ = 0, l_ = 0;
};

} // namespace dect2::ag
