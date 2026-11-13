// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pagers: the smart layer on top of the decoded messages (3.7). Pure C++ on PagerMessage, no radio, no clock of its own.
//   - text repair (#13): a POCSAG page that lost a code word keeps the bits that survived; the lost characters are filled in from the words that
//     capcode (and the channel) sent before, only when one reading clearly beats every other. Guessed characters are always marked.
//   - message intelligence (#12): positions read out of the text (decimal degrees, degrees and minutes, UTM, MGRS) or looked up in a place list the
//     user provides, the fields a capcode's messages have in common (job number, type, address ...), and incidents (pages of several capcodes about
//     the same event). Everything stays in memory on this computer; medical-looking text is left alone unless the privacy option is off.
#pragma once
#include "pager_tel.h"
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace dect2 {
namespace pager {

// ---- positions in text
struct GeoHit {
    double lat = 0, lon = 0;
    std::string kind;            // "coordinates", "UTM" or "MGRS"
    size_t pos = 0;              // where it starts in the text
};
// All positions in the text, in order. Plausibility is checked (latitude, zone, band); text that only looks like numbers gives nothing.
std::vector<GeoHit> findCoordinates(const std::string& text);
// UTM to WGS84; false when out of range. south: southern hemisphere.
bool utmToLatLon(int zone, bool south, double easting, double northing, double& lat, double& lon);
// MGRS (zone, band letter, 100 km square letters, easting and northing in metres inside the square) to WGS84
bool mgrsToLatLon(int zone, char band, char colLetter, char rowLetter, double e, double n, double& lat, double& lon);
// Great-circle distance in km
double distanceKm(double lat1, double lon1, double lat2, double lon2);

struct BrainOptions {
    bool repair = true;          // fill in lost characters (#13)
    bool intel = true;           // fields, positions, incidents (#12)
    bool analyseMedical = false; // false: pages that look medical are neither learned from nor analysed (privacy)
};

struct BrainStats {
    uint64_t repairedPages = 0;  // pages with at least one character filled in
    uint64_t guessedChars = 0;
    uint64_t lostChars = 0;      // characters left as '?'
    uint64_t pagesWithPos = 0;
    uint64_t sensitivePages = 0; // skipped for privacy
};

class PagerBrain {
public:
    BrainOptions opt;
    BrainStats stats;
    // The user's place list: lines "name,lat,lon" ('#' starts a comment, a tab works as well). A page that names one of them gets its position.
    int loadPlaces(const std::string& text);
    void clearPlaces();
    // Learn from the message, repair it, read its structure. wall: seconds since 1970 (tests give their own clock).
    void process(PagerMessage& m, int64_t wall);
    std::vector<PagerIncident> incidents() const;   // incidents with at least two pages, newest first
    void reset();                                   // forgets everything learned and every incident (not the place list)

    static bool looksMedical(const std::string& text);

private:
    struct Profile {
        std::unordered_map<std::string, uint32_t> words;    // the words this capcode sent, with counts
        uint32_t n = 0;                                      // clean messages seen
        int64_t last = 0;
        double latSum = 0, lonSum = 0; int posN = 0;         // positions seen (the capcode's usual area)
        double latMin = 90, latMax = -90, lonMin = 180, lonMax = -180;
        // segment roles: how the messages split, and what each segment holds
        struct Seg { uint32_t n = 0, digits = 0, addr = 0, pos = 0; std::unordered_map<std::string, uint32_t> vals; };
        char delim = 0;                                      // the splitting character (0 = none learned yet)
        std::vector<Seg> segs;
        std::unordered_map<int, uint32_t> nums;              // how many numbers of each length (digits) the capcode sent
        std::unordered_map<std::string, uint32_t> first;     // first word of each message
        uint32_t delimVotes[4] = {0, 0, 0, 0};
    };
    struct Recent {
        int64_t wall = 0; uint32_t cap = 0; bool flex = false; uint32_t incident = 0;
        std::vector<std::string> keys;                       // job numbers and the like
        std::vector<std::string> toks;                       // words of 3 or more letters, lowercase, sorted
        bool hasPos = false; double lat = 0, lon = 0;
    };
    struct Place { std::string lower, name; double lat, lon; };
    std::unordered_map<uint64_t, Profile> prof_;
    std::unordered_map<std::string, uint32_t> global_;
    std::unordered_map<int, uint32_t> globalNum_;
    std::vector<Place> places_;
    std::deque<Recent> recent_;
    std::vector<PagerIncident> inc_;
    uint32_t nextIncident_ = 1;

    void repairText(PagerMessage& m, Profile& pr);
    void learn(const std::string& text, Profile& pr);
    void analyse(PagerMessage& m, Profile& pr, int64_t wall);
    void segments(PagerMessage& m, Profile& pr, const std::string& text);
    void assignIncident(PagerMessage& m, int64_t wall);
};

} // namespace pager
} // namespace dect2
