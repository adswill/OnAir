// DVB-T receiver: blind FFT-size / guard-interval detection, tracking, TPS decoding, channel estimation from the scattered
// pilots, equalisation and the channel decoder (Viterbi, Reed-Solomon). Reports through the same telemetry as the T2 receiver.
#pragma once
#include "dect2/t2/t2rx.h"
#include <functional>
#include <memory>
#include <cstdint>
#include <vector>

namespace dect2 {

class DvbtReceiver {
public:
    DvbtReceiver();
    ~DvbtReceiver();
    void configure(double inputRateHz, double bandwidthMhz);
    void reset();
    // Hierarchical multiplexes: deliver the low-priority stream instead of the high-priority one (no effect otherwise)
    void setLowPriority(bool lp);
    // Opt-in for a stitched radio group (synth_group.h): the seams (Hz from the channel centre) and the overlap width; every segment then gets
    // its own common phase from its continual pilots. Empty (the default) = the normal path. Set before feeding.
    void setPhaseSegments(const std::vector<double>& seamsHz, double overlapHz);
    void feed(const cf32* x, size_t n);
    bool telemetry(RxTelemetry& out, uint64_t lastSeq);
    // Called with decoded 188-byte transport packets (transport_error_indicator set on packets that could not be repaired) and the
    // amount of signal time they represent.
    void setPacketCallback(std::function<void(const uint8_t* packets, size_t count, double seconds)> cb);
    // 0 nothing seen, 1 a periodic cyclic prefix (a DVB-T-like signal) was found, 2 TPS decoded, 3 transport stream flowing
    int detectLevel() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
