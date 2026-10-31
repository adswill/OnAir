// Weather-satellite pictures on the map: the ground position of every pixel of a scanned swath (Meteor-M LRPT, NOAA-style line
// scanners) from the orbit (TLE / SGP4) and the time the picture began, or of a geostationary full-disk picture (GOES) from the
// satellite longitude; the resampling of the picture into a map raster (Web Mercator for the live map, plate carree for files);
// and the exports: a GeoTIFF (EPSG:4326) and a KML GroundOverlay. Pure functions, no drawing, no files.
// Pixels are opaque 32-bit values; the top byte is the alpha (0 = no picture there), the rest is passed through unchanged.
#pragma once
#include "passes.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

enum class MapProj { PlateCarree, Mercator };

// A map raster: rows north to south. West/east in degrees (east may pass 180 for a swath across the date line), north/south in degrees.
struct GeoRaster {
    MapProj proj = MapProj::Mercator;
    double west = 0, east = 0, south = 0, north = 0;
    int w = 0, h = 0;
    std::vector<uint32_t> px;       // w * h
    bool empty() const { return w <= 0 || h <= 0 || px.empty(); }
};

// The scanner. Line k was scanned at startUnix + k / linesPerSec at the sub-satellite point; pixel x of `width` crosses the swath.
struct SwathSpec {
    double startUnix = 0;           // when line 0 was scanned
    double linesPerSec = 6;         // Meteor-M MSU-MR 6, NOAA APT 2
    int width = 1568;
    double swathKm = 2800;          // ground width of the picture across the track (Meteor-M MSU-MR about 2800 km)
    bool xToLeft = true;            // pixel x grows to the satellite's left (north-up for a descending pass with the first line on top)
    double timeOffsetSec = 0;       // clock correction added to the line times (the picture's start time is only known to a few seconds)
};

// The ground position of line `line` (may be fractional), pixel `x` (0 .. width-1, fractional allowed); spherical offset from the
// WGS-84 sub-satellite point. False when the orbit cannot be propagated.
bool swathLatLon(const Sgp4& sat, const SwathSpec& s, double line, double x, double& latDeg, double& lonDeg);

// Resample a swath picture (srcW = s.width, srcH lines) to a map raster of at most maxDim pixels along its longer side. Longitudes are
// unwrapped along the track so a pass over the date line stays one piece (east > 180). False when srcH < 2 or the orbit fails.
bool projectSwath(const Sgp4& sat, const SwathSpec& s, const uint32_t* src, int srcH, int maxDim, MapProj proj, GeoRaster& out);

// A geostationary full disk: the satellite sits on the equator at satLonEastDeg; the Earth's equatorial limb is diskDiameterPx wide
// in the square-ish srcW x srcH picture, centred. Fills the visible disk (up to the limb, not beyond 81 deg of the sub-point).
bool projectGeosDisk(const uint32_t* src, int srcW, int srcH, double satLonEastDeg, double diskDiameterPx, int maxDim, MapProj proj, GeoRaster& out);

// Map coordinate of a latitude in the projection (degrees for plate carree, ln tan for Mercator; latitude limited to 85.0511)
double projY(double latDeg, MapProj p);
double projLat(double y, MapProj p);

// Exports. The GeoTIFF is uncompressed 8-bit RGBA, EPSG:4326, and needs a plate-carree raster (false otherwise). The pixel bytes are
// R,G,B,A in memory order (the uint32 is read as little endian 0xAABBGGRR, as the app's textures are).
bool geoTiffRgba(const GeoRaster& r, std::vector<uint8_t>& out);
// KML with a GroundOverlay of the PNG (href) over the raster's box; opacity 0..1. Plate carree only for exact placement.
std::string kmlGroundOverlay(const std::string& name, const std::string& pngHref, const GeoRaster& r, double opacity = 1.0);
// The picture as RGBA bytes in memory order (for a PNG writer)
std::vector<uint8_t> rasterBytes(const GeoRaster& r);

}
