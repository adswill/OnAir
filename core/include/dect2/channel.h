// Multipath / fading detector: looks at the channel estimate (power-delay profile, |H(f)|) and at how the decoder
// quality moves from frame to frame, and says whether the signal is probably suffering from multipath or fading.
#pragma once
#include "t2rx.h"
#include <deque>
#include <string>
#include <vector>
#include <cstdint>

namespace dect2 {

struct EchoInfo {
    double delayUs = 0;   // relative to the strongest path (negative = arrives before it)
    double levelDb = 0;   // relative to the strongest path
    bool insideGuard = true;
};

enum class MultipathLevel { Unknown, None, Mild, Likely, Severe };

struct MultipathReport {
    MultipathLevel level = MultipathLevel::Unknown;
    std::vector<EchoInfo> echoes;  // strongest first
    double guardUs = 0;
    double notchDepthDb = 0;       // deepest dip of |H(f)| below its median (smoothed)
    int notches = 0;               // number of separate dips deeper than 6 dB
    double snrStdDb = 0;           // frame-to-frame SNR variation
    int fadeFrames = 0;            // recent frames in which a noticeable share of the FEC blocks failed
    int framesSeen = 0;
    std::string headline;
    std::vector<std::string> reasons;
};

const char* multipathName(MultipathLevel l);

class MultipathDetector {
public:
    void reset();
    // Feed every new telemetry snapshot; cheap enough to call at the UI rate.
    void update(const RxTelemetry& rx);
    const MultipathReport& report() const { return rep_; }

private:
    void analyseChannel(const RxTelemetry& rx);
    void finish();
    MultipathReport rep_;
    uint64_t lastFrames_ = ~0ull, lastOk_ = 0, lastBad_ = 0;
    struct FrameRec { double snr; double loss; };
    std::deque<FrameRec> hist_;
    int echoPts_ = 0, notchPts_ = 0;
    bool chOk_ = false;
};

} // namespace dect2
