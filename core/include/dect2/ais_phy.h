// AIS physical layer, the part that works on one burst: from complex samples at 48 kHz (5 per bit) of one channel to HDLC frames.
// The receiver (ais_rx.cpp) finds the bursts with a power detector and hands each one to aisDecodeBurst; tests call it directly.
#pragma once
#include "ais_proto.h"
#include "ring.h"
#include <vector>

namespace dect2 {

constexpr double kAisWorkRate = 48000.0;   // samples per second after the channel filter, 5 per bit
constexpr int kAisSps = 5;

struct AisPhyConfig {
    double intWidth = 5.0;      // the bit decision integrates the frequency over this many samples (5 = one bit)
    int hypotheses = 4;         // timing phases tried, best eye opening first
    int refine = 2;             // decision-directed passes that re-centre the slicer on the mean of the two levels
    double chanPassHz = 7500, chanStopHz = 15000;   // the channel filter ahead of the discriminator (read by the receiver when it is configured)
    double narrowPassHz = 3500, narrowStopHz = 8500;   // the second, narrower filter that is applied to a burst after its own frequency is known (0 = off)
    bool mlse = true;           // when the hard decisions fail, a Viterbi search over the Gaussian pulse's intersymbol interference
    int mode = 0;               // 0 integrate and dump
};

struct AisBurstResult {
    std::vector<ais::Bits> frames;   // payloads (without FCS) whose FCS was right
    int bad = 0;                     // framed bursts that failed the FCS or the stuffing checks
    double cfoHz = 0;                // mean frequency of the burst relative to the channel centre
    int tauTried = 0;                // timing phases that were tried
    bool narrow = false;             // decoded (or measured) with the narrow second pass
    int tauOk = -1;                  // timing phase (in tenths of a sample) that decoded; -1 none
};

// x[0..n): samples around the burst (some noise before and after is fine). [core0, core1) is where the power detector saw the signal;
// the carrier offset is measured inside it.
AisBurstResult aisDecodeBurst(const cf32* x, size_t n, size_t core0, size_t core1, const AisPhyConfig& cfg = AisPhyConfig());

} // namespace dect2
