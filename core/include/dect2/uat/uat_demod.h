// UAT 978 MHz downlink demodulator (SOQPSK-style dphi slicing, after dump978).
#pragma once
#include "uat_frame.h"
#include "dect2/common/ring.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace dect2 {

struct UatRawFrame {
    uint8_t data[UAT_UPLINK_FRAME_BYTES] = {};
    int dataBytes = 0;
    bool uplink = false;
    int rsErrors = 0;
    float levelDbfs = -120;
    double timeSec = 0;
};

constexpr double kUatBitRate = 1041666.6666666667;   // DO-282B

class UatDemod {
public:
    void configure(double inputRateHz);
    bool ready() const { return rate_ >= 1.9e6 - 1; }
    void reset();
    void feed(const cf32* x, size_t n);
    double timeSec() const { return samples_ / (rate_ > 0 ? rate_ : 1); }
    float noiseDbfs() const { return noiseDbfs_; }
    uint64_t candidates() const { return candidates_; }

    void setSink(std::function<void(UatRawFrame&)> cb) { sink_ = std::move(cb); }

private:
    void pushPhi(const cf32* x, size_t n);
    void processPhi();
    static uint16_t iqPhase(const cf32& v);

    double rate_ = 0;
    double workRate_ = 2 * kUatBitRate;
    double rsPos_ = 0;
    bool rsHave_ = false;
    cf32 rsLast_{0, 0};
    uint64_t samples_ = 0;
    float noiseDbfs_ = -120;
    uint64_t candidates_ = 0;
    std::vector<uint16_t> phi_;
    std::function<void(UatRawFrame&)> sink_;
};

} // namespace dect2
