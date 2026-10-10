// GNSS receiver: acquisition, tracking, navigation messages and position fix for the satellite navigation systems.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread (they must be safe while feed() runs).
#pragma once
#include "gnss_nav.h"
#include "gnss_tel.h"
#include "mode_tuning.h"
#include "ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// One pseudorange measurement as the receiver made it, for tests and tools
struct GnssMeasurement {
    int sys = 0, prn = 0;
    double rxTime = 0;           // the measurement instant on the receiver's clock: seconds of signal since the start
    double txTow = 0;            // the satellite's transmit time of the signal received at rxTime: its own clock, GPS time of week, seconds
    double dopplerHz = 0;
    double cn0 = 0;
    bool inFix = false;
    double residualM = 0;
};

class GnssReceiver {
public:
    GnssReceiver();
    ~GnssReceiver();

    void configure(double inputRateHz);
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal and the decoded state
    void feed(const cf32* x, size_t n);
    bool telemetry(GnssTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

    // Setters for the interface; safe while feed() runs
    void setCenterMhz(double mhz);                       // the tuned centre frequency (default 1575.42): decides which systems fit in the band
    void setSystems(unsigned mask);                      // gnssSystemBit() of the wanted systems (default: all that the build supports)
    void setWeekReference(int gpsWeek);                  // a week near today, for the 10 bit week numbers (default: from the computer's clock)
    void setSearchRange(double dopplerHalfWidthHz);      // half width of the first search window (default 10 kHz); wider ones follow while nothing is found
    // The radio's frequency error found in an earlier run (GnssTelemetry::cfoHz): the first search looks there before it widens
    void setFrequencyHint(double hz, bool valid);
    void setAcquisitionRate(int fftsPerMs);              // work given to the search per millisecond of signal (default 10)
    void setApproxPosition(double latDeg, double lonDeg, bool valid);   // a position hint: only used to choose which satellites to search first
    void setElevationMask(double deg);                   // satellites below this elevation are not used in the fix (default 5)
    // The navigation data base, for tests and tools (copies; false when the data is not complete yet)
    bool getEphemeris(int prn, GpsEphemeris& e, int* week = nullptr) const;               // GPS
    bool getEphemerisOf(int sys, int prn, GpsEphemeris& e, int* week = nullptr) const;   // any system with one (GPS, QZSS, Galileo)
    bool getAlmanac(int prn, GpsAlmanac& a) const;
    bool getIonoUtc(GpsIono& i, GpsUtc& u) const;
    // Tests and tools
    void setMeasurementCallback(std::function<void(const std::vector<GnssMeasurement>&)> cb);   // once a second, from feed()
    uint64_t workUnitsUsed() const;                      // acquisition FFTs so far
    void cpuBreakdown(double* front, double* track, double* acq) const;   // seconds of CPU spent in the front end, the channels and the search so far

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning gnssTuning();

} // namespace dect2
