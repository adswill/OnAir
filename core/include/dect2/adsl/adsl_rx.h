// ADS-L receiver: EASA ADS-L 4 SRD-860 traffic, status and ground uplink on 868.2, 868.4 MHz (M-band) and 869.525 MHz (O-band LDR and HDR), see adsl_proto.h.
// One capture centred on 868.875 MHz at 2 Msps holds every channel (and FLARM, which the engine feeds from the same samples in this mode); at 1 Msps around
// 868.3 MHz only the M-band is heard. A channel outside the capture is left out on its own.
// Receive only. No key is needed: the scrambling key is the published zero key; reserved key indices are counted.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct AdslTelemetry;   // adsl_tel.h

class AdslReceiver {
public:
    AdslReceiver();
    ~AdslReceiver();
    static inline bool toneDetect = true;   // 3.9: M-band chips by the energy at +-50 kHz instead of the FM discriminator (switch for comparisons)
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);                           // what the capture is tuned to; 0: unknown (868.875 MHz, or 868.3 MHz below 1.6 Msps)
    double flarmOffsetHz() const;                          // the signal offset that puts the FLARM receiver (which expects 868.3 MHz) on the same capture
    void setReference(double latDeg, double lonDeg);       // only for distance and bearing: ADS-L positions are absolute
    void clearReference();
    void setExpiry(double seconds);                        // an aircraft silent this long leaves the table (default 60)
    bool ready() const;
    void reset();                                          // the demodulators; the aircraft stay
    void feed(const cf32* x, size_t n);
    bool telemetry(AdslTelemetry& out, uint64_t lastSeq);
    void clearResults();
    std::string saveResults() const;
    bool loadResults(const std::string& blob);
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning adslTuning();

} // namespace dect2
