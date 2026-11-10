// DRM best-hour book (3.9, docs/SMART5-broadcast-3.9.md item 10). Shortwave DRM decodes at some hours and not at others. For each
// station and frequency heard, the seconds listened and the seconds with good audio are counted per UTC hour and kept between
// sessions (save()/load(), plain text). bestHours() ranks the hours; nextGood() gives the next UTC hour, from a time, whose
// success rate is at least `minRate` with enough listening behind it.
#pragma once
#include <array>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

struct DrmHourStat { double listenS = 0, goodS = 0; double rate() const { return listenS > 0 ? goodS / listenS : 0; } };

class DrmBestHour {
public:
    // one interval of listening to `station` on `khz`: dt seconds at UTC time `utc`, with good audio or not
    void add(const std::string& station, int khz, double utc, double dt, bool audioOk);
    const std::array<DrmHourStat, 24>* hours(const std::string& station, int khz) const;
    std::vector<int> bestHours(const std::string& station, int khz, double minListenS = 300) const;   // best first
    // UTC seconds of the start of the next good hour after `utc` (within 24 h); -1 when none qualifies
    double nextGood(const std::string& station, int khz, double utc, double minRate = 0.7, double minListenS = 300) const;
    std::string save() const;
    void load(const std::string& text);
    size_t entries() const { return book_.size(); }
private:
    std::map<std::pair<std::string, int>, std::array<DrmHourStat, 24>> book_;
};

} // namespace smart5
} // namespace dect2
