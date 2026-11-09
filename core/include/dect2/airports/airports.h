// Airport table: ICAO / IATA code, name, city, country, position, and the radio frequencies of each airport (tower, ground, ATIS, approach ...).
//
// The built-in table is data/airports/airports.tsv, built by tools/gen_airports.py from OurAirports (public domain, https://ourairports.com/data/)
// and compiled in as text. A newer copy can be imported from the two OurAirports CSV files (importOurAirports), which the app keeps in the
// user's settings folder and prefers to the built-in one.
//
// Frequencies are whole kHz: 118500 is the 25 kHz channel 118.500; 118505 is the 8.33 kHz channel named 118.505 (the channel 118.500 MHz with
// 8.33 spacing), 118510 is 118.50833 MHz. The same convention as the airband mode's channel names (airband_rx.h, airbandParse).
#pragma once
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "dect2/airband/airband_rx.h"

namespace dect2 {

struct AirportFreq {
    std::string type;        // TWR GND ATIS APP DEP CTAF UNICOM CLD AFIS FSS AWOS ASOS OPS ... (upper case, as OurAirports writes them)
    std::string desc;        // the description, "" when it only repeats the type
    int khz = 0;
    bool airband() const { return khz >= 118000 && khz <= 137000; }
};

struct Airport {
    std::string icao;        // ICAO code, or the local identifier where the airport has none
    std::string iata;
    std::string name, city, country;   // country: two letter ISO code
    double latDeg = 0, lonDeg = 0;
    int elevFt = 0;
    char kind = 'S';         // L large, M medium, S small, W seaplane base
    bool scheduled = false;  // has scheduled airline service
    std::vector<AirportFreq> freqs;
    size_t airbandCount() const { size_t n = 0; for (const auto& f : freqs) n += f.airband(); return n; }
};

// The name of a channel row found by searching for an airport code. The label carries the ICAO code, so a search by the IATA code (ASM finds HHAS)
// says why the row is there (#411).
inline std::string airportRowName(const Airport& a, const std::string& label, const std::string& query) {
    if (a.iata.empty() || query.size() != a.iata.size()) return label;
    for (size_t i = 0; i < query.size(); i++) if (std::toupper((unsigned char)query[i]) != std::toupper((unsigned char)a.iata[i])) return label;
    return label + " (" + a.iata + ")";
}

struct AirportReport { size_t airports = 0, freqs = 0, skipped = 0; std::vector<std::string> log; };

// A frequency in MHz as typed in a data file -> whole kHz on the raster; 0 when it is not a usable radio frequency. In the airband (118 - 137 MHz)
// a value on neither the 25 kHz nor the 8.33 kHz raster is moved to the nearest 25 kHz channel when it is within 5 kHz of one, else rejected.
int airportKhz(double mhz);
std::string airportTypeName(const std::string& type);                  // "TWR" -> "Tower"
double airportDistanceKm(double lat1, double lon1, double lat2, double lon2);   // great circle

struct AirportHit { size_t index = 0; double km = -1; };   // km: distance from the position given to search / near, -1 when none was given

const char* builtinAirportText();   // data/airports/airports.tsv as compiled in (Tools > Data packs exports it)

class AirportDb {
public:
    AirportReport parseTsv(const std::string& text);                    // replaces the contents; damaged lines are skipped and counted
    AirportReport importOurAirports(const std::string& airportsCsv, const std::string& frequenciesCsv);   // replaces the contents
    std::string toTsv() const;                                          // the table format of data/airports/airports.tsv
    static const AirportDb& builtin();

    size_t size() const { return v_.size(); }
    bool empty() const { return v_.empty(); }
    const std::vector<Airport>& all() const { return v_; }
    const Airport* find(const std::string& code) const;                  // ICAO or IATA code, any case
    // Text search: ICAO / IATA code first (exact, then prefix), then name and city words (all words must match, as prefix of a word, then anywhere).
    // From a position, equal matches are ordered by distance; larger airports before smaller ones otherwise.
    std::vector<AirportHit> search(const std::string& text, size_t maxHits = 40, const double* fromLatLon = nullptr, bool withFreqOnly = false) const;
    // Airports within radiusKm of a position, nearest first. Only those with at least one airband frequency when airbandOnly.
    std::vector<AirportHit> nearby(double latDeg, double lonDeg, double radiusKm, size_t maxHits = 200, bool airbandOnly = true) const;

private:
    std::vector<Airport> v_;
};

// The channels of an airport for the airband mode: every frequency in 118 - 137 MHz, once each (a frequency with two roles gets both names),
// labelled "EGLL Tower", "EGLL ATIS", "EGLL Ground 2" ...; sorted by frequency. skipped counts the frequencies outside the airband.
std::vector<AirbandChannel> airportAirbandChannels(const Airport& a, int* skipped = nullptr);

} // namespace dect2
