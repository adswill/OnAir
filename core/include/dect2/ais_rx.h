// AIS receiver. Placeholder: counts samples and reports "searching"; the mode owner replaces the body.
#pragma once
#include "mode_tuning.h"
#include "ais_phy.h"
#include "ais_tel.h"
#include "ring.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class AisReceiver {
public:
    AisReceiver();
    ~AisReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the wanted channel sits relative to 0 Hz in the input
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal and the decoded state
    void feed(const cf32* x, size_t n);
    bool telemetry(AisTelemetry& out, uint64_t lastSeq);
    void setPhyConfig(const AisPhyConfig& c);               // before configure(); the defaults are the tuned values
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning aisTuning();

} // namespace dect2
