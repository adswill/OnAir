// Acquisition: a parallel code-phase search by FFT over a grid of Doppler bins, with coherent integration over 2 ms and non-coherent summing over
// a few more, run in small slices so that feed() never stalls. One engine per band.
// The frequency window and the integration length can change between searches (setPlan): a radio's oscillator error moves every satellite by the same
// amount (up to 160 kHz on a cheap dongle), and a weak signal needs a longer integration. The sample clock of a radio comes from the same oscillator, so
// the code drifts against the samples in proportion to the frequency (100 ppm is 0.4 samples per millisecond): the non-coherent sum follows that drift.
#pragma once
#include "gnss_front.h"
#include <functional>
#include <memory>
#include <vector>

namespace dect2 {

struct GnssAcqHit {
    int prn = 0;
    double dopplerHz = 0;        // carrier Doppler plus the receiver's frequency error
    double codePhase = 0;        // in samples of the band, 0 .. N: the code period starts this many samples after `segStart` (and every N samples later)
    int64_t segStart = 0;        // band sample index of the start of the integrated segment
    float ratio = 0;             // peak over the mean power of the search grid
    float cn0Est = 0;            // dB-Hz, from the peak (rough)
    int blocks = 0;              // milliseconds integrated for this hit
};

struct GnssAcqConfig {
    int sys = 0;
    int fftLog2 = 12;            // samples per code period = 1 << fftLog2
    double fsOut = 4.096e6;
    int blocks = 16;             // milliseconds integrated (even), until setPlan() says otherwise
    int qMin = -8, qMax = 7;     // Doppler grid: steps of one FFT bin (fsOut / N: 1 kHz for a 1 ms code, 250 Hz for 4 ms) times four offsets, from qMin bins
                                 // (the first window; setPlan() moves it)
    double pfa = 1e-4;           // false alarm probability of one search of a satellite
    double rfHz = 1575.42e6;     // the carrier: the code drifts by frequency / rfHz against the samples
    bool coherentPairs = true;   // add two code periods coherently (GPS: 20 periods a bit); false for a signal with a data symbol every period (Galileo E1-B)
    std::vector<int> prns;       // the satellites to search, in the order of the first round
    // the replica of the code of one satellite, `n` samples of one code period, as complex (real) samples; false when the satellite has none
    std::function<bool(int prn, cf32* out, int n)> replica;
};

class GnssAcq {
public:
    void init(const GnssAcqConfig& cfg);
    void reset();
    // Spend about `units` FFTs of work (a unit is one FFT of the code period). `skip(prn)` says a satellite is tracked already. Hits are appended to `hits`.
    int work(const GnssBand& band, int units, const std::function<bool(int)>& skip, std::vector<GnssAcqHit>& hits);
    // A hit is old by the time its satellite's turn is over (seconds), and a sample clock that does not follow the oscillator moves the code phase in that time.
    // This looks for the satellite again in the newest samples, around the Doppler of the hit (three bins of 100 Hz), over all code phases,
    // integrating as long as the search that found it (at least 8 ms).
    bool confirm(const GnssBand& band, int prn, double dopplerHz, int hitBlocks, GnssAcqHit* out);
    // For the interface
    int currentPrn() const { return curPrn_; }
    float progress() const { return queue_.empty() ? 0.f : (float)qPos_ / (float)queue_.size(); }
    uint32_t rounds() const { return rounds_; }
    // The correlation power against code phase of the last satellite that was searched (decimated to 256 points, peak = 1), and its figures
    const std::vector<float>& lastCorr() const { return lastCorr_; }
    int lastPrn() const { return lastPrn_; }
    float lastRatio() const { return lastRatio_; }
    double lastDoppler() const { return lastDoppler_; }
    int lastPeakIndex() const { return lastPeakIdx_; }
    // Search these first (the satellites expected to be in view); the others follow
    void setPriority(const std::vector<int>& prns) { priority_ = prns; }
    float threshold() const { return threshold_; }
    // The window of the next searches: centre and half width in Hz (whole bins), and the milliseconds to integrate (even, 2..64).
    // The window applies from the next satellite on, the integration from the next segment.
    void setPlan(double centerHz, double halfWidthHz, int blocks);
    double windowCenterHz() const { return 0.5 * binHz_ * (qLo_ + qHi_ + 1); }
    double windowHalfHz() const { return 0.5 * binHz_ * (qHi_ - qLo_ + 1); }
    int blocksNow() const { return blocks_; }
    void endRound() { if (state_ == 1) qPos_ = queue_.size(); }   // give up the round in progress: the next call takes a new segment
    static constexpr int kMaxBlocks = 64;
private:
    void buildQueue(const std::function<bool(int)>& skip);
    void captureSegment(const GnssBand& band);
    void finishPrn(std::vector<GnssAcqHit>& hits);
    void startPrn();
    bool codeFft(size_t pi);
    GnssAcqConfig cfg_;
    int N_ = 0, nBins_ = 0;
    double binHz_ = 1000.0;
    float threshold_ = 5.0f;
    // the plan: wanted, and in use (the window for the satellite being searched, the integration for the segment held)
    int wantLo_ = -8, wantHi_ = 7, wantBlocks_ = 16;
    int qLo_ = -8, qHi_ = 7, blocks_ = 16;
    int thrBins_ = -1, thrPairs_ = -1;
    struct Spec { std::vector<float> re, im; };
    std::vector<Spec> data_;                  // 4 offsets x blocks
    std::vector<std::vector<float>> codeRe_, codeIm_;   // per index in cfg_.prns
    std::vector<char> codeValid_;
    // the grid is not kept: only the best bin so far (for the plot and the code phase), its two neighbours in frequency and the bin before
    std::vector<float> curP_, prevP_, bestP_;
    float best_ = 0, leftVal_ = 0, rightVal_ = 0;
    int bestBin_ = 0, bestN_ = 0;
    bool hasLeft_ = false, hasRight_ = false, needRight_ = false;
    int state_ = 0;                           // 0 wait for data, 1 searching
    int64_t segStart_ = 0;
    int64_t nextCapture_ = 0;
    std::vector<int> queue_, priority_;
    size_t qPos_ = 0;
    int curPrn_ = 0, curIdx_ = 0;
    int curBin_ = 0;                          // next step (0 .. 4 * nq)
    double sumP_ = 0; double cnt_ = 0;
    uint32_t rounds_ = 0;
    uint32_t roundsAll_ = 0;
    std::vector<float> lastCorr_;
    int lastPrn_ = 0, lastPeakIdx_ = -1;
    float lastRatio_ = 0;
    double lastDoppler_ = 0;
    std::vector<float> tmpRe_, tmpIm_, prevRe_, prevIm_;
    bool havePrev_ = false;
    int64_t idleGap_ = 0;
    int searched_ = 0;
};

} // namespace dect2
