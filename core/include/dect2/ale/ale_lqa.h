// ALE link-quality memory (3.7, docs/SMART-maritime-3.7.md): every call or sounding the receiver hears from a station is a measurement of the path
// station -> this receiver on that frequency at that time of day. The store keeps them per station x frequency x hour of the UTC day (with a half life of
// two weeks, so the table follows the seasons and the sunspot cycle) and answers "which frequency reaches that station best now": the score blends what
// the hour of the day has usually given (the hour before and after count too) with what was heard in the last half hour, weighted by how much data there is,
// and the HF propagation model of the beacon monitor (hfbeacon_prop.h) adjusts it when the band is known to be open or closed right now.
// Pure logic, no clock of its own, thread safe; save() / load() keep it between runs.
#pragma once
#include "dect2/hfbeacon/hfbeacon_prop.h"
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

struct AleLqaObs {
    double utc = 0;
    std::string station;             // the address that was heard
    double hz = 0;                   // dial frequency
    float snrDb = 0;                 // in 3 kHz
    float ber = 0;                   // fraction of word bits that were not unanimous in the 3-fold vote (0 .. 1)
    bool sounding = false;           // a sounding, else a call
};

struct AleLqaRank {
    std::string station;
    double hz = 0;
    double score = 0;                // 0 .. 100, higher is a better path now
    float snrDb = 0;                 // the estimate the score is built from
    float ber = 0;
    double obs = 0;                  // weight of the data behind it (decayed count)
    double lastUtc = 0;
    bool recent = false;             // heard in the last 30 minutes
    int propState = -1;              // hfp::State of its band now, -1 unknown
    std::string why;                 // "usually +14 dB at this hour (9 heard), heard 12 min ago, band open"
};

class AleLqa {
public:
    struct Config {
        double halfLifeDays = 14;
        double recentSec = 1800;
        double freqStepHz = 100;     // frequencies closer than this are the same channel
        size_t maxStations = 500;
        size_t maxChannels = 40;     // per station
    };
    AleLqa() = default;
    explicit AleLqa(const Config& c) : cfg_(c) {}
    static AleLqa& global();

    void add(const AleLqaObs& o);
    // Channels of one station, best first. prop: the band states of the propagation model (hfp::Model::now) or nullptr.
    std::vector<AleLqaRank> rank(const std::string& station, double nowUtc, const std::vector<hfp::BandNow>* prop = nullptr) const;
    // The best channel of every station (the table the screen shows), stations ordered by last heard
    std::vector<AleLqaRank> best(double nowUtc, const std::vector<hfp::BandNow>* prop = nullptr) const;
    std::vector<std::string> stations() const;
    // the 24 hour profile of a channel: mean SNR per UTC hour (weight 0 = no data)
    struct Hour { double w = 0; float snrDb = 0, ber = 0; };
    std::vector<Hour> profile(const std::string& station, double hz, double nowUtc) const;
    std::string save() const;
    void load(const std::string& text);
    void clear();
    uint64_t total() const;

private:
    struct Bin { double w = 0, snr = 0, ber = 0, stamp = 0; };       // weighted sums, decayed to `stamp`
    struct Chan { double hz = 0; Bin bin[24]; double lastUtc = 0; float lastSnr = 0, lastBer = 0; };
    struct Stn { std::map<long, Chan> ch; double lastUtc = 0; };
    double decay(double dt) const;
    static Bin decayed(const Bin& b, double now, double halfLife);
    AleLqaRank rankOf(const std::string& st, const Chan& c, double now, const std::vector<hfp::BandNow>* prop) const;
    long key(double hz) const { return (long)(hz / cfg_.freqStepHz + 0.5); }

    mutable std::mutex mu_;
    Config cfg_;
    std::map<std::string, Stn> st_;
    uint64_t total_ = 0;
};

} // namespace dect2
