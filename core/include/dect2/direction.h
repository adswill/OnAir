// Antenna direction finder: a guided sweep. The user points the antenna in a direction and confirms, the receiver measures the
// signal quality for a while, and the finder picks the next direction (N, E, S, W first, then the directions in between, then
// finer steps around the best) until it can say where to point. For omnidirectional antennas it compares places instead.
#pragma once
#include "channel.h"
#include <string>
#include <vector>

namespace dect2 {

enum class AntennaKind { Directional, Dipole, Omni };

// What the receiver says about the signal at one moment
struct DirSample {
    double qualityPct = 0;        // QualityMeter percent (0 when not locked)
    double snrDb = 0;
    bool locked = false;          // the transport stream is being decoded
    double occupancyDb = 0;       // in-band minus out-of-band power: still meaningful when nothing locks
    double lossPct = 0;           // share of lost FEC blocks / packets (0..100)
    MultipathLevel multipath = MultipathLevel::Unknown;
    double clipFraction = 0;
};

struct DirResult {
    double heading = 0;           // degrees clockwise from north (the user's own reference); spot index for omni antennas
    std::string label;            // "N", "NE", ... or "Spot 2"
    double score = 0;             // 0..100
    double qualityPct = 0, snrDb = 0, lockFraction = 0, lossPct = 0, occupancyDb = 0;
    MultipathLevel multipath = MultipathLevel::Unknown;
    bool overload = false;
    int samples = 0;
};

struct DirRecommendation {
    bool valid = false;
    double heading = 0;           // best estimate (interpolated), degrees
    std::string label;            // compass name of the heading
    double confidence = 0;        // 0..1
    bool flat = false;            // the signal is about the same in every direction
    bool symmetric = false;       // opposite directions are equally good (dipole / figure-8 antennas)
    std::string text;
    int bestIndex = -1;           // index into results()
};

const char* compassName(double deg);             // 16-point compass
double angDiff(double a, double b);              // signed smallest difference a - b in (-180, 180]
double occupancyDb(const std::vector<float>& dbfs, double fsMhz, double bwMhz);

class DirectionFinder {
public:
    enum class State { Idle, WaitConfirm, Measuring, Done };

    struct Config {
        double measureSec = 14;       // length of one measurement
        double settleSec = 3;         // ignored at the start of each measurement (AGC, receiver re-lock, antenna still moving)
        int maxMeasurements = 11;
        double flatSpread = 9;        // score range below which the direction does not matter
    };
    DirectionFinder() = default;
    explicit DirectionFinder(const Config& c) : cfg_(c) {}

    void start(AntennaKind kind);
    void stop();
    State state() const { return state_; }
    AntennaKind kind() const { return kind_; }

    // The place/direction the user is asked to go to (valid in WaitConfirm and Measuring)
    double targetHeading() const { return target_; }
    std::string targetLabel() const;
    std::string instruction() const;
    void confirm(double nowSec);                       // "I am pointing there": start measuring
    void skip();                                       // skip this direction
    void finishNow();                                  // omnidirectional: the user has tried all their spots (needs >= 2)
    void addSample(double nowSec, const DirSample& s); // call while Measuring (any rate)
    double progress(double nowSec) const;              // 0..1 of the current measurement
    const std::vector<DirResult>& results() const { return results_; }
    const DirRecommendation& recommendation() const { return rec_; }
    int measurementCount() const { return (int)results_.size(); }

private:
    void finishMeasurement();
    void planNext();
    void conclude();
    Config cfg_;
    State state_ = State::Idle;
    AntennaKind kind_ = AntennaKind::Directional;
    double target_ = 0;
    double t0_ = 0;
    std::vector<double> queue_;
    std::vector<DirResult> results_;
    DirRecommendation rec_;
    // accumulators for the running measurement
    struct Acc { int n = 0, nLocked = 0; double q = 0, snr = 0, loss = 0, occ = 0, clip = 0; MultipathLevel mp = MultipathLevel::Unknown; } acc_;
    int spotCount_ = 0;
    bool diagonalsAdded_ = false;
};

} // namespace dect2
