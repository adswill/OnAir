// Meteor scatter hourly rate against the shower calendar (3.9, docs/SMART5-science-3.9.md item 26). Radio meteor counts follow a
// daily curve (sporadics peak around 06 local time) plus the active showers, whose rate scales with the radiant's elevation.
// From the hourly counts this learns the station's sporadic curve (a mean and two harmonics of the local solar hour) and its sensitivity to shower meteors, gives the
// expected count for each hour, and flags an hour well above it as a possible outburst (3 sigma Poisson and 1.5 x expected).
// Shower list: dates, ZHR and radiants from the IMO working list of visual meteor showers (approximate, rounded); activity
// falls off as 10^(-B |days from peak|).
#pragma once
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

struct ShowerNow { std::string name; double zhrNow = 0, radiantElDeg = 0; };

// the showers active at t with their activity and radiant elevation from (lat, lon)
std::vector<ShowerNow> showersAt(double unixSec, double latDeg, double lonDeg);

struct MeteorHour { double t = 0; int count = 0; double expected = 0; bool outburst = false; double showerTerm = 0; };

class MeteorRate {
public:
    MeteorRate(double latDeg, double lonDeg) : lat_(latDeg), lon_(lonDeg) {}
    void hour(double hourStartUnix, int count);           // one completed hour
    double expected(double hourStartUnix) const;
    bool ready() const { return h_.size() >= 72; }        // three days
    double sensitivity() const { return k_; }             // counts per hour per ZHR x sin(radiant elevation)
    const std::vector<MeteorHour>& hours() const { return h_; }
    std::string text() const;                             // the last hour
private:
    void refit();
    double showerTerm(double t) const;
    double lat_, lon_;
    std::vector<MeteorHour> h_;
    double sporadic_[24] = {0};
    double k_ = 0;
    double coef_[6] = {0};   // the fitted model: sporadic mean, two harmonics of the local solar hour, shower sensitivity
};

} // namespace smart5
} // namespace dect2
