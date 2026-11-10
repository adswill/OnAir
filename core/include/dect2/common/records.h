// Records board: the personal best distance per mode (and per band for HF): farthest aircraft, ship, APRS station, mesh node, FT8 / WSPR
// spot ... each with the date, the identity and a few words. Kept on disk. docs/RECORDS-3.9.md.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct Record {
    std::string category;            // "ADS-B", "AIS", "APRS", "Mesh", "FT8", "WSPR", ...
    std::string band;                // "20m" for HF, "" otherwise
    double km = 0;
    std::string id, detail;          // the identity (callsign, hex, MMSI, node) and a few words (altitude, grid, SNR)
    int64_t utc = 0;
};

class RecordsBoard {
public:
    // a distance heard; returns true when it is a new record for its category and band (beaten by at least 0.1 km)
    bool offer(const std::string& category, const std::string& band, double km, const std::string& id, const std::string& detail, int64_t utc);
    std::vector<Record> all() const;                  // by category, then band
    const Record* get(const std::string& category, const std::string& band) const;
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }
    void clear(const std::string& category);          // one category ("" = all)
    std::string serialize() const;                    // one tab-separated line per record
    void parse(const std::string& text);
private:
    std::map<std::string, Record> r_;                 // category|band
    bool dirty_ = false;
};

}  // namespace dect2
