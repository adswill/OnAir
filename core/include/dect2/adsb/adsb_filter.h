// ADS-B filter and search (tar1090's filter box): text over address, callsign, registration, type and operator, an altitude band, squawks, emitter
// categories, ground / airborne, and military addresses. The table and the map show only what matches. docs/ADSB-FILTER-3.9.md.
#pragma once
#include "dect2/adsb/adsb_tel.h"
#include <cstdint>
#include <string>

namespace dect2 {

struct AdsbFilter {
    std::string text;           // words separated by spaces; each must be found (any case) in the address, callsign, registration, type or operator
    int altMinFt = -1, altMaxFt = -1;   // -1: no bound; an aircraft without an altitude fails a band (one on the ground counts as 0 ft)
    std::string squawks;        // "7700", "7500,7600,7700" or a prefix with * ("77*"); empty: any
    std::string categories;     // emitter categories, "A3,A5" or a letter ("B" = every B); empty: any
    int ground = 0;             // 0 any, 1 ground only, 2 airborne only
    bool militaryOnly = false;  // military address blocks (adsbMilitaryAddress)
    bool active() const { return !text.empty() || altMinFt >= 0 || altMaxFt >= 0 || !squawks.empty() || !categories.empty() || ground != 0 || militaryOnly; }
};

// The parts of the registration database the text search also looks at (empty when unknown).
struct AdsbFilterInfo { std::string registration, type, operatorName; };

bool adsbFilterMatch(const AdsbFilter& f, const AdsbAircraft& ac, const AdsbFilterInfo& info);

// Military blocks of the 24-bit address space (spotters' lists, not an ICAO document): US AE0000-AFFFFF, UK 43C000-43CFFF.
bool adsbMilitaryAddress(uint32_t icao);

// Text form for the settings (one line, key=value;...) and back; unknown keys are ignored
std::string adsbFilterToText(const AdsbFilter& f);
AdsbFilter adsbFilterFromText(const std::string& s);

}  // namespace dect2
