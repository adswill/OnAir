// DVB-T receiver: blind FFT-size / guard-interval detection, tracking, TPS decoding, channel estimation from the scattered
// pilots, equalisation and the channel decoder (Viterbi, Reed-Solomon). Reports through the same telemetry as the T2 receiver.
#pragma once
#include "dect2/t2/t2rx.h"
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
    // Hierarchical multiplexes: deliver the low-priority stream instead of the high-priority one (no effect otherwise)
    void setLowPriority(bool lp);
    void feed(const cf32* x, size_t n);
    // For live radios: the error correction (demapping, Viterbi, Reed-Solomon) runs on its own thread behind a bounded queue, and feed() returns
    // once the symbols are equalised; the packets follow on that thread, in order. Off (the default) feed() is synchronous: file sources and
    // tests see everything decoded when it returns. Call before the first feed(). blocking: a full queue makes feed() wait instead of
    // dropping symbols (benchmarks, recordings played as fast as they go).
    void setPipelined(bool on, bool blocking = false);
    // Waits until the error correction has decoded everything fed so far (pipelined mode; nothing to do otherwise)
    void flush();
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
