// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/geo/tilecache.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace fs = std::filesystem;
namespace {
using dect2::math::kPi;
fs::path fsPath(const std::string& p) { return fs::path(reinterpret_cast<const char8_t*>(p.c_str())); }
}

TileKey tileOf(double lat, double lon, int z) {
    z = std::max(0, std::min(19, z));
    const long n = 1L << z;
    lat = std::max(-85.0511, std::min(85.0511, lat));
    lon = std::fmod(std::fmod(lon + 180.0, 360.0) + 360.0, 360.0) - 180.0;
    const double s = std::sin(lat * kPi / 180.0);
    TileKey k; k.z = z;
    k.x = (int)std::max(0L, std::min(n - 1, (long)std::floor((lon + 180.0) / 360.0 * n)));
    k.y = (int)std::max(0L, std::min(n - 1, (long)std::floor((0.5 - std::log((1 + s) / (1 - s)) / (4 * kPi)) * n)));
    return k;
}

std::string tileRelPath(int z, int x, int y) { char b[64]; snprintf(b, sizeof b, "%d/%d/%d.png", z, x, y); return b; }
std::string tilePath(const std::string& root, int z, int x, int y) { return root + "/" + tileRelPath(z, x, y); }

bool parseTilePath(const std::string& rel, TileKey& k) {
    std::string s = rel;
    std::replace(s.begin(), s.end(), '\\', '/');
    while (!s.empty() && s[0] == '/') s.erase(0, 1);
    int z, x, y; char tail[8] = {};
    if (sscanf(s.c_str(), "%d/%d/%d.%7s", &z, &x, &y, tail) != 4 || std::string(tail) != "png") return false;
    if (z < 0 || z > 19 || x < 0 || y < 0 || x >= (1L << z) || y >= (1L << z)) return false;
    k.z = z; k.x = x; k.y = y;
    return true;
}

AreaPlan planArea(double south, double west, double north, double east, int minZoom, int maxZoom, int limit) {
    AreaPlan p;
    minZoom = std::max(0, std::min(19, minZoom)); maxZoom = std::max(minZoom, std::min(19, maxZoom));
    p.minZoom = minZoom; p.maxZoom = maxZoom;
    if (south > north) std::swap(south, north);
    struct Range { int x0, x1, y0, y1; };
    std::vector<std::vector<Range>> perZoom;
    long total = 0;
    for (int z = minZoom; z <= maxZoom; z++) {
        const TileKey sw = tileOf(south, west, z), ne = tileOf(north, east, z);
        const long n = 1L << z;
        std::vector<Range> r;
        if (west <= east) r.push_back({sw.x, ne.x, ne.y, sw.y});
        else { r.push_back({sw.x, (int)n - 1, ne.y, sw.y}); r.push_back({0, ne.x, ne.y, sw.y}); }   // across the date line
        for (const Range& q : r) total += (long)(q.x1 - q.x0 + 1) * (q.y1 - q.y0 + 1);
        perZoom.push_back(r);
        if (total > 10000000L) break;
    }
    p.count = total;
    p.estBytes = (uint64_t)total * kAvgTileBytes;
    if (total > limit) { p.tooMany = true; return p; }
    int z = minZoom;
    for (const auto& r : perZoom) {
        for (const Range& q : r)
            for (int y = q.y0; y <= q.y1; y++)
                for (int x = q.x0; x <= q.x1; x++) p.tiles.push_back({z, x, y});
        z++;
    }
    return p;
}

int deepestZoomFitting(double south, double west, double north, double east, int minZoom, int maxZoom, int limit) {
    int best = minZoom;
    for (int z = minZoom; z <= maxZoom; z++) {
        if (planArea(south, west, north, east, minZoom, z, limit).tooMany) break;
        best = z;
    }
    return best;
}

CacheUsage cacheUsage(const std::string& root) {
    CacheUsage u;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(fsPath(root), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->path().extension() != ".png") continue;
        std::error_code e;
        const auto sz = it->file_size(e);
        if (e) continue;
        u.files++; u.bytes += sz;
    }
    return u;
}

void touchTile(const std::string& path) {
    std::error_code ec;
    fs::last_write_time(fsPath(path), fs::file_time_type::clock::now(), ec);
}

long enforceCap(const std::string& root, uint64_t capBytes) {
    if (capBytes == 0) return 0;
    struct F { fs::file_time_type t; uint64_t sz; fs::path p; };
    std::vector<F> files;
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(fsPath(root), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->path().extension() != ".png") continue;
        std::error_code e;
        const auto sz = it->file_size(e); if (e) continue;
        const auto t = it->last_write_time(e); if (e) continue;
        files.push_back({t, sz, it->path()});
        total += sz;
    }
    if (total <= capBytes) return 0;
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.t < b.t; });
    long removed = 0;
    for (const F& f : files) {
        if (total <= capBytes) break;
        std::error_code e;
        if (fs::remove(f.p, e) && !e) { total -= f.sz; removed++; }
    }
    // empty x and z folders
    std::vector<fs::path> dirs;
    ec.clear();
    for (auto it = fs::recursive_directory_iterator(fsPath(root), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_directory(ec)) dirs.push_back(it->path());
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) { return a.string().size() > b.string().size(); });
    for (const auto& d : dirs) { std::error_code e; if (fs::is_empty(d, e) && !e) fs::remove(d, e); }
    return removed;
}

}
