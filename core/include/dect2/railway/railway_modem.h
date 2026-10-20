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
    std::array<double, 257> taps_{}, history_{};
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
private:
    ExactResampler resampler_;
    AudioDecoder audio_;
    double rate_ = 0, offset_ = 0, phase_ = 0, power_ = 0, meanDeviation_ = 0;
    cf32 previous_{0, 0};
    struct Filter {
        double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        std::complex<double> z1{}, z2{};
        std::complex<double> run(std::complex<double> x);
    };
    std::array<Filter, 2> channel_;
};
} // namespace dect2::railway
