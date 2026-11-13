// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Offline map tile cache: tile file naming (root/z/x/y.png), size cap with least-recently-used eviction (by file modification time),
// and the plan of an area download (how many tiles, about how big, never more than kMaxDownloadTiles). No network and no drawing here.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct TileKey { int z = 0, x = 0, y = 0; };

constexpr int kMaxDownloadTiles = 4000;            // hard limit for one "download this area" (the OSM tile usage policy forbids bulk downloads)
constexpr uint64_t kDefaultTileCapMb = 500;        // default size cap of the cache (setting mapCapMb)
constexpr uint64_t kAvgTileBytes = 20 * 1024;      // for the size estimate of a download

// Slippy-map tile of a position at a zoom (web mercator); latitudes are clamped to +-85.0511
TileKey tileOf(double latDeg, double lonDeg, int z);
// "z/x/y.png", or root + "/" + that
std::string tileRelPath(int z, int x, int y);
std::string tilePath(const std::string& root, int z, int x, int y);
// The key from a relative path "z/x/y.png" (also with a leading "/" or backslashes); false if it is not one
bool parseTilePath(const std::string& rel, TileKey& k);

struct AreaPlan {
    std::vector<TileKey> tiles;      // empty when tooMany
    long count = 0;                  // tiles in the area over all the zoom levels (counted even when tooMany, up to a few million)
    uint64_t estBytes = 0;           // count * kAvgTileBytes
    bool tooMany = false;            // count > the limit: nothing is planned, the caller says "zoom in"
    int minZoom = 0, maxZoom = 0;
};
// The tiles covering the box (south, west, north, east; west > east crosses the date line) for zoom levels minZoom..maxZoom.
// A box over more than `limit` tiles gives tooMany with no list. Zoom levels are clamped to 0..19.
AreaPlan planArea(double south, double west, double north, double east, int minZoom, int maxZoom, int limit = kMaxDownloadTiles);
// The deepest zoom (at most maxZoom, at least minZoom) whose plan from minZoom to it fits the limit; minZoom when even that does not
int deepestZoomFitting(double south, double west, double north, double east, int minZoom, int maxZoom, int limit = kMaxDownloadTiles);

struct CacheUsage { long files = 0; uint64_t bytes = 0; };
CacheUsage cacheUsage(const std::string& root);
// Mark a tile as used just now (its modification time), so the cap evicts older ones first
void touchTile(const std::string& path);
// Delete the least recently used tile files until the folder holds at most capBytes (empty folders are removed too).
// Returns the number of files deleted. capBytes 0 means no cap.
long enforceCap(const std::string& root, uint64_t capBytes);

}
