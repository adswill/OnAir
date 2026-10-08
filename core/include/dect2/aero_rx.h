// Inmarsat Aero receiver: finds the P channels in the band (up to 6), demodulates 600, 1200 and 10500 bit/s (rate found on its own),
// decodes frames and hands the SUs to the SU layer (aero_su.h) for logons and ACARS messages.
#pragma once
#include "mode_tuning.h"
#include "aero_tel.h"
#include "aero_demod.h"
#include "ring.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class AeroReceiver {
public:
    AeroReceiver();
    ~AeroReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the wanted channel sits relative to 0 Hz in the input
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal and the decoded state
    void feed(const cf32* x, size_t n);
    bool telemetry(AeroTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread
    // every decoded frame with the carrier it came from (offset from the user's frequency); for the tool and the tests
    void setFrameCallback(std::function<void(double offsetHz, const AeroFrameEvent&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning aeroTuning();

} // namespace dect2
