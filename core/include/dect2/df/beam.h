// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Receive beamforming for a coherent N-channel array (KrakenSDR / KerberosSDR, or a recorded N-channel file): delay-and-sum, MVDR (Capon,
// diagonal loading) and LCMV (look direction plus null constraints) weights for a chosen azimuth (and elevation on a 3D array), the polar
// beam pattern, measured gain / null depth, auto-steering to the strongest MUSIC peak, and a bank of up to four simultaneous beams that is
// offered to the normal receiver chain as a virtual single-channel IQ source. Receive only. Textbook algorithms (Bartlett, Capon 1969,
// Frost 1972 LCMV), written from scratch. Same conventions as df.h: azimuth degrees clockwise from the array's north, elevation degrees above
// the horizon, output y = w^H x, every weight set is distortionless (w^H a(look) = 1).
#pragma once
#include "dect2/df/df.h"
#include "dect2/df/source.h"
#include "dect2/source/source.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace dect2::df {

// Steering vector with elevation. Elevation 0 equals steering(); z positions are used when the array has them.
std::vector<cd> steeringAE(const Array& arr, double wavelength, double azDeg, double elDeg);

enum class BeamType { DelaySum, Mvdr, Lcmv };

struct Direction { double azDeg = 0, elDeg = 0; };

struct BeamSpec {
    BeamType type = BeamType::DelaySum;
    Direction look;
    std::vector<Direction> nulls;       // Lcmv: directions forced to zero (each costs one degree of freedom; at most n-1)
    double loading = 1e-2;              // MVDR / LCMV diagonal loading, fraction of the mean diagonal of R
    bool autoSteer = false;             // follow the strongest MUSIC peak (re-evaluated by BeamBank::recompute)
};

// `r` is the (calibrated) snapshot covariance, needed by Mvdr and Lcmv. Mvdr / Lcmv fall back to delay-and-sum when R cannot be inverted.
std::vector<cd> delaySumWeights(const Array& arr, double wavelength, Direction look);
std::vector<cd> mvdrWeights(const CMat& r, const Array& arr, double wavelength, Direction look, double loading = 1e-2);
std::vector<cd> lcmvWeights(const CMat& r, const Array& arr, double wavelength, Direction look, const std::vector<Direction>& nulls, double loading = 1e-2);
std::vector<cd> makeWeights(const BeamSpec& s, const CMat& r, const Array& arr, double wavelength);

// Response |w^H a|^2 in dB over azimuth 0..360 at a fixed elevation (0 dB = the look direction of a distortionless beam)
std::vector<double> beamPattern(const std::vector<cd>& w, const Array& arr, double wavelength, double elDeg = 0, double stepDeg = 1.0);
double beamResponse(const std::vector<cd>& w, const Array& arr, double wavelength, Direction d);   // linear |w^H a|^2
// Gain over one element for uncorrelated noise, dB: -10 log10(w^H w) with w^H a(look) = 1; at most 10 log10(n)
double whiteNoiseGainDb(const std::vector<cd>& w);
// Response toward d relative to the look direction, dB (negative = a null)
double nullDepthDb(const std::vector<cd>& w, const Array& arr, double wavelength, Direction look, Direction d);
// Half-power beamwidth around the look azimuth, degrees (360 when the pattern never falls 3 dB)
double beamwidthDeg(const std::vector<cd>& w, const Array& arr, double wavelength, Direction look);
// Strongest peak of `opt.method` on the covariance; false when none
bool strongestPeak(const CMat& r, const Array& arr, double wavelength, const Options& opt, double& azDeg);

struct BeamBankConfig {
    Array array;
    double wavelength = 3.0;
    std::vector<cd> calibration;        // per-element gains to remove (empty = none)
    std::vector<BeamSpec> beams;        // 1..4 (extra ones are dropped)
    Options df;                         // used by auto-steering
    double covForget = 0.9;             // covariance memory between blocks (0 = newest block only)
};

class BeamBank {
public:
    static constexpr int kMaxBeams = 4;
    explicit BeamBank(BeamBankConfig c);
    int beams() const { return (int)cfg_.beams.size(); }
    // One block x[channel][t]: updates the covariance, recomputes weights (and auto-steered looks), returns out[beam][t]
    std::vector<std::vector<cd>> process(const Frame& x);
    void setBeam(int i, const BeamSpec& s);
    const BeamSpec& beam(int i) const { return cfg_.beams[i]; }
    const std::vector<cd>& weights(int i) const { return w_[i]; }
    const CMat& covariance() const { return r_; }
    const BeamBankConfig& config() const { return cfg_; }
    void setCalibration(std::vector<cd> g) { cfg_.calibration = std::move(g); }
    void recompute();
private:
    BeamBankConfig cfg_;
    CMat r_;
    bool haveR_ = false;
    std::vector<std::vector<cd>> w_;
};

// One beam of a BeamBank as an ordinary receiver input ("Beam 1" ... "Beam 4"). A worker thread reads the array and writes that beam's
// samples to the ring. Beams of one bank share the array through one reader, so run one BeamIqSource per bank at a time.
class BeamIqSource : public IqSource {
public:
    BeamIqSource(std::shared_ptr<BeamBank> bank, std::shared_ptr<MultiChannelSource> src, int beam);
    ~BeamIqSource() override { stop(); }
    bool start(const TuneSettings& s, IqRing& ring, std::string& err) override;
    void stop() override;
    bool retune(const TuneSettings&, std::string&) override { return true; }
    double sampleRate() const override { return src_ ? src_->sampleRate() : 0; }
    static std::string name(int beam) { return "Beam " + std::to_string(beam + 1); }
private:
    std::shared_ptr<BeamBank> bank_;
    std::shared_ptr<MultiChannelSource> src_;
    int beam_;
    std::atomic<bool> run_{false};
    std::thread th_;
};

}   // namespace dect2::df
