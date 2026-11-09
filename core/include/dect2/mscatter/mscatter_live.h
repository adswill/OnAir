// Meteor scatter detector for the engine: IQ in, pings out.
// A fixed transmitter (the GRAVES radar at 143.050 MHz, or any CW beacon) is received far beyond the horizon only when a meteor trail reflects it.
// The receiver filters the capture to about 12 kHz, finds the beacon line, follows it, and watches the bins around it for narrowband echoes
// (a Doppler-shifted copy of the beacon) that rise above their own long-term level. Each ping is counted with start time, duration,
// peak level over the background and Doppler; rates per hour and per day are kept, and every ping can be appended to a CSV file.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "dect2/geo/passes.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct MsTelemetry;   // mscatter_tel.h

class MscatterLiveReceiver {
public:
    MscatterLiveReceiver();
    ~MscatterLiveReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);            // not used: the beacon is searched around the centre
    void setCenterHz(double hz);                // the radio's centre, for the CSV and the screen
    bool ready() const;
    void reset();                               // the filters and the detector; counts and the list stay
    void clearResults();                        // empties the list and the counts
    void setThresholdDb(double db);             // detection level over the background (default 13)
    void setLogFile(const std::string& path);   // append every ping as a CSV line ("" off)
    // Satellite echoes of GRAVES: the elements to match them with and where the receiver is (an empty list: echoes are listed unnamed)
    void setSatelliteMatch(const std::vector<Tle>& tles, double latDeg, double lonDeg, double altKm);
    void setBeaconMhz(double mhz);              // the transmit frequency for the Doppler scale (default 143.050)
    void setStartUtc(int64_t utcSec);           // tests: the UTC second of the first sample (default: now)
    void feed(const cf32* x, size_t n);
    bool telemetry(MsTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning mscatterTuning();

} // namespace dect2
