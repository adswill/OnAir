// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// UAT 978 MHz demodulator (dphi slicing at twice the 1.041667 Mbit/s bit rate, after dump978; soft erasures since 3.9: uat_demod.cpp).
#pragma once
#include "uat_frame.h"
#include "dect2/common/ring.h"
#include "dect2/dsp/exact_resampler.h"
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

class UatDemod {
public:
    void configure(double inputRateHz);
    bool ready() const { return rate_ >= 1.9e6 - 1; }
    void reset();
    void feed(const cf32* x, size_t n);
    double timeSec() const { return samples_ / (rate_ > 0 ? rate_ : 1); }
    float noiseDbfs() const { return noiseDbfs_; }
    uint64_t candidates() const { return candidates_; }
    uint64_t erasureFixes() const { return erasureFixes_; }   // frames that only decoded with erasures
    void setSoft(bool on) { soft_ = on; }
    void setPrefilter(int k) { pre_ = k; }                     // test knob: 0 none, 1 three taps, 2 five taps                     // test knob: the erasure retries (default on)
    static constexpr double kBitRate = 1041666.6666667;       // DO-282B
    static constexpr double kWorkRate = 2 * kBitRate;

    void setSink(std::function<void(UatRawFrame&)> cb) { sink_ = std::move(cb); }

private:
    void pushPhi(const cf32* x, size_t n);
    void processPhi();
    int demodDownlink(const uint16_t* phi, uint8_t* to, int* rs);
    int demodUplink(const uint16_t* phi, uint8_t* to, int* rs);
    float levelOf(int index, int bits) const;

    double rate_ = 0;
    bool pass_ = true, soft_ = true;
    int pre_ = 1;
    cf32 pf_[5] = {};
    ExactResampler rs_;
    std::vector<cf32> tmp_;
    uint64_t samples_ = 0, workBase_ = 0;          // input samples fed; work-rate samples dropped from the front of phi_
    float noiseDbfs_ = -120;
    double blkSum_ = 0, nf_ = 0; int blkN_ = 0;
    uint64_t candidates_ = 0, erasureFixes_ = 0;
    std::vector<uint16_t> phi_;
    std::vector<float> pw_;
    std::function<void(UatRawFrame&)> sink_;
};

} // namespace dect2
