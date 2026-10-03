// DVB-T receiver: blind FFT-size / guard-interval detection, tracking, TPS decoding, channel estimation from the scattered
// pilots, equalisation and the channel decoder (Viterbi, Reed-Solomon). Reports through the same telemetry as the T2 receiver.
#pragma once
#include "t2rx.h"
#include <functional>
#include <memory>
#include <cstdint>

namespace dect2 {

class DvbtReceiver {
public:
    DvbtReceiver();
    ~DvbtReceiver();
    void configure(double inputRateHz, double bandwidthMhz);
    void reset();
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
