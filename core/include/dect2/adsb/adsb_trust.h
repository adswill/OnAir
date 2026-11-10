// Per-aircraft trust score: does this track look like a real aircraft? It compares what an aircraft claims with what the receiver can check by
// itself: the received level against a level-versus-range curve learned from this station's own confirmed aircraft, the radio horizon, jumps of the
// claimed position, the claimed ground speed against the speed of the positions, a registration callsign against the address, one callsign on two
// addresses at once. Each finding is a reason in plain text. A low score is a hint, not proof: a shaded antenna, tropospheric ducting or a
// transponder fault give some of the same symptoms.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dect2 {

enum AdsbTrustFlag : unsigned {
    kTrustTooLoud = 1, kTrustTooWeak = 2, kTrustHorizon = 4, kTrustJump = 8, kTrustSpeed = 16, kTrustRegistration = 32, kTrustDuplicate = 64,
};

struct AdsbTrustObs {
    uint32_t icao = 0;
    double timeSec = 0;
    double lat = 0, lon = 0;
    bool hasRange = false; double rangeNm = 0, bearingDeg = 0;
    double levelDbfs = -120;
    bool hasLevel = true;       // false for positions from another receiver: its level says nothing about this antenna
    bool confirmed = false;     // a global CPR decode (or one chained from it)
    bool relayed = false;       // TIS-B / ADS-R: sent by a ground station, so its level does not follow the claimed range
    bool ground = false;
    bool hasAlt = false; int altFt = 0;
    bool hasGroundSpeed = false; double groundSpeedKt = 0;
    std::string callsign;
};

struct AdsbTrustVerdict {
    int score = -1;             // 0 - 100, -1 not enough seen yet
    unsigned flags = 0;
    std::string why;            // the reasons, comma separated; empty when nothing was found
    bool doubtful() const { return score >= 0 && score < 80; }
    bool ghost() const { return score >= 0 && score < 50; }
};

constexpr double kTrustHoldSec = 180;   // how long a finding counts after it was last seen

class AdsbTrustModel {
public:
    void observe(const AdsbTrustObs& o);
    AdsbTrustVerdict verdict(uint32_t icao, double now) const;
    void tick(double now);              // forget aircraft not heard for ten minutes
    void shiftTime(double dt);
    void clear();
    bool calibrated() const;            // the level curve has enough confirmed aircraft behind it
    int calibrationAircraft() const { return (int)learned_.size(); }
    size_t calibrationSamples() const { return total_; }
    // The expected value of level + 20 log10(range) at a bearing: the percentile p (0..1) of the learned curve
    bool curvePercentile(double bearingDeg, double p, double& out) const;

private:
    static constexpr int kBins = 320, kSectors = 12;     // 0.5 dB bins from -80 dB up
    struct Rec {
        double lastSeen = 0, lastLat = 0, lastLon = 0, lastT = -1e9;
        double baseLat = 0, baseLon = 0, baseT = -1e9, speedSum = 0; int speedN = 0;
        double hit[7] = {-1e9, -1e9, -1e9, -1e9, -1e9, -1e9, -1e9};     // the time of the last finding, per flag
        double seen[7] = {-1e9, -1e9, -1e9, -1e9, -1e9, -1e9, -1e9};    // the last observation of it, reached its count or not
        int count[7] = {};
        int positions = 0;
        double learnT = -1e9;
        std::string callsign;
    };
    void note(Rec& r, int flag, double t, int need = 1);
    struct Hist { uint32_t bin[kBins] = {}; uint32_t n = 0; };
    bool pct(const Hist& h, double p, double& out) const;
    Hist sector_[kSectors], all_;
    size_t total_ = 0;
    std::unordered_map<uint32_t, double> learned_;       // aircraft that fed the curve, with the last time
    std::unordered_map<uint32_t, Rec> rec_;
};

} // namespace dect2
