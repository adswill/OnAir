// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#pragma once
#include "railway_proto.h"
#include "dect2/common/ring.h"
#include "dect2/dsp/exact_resampler.h"
#include <array>
#include <complex>
#include <map>

namespace dect2::railway {
// Same streaming audio chain for imports and FM-demodulated IQ. State is never
// reset at a callback boundary; parallel symbol phases share the tone detector.
class AudioDecoder {
public:
    AudioDecoder();
    void feed(const float* audio, size_t n, std::vector<Message>& out);
    uint64_t syncs() const;
    uint64_t failures() const;
    double timeSec() const { return double(samples_) / 48000; }
private:
    std::array<double, 257> taps_{};
    std::array<double, 514> history_{};   // each sample twice (at head_ and head_ + 257): the filter reads one contiguous run
    size_t head_ = 0, toneHead_ = 0;
    std::array<std::array<std::complex<double>, 40>, 2> toneHistory_{};
    std::array<std::complex<double>, 2> sums_{};
    std::array<FrameFinder, 9> finders_;
    uint64_t samples_ = 0;
    double phase_ = 0, step_ = 0.1;
    bool previous_ = false;
    std::map<std::vector<uint8_t>, double> recent_;
};
class Modem {
public:
    void configure(double rate);
    void setOffset(double hz) { offset_ = hz; }
    void reset();
    void feed(const cf32* iq, size_t n, std::vector<Message>& out);
    double timeSec() const { return audio_.timeSec(); }
    float levelDb() const;
    double frequencyErrorHz() const { return meanDeviation_ * 48000 / 6.28318530717958647692; }
    uint64_t syncs() const { return audio_.syncs(); }
    uint64_t failures() const { return audio_.failures(); }
    double afcHz() const { return afcHz_; }
private:
    ExactResampler resampler_;
    AudioDecoder audio_;
    // 4.1: a second, narrow path (+-5.5 kHz instead of +-14 kHz before the discriminator) that follows the carrier. The wide path keeps the
    // +-8 kHz pull-in of the original receiver; the narrow one moves the FM threshold about 4 dB lower. Both decode; a frame both find counts once.
    AudioDecoder narrowAudio_;
    double afcHz_ = 0, afcPhase_ = 0, widePow_ = 0, wideFloor_ = 0;
    int floorN_ = 0;
    std::complex<double> r1_{}, noiseR1_{};
    cf32 narrowPrev_{0, 0};
    std::map<std::vector<uint8_t>, double> recent_;
    double rate_ = 0, offset_ = 0, phase_ = 0, power_ = 0, meanDeviation_ = 0;
    cf32 previous_{0, 0};
    struct Filter {
        double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        std::complex<double> z1{}, z2{};
        std::complex<double> run(std::complex<double> x);
    };
    std::array<Filter, 2> channel_, narrow_;
};
} // namespace dect2::railway
