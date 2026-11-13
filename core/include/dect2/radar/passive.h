// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Passive coherent location: aircraft detected from the echoes of a broadcast transmitter (FM, DVB-T) using two coherent channels, a reference
// channel (antenna toward the transmitter) and a surveillance channel (antenna toward the sky). Chain: clutter / direct-path cancellation (ECA
// least squares or NLMS) -> cross-ambiguity function (bistatic range x Doppler) -> 2-D cell-averaging CFAR -> Kalman tracking of the bistatic
// range and range rate -> range ellipses for a map. Textbook method (Malanowski, "Signal Processing for Passive Bistatic Radar"; Colone et al.,
// ECA-B); written from the published descriptions. A simulator with known targets is in this header too: nothing here needs hardware, and nothing
// has been run on a real radio. See docs/passive-radar.md.
#pragma once
#include "dect2/df/source.h"
#include <complex>
#include <cstdint>
#include <vector>

namespace dect2::radar {

using cd = std::complex<double>;
constexpr double kC = 299792458.0;

// ---- 1. clutter and direct-path cancellation ------------------------------------------------------------------------------------------------
// Removes from `surv` everything that is a delayed copy (0..taps-1 samples) of `ref`: the direct path and static multipath. Block least squares
// (Toeplitz approximation of the normal equations, diagonal loading). Returns the cancellation depth in dB (power before / after).
double cancelEca(const std::vector<cd>& ref, std::vector<cd>& surv, int taps);
// The same with a normalised LMS filter run over the block (streaming-friendly; converges slower, so it cancels less on a short block).
double cancelNlms(const std::vector<cd>& ref, std::vector<cd>& surv, int taps, double mu = 0.5, int passes = 2);

// ---- 2. cross-ambiguity function ------------------------------------------------------------------------------------------------------------
struct CafParams {
    double sampleRate = 250e3;
    double carrierHz = 100e6;
    int decim = 250;        // samples summed per slow-time sample (anti-alias by boxcar, droops ~4 dB at the Doppler edge)
    int dopplerBins = 512;  // power of two; the CPI is dopplerBins * decim samples
    int rangeBins = 64;     // delays 0 .. rangeBins-1 samples
    size_t cpiSamples() const { return (size_t)decim * dopplerBins; }
    double rangeStepM() const { return kC / sampleRate; }          // bistatic range per delay sample
    double lambda() const { return kC / carrierHz; }
    double dopplerStepHz() const { return sampleRate / decim / dopplerBins; }
};
struct Caf {
    CafParams p;
    int nr = 0, nd = 0;
    std::vector<float> pwr;   // [nd][nr], Doppler fft-shifted: index nd/2 is 0 Hz
    float at(int d, int r) const { return pwr[(size_t)d * nr + r]; }
    double rangeM(double bin) const { return bin * p.rangeStepM(); }
    double dopplerHz(double bin) const { return (bin - nd / 2) * p.dopplerStepHz(); }
};
// `resid` is the cleaned surveillance signal; both have at least cpiSamples() samples (the first cpiSamples() are used).
Caf crossAmbiguity(const std::vector<cd>& ref, const std::vector<cd>& resid, const CafParams& p);

// ---- 3. CFAR --------------------------------------------------------------------------------------------------------------------------------
struct CfarParams {
    double pfa = 1e-6;        // per cell, assuming exponentially distributed noise power
    int guard = 2, train = 6; // cells each side, in both axes
    int minRangeBin = 12;     // the cancelled zone (taps) and the direct-path residue are skipped
    double zeroDopplerHz = 6; // |Doppler| below this is clutter residue
};
struct Detection {
    double rangeM = 0;        // bistatic range difference: path transmitter-target-receiver minus the baseline
    double dopplerHz = 0;
    double snrDb = 0;
    int rBin = 0, dBin = 0;
};
std::vector<Detection> cfarDetect(const Caf& c, const CfarParams& cp);

// ---- 4. tracking ----------------------------------------------------------------------------------------------------------------------------
struct Track {
    int id = 0;
    double r = 0, rdot = 0;           // bistatic range difference (m) and its rate (m/s)
    double p00 = 0, p01 = 0, p11 = 0; // covariance
    int hits = 0, misses = 0;
    bool confirmed = false;
    double snrDb = 0;
    std::vector<std::pair<double, double>> hist;   // (time s, range m)
};
struct TrackerParams {
    double accelSigma = 3.0;          // m/s^2 process noise (aircraft manoeuvre)
    double rangeSigma = 700;          // m measurement noise
    double rateSigma = 6;             // m/s
    double gate = 13.8;               // chi-square 2 dof, 99.9 %
    int confirmHits = 3, dropMisses = 3;
};
class Tracker {
public:
    explicit Tracker(TrackerParams p = {}, double lambda = 3.0) : tp_(p), lambda_(lambda) {}
    // dt: seconds since the previous update. Detections carry Doppler: the range rate is -lambda * Doppler.
    void update(const std::vector<Detection>& dets, double dt);
    const std::vector<Track>& tracks() const { return tr_; }
    void clear() { tr_.clear(); t_ = 0; }
private:
    TrackerParams tp_;
    double lambda_;
    double t_ = 0;
    int nextId_ = 1;
    std::vector<Track> tr_;
};

// ---- 5. geometry: range ellipses ------------------------------------------------------------------------------------------------------------
struct LatLon { double lat = 0, lon = 0; };
double distanceM(LatLon a, LatLon b);                         // spherical, metres
double bearingDeg(LatLon from, LatLon to);                    // 0 = north
// The set of points whose path transmitter -> point -> receiver is `baseline + excessM` long: an ellipse with the foci at tx and rx.
std::vector<LatLon> bistaticEllipse(LatLon tx, LatLon rx, double excessM, int points = 120);

// ---- whole chain ------------------------------------------------------------------------------------------------------------------------------
struct RadarConfig {
    CafParams caf;
    CfarParams cfar;
    TrackerParams trk;
    int ecaTaps = 12;
    bool useNlms = false;
};
struct RadarResult {
    Caf caf;
    std::vector<Detection> dets;
    double cancelDb = 0;
    std::vector<float> dopplerRow;   // dB per Doppler bin: strongest range cell beyond the cancelled zone (one waterfall line)
};
class PassiveRadar {
public:
    explicit PassiveRadar(RadarConfig c = {}) : cfg_(c), trk_(c.trk, c.caf.lambda()) {}
    RadarConfig& config() { return cfg_; }
    // One CPI. Cancels, correlates, detects and updates the tracker (dt = CPI length unless given).
    RadarResult process(const std::vector<cd>& ref, std::vector<cd> surv, double dt = 0);
    const Tracker& tracker() const { return trk_; }
    void reset() { trk_.clear(); }
private:
    RadarConfig cfg_;
    Tracker trk_;
};

// ---- simulator ----------------------------------------------------------------------------------------------------------------------------------
struct SimTarget {
    double rangeM = 40e3;     // bistatic range difference at t = 0
    double rateMps = -60;     // d(range)/dt: negative = approaching (positive Doppler)
    double levelDb = -75;     // power relative to the direct path
};
struct SimClutter { int delaySamples = 0; double levelDb = 0; double phase = 0; };
struct PassiveSimConfig {
    double sampleRate = 250e3, carrierHz = 100e6;
    std::vector<SimTarget> targets;
    std::vector<SimClutter> clutter = {{0, 0, 0.3}, {3, -22, 1.1}, {7, -30, 2.0}, {10, -38, -0.8}};
    double noiseDb = -65;     // surveillance noise power relative to the direct path
    double refSnrDb = 55;     // reference channel quality (it carries the transmitter's own noise and multipath)
    bool fm = true;           // false: flat noise-like (an OFDM / DVB-T-like) waveform
    uint64_t seed = 1;
};
// Channel 0 = reference, channel 1 = surveillance. Truth range of target i at time t: rangeM + rateMps * t.
class PassiveSimSource : public df::MultiChannelSource {
public:
    explicit PassiveSimSource(PassiveSimConfig c);
    int channels() const override { return 2; }
    double sampleRate() const override { return cfg_.sampleRate; }
    double centerFreq() const override { return cfg_.carrierHz; }
    bool read(df::Frame& out, size_t n) override;
    double time() const { return (double)t_ / cfg_.sampleRate; }
    PassiveSimConfig& config() { return cfg_; }
private:
    PassiveSimConfig cfg_;
    df::Rng rng_;
    uint64_t t_ = 0;
    double mA_ = 0, mB_ = 0, ph_ = 0;
    std::vector<cd> hist_;      // transmitted waveform history for the delays
};

// Two single-channel cf32 files (reference, surveillance) read in step, as one two-channel source.
class TwoFileSource : public df::MultiChannelSource {
public:
    TwoFileSource(const std::string& refPath, const std::string& survPath, double sampleRate, double carrierHz);
    ~TwoFileSource() override;
    bool ok() const { return a_ && b_; }
    int channels() const override { return 2; }
    double sampleRate() const override { return fs_; }
    double centerFreq() const override { return fc_; }
    bool read(df::Frame& out, size_t n) override;
private:
    void *a_ = nullptr, *b_ = nullptr;
    double fs_, fc_;
};

}   // namespace dect2::radar
