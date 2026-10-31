// Inmarsat-C receiver for the NCS common channel: 1200 symbol/s BPSK in frames of 8.64 s with EGC messages (SafetyNET, FleetNET).
// The channel at -tuneOffsetHz is decoded; a search of the whole capture band adds up to three more Inmarsat-C channels it finds (telemetry().channels).
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "inmc_tel.h"
#include "dect2/common/ring.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class InmcReceiver {
public:
    InmcReceiver();
    ~InmcReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the wanted channel sits relative to 0 Hz in the input
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal (the messages stay: clearResults)
    void setChannelSearch(bool on);                      // look for more channels in the band (default on); off drops the ones found
    void setSearchSpan(double hz);                       // that search stays within +-hz of 0 Hz (0 = the whole input, the default)
    void feed(const cf32* x, size_t n);
    bool telemetry(InmcTelemetry& out, uint64_t lastSeq);
    // The message list stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning inmcTuning();

} // namespace dect2
