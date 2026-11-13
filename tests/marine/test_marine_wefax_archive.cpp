// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// WEFAX archive rules: names, side-car JSON, station match, oldest-first cleanup that spares starred charts, sort/search, thumbnails.
#include "dect2/marine/wefax_archive.h"
#include "dect2/marine/marine_wefax_stations.h"
#include <cmath>
#include <cstring>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    // names
    const long long t0 = 1792672245LL;   // 2026-10-22 12:30:45 UTC
    const std::string b = wefaxArchiveBase(t0, 7878.1);
    CHECK(b == "wefax_20261022T123045Z_7878.1kHz", "base %s", b.c_str());
    long long pt = 0; double pk = 0;
    CHECK(wefaxArchiveParseBase(b, pt, pk) && pt == t0 && std::fabs(pk - 7878.1) < 1e-9, "parse");
    CHECK(!wefaxArchiveParseBase("holiday.png", pt, pk), "foreign name");
    CHECK(wefaxArchiveBase(0, 3855) == "wefax_19700101T000000Z_3855.0kHz", "epoch");
    CHECK(wefaxArchiveUniqueBase(b, {}) == b, "free");
    CHECK(wefaxArchiveUniqueBase(b, {b}) == b + "_2", "second");
    CHECK(wefaxArchiveUniqueBase(b, {b, b + "_2"}) == b + "_3", "third");

    // side-car round trip
    WefaxArchiveMeta m; m.dialKhz = 7878.1; m.ioc = 576; m.lpm = 120; m.startUtc = t0; m.stopUtc = t0 + 1200; m.width = 1809; m.height = 1500;
    m.station = "DWD \"Pinneberg\""; m.stationId = "DDK"; m.snrDb = 17.5; m.phasingLines = 20; m.endReason = "stop tone"; m.starred = true;
    const std::string js = wefaxMetaToJson(m);
    WefaxArchiveMeta r;
    CHECK(wefaxMetaFromJson(js, r), "parse json");
    CHECK(r.ioc == 576 && r.lpm == 120 && r.startUtc == t0 && r.stopUtc == t0 + 1200 && r.width == 1809 && r.height == 1500, "ints");
    CHECK(r.station == m.station && r.stationId == "DDK" && r.endReason == "stop tone" && r.starred, "strings");
    CHECK(std::fabs(r.dialKhz - 7878.1) < 1e-3 && std::fabs(r.snrDb - 17.5) < 0.05 && r.phasingLines == 20, "numbers");
    CHECK(js.find("2026-10-22T12:30:45Z") != std::string::npos, "iso time");
    CHECK(!wefaxMetaFromJson("{\"mode\":\"navtex\"}", r) && !wefaxMetaFromJson("garbage", r), "foreign json");

    // station match: a carrier or its USB dial
    const WefaxStation* s = wefaxStationForDial(7878.1);
    CHECK(s != nullptr, "7878.1 matches no station");
    CHECK(wefaxStationForDial(7880.0) == s, "carrier also matches");
    CHECK(wefaxStationForDial(1234.5) == nullptr, "no station");

    // cleanup: oldest first, starred spared, only as far as needed
    std::vector<WefaxArchiveEntry> e(5);
    const long long ts[5] = {500, 100, 300, 200, 400};
    for (int i = 0; i < 5; i++) { e[i].startUtc = ts[i]; e[i].bytes = 100; e[i].base = "e" + std::to_string(i); }
    e[1].starred = true;   // the oldest is starred
    auto p = wefaxCleanupPlan(e, 300);   // total 500: drop the two oldest unstarred (idx3 at 200, idx2 at 300)
    CHECK(p.size() == 2 && p[0] == 3 && p[1] == 2, "plan %zu", p.size());
    CHECK(wefaxCleanupPlan(e, 0).empty() && wefaxCleanupPlan(e, 500).empty(), "no limit / fits");
    for (auto& x : e) x.starred = true;
    CHECK(wefaxCleanupPlan(e, 100).empty(), "all starred never deleted");
    e[1].starred = false;
    p = wefaxCleanupPlan(e, 100);
    CHECK(p.size() == 1 && p[0] == 1, "only unstarred goes");

    // sort and search
    std::vector<WefaxArchiveEntry> g(4);
    g[0].base = "a"; g[0].startUtc = 10; g[0].dialKhz = 7878.1; g[0].station = "DWD Pinneberg";
    g[1].base = "b"; g[1].startUtc = 20; g[1].dialKhz = 4609.1; g[1].station = "";
    g[2].base = "c"; g[2].startUtc = 30; g[2].dialKhz = 12000.0; g[2].station = "Northwood";
    g[3].base = "d"; g[3].startUtc = 40; g[3].dialKhz = 4609.1; g[3].station = "Northwood"; g[3].starred = true;
    auto l = wefaxGalleryList(g, WefaxSort::TimeNewest, "");
    CHECK(l.size() == 4 && l[0] == 3 && l[3] == 0, "newest");
    l = wefaxGalleryList(g, WefaxSort::TimeOldest, "");
    CHECK(l[0] == 0 && l[3] == 3, "oldest");
    l = wefaxGalleryList(g, WefaxSort::Station, "");
    CHECK(l[0] == 0 && l[1] == 3 && l[2] == 2 && l[3] == 1, "station order");
    l = wefaxGalleryList(g, WefaxSort::Frequency, "");
    CHECK(l[0] == 3 && l[1] == 1 && l[2] == 0 && l[3] == 2, "frequency");
    l = wefaxGalleryList(g, WefaxSort::TimeNewest, "NORTH");
    CHECK(l.size() == 2, "search station");
    l = wefaxGalleryList(g, WefaxSort::TimeNewest, "4609");
    CHECK(l.size() == 2, "search freq");
    l = wefaxGalleryList(g, WefaxSort::TimeNewest, "", true);
    CHECK(l.size() == 1 && l[0] == 3, "starred only");

    // thumbnails
    int tw, th;
    wefaxThumbSize(1809, 1500, 180, tw, th);
    CHECK(tw == 180 && th == 149, "thumb size %d x %d", tw, th);
    wefaxThumbSize(100, 400, 200, tw, th);
    CHECK(tw == 50 && th == 200, "tall %d x %d", tw, th);
    wefaxThumbSize(50, 40, 200, tw, th);
    CHECK(tw == 50 && th == 40, "never upscaled");
    wefaxThumbSize(1000, 2, 100, tw, th);
    CHECK(tw == 100 && th == 1, "at least 1 px %d x %d", tw, th);
    std::vector<uint8_t> src(8 * 4), out;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) src[y * 8 + x] = x < 4 ? 0 : 200;
    wefaxThumbnail(src.data(), 8, 4, 2, 1, out);
    CHECK(out.size() == 2 && out[0] == 0 && out[1] == 200, "box average");
    wefaxThumbnail(src.data(), 8, 4, 1, 1, out);
    CHECK(out.size() == 1 && out[0] == 100, "mean");
    return testSummary("wefax_archive");
}
