// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MMSI formats (ITU-R M.585): what kind of station a number names and which country the MID belongs to, plus an optional local name table.
#pragma once
#include <string>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dect2 {
namespace marine {

enum class MmsiKind { Unknown, Ship, CoastStation, Group, SarAircraft, Handheld, CraftAssociated, AisSart, Mob, EpirbAis, AtoN, Special };

struct MmsiInfo {
    MmsiKind kind = MmsiKind::Unknown;
    int mid = 0;                 // maritime identification digits, 0 when the number has none
    std::string country;         // "" when the MID is not allocated
    bool partial = false;        // the text holds '~' for digits that could not be read
};

// Digits only (9 of them, '~' allowed for unreadable ones). 00MIDxxxx coast, 0MIDxxxxx group, MIDxxxxxx ship, 111MIDxxx SAR aircraft,
// 8MIDxxxxx handheld VHF with DSC, 98MIDxxxx craft associated with a parent ship, 970/972/974 xxxxxx AIS-SART/MOB/EPIRB-AIS, 99MIDxxxx AtoN.
MmsiInfo mmsiInfo(const std::string& mmsi);
const char* mmsiKindName(MmsiKind k);
const char* midCountry(int mid);        // "" for an unallocated MID

// The local table of ships and coast stations: learned from AIS static data (types 5 and 24) and imported from the user's own TSV / CSV.
// Both the DSC and the AIS views read it. Thread safe.
struct ShipRec {
    std::string mmsi, name, callsign, type, country, station, location;   // station / location: coast stations ("Land's End Radio", "Cornwall")
    bool hasPos = false;
    double lat = 0, lon = 0;
    int source = 0;                                                       // 0 learned from AIS, 1 imported
};

// Digits of an MMSI cell: spaces and a spreadsheet apostrophe dropped; the zeros a spreadsheet cut off coast station / group numbers put back. "" when it is none.
std::string normaliseMmsi(const std::string& cell);
// One coordinate: "51.5", "-3.25", "51 30.5 N", degrees / minutes / seconds with degree and minute marks, "N 51 30", "51-30.5N", "5130N" / "00312W" (ddmm),
// decimal comma. hemi gets N/S/E/W when written. strict (used to find unlabelled columns): a bare integer is not a coordinate.
bool parseCoordinate(const std::string& s, double& deg, char* hemi = nullptr, bool strict = false);
// Latitude and longitude in one string, in either order when hemisphere letters are written: "51 30.5 N 003 12.3 W", "N51 30 E003 12", "51.5/-3.2", "51.5, -3.2".
bool parsePosition(const std::string& s, double& lat, double& lon);

struct ImportStats { int read = 0, added = 0, updated = 0, unchanged = 0, skipped = 0, withPos = 0, coast = 0, ships = 0; };
struct ImportPreview {
    bool ok = false;                              // a column of MMSI numbers was found
    char delim = ',';                             // tab, ';', '|', ',' or ' ' (two or more spaces)
    bool header = false;
    std::vector<std::string> columns;             // per column: "mmsi", "name", "callsign", "type", "country", "station", "location", "lat", "lon", "position" or ""
    std::vector<std::vector<std::string>> rows;   // the first rows of data
    int records = 0;                              // rows with a readable MMSI
};

class ShipDb {
public:
    // What importDetailed() would make of the text: delimiter, header and the column roles found, the first rows. Nothing is stored.
    static ImportPreview preview(const std::string& text, size_t maxRows = 8);
    // As importText(), with the numbers: new MMSIs, changed ones, skipped rows. Merge rule: a non-empty cell of the file replaces the stored value, an empty cell
    // never erases one, a position is replaced only by a new position, the MMSI is the key (one record per number).
    ImportStats importDetailed(const std::string& text);
    // Text with one record per line: tab, semicolon or comma separated, optional header row (mmsi, name, callsign, type, country, station,
    // location, lat, lon in any order). Without a header: MMSI, name, callsign, type, country, location. '#' lines are comments.
    // Returns the number of records read; replaced fields come from the file (imports win over AIS).
    int importText(const std::string& text);
    std::string exportTsv() const;                       // header row + all records, sorted by MMSI
    void learn(const std::string& mmsi, const std::string& name, const std::string& callsign, const std::string& type);   // from AIS; never replaces an imported name
    bool find(const std::string& mmsi, ShipRec& out) const;
    std::string name(const std::string& mmsi) const;     // "" when unknown; a coast station gives "name, location"
    size_t size() const;
    uint64_t version() const;                            // grows with every change
    bool takeDirty();                                    // true once after a change: the app saves the file then
    std::vector<ShipRec> positioned() const;             // the records that have a position (coast stations ...)
    void clear();
    static ShipDb& global();
private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, ShipRec> m_;
    uint64_t ver_ = 0;
    bool dirty_ = false;
};

} // namespace marine
} // namespace dect2
