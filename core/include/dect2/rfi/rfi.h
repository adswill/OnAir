// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Interference (RFI) hunter: watches the noise floor and the spurs of repeated sweeps (core/include/dect2/sweep), tells new interference
// from the baseline, guesses the kind of source from the shape of the spectrum, and turns sweeps into a loud / quiet reading for a walk
// with a log of readings by time and (optional) position. Source-agnostic: it takes the Sweep structs runSchedule() makes.
// The classification is a set of documented rules of thumb (docs/rfi-hunter.md), not a measurement: it names the likeliest kind with a confidence.
#pragma once
#include "dect2/sweep/sweep.h"
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace dect2 {
namespace rfi {

using sweep::Sweep;

enum class Kind { Unknown, SwitchingSupply, LedLamp, Powerline, LoLeak };
const char* kindName(Kind k);    // English text (the window translates it): "Switching power supply", "LED lamp / driver", ...

struct Spur { double freqHz = 0, bwHz = 0; float levelDb = 0; };            // narrow signal, level above its surroundings
struct Hump { double startHz = 0, stopHz = 0; float riseDb = 0; int notches = 0; int hamNotches = 0; };   // wide raised region (broadband noise)

struct SpurConfig {
    double thresholdDb = 8;       // a bin is part of a spur when it is this far above the local level and above the noise floor
    double maxBwHz = 100e3;       // wider runs are not spurs (signals, humps)
    double localWindowHz = 400e3; // the local level is the median over this width
};
std::vector<Spur> findSpurs(const Sweep& s, const SpurConfig& c = SpurConfig());
// Wide regions raised by minRiseDb over ref (one dB value per bin, e.g. the baseline) or, when ref is empty, over the sweep's noise floor
std::vector<Hump> findHumps(const Sweep& s, const std::vector<float>& ref, double minRiseDb = 4, double minWidthHz = 400e3);

// Equally spaced spurs: spacing, offset (frequency of tooth 0) and how many of the given spurs sit on the grid
struct Comb { double spacingHz = 0, startHz = 0; int hits = 0, expected = 0; std::vector<int> members; bool ok() const { return hits >= 4; } };
Comb findComb(const std::vector<Spur>& spurs, double binHz);

// One guess of a source: what, how sure (0..1), where
struct Finding {
    Kind kind = Kind::Unknown;
    float confidence = 0;
    double startHz = 0, stopHz = 0;
    double spacingHz = 0;          // comb spacing (switching supply, LED driver), else 0
    int count = 0;                 // spurs in the comb
    float levelDb = 0;             // strongest part above the surroundings
    double clockHz = 0;            // LO leak: the common clock it is a harmonic of, 0 when none
    int hamNotches = 0;            // powerline: notches that fall on amateur bands
    std::vector<double> members;   // frequencies of the spurs that belong to this finding
};
std::vector<Finding> classify(const std::vector<Spur>& spurs, const std::vector<Hump>& humps, double binHz);

// ---- the monitor ----
struct MonitorConfig {
    int learnSweeps = 5;           // sweeps used for the baseline
    int confirmSweeps = 2;         // a new spur / rise must be seen in this many sweeps in a row to raise an alert
    int clearSweeps = 3;           // gone for this many sweeps: cleared
    double floorRiseDb = 3;        // noise floor above the baseline by this much is an alert
    double humpRiseDb = 4;
    double humpMinWidthHz = 400e3;
    size_t maxHistory = 20000;
    SpurConfig spur;
};
struct FloorPoint { double time = 0; float floorDb = 0, deltaDb = 0; };
struct Alert {
    enum Type { NewSpur, FloorRise, NewHump, Cleared } type = NewSpur;
    double time = 0, freqHz = 0, bwHz = 0;
    float levelDb = 0;             // dB above the baseline (floor rise: dB of the rise)
    Kind kind = Kind::Unknown;
    float confidence = 0;
    double spacingHz = 0;
    int count = 0;
};

class Monitor {
public:
    explicit Monitor(const MonitorConfig& c = MonitorConfig()) : cfg_(c) {}
    // Adds a sweep; the alerts it raises. A sweep of another range or bin width restarts the baseline.
    std::vector<Alert> add(const Sweep& s);
    void relearn();                              // forget the baseline and learn again from the next sweeps
    void accept();                               // what is there now becomes normal
    bool learning() const { return !ready_; }
    int learned() const { return (int)learn_.size(); }
    const MonitorConfig& config() const { return cfg_; }
    void setConfig(const MonitorConfig& c) { cfg_ = c; }
    const std::vector<float>& baseline() const { return base_; }
    const Sweep& baselineSweep() const { return baseSweep_; }
    float baselineFloorDb() const { return floorBase_; }
    const std::vector<Spur>& baselineSpurs() const { return baseSpurs_; }
    const std::deque<FloorPoint>& history() const { return hist_; }
    const std::vector<Alert>& log() const { return log_; }
    const std::vector<Finding>& active() const { return active_; }   // what is classified right now (confirmed new interference)
    bool exportLogCsv(const std::string& path, std::string& err) const;      // alerts
    bool exportFloorCsv(const std::string& path, std::string& err) const;    // floor history

private:
    struct Cand { int type = 0; double freqHz = 0, bwHz = 0; float levelDb = 0; int hits = 0, miss = 0; bool alerted = false; Kind kind = Kind::Unknown; float conf = 0; double spacing = 0; int count = 0, ham = 0, notch = 0; };
    bool sameGrid(const Sweep& s) const;
    MonitorConfig cfg_;
    bool ready_ = false;
    std::vector<Sweep> learn_;
    Sweep baseSweep_;
    std::vector<float> base_;
    std::vector<Spur> baseSpurs_;
    float floorBase_ = 0;
    std::vector<Cand> cands_;
    int floorHits_ = 0, floorClear_ = 0;
    bool floorAlerted_ = false;
    std::deque<FloorPoint> hist_;
    std::vector<Alert> log_;
    std::vector<Finding> active_;
    Sweep last_;
};

// ---- the walk ----
struct Reading {
    double time = 0, lat = 0, lon = 0;
    bool hasPos = false;
    float powerDb = 0;      // band power (dBFS per sweep, after leaving out the ignored signals)
    float levelDb = 0;      // above the reference
    float floorDb = 0;
    double peakHz = 0;      // strongest spur
    float peakDb = 0;       // ... above its surroundings
    Kind kind = Kind::Unknown;
};
struct WalkConfig {
    double rangeDb = 25;    // the meter is full at this many dB above the reference
    double attack = 0.6;    // smoothing of the meter: fast up ...
    double decay = 0.2;     // ... slow down (per reading)
    SpurConfig spur;
};
class Walk {
public:
    explicit Walk(const WalkConfig& c = WalkConfig()) : cfg_(c) {}
    // Turns a sweep into a reading (logged). pos may be null.
    const Reading& add(const Sweep& s, const double* lat, const double* lon);
    void setReferenceNow();                          // the quiet level: the present smoothed power is 0 dB
    void clearReference() { manualRef_ = false; }    // the quietest reading so far is the reference again
    void ignoreNow();                                // spurs seen now are normal (broadcast carriers, known devices): left out of the power from now on
    void clearIgnore() { ignore_.clear(); }
    void clear();
    float meter() const { return meter_; }           // 0 (quiet) .. 1 (loud)
    float smoothedDb() const { return smoothed_; }
    float referenceDb() const { return manualRef_ ? ref_ : minSmoothed_; }
    int trend() const;                               // +1 getting louder, -1 quieter, 0 steady (last readings)
    const std::vector<Reading>& readings() const { return rd_; }
    int loudest() const;                             // index of the loudest reading, -1 when none
    const WalkConfig& config() const { return cfg_; }
    void setConfig(const WalkConfig& c) { cfg_ = c; }
    bool exportCsv(const std::string& path, std::string& err) const;
    bool exportGpx(const std::string& path, std::string& err) const;   // one waypoint per reading with a position; the name carries the level
private:
    WalkConfig cfg_;
    std::vector<Reading> rd_;
    std::vector<Spur> ignore_;
    std::vector<float> recent_;
    float smoothed_ = 0, minSmoothed_ = 0, ref_ = 0, meter_ = 0;
    bool manualRef_ = false, have_ = false;
    Sweep last_;
};

// ---- a position from an NMEA 0183 sentence (GGA or RMC, any talker); the checksum is checked when the line has one ----
struct Fix { bool valid = false; double lat = 0, lon = 0; int sats = 0; double utcSec = -1; };
bool parseNmea(const std::string& line, Fix& fix);

// ---- synthetic sweeps for tests and the demo ----
struct SimSource {
    enum Type { Carrier, Comb, Hump, PlcHf, Lo } type = Carrier;
    double freqHz = 0;          // carrier / LO: frequency; comb: first tooth; hump: start
    double spacingHz = 0;       // comb
    double widthHz = 0;         // hump / PLC: width
    int teeth = 0;
    float levelDb = 20;
    double onSec = 0, offSec = 1e18;   // on between these times
};
struct SimScene {
    double startHz = 87e6, stopHz = 108e6, binHz = 25e3;
    float floorDb = -80;
    std::vector<SimSource> sources;
    uint64_t seed = 1;
};
Sweep simulateSweep(const SimScene& sc, double t);

} // namespace rfi
} // namespace dect2
