// Sunrise and sunset marks per VLF path (3.9, docs/SMART5-science-3.9.md item 25). A SuperSID / VLF trace has steps every morning
// and evening as the day-night terminator crosses the path; they look like flares to the eye. For the path from the transmitter
// to the receiver this gives the times the terminator crosses the transmitter, the great-circle midpoint and the receiver, both
// at the ground (Sun at -0.833 deg) and at the D-region height (85 km: the Sun at -9.3 deg there is already shining on the
// layer), and the share of the path's D region that is sunlit at any time.
#pragma once
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

enum class TermPoint { Transmitter, Midpoint, Receiver };

struct TermEvent { double t = 0; TermPoint at = TermPoint::Transmitter; bool sunrise = true; bool dRegion = false; };

struct VlfPath { double txLat = 0, txLon = 0, rxLat = 0, rxLon = 0; };

// events in [dayStart, dayStart + 86400), sorted by time
std::vector<TermEvent> vlfTerminator(const VlfPath& p, double dayStartUnix);
// share (0 - 1) of the path's D region (85 km) in sunlight at t
double vlfSunlitFraction(const VlfPath& p, double t);
void greatCirclePoint(double lat1, double lon1, double lat2, double lon2, double f, double& lat, double& lon);
std::string termText(const TermEvent& e);

} // namespace smart5
} // namespace dect2
