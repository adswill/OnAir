// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GNSS receiver: acquisition, tracking, navigation messages and position fix for the satellite navigation systems.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread (they must be safe while feed() runs).
#pragma once
#include "gnss_glo.h"
#include "gnss_nav.h"
#include "gnss_tel.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// One pseudorange measurement as the receiver made it, for tests and tools
struct GnssMeasurement {
    int sys = 0, prn = 0;
    int fcn = 0;                 // GLONASS: the frequency channel (prn is its slot number, 0 until string 4 has said)
    double rxTime = 0;           // the measurement instant on the receiver's clock: seconds of signal since the start
    double txTow = 0;            // the satellite's transmit time of the signal received at rxTime: its own clock, seconds of its system's week
                                 // (GPS, QZSS: GPS time; Galileo: GST; BeiDou: BDT) or of the day (GLONASS: the satellite's GLONASS time of day)
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
    // Compare the computer's clock with the satellites' UTC (a radio or the test signal, not a file): GnssTelemetry::clockOffsetMs
    void setClockCompare(bool on);
    void setElevationMask(double deg);                   // satellites below this elevation are not used in the fix (default 5)
    // The navigation data base, for tests and tools (copies; false when the data is not complete yet)
    bool getEphemeris(int prn, GpsEphemeris& e, int* week = nullptr) const;               // GPS
    bool getEphemerisOf(int sys, int prn, GpsEphemeris& e, int* week = nullptr) const;   // any system with one (GPS, QZSS, Galileo, BeiDou)
    bool getGloEphemeris(int fcn, GloEphemeris& e) const;                                // GLONASS, by frequency channel
    bool getAlmanac(int prn, GpsAlmanac& a) const;
    bool getIonoUtc(GpsIono& i, GpsUtc& u) const;
    // Tests and tools
    // Every SBAS message with a good CRC: the PRN, the 250 message bits (one per byte, preamble first) and the signal time in seconds. Called from feed().
    void setSbasCallback(std::function<void(int prn, const uint8_t* bits250, double signalSecs)> cb);
    void setMeasurementCallback(std::function<void(const std::vector<GnssMeasurement>&)> cb);   // once a second, from feed()
    uint64_t workUnitsUsed() const;                      // acquisition FFTs so far
    void cpuBreakdown(double* front, double* track, double* acq) const;   // seconds of CPU spent in the front end, the channels and the search so far

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning gnssTuning();

// The tunes the GNSS screen offers: L1 / E1 (GPS, Galileo, QZSS, SBAS), BeiDou B1I and GLONASS L1 (its fourteen channels, 1598.06 .. 1605.38 MHz),
// each on its own carrier, and All systems: one tune at 1582 MHz that spans 1559 .. 1606 MHz at 50 Msps. A tune is recognised by its centre within
// 1 kHz; any other frequency is a custom one.

enum GnssTune { GnssTuneCustom = -1, GnssTuneL1 = 0, GnssTuneB1I = 1, GnssTuneGlo = 2, GnssTuneAll = 3 };
constexpr double kGnssTuneMhz[4] = {1575.42, 1561.098, 1602.0, 1582.0};
constexpr double kGnssAllRate = 50e6;          // the All systems tune: B1I's lower lobe to GLONASS k = +6's upper one around 1582 MHz
int gnssTuneOf(double mhz);
// The radio rate a tune needs ("Auto" of the sample-rate control): 4 Msps for L1 and for B1I, 10 Msps for GLONASS (all fourteen channels), 50 Msps
// for All systems; a custom frequency keeps the mode's 4 Msps
double gnssAutoRate(double mhz);
// A short name of the tune for the settings key of a chosen rate ("" for L1, so the rate chosen before the other tunes existed stays)
const char* gnssTuneKey(double mhz);

} // namespace dect2
