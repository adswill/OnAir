// Inmarsat all-carriers receiver in the engine's contract (configure / setSignalOffset / ready / reset / feed / telemetry): the carrier
// detector, the parallel Aero and Inmarsat-C decoders and the voice mixer (docs/INMARSAT-REVAMP.md). feed() runs on the analysis thread,
// the setters and telemetry() on the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct InmsatTelemetry;   // inmsat_tel.h
namespace inmsat { class VoiceMixer; }

class InmsatReceiver {
public:
    InmsatReceiver();
    ~InmsatReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);          // the radio's offset from the user's frequency (the capture centre is at freq + hz)
    bool ready() const;                       // false below 1.2 Msps
    void reset();                             // after a retune: carriers and windows go, the decoded tables stay
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(InmsatTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setCenterFrequency(double hz);       // absolute frequency of the capture centre (0 unknown): the carrier table shows absolute frequencies
    void setLocation(bool known, double latDeg, double lonDeg);   // the user's position (satellite identification)
    void setSatelliteOverride(const std::string& name);           // "" = automatic
    void setFocus(int carrierId);
    void setBudget(double units);             // CPU budget (Advanced); an Aero window costs 1
    void setInmarsatC(bool on);
    inmsat::VoiceMixer& voice();              // mute / solo / recording per call
    void setVolume(float v);                  // the mode's sound: the mixed C-channel voice goes to the default output (like the other voice modes)
    void setMuted(bool m);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning inmsatTuning();

} // namespace dect2
