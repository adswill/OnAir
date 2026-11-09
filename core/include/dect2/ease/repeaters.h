// "Repeaters near me": a list the user imported (OnAir ships none and fetches none), filtered by mode and distance. Pure, no I/O.
#pragma once
#include <string>
#include <vector>

namespace dect2::ease {

struct Repeater { double mhz = 0, lat = 0, lon = 0; std::string call, where, mode; int colourCode = -1; };
// A CSV with a header row naming at least a frequency column and either latitude and longitude or a locator (common spellings are accepted).
// Fills skipped with the rows that had no usable frequency or position. On a file without those columns returns an empty list and sets err.
std::vector<Repeater> parseRepeaterCsv(const std::string& text, int& skipped, std::string& err);
struct RepeaterNear { const Repeater* r = nullptr; double km = 0; };
// Modes: "DMR", "D-STAR", "Fusion", "M17". A row with no mode text is kept (the list may be of that mode only).
bool repeaterMatchesMode(const Repeater& r, const std::string& mode);
std::vector<RepeaterNear> nearestRepeaters(const std::vector<Repeater>& list, const std::string& mode, double lat, double lon, int maxCount = 40, double maxKm = 300);

} // namespace dect2::ease
