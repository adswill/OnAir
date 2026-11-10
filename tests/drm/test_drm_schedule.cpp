// The DRM broadcast schedule reader (drm_schedule.h): the page's JSON and medium wave table, times past midnight, day lists and ranges,
// frequency texts with dates in them, what is on air at a given UTC time, the next start, and pages it cannot read.
// The page below is a made-up one in the layout of www.drm.org's schedule page (no real schedule data is kept in the repository).
// Optional: DRM_SCHEDULE_PAGE=<a saved copy of the real page> also reads that one and prints what is on air now.
#include "dect2/drm/drm_schedule.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace dect2::drmsched;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char* kPage = R"HTML(<html><head><title>x</title></head><body>
<div id="sw-broadcasts"></div>
<script id="drm-sw-data" type="application/json">
{ "schemaVersion": 1, "season": "B26", "validFrom": "2026-10-25", "validTo": "2027-03-27", "updatedAt": "2026-10-26",
  "sources": {"a": {"name": "x"}},
  "records": [
    {"id": "t1", "region": "region_1", "values": ["0900-1400", "Test Radio One", "6195", "Daily", "English", "Europe", "Somewhere", "100", "90"], "confidence": "confirmed", "label": "Confirmed"},
    {"id": "t2", "region": "region_2", "values": ["2025-1805", "Night Radio", "6030", "Daily", "Chinese", "Asia", "Elsewhere", "30", "Non Directional"], "confidence": "confirmed"},
    {"id": "t3", "region": "region_1", "values": ["1800-1900", "Twice Weekly", "13730", "Tue & Thu", "Multi", "Africa", "Site", "100", "158"], "confidence": "confirmed"},
    {"id": "t4", "region": "region_4", "values": ["1759-1858 1859-1958", "Two Windows", "11690", "Sun-Fri", "English", "Pacific", "Site", "35", "35"], "confidence": "confirmed"},
    {"id": "t5", "region": "region_1", "values": ["0500-0600", "Seasonal", "5875 (until 30 Aug) 3955 (from 1 Sept)", "Occasionally", "English", "Europe", "Site", "100", "78"], "confidence": "uncertain"},
    {"id": "t6", "region": "region_2", "values": ["0800-1200 2200-0100", "Split", "9655", "Daily", "Chinese", "Asia", "Site", "30", "98"]},
    {"id": "bad", "region": "region_1", "values": ["0900-1000", "", "", "Daily"]},
    {"id": "nofreq", "region": "region_1", "values": ["0900-1000", "No Frequency", "tba", "Daily"]}
  ],
  "archivedRecords": [ {"id": "old", "values": ["0500-0600", "Gone", "3955", "Daily"]} ]
}
</script>
<div id="drm-india-schedule"><table><thead><tr><th>Location</th><th>DRM frequency</th><th>Max. AM power</th></tr></thead>
<tbody>
<tr class="region_3"><td><span class="in-city">Town A</span><span class="in-state">State &amp; Co</span></td><td>549 kHz</td><td>100 kW</td></tr>
<tr class="region_3"><td><span class="in-city">Town B</span><span class="in-state">Other</span></td><td>1368 kHz</td><td>20 kW</td></tr>
</tbody></table></div>
</body></html>)HTML";

// 2026-11-10 is a Tuesday
static std::time_t at(int day, int hh, int mm) { return (std::time_t)1794268800 + (std::time_t)(day - 10) * 86400 + hh * 3600 + mm * 60; }

static const Broadcast* find(const Schedule& s, const char* name) {
    for (const auto& b : s.items) if (b.station == name) return &b;
    return nullptr;
}

