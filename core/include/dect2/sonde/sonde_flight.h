// A radiosonde flight as a track for Google Earth / OsmAnd (auto_rx KML, SondeHub tracker): the ascent and the descent as two lines with
// altitude, and points for the launch, the burst, the last position (landed when heard low) and the predicted landing. docs/SONDE-TRACK-3.9.md.
#pragma once
#include "dect2/export/exporters.h"
#include "dect2/sonde/sonde_land.h"
#include "dect2/sonde/sonde_tel.h"
#include <string>
#include <vector>

namespace dect2 {

struct SondeFlight {
    std::vector<xport::GeoPoint> points;    // launch, burst, last / landed, predicted landing
    std::vector<xport::GeoTrack> tracks;    // "ascent", "descent"
    bool burst = false, landed = false;
};

// landedBelowM: the last position counts as the landing when it is this low and the sonde was coming down
SondeFlight sondeFlight(const SondeInfo& s, const SondeLanding& predicted, double landedBelowM = 1500);

}  // namespace dect2
