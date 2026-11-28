// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor scatter detector parts, written for the receiver (mscatter_live.cpp) and tested on their own (tests/mscatter).
//   Front       : third-order CIC decimator, the capture down to about 12 kHz
//   Detector    : spectrogram (2048-point Hann FFT, hop 256) of the work rate, beacon line follower, per-bin background, echo tracker
//   Counts      : pings per UTC hour and day
//   msShowers   : the annual meteor showers around a date (the overlay of the rate chart)
//   msCsv*      : the CSV export
#pragma once
#include "dect2/mscatter/mscatter_tel.h"
#include "dect2/common/ring.h"
#include "dect2/dsp/fftutil.h"
#include <complex>
#include <cstdint>
#include <map>
#include <vector>

namespace dect2 {
namespace ms {

constexpr int kFft = 2048, kHop = 256;
constexpr double kWorkRate = 12000;        // wanted work rate; the real one is rate / round(rate / kWorkRate)
constexpr int kGuardBins = 3;              // echoes closer than this to the beacon line are masked (+-17 Hz)
constexpr double kEchoSpanHz = 500;        // echoes are looked for this far from the beacon line

// Choices (#462): the spectrogram size, and the analysis bandwidth (the work rate after the decimator). Defaults are kFft and kWorkRate.
int clampFft(int n);                                       // nearest allowed size: 1024, 2048, 4096 or 8192
int hopFor(int fftSize);                                   // a quarter of the Hann window is not used: the hop is one eighth of the size
double clampBandwidthHz(double hz);                        // 3 kHz to 48 kHz
int wfHalfBins(int fftSize);                               // waterfall half width in bins (+-500 Hz at the default work rate: 85 at 2048)
double wfBinsPerColumn(int fftSize);                       // bins merged into one waterfall column (2 at the default size)
int guardBins(int fftSize);                                // bins next to the beacon line that are masked (3 at the default size)

int decimation(double inputRate, double targetRate = kWorkRate);   // 1 when the capture is already at the work rate
double cicNoiseGain(int d);                                // output noise variance / input noise variance of the Front

class Front {
public:
    void configure(double inputRate, double targetRate = kWorkRate);
    void reset();
    int decim() const { return d_; }
    double workRate() const { return rate_ / d_; }
    // returns the number of work-rate samples written to out (cap: n / decim + 1)
    size_t process(const cf32* x, size_t n, std::complex<float>* out);
private:
    int d_ = 1, idx_ = 0, cnt_ = 0;
    double rate_ = 0, scale_ = 1;
    std::vector<double> bre_[3], bim_[3];
    double sre_[3] = {}, sim_[3] = {};
};

struct RawPing {
    double startSec = 0, durSec = 0, peakSnrDb = 0, dopplerHz = 0, driftHzPerSec = 0;
    int kind = 0;
    std::vector<float> profile;
};

class Detector {
public:
    void configure(double workRate, int fftSize = kFft);
    void reset();
    int fftSize() const { return n_; }
    void setThresholdDb(double db) { thr_ = db; }
    double thresholdDb() const { return thr_; }
    void push(const std::complex<float>* x, size_t n);          // work-rate samples
    std::vector<RawPing> take();                                // pings finished since the last call
    // state for the screen
    bool beaconFound() const { return kb_ >= 0; }
    double beaconHz() const { return beaconHz_; }               // relative to the centre
    double beaconSnrDb() const { return beaconSnr_; }           // over the noise in one bin
    double beaconDbfs() const { return beaconDbfs_; }
    double noiseDbfs() const { return noiseDbfs_; }
    double binHz() const { return binHz_; }
    double timeSec() const { return frames_ * (double)hop_ / rate_; }
    bool active() const { return active_; }
    double activeSnr() const { return activeDb_; }
    uint64_t rejected() const { return rejected_; }
    bool warm() const { return frames_ * (double)hop_ / rate_ >= kWarmSec; }
    // the newest waterfall rows (oldest first), kMsWfRows x kMsWfCols, and the long-term spectrum around the beacon
    void waterfall(std::vector<uint8_t>& wf, std::vector<float>& spec) const;
    static constexpr double kWarmSec = 5.0;

private:
    void frame();
    double rate_ = 0, binHz_ = 0, thr_ = 13, sumw_ = 1;
    int n_ = kFft, hop_ = kHop, guard_ = kGuardBins;
    Fft fft_{kFft};
    std::vector<float> win_;
    std::vector<cf32> buf_;
    size_t fill_ = 0;
    uint64_t frames_ = 0;
    std::vector<float> P_, S_, B_;
    int kb_ = -1, cand_ = -1, candCount_ = 0, beaconLost_ = 0;
    double beaconHz_ = 0, beaconSnr_ = 0, beaconDbfs_ = -200, noiseDbfs_ = -200, noiseMean_ = 0;
    bool active_ = false;
    int pend_ = 0, pendBin_ = 0, trackBin_ = 0, silent_ = 0;
    uint64_t startFrame_ = 0, lastAbove_ = 0;
    double activeDb_ = 0;
    std::vector<float> lv_;
    std::vector<float> dop_;
    std::vector<uint64_t> fr_;
    bool bbPrev_ = false;
    uint64_t rejected_ = 0;
    std::vector<RawPing> done_;
    // waterfall
    std::vector<uint8_t> wf_;
    std::vector<uint8_t> row_;
    int rowFrames_ = 0, wfRows_ = 0;
    std::vector<float> specOut_;
};

// Pings per UTC hour and per UTC day (the lists in the telemetry).
class Counts {
public:
    void add(int64_t utc);
    void clear() { hour_.clear(); day_.clear(); }
    void fill(int64_t nowUtc, MsTelemetry& t) const;
private:
    std::map<int64_t, int> hour_, day_;
};

// The annual showers active around nowUtc or peaking within 45 days, nearest peak first.
void msShowers(int64_t nowUtc, std::vector<MsShower>& out);
int msDayOfYear(int64_t utc);   // 1..366

std::string csvHeader();
std::string csvLine(const MsPing& p, double beaconHz);   // beaconHz: the absolute frequency of the beacon line (0 unknown)

} // namespace ms
} // namespace dect2
