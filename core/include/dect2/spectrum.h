#pragma once
#include "ring.h"
#include <cstdint>
#include <vector>

namespace dect2 {

struct SignalStats {
    float peak = 0;        // linear, full scale = 1
    float rmsDbfs = -120;
    float clipFraction = 0; // fraction of samples within 1 LSB of full scale
    float dcI = 0, dcQ = 0;
    uint32_t hist[64] = {}; // |I| and |Q| magnitude histogram (ADC occupancy)
};

struct SpectrumFrame {
    std::vector<float> dbfs; // fftSize bins, DC-centred (-fs/2 .. +fs/2)
    SignalStats stats;
    uint64_t seq = 0;
};

class SpectrumAnalyzer {
public:
    explicit SpectrumAnalyzer(size_t fftSize = 4096);
    ~SpectrumAnalyzer();
    size_t fftSize() const { return n_; }

    // Consume samples; average power over everything fed since the last takeFrame().
    void feed(const cf32* x, size_t n);
    bool takeFrame(SpectrumFrame& out); // false if nothing accumulated
    void reset();                       // forget accumulated data (after retuning)

private:
    struct Impl;
    Impl* p_;
    size_t n_;
};

} // namespace dect2
