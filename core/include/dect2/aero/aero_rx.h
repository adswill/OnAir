// Inmarsat Aero receiver: finds the P channels in the band (up to 6), demodulates 600, 1200 and 10500 bit/s (rate found on its own),
// decodes frames and hands the SUs to the SU layer (aero_su.h) for logons and ACARS messages.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "aero_tel.h"
#include "aero_demod.h"
#include "dect2/common/ring.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class AeroReceiver {
public:
    AeroReceiver();
    ~AeroReceiver();

    void configure(double inputRateHz);
    void setSearchLimit(double hz);                      // 0 = whole input; else the band search only looks within +-hz of 0 (the Inmarsat window's pass-band: the rest of its input is empty)
    void setSignalOffset(double hz);                     // where the wanted channel sits relative to 0 Hz in the input
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal (the decoded tables stay: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(AeroTelemetry& out, uint64_t lastSeq);
    // The logons, messages, aircraft and SU type counts stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread
    // every decoded frame with the carrier it came from (offset from the user's frequency); for the tool and the tests
    void setFrameCallback(std::function<void(double offsetHz, const AeroFrameEvent&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning aeroTuning();

} // namespace dect2
