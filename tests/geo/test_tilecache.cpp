// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tile keys and paths, the area plan with its hard limit, LRU eviction by modification time under a cap
#include "dect2/geo/tilecache.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace dect2;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void make(const fs::path& p, size_t n, std::chrono::seconds age) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << std::string(n, 'x');
    fs::last_write_time(p, fs::file_time_type::clock::now() - age);
}

int main() {
    // keys
    TileKey k = tileOf(0, 0, 1); CHECK(k.x == 1 && k.y == 1, "origin z1: %d/%d", k.x, k.y);
    k = tileOf(54.687, 25.280, 10); CHECK(k.z == 10 && k.x == 583 && k.y == 325, "Vilnius z10: %d/%d", k.x, k.y);
    k = tileOf(90, 180, 3); CHECK(k.x >= 0 && k.x <= 7 && k.y == 0, "clamped: %d/%d", k.x, k.y);
    CHECK(tileRelPath(5, 3, 7) == "5/3/7.png", "rel");
    CHECK(tilePath("/a/b", 5, 3, 7) == "/a/b/5/3/7.png", "path");
    TileKey q;
    CHECK(parseTilePath("5/3/7.png", q) && q.z == 5 && q.x == 3 && q.y == 7, "parse");
    CHECK(parseTilePath("\\5\\3\\7.png", q), "parse backslash");
    CHECK(!parseTilePath("5/40/7.png", q), "x out of range");
    CHECK(!parseTilePath("5/3/7.jpg", q) && !parseTilePath("junk", q), "not tiles");

    // plan
    AreaPlan p = planArea(54.6, 25.2, 54.8, 25.4, 10, 12);
    CHECK(!p.tooMany && p.count == (long)p.tiles.size() && p.count > 3, "small plan: %ld", p.count);
    CHECK(p.estBytes == (uint64_t)p.count * kAvgTileBytes, "estimate");
    for (const TileKey& t : p.tiles) CHECK(t.z >= 10 && t.z <= 12 && t.x >= 0 && t.y >= 0 && t.x < (1 << t.z), "tile in range");
    AreaPlan w = planArea(-80, -179, 80, 179, 0, 12);
    CHECK(w.tooMany && w.tiles.empty() && w.count > kMaxDownloadTiles, "the world is refused: %ld", w.count);
    AreaPlan one = planArea(10, 10, 10, 10, 5, 5);
    CHECK(one.count == 1 && one.tiles.size() == 1, "a point is one tile");
    AreaPlan dl = planArea(-10, 170, 10, -170, 6, 6);   // across the date line
    CHECK(!dl.tooMany && dl.count > 2, "date line: %ld", dl.count);
    bool lo = false, hi = false;
    for (const TileKey& t : dl.tiles) { if (t.x < 4) lo = true; if (t.x > 60) hi = true; }
    CHECK(lo && hi, "date line has both sides");
    AreaPlan lim = planArea(54.0, 24.0, 55.0, 26.0, 8, 14, 100);
    CHECK(lim.tooMany, "custom limit");
    int d = deepestZoomFitting(54.6, 25.2, 54.8, 25.4, 8, 19);
    CHECK(d >= 10 && d < 19, "deepest fitting: %d", d);
    CHECK(!planArea(54.6, 25.2, 54.8, 25.4, 8, d).tooMany && planArea(54.6, 25.2, 54.8, 25.4, 8, d + 1).tooMany, "deepest is the last that fits");

    // cache
    const fs::path root = fs::temp_directory_path() / "onair_tilecache_test";
    fs::remove_all(root);
    make(root / "3/1/1.png", 1000, std::chrono::seconds(1000));   // oldest
    make(root / "3/1/2.png", 1000, std::chrono::seconds(500));
    make(root / "4/2/2.png", 1000, std::chrono::seconds(100));
    make(root / "4/2/3.png", 1000, std::chrono::seconds(10));     // newest
    make(root / "6/7/7.png", 1000, std::chrono::seconds(2000));   // older than all
    CacheUsage u = cacheUsage(root.string());
    CHECK(u.files == 5 && u.bytes == 5000, "usage %ld %llu", u.files, (unsigned long long)u.bytes);
    CHECK(enforceCap(root.string(), 0) == 0, "cap 0 is no cap");
    CHECK(enforceCap(root.string(), 5000) == 0, "under cap");
    touchTile((root / "3/1/1.png").string());                      // used just now: no longer the oldest
    CHECK(enforceCap(root.string(), 2500) == 3, "three removed");
    CHECK(fs::exists(root / "3/1/1.png") && fs::exists(root / "4/2/3.png"), "newest kept");
    CHECK(!fs::exists(root / "3/1/2.png") && !fs::exists(root / "4/2/2.png"), "oldest removed");
    CHECK(!fs::exists(root / "6"), "empty folder removed");
    u = cacheUsage(root.string());
    CHECK(u.files == 2 && u.bytes <= 2500, "after: %ld", u.files);
    CHECK(enforceCap((root / "nothing").string(), 100) == 0, "missing folder");
    fs::remove_all(root);

    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("tilecache OK\n");
    return 0;
}