int main() {
    // the reference time: 2026-11-10 00:00 UTC is a Tuesday
    {
        std::tm t{}; std::time_t z = at(10, 0, 0);
#ifdef _WIN32
        gmtime_s(&t, &z);
#else
        gmtime_r(&z, &t);
#endif
        CHECK(t.tm_wday == 2 && t.tm_mday == 10 && t.tm_mon == 10, "reference date: wday %d mday %d mon %d", t.tm_wday, t.tm_mday, t.tm_mon);
    }
    Schedule s;
    std::string err;
    CHECK(parsePage(kPage, s, err), "page not read: %s", err.c_str());
    CHECK(s.season == "B26" && s.validFrom == "2026-10-25" && s.validTo == "2027-03-27" && s.updatedAt == "2026-10-26", "season fields: %s %s %s %s",
          s.season.c_str(), s.validFrom.c_str(), s.validTo.c_str(), s.updatedAt.c_str());
    CHECK(s.items.size() == 8, "%zu entries (6 short wave + 2 medium wave expected; the empty, frequency-less and archived ones skipped)", s.items.size());
    CHECK(!find(s, "Gone") && !find(s, "No Frequency"), "archived or frequency-less entry listed");

    const Broadcast* one = find(s, "Test Radio One");
    CHECK(one && one->khz.size() == 1 && one->khz[0] == 6195 && one->windows.size() == 1 && one->windows[0].startMin == 540 && one->windows[0].endMin == 840 &&
          one->language == "English" && one->region == "region_1" && one->confidence == "confirmed", "first entry fields");
    if (one) {
        CHECK(onAirAt(*one, at(10, 9, 0)) == OnAir::Yes && onAirAt(*one, at(10, 13, 59)) == OnAir::Yes, "on air at the window's edges inside");
        CHECK(onAirAt(*one, at(10, 8, 59)) == OnAir::No && onAirAt(*one, at(10, 14, 0)) == OnAir::No, "off just outside");
        CHECK(minutesToNext(*one, at(10, 8, 0)) == 60 && minutesToNext(*one, at(10, 15, 0)) == 18 * 60 && minutesToNext(*one, at(10, 10, 0)) == 0, "next start");
    }
    // past midnight: 20:25 until 18:05 the next day
    const Broadcast* night = find(s, "Night Radio");
    CHECK(night && night->windows.size() == 1 && night->windows[0].startMin == 20 * 60 + 25 && night->windows[0].endMin == 18 * 60 + 5, "wrap window");
    if (night) {
        for (int hh : {20, 23, 0, 5, 12, 18}) {
            const int mm = hh == 20 ? 30 : hh == 18 ? 4 : 0;
            CHECK(onAirAt(*night, at(10, hh, mm)) == OnAir::Yes, "night radio at %02d:%02d", hh, mm);
        }
        CHECK(onAirAt(*night, at(10, 19, 0)) == OnAir::No && onAirAt(*night, at(10, 18, 5)) == OnAir::No && onAirAt(*night, at(10, 20, 24)) == OnAir::No, "night radio gap");
    }
    // days: Tue & Thu; 10 Nov is a Tuesday
    const Broadcast* tt = find(s, "Twice Weekly");
    if (tt) {
        CHECK(onAirAt(*tt, at(10, 18, 30)) == OnAir::Yes, "Tuesday");
        CHECK(onAirAt(*tt, at(11, 18, 30)) == OnAir::No, "Wednesday");
        CHECK(onAirAt(*tt, at(12, 18, 30)) == OnAir::Yes, "Thursday");
        CHECK(minutesToNext(*tt, at(10, 19, 0)) == 47 * 60, "next is Thursday 18:00: %d min", minutesToNext(*tt, at(10, 19, 0)));
    } else CHECK(false, "Tue & Thu entry missing");
    // two windows and a day range that wraps (Sun-Fri: not Saturday)
    const Broadcast* two = find(s, "Two Windows");
    CHECK(two && two->windows.size() == 2, "two windows");
    if (two) {
        CHECK(onAirAt(*two, at(10, 19, 30)) == OnAir::Yes && onAirAt(*two, at(10, 17, 58)) == OnAir::No, "second window");
        CHECK(onAirAt(*two, at(14, 18, 0)) == OnAir::No && onAirAt(*two, at(15, 18, 0)) == OnAir::Yes, "Saturday off, Sunday on");
    }
    // a frequency text with dates: both frequencies, not the day numbers; "Occasionally" = maybe
    const Broadcast* seas = find(s, "Seasonal");
    CHECK(seas && seas->khz.size() == 2 && seas->khz[0] == 5875 && seas->khz[1] == 3955, "seasonal frequencies");
    if (seas) CHECK(onAirAt(*seas, at(10, 5, 30)) == OnAir::Maybe && onAirAt(*seas, at(10, 7, 0)) == OnAir::No, "occasional = maybe inside the window");
    // two windows, the second past midnight
    const Broadcast* split = find(s, "Split");
    if (split) {
        CHECK(split->windows.size() == 2 && onAirAt(*split, at(10, 0, 30)) == OnAir::Yes && onAirAt(*split, at(10, 1, 0)) == OnAir::No && onAirAt(*split, at(10, 9, 0)) == OnAir::Yes, "split windows");
    } else CHECK(false, "split entry missing");
    // medium wave: no times
    int mw = 0;
    for (const auto& b : s.items) if (b.mediumWave) {
        mw++;
        CHECK(b.windows.empty() && onAirAt(b, at(10, 12, 0)) == OnAir::Unknown && minutesToNext(b, at(10, 12, 0)) == -1, "medium wave has no times");
        CHECK(b.region == "region_3" && !b.khz.empty(), "medium wave fields");
    }
    CHECK(mw == 2, "%d medium wave rows", mw);
    for (const auto& b : s.items) if (b.mediumWave && b.khz[0] == 549) CHECK(b.site == "Town A State & Co", "cell text: '%s'", b.site.c_str());

    // the pieces on their own
    std::vector<Window> w;
    CHECK(!parseTimes("", w) && !parseTimes("tba", w) && !parseTimes("12345-67890", w), "no times");
    CHECK(parseTimes("0000-2400", w) && w.size() == 1 && w[0].startMin == 0 && w[0].endMin == 1440, "whole day");
    CHECK(parseKhz("13740").size() == 1 && parseKhz("26 kHz").empty() && parseKhz("40000").empty() && parseKhz("7325 / 9500").size() == 2, "kHz");
    CHECK(dayMatches("Daily", 3) == 1 && dayMatches("Days unconfirmed", 3) == -1 && dayMatches("Various", 0) == -1, "daily / unknown");
    CHECK(dayMatches("Mon-Fri", 0) == 0 && dayMatches("Mon-Fri", 1) == 1 && dayMatches("Mon-Fri", 5) == 1 && dayMatches("Mon-Fri", 6) == 0, "Mon-Fri");
    CHECK(dayMatches("Fri-Mon", 6) == 1 && dayMatches("Fri-Mon", 0) == 1 && dayMatches("Fri-Mon", 3) == 0, "range over the week end");
    CHECK(dayMatches("Sat, Sun", 6) == 1 && dayMatches("Sat, Sun", 1) == 0, "list");
    CHECK(std::string(regionName("region_4")) == "Pacific", "region name");
    // pages it cannot read
    Schedule e;
    CHECK(!parsePage("<html>nothing</html>", e, err) && !err.empty(), "empty page");
    CHECK(!parsePage("<script id=\"drm-sw-data\" type=\"application/json\">{ broken</script>", e, err), "broken JSON");
    std::string cut = kPage; cut.resize(cut.size() / 3);
    Schedule c;
    parsePage(cut, c, err);   // a page cut short must not crash; whatever it reads is complete entries
    for (const auto& b : c.items) CHECK(!b.station.empty() && !b.khz.empty(), "cut page: half an entry");

    if (const char* p = getenv("DRM_SCHEDULE_PAGE")) {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        Schedule r;
        const bool ok = parsePage(ss.str(), r, err);
        int sw = 0, now = 0;
        for (const auto& b : r.items) { if (!b.mediumWave) sw++; if (onAirAt(b, std::time(nullptr)) == OnAir::Yes) now++; }
        printf("real page: %s, season %s updated %s, %zu entries (%d short wave), %d on air now\n", ok ? "read" : err.c_str(), r.season.c_str(), r.updatedAt.c_str(), r.items.size(), sw, now);
        CHECK(ok && sw > 0, "the saved real page was not read");
    }
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("drm schedule: all passed\n");
    return 0;
}
