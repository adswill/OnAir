// AIS arrivals board for one port (ShipPlotter, MarineTraffic port view): vessels whose declared destination (message 5 / 24) matches the
// port, sorted by ETA, with the distance and an ETA worked out from speed and distance beside the declared one. docs/AIS-ARRIVALS-3.9.md.
#pragma once
#include "dect2/ais/ais_tel.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct AisPort {
    std::string name;                 // "Klaipeda"
    std::string locode;               // UN/LOCODE "LTKLJ" (optional)
    std::vector<std::string> aliases; // other spellings the crews type ("KLAIPEDA ROADS", "KLP")
    bool hasPos = false;
    double lat = 0, lon = 0;          // for the distance and the worked-out ETA
};

// "LT KLJ", "LTKLJ", "KLAIPEDA", "KLAIPEDA>RIGA" (the part after '>' is the destination), "FOR ORDERS" ...: does the text name the port?
// Case, spaces, punctuation and a country prefix are ignored; one wrong or missing letter is allowed in names of 6 letters or more.
bool aisDestinationMatches(const std::string& destination, const AisPort& port);
std::string aisDestinationNorm(const std::string& s);   // the comparison form: upper case letters and digits, the part after the last '>'

// the declared ETA (month, day, hour, minute; no year) as UTC seconds: the next time at or after now - 1 day it falls on; 0 when not given
int64_t aisEtaUtc(int month, int day, int hour, int minute, int64_t nowUtc);

struct AisArrival {
    uint32_t mmsi = 0;
    std::string name, destination, type;
    int64_t etaDeclared = 0;          // UTC, 0 not given
    int64_t etaWorked = 0;            // now + distance / speed, 0 when there is no position, speed under 0.5 kn or no port position
    double distKm = -1;               // -1 unknown
    float sog = -1;
    float draughtM = 0;
};

// the board: matching vessels, by declared ETA (those without, by worked-out ETA, last)
std::vector<AisArrival> aisArrivals(const std::vector<AisVessel>& vessels, const AisPort& port, int64_t nowUtc);

// the port as one line for the settings ("name|locode|lat|lon|alias;alias") and back
std::string aisPortToText(const AisPort& p);
AisPort aisPortFromText(const std::string& s);

}  // namespace dect2
