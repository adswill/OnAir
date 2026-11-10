// The DRM broadcast schedule of the DRM Association (www.drm.org/drm-in-the-world/broadcast-schedule/): read from the page the app fetches
// when the user asks (the page says "All rights reserved", so the schedule is never bundled), kept as a cached copy, credited where it is shown.
// The page carries the short wave schedule as JSON (<script id="drm-sw-data">: season, validity, records of [time, station, kHz, days,
// language, target, site, kW, azimuth]) and the medium wave transmitters of All India Radio as a table (location, kHz, power; no times).
#pragma once
#include <ctime>
#include <string>
#include <vector>

namespace dect2::drmsched {

constexpr const char* kUrl = "https://www.drm.org/drm-in-the-world/broadcast-schedule/";
constexpr const char* kCredit = "DRM broadcast schedule: Digital Radio Mondiale Association, www.drm.org";

struct Window { int startMin = 0, endMin = 0; };   // UTC minutes of the day; endMin <= startMin: past midnight

struct Broadcast {
    std::string id, region, station, days, language, target, site, power, azimuth, confidence, label, note;
    std::string timeText, freqText;   // as published
    std::vector<Window> windows;      // empty: no times published (medium wave list)
    std::vector<double> khz;          // every frequency the entry names ("5875 (until 30 Aug) 3955 (from 1 Sept)": both)
    bool mediumWave = false;          // from the medium wave table
};

struct Schedule {
    std::string season, validFrom, validTo, updatedAt;
    std::vector<Broadcast> items;
    bool ok() const { return !items.empty(); }
};

// Reads the page (HTML as fetched). False and err when neither the JSON nor the table is there.
bool parsePage(const std::string& html, Schedule& out, std::string& err);

// "0900-1400", "1759-1858 1859-1958", "2025-1805" (past midnight); false when nothing in it is a time
bool parseTimes(const std::string& s, std::vector<Window>& out);
// every number in the frequency text that is a plausible broadcast frequency in kHz (100 .. 30000)
std::vector<double> parseKhz(const std::string& s);

// Whether the days text includes this weekday (0 = Sunday .. 6 = Saturday): 1 yes, 0 no, -1 not known ("Occasionally", "Days unconfirmed", ...)
int dayMatches(const std::string& days, int weekday);

enum class OnAir { No, Yes, Maybe, Unknown };   // Maybe: inside a window on a day the schedule does not pin down; Unknown: no times published
OnAir onAirAt(const Broadcast& b, std::time_t utc);
// minutes until the next start (0 while on air; -1: no times)
int minutesToNext(const Broadcast& b, std::time_t utc);
const char* regionName(const std::string& region);   // "region_1" -> "Europe, Middle East, Africa"

}  // namespace dect2::drmsched
