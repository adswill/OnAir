// HFDL ground stations near the user and the frequencies they use now (4.0, docs/HFDL-STATIONS-4.0.md). Two sources, merged:
//  - the squitters this receiver hears: every ground station sends an SPDU every 32 s that lists its active frequencies (free, offline),
//  - optionally a community list: hfdl.observer/active.json (the Airframes ground-station API shape: {"ground_stations": [{"id", "name",
//    "last_updated" (UNIX s; negative = seconds before now), "frequencies": {"active": [kHz ...]}, "stratum", "update_source"}], "when"}).
//    The network fetch is in the app (opt-in, app/hfdl/hfdl_stations_ui.cpp); this file has the pure logic only: parsing, merging, the distance
//    and bearing sort, the hints for the band choice and the tune point. No clock of its own: callers pass UNIX seconds.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

// one ground station of a community list
struct HfdlLiveStation {
    int id = 0;
    std::string name;
    double updatedSec = 0;           // UNIX seconds of the list's last update for this station (0: never)
    int stratum = -1;                // the list's source class (hfdlobserver: 0 system table, 1 cache, 2 aircraft performance report, 3 squitter, 4 heard by the list's own receiver); -1 not given
    int updateSource = 0;            // the ground station whose squitter gave the update (0 not given)
    std::vector<int> activeKhz;      // sorted, no duplicates
};
// Parses the list. nowSec turns a negative last_updated (seconds before now) into a time. False (and err) when the text is not such a list;
// stations without an id are skipped, frequencies outside 2 - 30 MHz are dropped.
bool hfdlParseLiveList(const std::string& json, double nowSec, std::vector<HfdlLiveStation>& out, std::string* err = nullptr);

// a station heard directly by this receiver, with the frequencies its last squitter announced
struct HfdlHeardStation { int id = 0; double heardSec = 0; std::vector<int> activeKhz; };

enum HfdlSrc : int { kHfdlSrcLive = 1, kHfdlSrcSquitter = 2 };

struct HfdlNearbyFreq { int khz = 0; int sources = 0; };   // sources: kHfdlSrc* bits that list it now
struct HfdlNearby {
    int id = 0;
    std::string name;
    bool hasPos = false;
    double lat = 0, lon = 0, km = -1, bearingDeg = 0;   // km -1: no user position or no station position
    double updatedSec = 0;           // of the list shown (the newer source)
    int source = 0;                  // kHfdlSrc* of the list shown; both bits when the other source is fresh and agrees on a frequency
    std::vector<HfdlNearbyFreq> freqs;
};

struct HfdlNearbyConfig {
    double maxAgeSec = 3600;         // an update older than this is not "now" (hfdlobserver keeps squitter and cache data an hour too)
    bool keepStale = false;          // true: stations with only stale data are listed with no frequencies (the panel shows them greyed)
};
// The merged list: per station the newer of the fresh live entry and the fresh squitter (a squitter replaces the whole active list, so the
// lists are not unioned); the frequencies the other fresh source lists too get its bit. Sorted nearest first when havePos (stations without
// a known position last), else by id. Positions and names come from the built-in station table (hfdl_proto.h), the name from the list otherwise.
std::vector<HfdlNearby> hfdlNearby(const std::vector<HfdlLiveStation>& live, const std::vector<HfdlHeardStation>& heard, bool havePos, double lat,
                                   double lon, double nowSec, const HfdlNearbyConfig& cfg = HfdlNearbyConfig());

// The active frequencies worth trying from here: those of the stations within maxKm, or when none is that near, of the nearest
// minStations; nearest station first. Without a position: every listed frequency. No duplicates.
std::vector<int> hfdlHintKhz(const std::vector<HfdlNearby>& n, double maxKm = 6000, size_t minStations = 3);

// Where to tune the radio for a band (an index of hfdlBands()) given the frequencies wanted there (most wanted first) and the sample rate:
// the band centre when the usable span (0.8 x rate) holds the whole band, else a centre whose span holds the first wanted frequency and as many
// of the others as fit. MHz. No wanted frequency inside the band: the band centre.
double hfdlTunePointMhz(int band, const std::vector<int>& wantedKhz, double sampleRateHz);

}  // namespace dect2
