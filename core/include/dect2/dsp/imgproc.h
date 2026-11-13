// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Picture enhancement (contrast stretch, gamma, equalise, invert, false colour, thermal palette) and the map projection of a satellite
// pass: swath geometry from the sub-satellite ground track (spherical earth), equirectangular reprojection, graticule, world file.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace img {

// ---- enhancement: all work on 8-bit values; a LUT is 256 entries
// the values at the lo and hi percentiles (0 .. 100) of a grey plane, zeros ignored when skipZero (nothing received yet)
void percentileRange(const uint8_t* v, size_t n, double loPct, double hiPct, bool skipZero, int& lo, int& hi);
std::array<uint8_t, 256> stretchLut(int lo, int hi);                 // lo -> 0, hi -> 255, linear
std::array<uint8_t, 256> gammaLut(double gamma);                     // out = 255 * (in/255)^(1/gamma): above 1 brightens
std::array<uint8_t, 256> equaliseLut(const uint8_t* v, size_t n, bool skipZero);
std::array<uint8_t, 256> invertLut();
std::array<uint8_t, 256> composeLut(const std::array<uint8_t, 256>& first, const std::array<uint8_t, 256>& then);
// the thermal palette for infrared: black - blue - cyan - green - yellow - red - white along 0 .. 255
void thermalColour(uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b);
// the false-colour presets of the multi-channel (Meteor) pictures: the channels (index 0 ..) behind red, green and blue (-1: as received);
// thermal: the blue entry is the channel the palette is applied to
struct Preset { const char* name; int r, g, b; bool thermal; };
const std::vector<Preset>& falseColourPresets();
// RGBA from up to three grey planes (null plane = 0), all n pixels; thermal: the third plane through the palette (cold cloud tops white)
void composeRgba(const uint8_t* pr, const uint8_t* pg, const uint8_t* pb, size_t n, bool thermal, uint8_t* rgba);
// a LUT applied to the colour channels of RGBA (alpha kept)
void applyLutRgba(uint8_t* rgba, size_t nPixels, const std::array<uint8_t, 256>& lut);
// luminance (Rec. 601) of RGBA pixels as a grey plane
std::vector<uint8_t> lumaOf(const uint8_t* rgba, size_t nPixels);

// ---- ground track and swath
struct TrackPoint { double lat = 0, lon = 0; };   // degrees, sub-satellite point of one image row (a strip of lines)
struct SwathGeom {
    std::vector<TrackPoint> track;   // one per image row, in time order
    double swathKm = 2800;           // the scan width on the ground
    int cols = 1568;                 // pixels across
    bool colZeroRight = true;        // column 0 on the right of the direction of flight (the Meteor pictures as the receiver stores them)
};
constexpr double kEarthKm = 6371.0;
// the point reached from p going distKm along the great circle with the bearing (degrees from north)
TrackPoint destination(const TrackPoint& p, double bearingDeg, double distKm);
double distanceKm(const TrackPoint& a, const TrackPoint& b);
double bearingDeg(const TrackPoint& a, const TrackPoint& b);
// the ground position of pixel (row, col); row may be fractional (between two track points)
TrackPoint pixelLatLon(const SwathGeom& g, double row, double col);
// the picture position of a ground point; false when it is outside the track's rows or the swath
bool latLonPixel(const SwathGeom& g, const TrackPoint& p, double& row, double& col);

// ---- equirectangular projection
struct MapImage {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;                  // alpha 0 where the pass did not cover
    double west = 0, north = 0, degPerPx = 0;   // the outer upper-left corner, degrees; square pixels
};
// rgba: lines of cols pixels; linesPerTrackPoint: picture lines between track points (8 for a Meteor strip)
// maxDim: the longer side of the result in pixels at most; the pixel size is never finer than the source
bool projectSwath(const uint8_t* rgba, int cols, int lines, int linesPerTrackPoint, const SwathGeom& g, int maxDim, MapImage& out);
// graticule lines every stepDeg, 1 pixel (colour as bytes r, g, b, a packed little-endian), no coastlines
void drawGraticule(MapImage& m, double stepDeg, uint32_t rgbaColour);
// the world file (.pgw): six lines, pixel x size, 0, 0, -pixel y size, x and y of the centre of the upper-left pixel
std::string worldFile(const MapImage& m);

}  // namespace img
}  // namespace dect2
