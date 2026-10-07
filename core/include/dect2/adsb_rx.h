// ADS-B receiver: ADS-B / Mode S aircraft messages on 1090 MHz.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread (they must be safe while feed() runs).
#pragma once
#include "adsb_tel.h"
#include "adsb_track.h"
#include "mode_tuning.h"
#include "ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class AdsbReceiver {
public:
    AdsbReceiver();
    ~AdsbReceiver();

    void configure(double inputRateHz);
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal, drop queued output
    void feed(const cf32* x, size_t n);
    bool telemetry(AdsbTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

    // Setters for the interface; safe while feed() runs
    void setReference(double latDeg, double lonDeg);     // the receiver's position: local decoding of the first position, distance and bearing, surface positions
    void clearReference();
    void setCorrection(int bits);                        // error correction of DF11 / 17 / 18: 0 off, 1 one bit (default), 2 two bits
    void setExpiry(double seconds);                      // an aircraft that has been silent this long leaves the table (default 60)
    void clearAircraft();                                // empty the table and the statistics
    // Tests and tools: every good message, called from feed() (not under the receiver's lock)
    void setFrameCallback(std::function<void(const AdsbFrame&)> cb);
    void setThresholds(float pulseOverNoise, float pulseOverGap);   // preamble test, normally 3.0 and 2.0

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning adsbTuning();

} // namespace dect2
