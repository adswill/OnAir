// RDS alternative frequencies: which AF is best now, and following the station to it (as a car radio does) with the PI proved after the retune.
// A pure state machine: the app gives it the telemetry and the level of each AF from the spectrum, and tunes when it says so.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct FmAfCandidate {
    double mhz = 0;
    bool inSpan = false;         // the level was measured in the current capture (no retune); false: unknown until a probe
    float levelDb = -200;
    bool rejected = false;       // the PI did not match the last time: a regional variant or another station
};

struct FmAfCommand {
    enum Type { None, Tune, Back } type = None;
    double mhz = 0;              // Tune or Back: where to go
    bool confirmed = false;      // Tune done and PI proved: the follow is complete
    std::string why;             // plain words for the status line
};

struct FmAfConfig {
    bool autoFollow = false;     // leave the station by itself when it gets weak (off: only requestBest)
    float snrTriggerDb = 25;     // only below this SNR is an auto-follow considered
    float marginDb = 6;          // the AF must be this much stronger than the current one
    double confirmSec = 2.5;     // time for the PI to arrive after a retune
    double holdSec = 20;         // no further retune this soon after one
    double probeEverySec = 60;   // auto-follow: at most one blind probe (AF outside the capture, about 1 s away) per this long
    double rejectSec = 600;      // a wrong-PI frequency is not tried again for this long
};

class FmAfFollow {
public:
    void setConfig(const FmAfConfig& c) { cfg_ = c; }
    const FmAfConfig& config() const { return cfg_; }
    void reset();                     // a station change by the user
    // Once per telemetry update. levelOf(mhz, db) fills the level and returns true when mhz lies inside the capture.
    FmAfCommand update(double now, double tunedMhz, int pi, float snrDb, float levelDb, const std::vector<double>& af, const std::function<bool(double, float&)>& levelOf);
    void requestBest();               // one click: go to the strongest AF now (the PI is still proved)
    bool busy() const { return state_ != Idle; }
    const std::vector<FmAfCandidate>& candidates() const { return cand_; }
private:
    enum State { Idle, Confirming } state_ = Idle;
    FmAfConfig cfg_;
    std::vector<FmAfCandidate> cand_;
    std::map<int, double> rejected_;  // MHz * 10 -> until
    double origin_ = 0, target_ = 0, since_ = 0, holdUntil_ = 0, lastProbe_ = -1e18;
    int wantPi_ = 0;
    float originSnr_ = 0;
    bool request_ = false, probing_ = false;
    size_t probeIdx_ = 0;
};

} // namespace dect2
