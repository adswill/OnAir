// ATSC 1.0 (8-VSB) receiver: matched filter + resampler, pilot carrier loop, symbol clock from the segment sync, field sync
// detection, per-field least-squares channel equaliser trained on the field/segment sync symbols, trellis/RS decoding.
#pragma once
#include "atsc_tel.h"
#include "ring.h"
#include <functional>
#include <vector>
#include <memory>
#include <cstdint>

namespace dect2 {

class AtscReceiver {
public:
    AtscReceiver();
    ~AtscReceiver();
    // `inputRateHz`: complex sample rate of the capture; the 6 MHz channel is centred on 0 Hz. Needs at least 6.5 Msps.
    void configure(double inputRateHz);
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(AtscTelemetry& out, uint64_t lastSeq);
    // Decoded 188-byte transport packets (transport_error_indicator set on uncorrectable ones) and the signal time they represent
    void setPacketCallback(std::function<void(const uint8_t* packets, size_t count, double seconds)> cb);
    // 0 nothing, 1 pilot, 2 segment sync, 3 field sync, 4 transport stream flowing
    int detectLevel() const;
    bool rateOk() const;
    // Waits until the field worker thread has finished everything queued so far (used by tests; the callback runs on that thread)
    void flush();
    // true: when the field worker is busy the sample thread waits for it (files, tests). false (default): fields are dropped and the
    // decoder restarts, which is what a live source needs so that the sample path never stalls.
    void setBlocking(bool b);
    // Test hook: called with the equalised symbol levels (kFieldSyms of them, field sync segment first) of every decoded field.
    void setLevelTap(std::function<void(const std::vector<float>& levels)> f);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
