// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Frequency-error (ppm) calibration helpers behind Tools > Calibrate frequency: the reference lists (FM, ATSC, DVB-T, DAB, GSM, time and
// AM carriers), tone estimation (parabolic peak, pilot phase tracking), averaging with a confidence interval, sanity limits, the quick
// check and the drift track. Pure functions, no radio, no threads. Sign convention as selfcal.h: positive ppm = the radio's clock runs fast.
#pragma once
#include <complex>
#include <cstddef>
#include <string>
#include <vector>

namespace dect2::selfcal {

// ---- estimation
// ppm from a typed pair: the carrier should be expectedHz and the radio shows it at observedHz (manual method).
double ppmFromObserved(double expectedHz, double observedHz);
// Fractional peak position of a magnitude spectrum around bin k (parabola through the log of k-1, k, k+1); returns k + delta,
// delta in [-0.5, 0.5]. k at an edge or a zero neighbour returns k.
double parabolicPeak(const float* mag, size_t n, size_t k);
// Strongest bin of a magnitude spectrum, then parabolicPeak. Returns the position in bins (-1 for an empty input).
double peakBin(const float* mag, size_t n);
struct ToneFit {
    bool valid = false;
    double offsetHz = 0;    // tone frequency minus approxHz
    double sigmaHz = 0;     // 1-sigma of offsetHz from the phase-fit residual
    double snrDb = 0;       // per-block tone SNR from the phase residual, rough
};
// Pilot phase tracking: mix by approxHz, sum blocks of blockLen samples, fit a line to the unwrapped block phase. Good to a small
// fraction of 1/(duration) in Hz at useful SNR (sub-mHz for a minute of a clean carrier). approxHz must be within about 1/(4 block) of the tone.
ToneFit fitToneByPhase(const std::complex<float>* x, size_t n, double fs, double approxHz, size_t blockLen = 0);

// ---- averaging and confidence
// Inverse-variance running mean of ppm measurements. The 1-sigma of the mean is the larger of the formal one and the scatter of the
// samples around the mean (so a drifting or biased reference does not look better than it is).
class PpmAverager {
public:
    void reset();
    void add(double ppm, double sigmaPpm);
    int count() const { return n_; }
    double mean() const;
    double sigma() const;                            // 1-sigma of the mean (ppm)
    double ci95() const { return 1.96 * sigma(); }   // half-width of the 95 % interval (ppm)
    bool confident(double targetCi95Ppm = 0.1) const { return n_ >= 3 && ci95() < targetCi95Ppm; }
private:
    int n_ = 0;
    double sw_ = 0, swx_ = 0, swxx_ = 0, sw2_ = 0;
};

// ---- sanity
enum class Tcxo { Unknown, No, Yes };
struct Sanity {
    bool accepted = true;
    std::vector<std::string> notes;       // plain-words warnings or the reason for the rejection
};
// Rejects |ppm| > 300 when the radio's oscillator class is known (No or Yes); with Tcxo::Unknown nothing is rejected but a strong warning is
// given. Tcxo::Yes: more than 50 ppm is rejected (a TCXO radio is within a few ppm: wrong channel or reference). subCarrier: the measured
// tone is not a main carrier (FM pilot): warns that the station's own error adds in. ci95Ppm: warns if wide.
Sanity checkPpm(double ppm, Tcxo tcxo, bool subCarrier, double ci95Ppm);

// ---- quick check: one short measurement made with the correction already applied
enum class QuickVerdict { Good, Warm, Off };
struct QuickResult { QuickVerdict verdict = QuickVerdict::Good; double deltaPpm = 0; std::string text; };
QuickResult quickCheck(double residualPpm, double sigmaPpm);

// ---- drift track (the optional 10 minute graph): a sample every intervalSec, slope by weighted least squares
class DriftTrack {
public:
    explicit DriftTrack(double durationSec = 600, double intervalSec = 30) : dur_(durationSec), iv_(intervalSec) {}
    void reset() { t_.clear(); p_.clear(); s_.clear(); }
    bool due(double tSec) const { return t_.empty() || tSec - t_.back() >= iv_; }
    void add(double tSec, double ppm, double sigmaPpm);
    bool finished(double tSec) const { return !t_.empty() && tSec - t_.front() >= dur_; }
    size_t size() const { return t_.size(); }
    double time(size_t i) const { return t_[i] - t_.front(); }
    double ppm(size_t i) const { return p_[i]; }
    double sigma(size_t i) const { return s_[i]; }
    bool slope(double& ppmPerMin) const;   // false with fewer than 3 points
    std::string note() const;              // the warm-up advice for the measured slope
private:
    double dur_, iv_;
    std::vector<double> t_, p_, s_;
};

// ---- references
enum class RefKind { FmPilot, Carrier, DecoderOffset, GsmFcch };
struct RefChannel {
    std::string group;      // "FM", "ATSC", "DVB-T", "DAB", "GSM", "Time", "AM"
    std::string label;
    double nominalHz = 0;   // the absolute frequency to compare against (pilot, carrier, tone; channel centre for decoder offsets;
                            // FM: the station carrier, the pilot is at 19 kHz)
    RefKind kind = RefKind::Carrier;
    bool subCarrier = false;   // not a main carrier: the transmitter's own error adds in
    std::string note;
};
std::vector<RefChannel> fmChannels();       // 87.5 .. 108.0 MHz, 100 kHz raster
std::vector<RefChannel> atscChannels();     // US ch 2..36 pilots
std::vector<RefChannel> dvbtChannels();     // VHF 7 MHz ch 5..12 and UHF 8 MHz ch 21..60 centres (from the decoder's carrier offset)
std::vector<RefChannel> dabBlocks();        // Band III 5A..13F
std::vector<RefChannel> gsmChannels();      // GSM 900 ARFCN 1..124 and DCS 1800 ARFCN 512..885: the FCCH tone, 67.708 kHz above the carrier
std::vector<RefChannel> timeCarriers();     // WWV/WWVH, CHU, DCF77, MSF, WWVB, JJY, BPC, RBU, BPM
RefChannel amCarrier(double hz);            // an AM broadcast carrier of known frequency typed by the user
std::vector<RefChannel> allReferences();

}   // namespace dect2::selfcal
