// plane-alert-db (sdr-enthusiasts/plane-alert-db, the list planefence / plane-alert use): aircraft by ICAO address with operator, type, tags and a
// category (military, government, police, historic ...). Imported as one watchlist per category, so alerts are switched per category, and
// shown as a column in the ADS-B table. Header (2026-11): $ICAO,$Registration,$Operator,$Type,$ICAO Type,#CMPG,$Tag 1,$#Tag 2,$#Tag 3,Category,$#Link.
#pragma once
#include "dect2/watchlist/watchlist.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2::watchlist {

struct PlaneAlertEntry {
    uint32_t icao = 0;
    std::string registration, operatorName, type, icaoType, cmpg, category;
    std::vector<std::string> tags;
    std::string tagText() const;     // "Cargo, Tactical Transport"
};

// columns are found by their names (the $ and # marks ignored), so the order may change; false + err when there is no ICAO column
bool parsePlaneAlert(const std::string& csv, std::vector<PlaneAlertEntry>& out, std::string& err);
// one aircraft list per category, named "plane-alert: <category>", ids "pa-<n>"; entries are 6-digit hex addresses
std::vector<List> planeAlertLists(const std::vector<PlaneAlertEntry>& v);
bool isPlaneAlertList(const List& l);
// address -> entry, for the table column
std::map<uint32_t, PlaneAlertEntry> planeAlertIndex(const std::vector<PlaneAlertEntry>& v);

}  // namespace dect2::watchlist
