// Aircraft positions from Aero ACARS messages: ADS-C reports (aero_adsc.h) and the plain-text position reports whose layout
// is published. Text formats (each from airframesio/acars-decoder-typescript, lib/plugins, with the example messages of its tests):
//   any label, ARINC 702 style "POS" report (Label_H1_POS / Arinc702): "POSN43312W123174,EASON,215754,370,..." and the same
//     position after "/PS" in slash-field reports ("#M1BPOS/.../PSN42579W108090,173207,320,..."). Latitude N/S + 5 digits as
//     degrees and minutes with tenths (DDMMm), longitude E/W + 6 digits (DDDMMm); then a waypoint (may be left out or empty),
//     the time HHMMSS and the flight level (3 digits, x 100 ft).
//   label 20 "POSN38160W077075,..." (Label_20_POS): the same field shape but thousandths of a degree.
//   label 16 "N 44.203,W 86.546,31965,6, 290" and "N 28.177/W 96.055" (Label_16_N_Space): decimal degrees, altitude in feet.
#pragma once
#include <string>
#include <vector>

namespace dect2 {

struct AeroPosition {
    bool valid = false;
    int source = 0;                   // 1 ADS-C, 2 text report
    std::string kind;                 // "ADS-C basic report", "POS report", ...
    double lat = 0, lon = 0;
    bool hasAlt = false;
    int altFt = 0;
    bool hasTrack = false;            // track over the ground (ADS-C earth reference) or true heading (air reference)
    double trackDeg = 0;
    bool hasSpeed = false;
    double speedKt = 0;
    double secPastHour = -1;          // ADS-C time stamp
    int secOfDay = -1;                // text reports: HHMMSS of the report, UTC
    std::string flightId;             // ADS-C flight id group
    unsigned icao = 0;                // ADS-C airframe id group
    std::vector<std::pair<double, double>> route;   // ADS-C predicted route: next and next + 1 waypoints
};

// Text reports only. False when the text has none of the formats above.
bool aeroParseTextPosition(const std::string& label, const std::string& text, AeroPosition& out);

// Everything this mode knows about one message: the position if any, and in `detail` the decoded ADS-C groups in plain words
// (empty when the message has no ADS-C). `uplink` as the SU layer found it.
bool aeroPositionFromMessage(const std::string& label, const std::string& text, bool uplink, AeroPosition& out, std::string* detail = nullptr);

} // namespace dect2
