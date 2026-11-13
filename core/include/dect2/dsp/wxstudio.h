// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Weather picture studio helpers on top of imgproc.h: histogram, tone curves, colour palettes, simplified coastline and border vectors drawn
// on a map, crop, stitching several passes into one mosaic, and an own GeoTIFF writer (uncompressed RGBA, EPSG:4326). Pure local processing.
#pragma once
#include "dect2/dsp/imgproc.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace img {

// counts of each 8-bit value; skipZero leaves out value 0 (nothing received yet)
void histogramOf(const uint8_t* v, size_t n, bool skipZero, std::array<uint32_t, 256>& bins);

// ---- tone curves: control points x, y in 0 .. 1, joined by a monotone cubic (no overshoot)
struct CurvePoint { double x, y; };
std::array<uint8_t, 256> curveLut(const std::vector<CurvePoint>& pts);
struct CurvePreset { const char* name; std::vector<CurvePoint> pts; };
const std::vector<CurvePreset>& curvePresets();   // entry 0 is the identity

// ---- colour palettes applied to the luminance of a picture (entry 0 = none)
const std::vector<const char*>& paletteNames();
void paletteColour(int index, uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b);
void applyPaletteRgba(uint8_t* rgba, size_t nPixels, int index);

// ---- vectors: simplified outlines drawn by hand for this project (accuracy about 50 - 100 km), not taken from a dataset
enum : unsigned { kVecCoast = 1, kVecBorder = 2 };
void drawVectors(MapImage& m, unsigned kinds, uint32_t rgbaColour);

// ---- crop and mosaic. Both keep the geography: the result's west/north/degPerPx describe it.
bool cropMap(const MapImage& in, double west, double north, double east, double south, MapImage& out);
// the passes on one grid (pixel size of the finest, coarser when the union is wider than maxDim); where passes overlap, the values are averaged
bool mosaicMaps(const std::vector<MapImage>& parts, int maxDim, MapImage& out);
// the covered area as a TIFF with GeoTIFF tags (ModelPixelScale, ModelTiepoint, GeoKeyDirectory: geographic WGS84, pixel is area)
std::vector<uint8_t> geoTiff(const MapImage& m);

}  // namespace img
}  // namespace dect2
