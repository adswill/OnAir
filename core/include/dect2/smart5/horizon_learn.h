// Learned horizon and next good pass (3.9, docs/SMART5-sat-3.9.md item 16). A pass predictor assumes a flat 0 deg (or a fixed
// 10 deg) horizon, but trees, buildings and local noise decide where your station really decodes. Every decoded pass teaches,
// per 30 deg azimuth sector, the elevation at which frames start to arrive; the next passes are then scored by the minutes they
// will spend above that learned horizon, and the best one to record is picked.
// Feed: sample(az, el, decoding) during passes (once a second, or per frame window); predict() takes a pass's track.
#pragma once
#include <array>
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

struct TrackPoint { double t = 0, azDeg = 0, elDeg = 0; };
struct PassScore { int index = -1; double decodeMin = 0, maxElDeg = 0; };

class HorizonLearn {
public:
    static constexpr int kSectors = 12, kBins = 91;   // 30 deg sectors, 1 deg elevation bins
    explicit HorizonLearn(double defaultHorizonDeg = 10) : def_(defaultHorizonDeg) {}
    void sample(double azDeg, double elDeg, bool decoding);
    double horizon(double azDeg) const;              // learned (or the default while a sector has too little data)
    bool learned(double azDeg) const;
    double predictMinutes(const std::vector<TrackPoint>& pass) const;
    PassScore best(const std::vector<std::vector<TrackPoint>>& passes) const;
    std::string save() const;
    void load(const std::string& s);
private:
    static int sector(double az);
    struct Bin { float ok = 0, all = 0; };
    std::array<std::array<Bin, kBins>, kSectors> h_{};
    double def_;
};

} // namespace smart5
} // namespace dect2
