// The theoretical range outline (tar1090 + heywhatsthat): rings per altitude where an aircraft is just above the terrain horizon of the
// antenna. From the heywhatsthat upintheair.json the user downloads, or worked out from the user's own elevation files (SRTM .hgt, GeoTIFF:
// dect2/prop/prop_terrain.h). Drawn next to the actual outline. docs/ADSB-HORIZON-3.9.md.
#pragma once
#include "dect2/prop/prop_terrain.h"
#include <string>
#include <vector>

namespace dect2 {

struct HorizonRing {
    double altM = 0;                                 // aircraft altitude, metres above sea level
    std::vector<std::pair<double, double>> pts;      // lat, lon around the antenna
};

// heywhatsthat's upintheair.json: {"id":..,"lat":..,"lon":..,"rings":[{"alt":"3048","points":[[lat,lon],...]},...]} (alt in metres, as text or a number)
bool parseUpintheair(const std::string& json, std::vector<HorizonRing>& out, std::string& err);

// Worked out: for each bearing, the farthest distance at which an aircraft at each altitude still clears the highest terrain angle seen from
// the antenna (4/3 earth radius for refraction). Missing heights count as sea level. antennaAglM: the antenna above the ground at the site.
std::vector<HorizonRing> terrainHorizon(const prop::Terrain& t, double lat, double lon, double antennaAglM, const std::vector<double>& altsM,
                                        int bearings = 360, double maxKm = 500, double stepKm = 0.5);

// the destination point (great circle) d km from lat/lon on a bearing
void horizonDest(double lat, double lon, double bearingDeg, double km, double& outLat, double& outLon);

}  // namespace dect2
