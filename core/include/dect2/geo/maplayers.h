// Map layers: the receivers' result lists turned into plain markers (position, label, age, track) for the live map, and the
// geometry of a satellite (sub-point, footprint circle, ground track). Pure functions, no drawing.
#pragma once
#include "dect2/adsb/adsb_tel.h"
#include "dect2/ais/ais_tel.h"
#include "dect2/dmr/dmr_tel.h"
#include "dect2/packet/packet_tel.h"
#include "dect2/vdes/vdes_tel.h"
#include "passes.h"
#include "dect2/sonde/sonde_tel.h"
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

struct TetraTelemetry;
enum MapLayer { LayerAdsb = 0, LayerAis, LayerAprs, LayerSonde, LayerSat, LayerCell, LayerDmr, LayerCount };

struct MapMarker {
    int layer = LayerAdsb;
    std::string id;                    // ICAO address, MMSI, callsign, serial, NORAD number
    std::string label;                 // what is drawn next to it: callsign, ship name, station, serial
    double lat = 0, lon = 0;
    bool hasAlt = false; double altM = 0;
    bool hasSpeed = false; double speedKt = 0;
    double headingDeg = -1;            // -1: unknown
    double ageSec = 0;                 // since last heard
    std::string detail;                // one more line for the info popup (type, destination, comment)
    std::vector<std::pair<float, float>> track;   // lat, lon, oldest first
};

std::vector<MapMarker> markersFromAdsb(const AdsbTelemetry& t);     // aircraft with a position
std::vector<MapMarker> markersFromTetra(const TetraTelemetry& t);    // clear LIP positions (tetra_sds.h)
std::vector<MapMarker> markersFromAis(const AisTelemetry& t);       // vessels with a position
std::vector<MapMarker> markersFromVdes(const VdesTelemetry& t);     // VDES ASM frames that carried an AIS position, the latest per MMSI (drawn on the AIS ships layer)
std::vector<MapMarker> markersFromAprs(const PacketTelemetry& t);   // stations and objects with a position
std::vector<MapMarker> markersFromSonde(const SondeTelemetry& t);   // sondes with a position, their flight track
std::vector<MapMarker> markersFromDmr(const DmrTelemetry& t);       // radios with a position, their movement track

// A satellite on the map: sub-point, the circle on the ground it sees (above `minElDeg`), and its ground track from
// `pastMin` before to `futureMin` after `unixSec`, one point per `stepMin`.
struct SatMapPoint {
    std::string name;
    int norad = 0;
    double lat = 0, lon = 0, altKm = 0;
    double footprintKm = 0;                                   // radius on the ground
    std::vector<std::pair<double, double>> track;             // lat, lon; time order
    size_t nowIndex = 0;                                      // index in track of `unixSec`
};
// Ground radius (km, spherical Earth) of the area from which a satellite at altKm stands at least minElDeg above the horizon
double footprintRadiusKm(double altKm, double minElDeg = 0);
bool satelliteMapPoint(const Sgp4& s, double unixSec, double pastMin, double futureMin, double stepMin, SatMapPoint& out, double minElDeg = 0);
// n points (lat, lon) on the circle of radius km around a position (for drawing a footprint)
std::vector<std::pair<double, double>> circlePoints(double lat, double lon, double radiusKm, int n = 72);

// The age in words: "12 s", "3 min", "2 h 5 min"
std::string ageText(double sec);

}
