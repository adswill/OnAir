// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EUROCONTROL ASTERIX surveillance data: the data block framing and the data items of categories 021 (ADS-B target reports, edition 2.x user
// application profile), 048 (monoradar target reports) and 062 (system track data). Written from the public category descriptions.
// Only what a track table and a map need is read out of each record; every other data item is skipped by its length, and when an item is met whose
// length is not known the rest of that record is left alone (Target::partial). Nothing here talks to the network: see asterix_engine.h.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace asterix {

struct Target {
    int cat = 0;
    int sac = -1, sic = -1;               // data source identifier (item 010)
    bool hasTime = false;
    double todS = 0;                      // time of day (UTC, seconds since midnight) the record gives
    int trackNo = -1;
    int64_t address = -1;                 // 24 bit aircraft address, -1 when none
    std::string callsign;
    int squawk = -1;                      // Mode 3/A code as four octal digits written as decimal (7700 = 07700 octal), -1 when none
    bool squawkInvalid = false;           // the code is flagged not validated or garbled
    bool hasLatLon = false;
    double lat = 0, lon = 0;              // WGS-84 degrees
    bool hasPolar = false;
    double rhoNm = 0, thetaDeg = 0;       // CAT 048 measured position: range from the radar and azimuth from north
    bool hasXy = false;
    double xM = 0, yM = 0;                // Cartesian position in metres from the system or radar origin (CAT 048 item 042 in nautical miles converted)
    bool hasLevel = false;
    double flightLevel = 0;               // FL (hundreds of feet)
    bool hasAltFt = false;
    double altFt = 0;                     // geometric height or altitude, feet
    bool hasGs = false;
    double gsKt = 0;                      // ground speed
    bool hasTrack = false;
    double trackDeg = 0;                  // direction over the ground (heading for CAT 048 item 200)
    bool hasVrate = false;
    double vrateFpm = 0;
    bool hasTas = false;
    double tasKt = 0;
    bool onGround = false;
    std::string kind;                     // what the record is: "SSR + PSR", "Mode S roll-call", "ADS-B", "track", ...
    std::string dep, dst;                 // CAT 062 flight plan related data (item 390): departure and destination aerodromes
    bool partial = false;                 // an item of unknown length ended the decoding of this record
    std::vector<uint8_t> raw;             // the record's bytes (without the category and length)
    std::string items;                    // the data items present, e.g. "010 140 020 040 070 090 220 240 161"
};

struct BlockStats {
    uint32_t blocks = 0, records = 0, badBlocks = 0, partialRecords = 0, otherCategory = 0;
    uint32_t perCat[256] = {};
};

// Split a buffer (a datagram or a stream chunk holding whole data blocks) into blocks and decode the records of the categories above.
// Records of other categories are counted in stats only. Returns the number of bytes consumed (blocks with a bad length stop the walk).
size_t decodeBlocks(const uint8_t* d, size_t n, std::vector<Target>& out, BlockStats* stats = nullptr);

const char* categoryName(int cat);            // "CAT 048 monoradar target reports" ...
std::string squawkText(int squawk);           // "7700" or "----"
std::string addressText(int64_t address);     // "4CA7B1" or ""

// The ICAO 6 bit character set of the aircraft identification items
std::string icao6(const uint8_t* d, size_t nChars);

// ------------------------------------------------------------------ encoding (the test signal and the tests)
// Records of the three categories from a Target (the fields that are set), with the items this decoder reads; blocks are `cat`, a 16 bit length and records.
// CAT 021 uses the edition 2.x layout, 048 edition 1.x, 062 edition 1.x.
std::vector<uint8_t> encodeRecord(const Target& t);
std::vector<uint8_t> wrapBlock(int cat, const std::vector<std::vector<uint8_t>>& records);

}  // namespace asterix
}  // namespace dect2
