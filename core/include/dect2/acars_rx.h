// ACARS receiver: every ACARS channel in the captured band at once (up to 8 carriers decoded at the same time).
#pragma once
#include "mode_tuning.h"
#include "acars_tel.h"
#include "ring.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

class AcarsReceiver {
public:
    AcarsReceiver();
    ~AcarsReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the radio's tuned frequency sits relative to 0 Hz in the input (0 for this mode)
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal and the decoded state
    void feed(const cf32* x, size_t n);
    bool telemetry(AcarsTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

    // UI and engine setters (any thread)
    void setCenterHz(double hz);                         // the frequency the radio is tuned to; channel frequencies and the 25 kHz grid are taken from it (default 131.5 MHz)
    void setChannels(const std::vector<double>& hz);     // watch exactly these channels (absolute Hz, e.g. 8.33 kHz ones); empty = find carriers on the 25 kHz grid
    void setThresholdDb(double db);                      // carrier-to-noise needed to start decoding a channel (default 8)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning acarsTuning();

} // namespace dect2
