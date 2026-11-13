// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Local elevation data for the propagation tool: SRTM .hgt tiles and simple GeoTIFF files the user already has (nothing is downloaded),
// height lookups, path profiles and the delta-Bullington terrain diffraction (ITU-R P.526 section 4.5 / P.1812 section 4.3, simplified).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::prop {

struct ElevTile {
    std::string name;
    double latTop = 0, lonLeft = 0;   // centre of the first sample (row 0 = north, column 0 = west)
    double dLat = 0, dLon = 0;        // sample spacing, degrees (positive)
    int w = 0, h = 0;
    std::vector<int16_t> z;           // metres, row by row; kVoid where there is no data
    static constexpr int16_t kVoid = -32768;
    double latBottom() const { return latTop - dLat * (h - 1); }
    double lonRight() const { return lonLeft + dLon * (w - 1); }
    bool covers(double lat, double lon) const;
    bool heightAt(double lat, double lon, double& m) const;   // bilinear; false outside or on a void
};

struct Terrain {
    std::vector<ElevTile> tiles;
    bool empty() const { return tiles.empty(); }
    bool heightAt(double lat, double lon, double& m) const;   // first tile that has it
    std::string describe() const;                             // "2 tiles, 53.0..55.0 N, 24.0..26.0 E"
};

// SRTM .hgt: the file name gives the south-west corner (N54E025.hgt), the size the spacing (1201 or 3601 per side), big-endian int16
bool parseHgt(const std::string& fileName, const std::vector<uint8_t>& bytes, ElevTile& t, std::string& err);
// Uncompressed GeoTIFF in geographic (WGS 84) coordinates: one band of int16 / uint16 / int32 / float32, strips or tiles, either byte order
bool parseGeoTiff(const std::vector<uint8_t>& bytes, ElevTile& t, std::string& err);
// By extension and content; adds the tile to t. err says why not (compressed, projected coordinates, ...)
bool loadElevationFile(const std::string& path, Terrain& t, std::string& err);

// 4.0: opt-in tile download. The tiles are the public "Terrain Tiles" on AWS (Mapzen / Linux Foundation, open data; 1 arc-second, mostly
// SRTM, see THIRD_PARTY.md), one gzip SRTM .hgt per 1 x 1 degree, named after the south-west corner. loadElevationFile reads them
// (".hgt.gz", kept at 3 arc-seconds).
std::string demTileName(int latSouth, int lonWest);              // "N54E025"
std::string demTileUrl(int latSouth, int lonWest);               // https://s3.amazonaws.com/elevation-tiles-prod/skadi/N54/N54E025.hgt.gz
// the tiles that cover a circle of radiusKm around a point, and the straight path to a second point when lat2 / lon2 are finite; at most maxTiles
std::vector<std::pair<int, int>> demTilesFor(double lat, double lon, double radiusKm, double lat2, double lon2, int maxTiles = 9);

struct Profile {
    std::vector<double> dKm, hM;   // distance from the transmitter, ground height above sea level
    bool complete = false;         // both ends had data
};
Profile pathProfile(const Terrain& t, double lat1, double lon1, double lat2, double lon2, int samples);

// Bullington loss (dB) of a profile, antenna heights above sea level at both ends, 4/3 earth
double bullingtonDb(const std::vector<double>& dKm, const std::vector<double>& hM, double htsM, double hrsM, double fMHz);
struct TerrainLoss {
    double diffractionDb = 0;      // delta-Bullington: actual profile + max(smooth spherical - smooth Bullington, 0)
    double effTxM = 0, effRxM = 0; // antenna heights above the mean terrain of the path (for the ground reflection)
    bool los = true;               // no obstacle cuts the straight line
    double thetaMrad = 0;          // angular distance for troposcatter: d / ae plus both horizon elevation angles (P.452)
};
TerrainLoss terrainLoss(const Profile& pr, double txHeightM, double rxHeightM, double fMHz);

} // namespace dect2::prop
