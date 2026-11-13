// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Long-range AIS: message 27 (ITU-R M.1371-5 section 3.3.8.2.24 and table 79) and the two long-range channels.
// Message 27 is the 96-bit position report that class A stations send for satellite receivers (the "space AIS" channels 75 = 156.775 MHz and
// 76 = 156.825 MHz, identified for this by WRC-15): the content of messages 1 - 3 in a short frame (position to 1/10 minute, speed in whole
// knots, course in whole degrees, navigational status, a GNSS position latency flag) so the burst ends early in its 26.7 ms slot and still fits after
// the propagation delay to a satellite (about 7 ms of room: a slant range of some 2000 km). A station alternates between the two channels, nominally every 3 minutes (each channel once every 6).
// The message carries no time: a receiver time-stamps every reception (AisRxRecord in ais_tel.h).
// Background and the satellite analysis it allows: Li, Chen, Chen, "Long-range AIS message analysis based on the TianTuo-3 micro satellite",
// Acta Astronautica 2017, doi:10.1016/j.actaastro.2017.02.014. docs/SATELLITE_AIS.md has the details.
#pragma once
#include "ais_proto.h"
#include <cstdint>
#include <vector>

namespace dect2 {
namespace ais {

constexpr double kLongRangeHz[2] = {156.775e6, 156.825e6};   // channel 75 ('C') and channel 76 ('D')
constexpr double kAisMidHz = 162.000e6;                       // middle of AIS 1 and AIS 2
constexpr double kLongRangeMidHz = 156.800e6;                 // middle of channels 75 and 76
constexpr double kAisWideCentreHz = 159.400e6;                // one capture centre that holds all four channels
constexpr double kPairMarginHz = 40e3;                        // a pair's front end needs its middle this far inside the capture's edge
constexpr double kLongRangeIntervalSec = 180;                 // nominal reporting interval of message 27 (each channel once every 6 minutes)

struct LongRangeReport {
    int repeat = 0;
    uint32_t mmsi = 0;
    bool posAccuracy = false;    // 1 = better than 10 m
    bool raim = false;
    int navStatus = 15;          // 0 - 15 (15 = not defined)
    double lat = 91, lon = 181;  // degrees; 91 / 181 = not available. On the air in 1/10 minute (about 185 m)
    int sogKn = 63;              // whole knots 0 - 62, 63 = not available
    int cogDeg = 511;            // whole degrees 0 - 359, 511 = not available
    bool latencyOver5s = true;   // GNSS position latency: false = under 5 s, true = 5 s or more (the default)
};

// The 96 payload bits (without FCS) of message 27. Values out of range are sent as "not available"; lat / lon are rounded to 1/10 minute.
Bits encodeLongRange(const LongRangeReport& r);
// False unless the payload is a message 27 of 96 bits (168 is accepted as some receivers pad it).
bool decodeLongRange(const Bits& payload, LongRangeReport& out);

// Which of the four channels a capture covers (bit c = channel c, as AisTelemetry: A, B, 75, 76). The channels come in pairs (A+B, 75+76),
// each pair needs its middle kPairMarginHz inside the edge of the capture.
int aisChannelsCovered(double centreHz, double sampleRateHz);
// The sample rate that holds all four channels at kAisWideCentreHz (about 5.4 Msps; 6 Msps or more in practice).
double aisWideRateHz();

// A retune plan for a radio that cannot hold all four channels: AIS 1 + 2 for aisDwellSec, then 75 + 76 for longRangeDwellSec (at least one
// full reporting interval of message 27, so every class A station in range is heard once). One step at kAisWideCentreHz when the rate covers all.
struct AisTuneStep { double centreHz = 0; double dwellSec = 0; int channelMask = 0; };
std::vector<AisTuneStep> aisTunePlan(double sampleRateHz, double aisDwellSec = 60, double longRangeDwellSec = 200);

} // namespace ais
} // namespace dect2
