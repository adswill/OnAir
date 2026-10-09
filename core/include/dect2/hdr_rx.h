// HD Radio (NRSC-5, USA): the digital sidebands of hybrid FM and AM stations.
// Skeleton: counts the samples, measures the input level and reports four times a second of signal (state 0: no decoder yet); the
// mode's worker replaces the body. Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis
// thread (never blocks), telemetry() and the setters from the interface thread.
#pragma once
#include "mode_tuning.h"
#include "hdr_tel.h"
#include "ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class HdrReceiver {
public:
    HdrReceiver();
    ~HdrReceiver();

    void configure(double inputRateHz);
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal and the decoded state
    void feed(const cf32* x, size_t n);
    bool telemetry(HdrTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning hdrTuning();

} // namespace dect2
