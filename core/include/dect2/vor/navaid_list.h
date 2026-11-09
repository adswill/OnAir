// The navigation aids near a position, from the OurAirports navaids.csv (public domain, https://ourairports.com/data/), for the "near me" lists of the
// VOR, NDB and DME modes (docs/EASE-3.8.md). Community data, not for navigation. The file does not carry ILS; localizers stay on the VOR list import.
#pragma once
#include <string>
#include <vector>

namespace dect2 {

enum NavaidKind : unsigned { NavaidVor = 1, NavaidNdb = 2, NavaidDme = 4 };

struct Navaid {
    std::string ident, name, type, country;
    double freqMhz = 0;     // what to tune: the VOR or NDB frequency, or the DME's own frequency for a DME
    double lat = 0, lon = 0;
    unsigned kind = 0;
};

// Parses the CSV text; returns the stations read (a line that cannot be read is skipped); err says why none.
int navaidParseCsv(const std::string& text, std::vector<Navaid>& out, std::string& err);

struct NavaidNear { size_t index = 0; double km = 0, bearingDeg = 0; };
// The stations of these kinds within maxKm of the position, nearest first, at most n
std::vector<NavaidNear> navaidNearest(const std::vector<Navaid>& db, double lat, double lon, unsigned kindMask, double maxKm, size_t n);
// The pair of stations whose bearings from the position cross closest to a right angle: returns indices into near, or false with fewer than two usable stations
bool navaidBestCrossing(const std::vector<NavaidNear>& near, int& a, int& b, double* crossDeg = nullptr);

}  // namespace dect2
