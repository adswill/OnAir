// Pagers: POCSAG and FLEX paging messages on one 25 kHz channel.
// Skeleton: counts the samples, measures the input level and reports four times a second of signal (state 0: no decoder yet); the
// mode's worker replaces the body. Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis
// thread (never blocks), telemetry() and the setters from the interface thread.
#pragma once
#include "mode_tuning.h"
#include "pager_tel.h"
#include "ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class PagerReceiver {
public:
    PagerReceiver();
    ~PagerReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the user's frequency sits relative to 0 Hz in the input
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal and the decoded state
    void feed(const cf32* x, size_t n);
    bool telemetry(PagerTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning pagerTuning();

} // namespace dect2
