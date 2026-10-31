// Per-unit gain-step calibration for the automatic gain (docs/AUTOGAIN.md, "Calibrate the unit"). The radio profiles hold TYPICAL step tables; a real R820T's
// steps vary by several dB with frequency and from unit to unit. The calibrator steps through every gain setting of the radio's overall control while the
// input is a steady noise (the antenna disconnected, or a band with nothing strong in it), measures the noise floor at each, and derives the real gain of every
// step from the floor differences (valid while the floor is above the ADC's own noise). The result is stored per device serial and replaces the nominal table
// in the controller (GainStage::actualDb), and flags units that are probably bad: a gain control with no effect, steps that go the wrong way, steps far from nominal.
#pragma once
#include "dect2/source/autogain.h"
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace dect2::ag {

struct GainCal {
    std::string serial, radioId;
    double freqHz = 0;
    std::vector<double> nominal;      // the stage's steps (settings)
    std::vector<double> actual;       // real gain at each step, dB (anchored at the first step's nominal value)
    std::vector<double> floorDbfs;    // the noise floor measured at each step
    std::vector<char> sure;           // 1: measured above the ADC's noise; 0: copied from the nominal difference
    std::vector<std::string> flags;   // what looks wrong with this unit, in plain words
    double worstStepErrDb = 0;        // the largest |real step - nominal step| among the sure steps
    bool valid() const { return !actual.empty() && actual.size() == nominal.size(); }
    bool bad() const { return !flags.empty(); }
};

// Tick-based like the controller (no clock of its own): step through every setting of the first settable overall stage
class GainCalibrator {
public:
    // chanBwHz/offsetHz as in AgInput; serial identifies the unit
    void begin(const RadioProfile& r, const std::string& serial, double now, double freqHz);
    // true when `out` must be applied
    bool tick(const AgInput& in, AgCommand& out);
    bool done() const { return done_; }
    double progress() const { return nominal_.empty() ? 0 : std::min(1.0, (double)idx_ / (double)nominal_.size()); }
    const GainCal& result() const { return cal_; }
    const std::string& text() const { return text_; }
private:
    void finish();
    RadioProfile r_;
    GainCal cal_;
    std::vector<double> nominal_;
    int stage_ = -1, idx_ = -1;
    double changedAt_ = 0, settle_ = 0.5, dwell_ = 0.7, sumFloor_ = 0, sumMargin_ = 0; int n_ = 0;
    bool done_ = false, started_ = false, clipped_ = false;
    std::vector<double> floors_, margins_;
    std::string text_;
};

// Replace the nominal steps of the profile's overall stage by the measured ones (when the calibration matches the stage); false: not applied
bool applyCalibration(RadioProfile& r, const GainCal& c);

// The calibrations of the app, per serial: serialise / parse for the settings
class GainCalStore {
public:
    void put(const GainCal& c) { m_[c.serial] = c; }
    const GainCal* find(const std::string& serial) const { auto it = m_.find(serial); return it == m_.end() ? nullptr : &it->second; }
    void forget(const std::string& serial) { m_.erase(serial); }
    const std::map<std::string, GainCal>& entries() const { return m_; }
    std::string serialise() const;
    bool parse(const std::string& s);
private:
    std::map<std::string, GainCal> m_;
};

} // namespace dect2::ag
