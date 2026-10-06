// FM broadcast radio receiver (87.5-108 MHz, mono, stereo with 19 kHz pilot, and RDS)
#pragma once
#include "fm_tel.h"
#include "ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace dect2 {

class FmReceiver {
public:
    FmReceiver();
    ~FmReceiver();
    
    void configure(double inputRateHz);     // Configure for any input sample rate
    void reset();                            // Reset receiver state
    void feed(const cf32* x, size_t n);     // Process IQ samples
    bool telemetry(FmTelemetry& out, uint64_t lastSeq);  // Get latest telemetry
    
    // Audio playback control
    void setVolume(float v);
    void setMuted(bool m);
    
    // Test/diagnostic hooks
    void setDemodTap(std::function<void(const float*, size_t)> cb);  // Tap demodulated audio
    void setRdsTap(std::function<void(const uint8_t*, int)> cb);     // Tap RDS groups

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
