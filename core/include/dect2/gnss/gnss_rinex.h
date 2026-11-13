// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RINEX 3.04 observation output of the GNSS receiver's measurements (pseudorange, Doppler, C/N0), for post-processing in RTKLIB and
// similar tools. Code only: the receiver keeps no carrier phase, so there is no L observable.
// The receiver clock: the first epoch puts the receive time 70 ms after the latest transmit time (whole milliseconds), then it runs
// with the sample count; it jumps by whole milliseconds whenever the mean travel time leaves 60..95 ms (a sample clock off by many ppm),
// as hardware receivers do. The position solver estimates the remaining bias, as for any receiver.
// Systems written: GPS (G, C1C), Galileo (E, C1B: E1-B data), BeiDou (C, C2I: B1I), QZSS (J, C1C), SBAS (S, C1C). GLONASS
// measurements are left out (their time of day would need the leap seconds and the frequency slots in the header).
#pragma once
#include "dect2/gnss/gnss_nav.h"
#include "dect2/gnss/gnss_rx.h"
#include <string>
#include <vector>

namespace dect2 {

struct GnssRinexInfo {
    std::string program = "OnAir";
    std::string marker = "ONAIR";
    std::string runDate;             // "YYYYMMDD HHMMSS UTC"; empty: left blank
    double approxEcef[3] = {0, 0, 0};
    int gpsWeek = 0;                 // the GPS week of the first epoch (no roll-over: the full week number)
};

class GnssRinexWriter {
public:
    explicit GnssRinexWriter(const GnssRinexInfo& info) : info_(info) {}
    // one measurement epoch (all with the same rxTime): its RINEX record, or "" when it holds no satellite of a written system
    std::string epoch(const std::vector<GnssMeasurement>& m);
    // the header, valid once the first epoch has been written (it carries the time of the first observation and the systems seen)
    std::string header() const;
    int epochs() const { return nEpochs_; }
    // GPS week and seconds to the calendar (GPS time scale, no leap seconds)
    static void gpsToCalendar(int week, double tow, int& y, int& mo, int& d, int& h, int& mi, double& s);

private:
    GnssRinexInfo info_;
    bool started_ = false;
    double rx0_ = 0, t0_ = 0;        // the first epoch: receiver sample time and its GPS time of week
    double steerS_ = 0;              // whole millisecond clock jumps so far
    int week0_ = 0;
    unsigned sysSeen_ = 0;
    int nEpochs_ = 0;
    double firstTow_ = 0;
    int firstWeek_ = 0;
};

// RINEX 3.04 navigation file of the GPS ephemerides the receiver decoded (so RTKLIB can solve from the two files alone)
std::string gnssRinexNavHeader(const std::string& program, const std::string& runDate = "");
// one GPS ephemeris record; `week` is the full GPS week of its toe
std::string gnssRinexNavGps(const GpsEphemeris& e, int week);

// the time of the system's transmit time stamp on the GPS time scale (BeiDou time is 14 s behind GPS time); false: not written
bool gnssRinexGpsTime(int sys, double txTow, double& gpsTow);

} // namespace dect2
