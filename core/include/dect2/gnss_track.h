// One tracking channel: carrier and code loops, C/N0, bit synchronisation and the framing of the navigation message.
// A channel reads whole code periods from its band's buffer; every epoch it updates the loops. The message layer is the LNAV of GPS for now.
#pragma once
#include "gnss_front.h"
#include "gnss_nav.h"
#include "gnss_tel.h"
#include <cstdint>
#include <deque>
#include <vector>

namespace dect2 {

// The static description of a signal that a channel tracks
struct GnssSignalSpec {
    int sys = 0;
    double chipRate = 1.023e6;
    int codeLen = 1023;
    double rfHz = 1575.42e6;      // carrier of this signal (for the code-carrier aiding ratio)
    double fsOut = 4.096e6;       // the band's rate
    int halfSpacing = 2;          // samples between prompt and early (and late)
};

struct GnssSubframeEvent {
    LnavSubframe sf;
    double towStart = 0;          // time of week of the first bit of the subframe, from its handover word
    int64_t startEpoch = 0;
};

class GnssTracker {
public:
    int sys = 0, prn = 0, fcn = 0;
    // Start tracking at band sample `idx` (the leading edge of the sample) where the code phase is `phase` chips, with this Doppler estimate.
    void start(const GnssSignalSpec& spec, const uint8_t* chips, int prn, double dopplerHz, int64_t idx, double phase);
    // Process one code period if the band has the samples. Returns false when it has not.
    bool step(const GnssBand& band);
    int64_t position() const { return pos_; }          // the next band sample this channel needs
    // ---- state
    int state() const;                                  // GnssChState without the ephemeris level (the receiver adds that)
    bool lost() const { return lost_; }
    bool carrierLocked() const { return locked_; }
    bool bitSynced() const { return bitSync_; }
    bool frameSynced() const { return frameSync_; }
    bool timeValid() const { return timeValid_; }
    float cn0() const { return (float)cn0Db_; }
    double dopplerHz() const { return fcar_; }
    double codePhase() const { return phi_; }
    double lockSeconds() const { return (double)locked_epochs_ * 1e-3; }
    uint32_t framesOk() const { return framesOk_; }
    uint32_t framesBad() const { return framesBad_; }
    uint64_t epochs() const { return ecount_; }
    // subframes decoded since the last call
    std::vector<GnssSubframeEvent>& events() { return events_; }
    // The GPS time of week of the transmission of the code epoch that began at receive time tRx (seconds of the band's clock); false when no time is known
    // yet or tRx is outside the recent epochs. The transmit time is the satellite's own clock reading.
    bool transmitTime(double tRx, double* tow) const;
    // the latest epoch: its receive time; so the receiver can tell when a measurement instant is covered
    double latestEpochTime() const { return lastEpochT_; }
    // the last prompt values, the carrier phase error and the early-late discriminator for the interface
    const std::deque<std::pair<float, float>>& scatter() const { return scatter_; }
    const std::vector<float>& cn0History() const { return cn0Hist_; }
    // Doppler change per second (from the carrier loop's integrator), for the velocity solution
    double carrierRate() const { return 0; }
    bool wasPullInTimeout() const { return pullInTimeout_; }
    int framesSinceSync() const { return framesSinceSync_; }
    double codeFrequencyHz() const { return codeHz_; }
    GnssSignalSpec spec;
private:
    void epoch(const GnssBand& band, int N, double tStart);
    void bitLayer(float I);
    void pushBit(int bit);
    void trySubframe();
    // code and carrier
    std::vector<float> chips_;
    double phi_ = 0;              // code phase in chips at the leading edge of sample pos_
    double fcar_ = 0;             // carrier frequency in the band, Hz
    double fcar0_ = 0;            // the Doppler it started with
    double theta_ = 0;            // carrier phase in cycles at the leading edge of sample pos_
    int64_t pos_ = 0;
    // loops
    bool locked_ = false;
    bool lost_ = false;
    bool pullInTimeout_ = false;
    double pllInt_ = 0, pllF0_ = 0;
    double dllInt_ = 0, dllRate_ = 0;   // code loop: integrator and the rate correction on top of the carrier aiding, chips/s
    double prevI_ = 0, prevQ_ = 0;
    bool havePrev_ = false;
    double fllErr_ = 0;           // EMA of the frequency error, Hz
    uint64_t ecount_ = 0;
    uint64_t locked_epochs_ = 0;
    uint64_t pullEpochs_ = 0;
    double lockIdx_ = 0;          // EMA of the phase lock indicator
    double m2_ = 0, m4_ = 0;
    int cnInit_ = 0;
    double cn0Db_ = 0, cnSmooth_ = 0;
    int lowCn0Epochs_ = 0, lowLockEpochs_ = 0;
    double lastEpochT_ = 0;
    // scratch
    std::vector<float> rep_;
    // bit layer
    int hist_[20] = {};
    int prevSign_ = 0;
    uint64_t syncEpoch_ = 0;
    bool bitSync_ = false;
    int syncPhase_ = 0;
    int bitHistTotal_ = 0;
    double bitAcc_ = 0;
    int bitCount_ = 0;
    int64_t firstBitEpoch_ = 0;
    int64_t bitsPushed_ = 0;
    std::vector<uint8_t> bits_;       // the most recent hard bits
    int64_t bitsBase_ = 0;            // absolute bit number of bits_[0]
    bool frameSync_ = false;
    int64_t lastSubframeBit_ = 0;
    int framesSinceSync_ = 0;
    int badInRow_ = 0;
    uint32_t framesOk_ = 0, framesBad_ = 0;
    bool timeValid_ = false;
    double towAtEpoch0_ = 0;          // GPS time of week (satellite clock) of epoch number 0, so epoch k began at towAtEpoch0_ + k * 1 ms
    std::vector<GnssSubframeEvent> events_;
    // history of the epoch start times (receive time seconds), for the transmit-time lookup
    std::deque<double> epochT_;       // the last 256
    uint64_t epochTFirst_ = 0;        // epoch number of epochT_.front()
    std::deque<std::pair<float, float>> scatter_;
    std::vector<float> cn0Hist_;
    uint64_t lastHistEpoch_ = 0;
    double codeHz_ = 0;
};

} // namespace dect2
