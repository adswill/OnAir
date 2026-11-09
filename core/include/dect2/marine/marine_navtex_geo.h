// Where a NAVTEX / SITOR-B message is talking about: positions written in the text ("56-30N 003-15E", "5630.5N 00315.2E", "N56 30 E003 15",
// "56.5N 3.25E", "56°30'N 003°15'E") and named sea areas ("NORTH SEA", "GULF OF FINLAND" ...). Pure text code, no radio and no clock.
// The area boxes are coarse (a degree or two): they say "this warning is about that sea", not where exactly.
#pragma once
#include <string>
#include <vector>

namespace dect2 {
namespace marine {

struct GeoRef {
    double lat = 0, lon = 0;
    std::string text;                 // the part of the message it was read from
};

struct AreaRef {
    std::string name;                 // "North Sea"
    double latMin = 0, latMax = 0, lonMin = 0, lonMax = 0;
    bool contains(double la, double lo) const { return la >= latMin && la <= latMax && lo >= lonMin && lo <= lonMax; }
};

// Every latitude/longitude pair in the text, in the order written. Degrees of minutes beyond 59.99, latitudes beyond 90 and longitudes beyond 180 are refused.
std::vector<GeoRef> navtexPositions(const std::string& text);
// The named sea areas the text mentions (upper or lower case), each once.
std::vector<AreaRef> navtexAreas(const std::string& text);
const std::vector<AreaRef>& navtexAreaTable();

} // namespace marine
} // namespace dect2
