// Engine-thread sensors for the automatic gain (docs/AUTOGAIN.md, "Auto v2"). The old loop read the display analyser's statistics: every
// fourth sample, averaged over a GUI frame, so short overload bursts were under-counted and the loop stalled with the window hidden. The
// GainMeter looks at EVERY sample of the radio's block, on the engine's thread, before any correction:
//   - the exact peak, the number of samples within 1 LSB of full scale, a 256-bin histogram of max(|I|,|Q|) (robust peak p99.99),
//   - a leaky peak-hold with a 2 s tail (pulsed signals: ADS-B pulses are far under 0.1 % of the samples),
//   - a 10 Hz power spectrum from its own 2048-point FFT (Hann), averaged by power, for the channel SNR meter and the out-of-band meter.
// The GUI / the controller drain it with take(): everything since the last take, whatever thread or rate asks, so Auto works headless.
#pragma once
#include "dect2/source/autogain.h"
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <mutex>
#include <vector>

namespace dect2::ag {

class GainMeter {
public:
    static constexpr int kFft = 2048;
    static constexpr int kHist = 256;
    GainMeter();
    void configure(double rateHz, int adcBits);     // a new capture rate / ADC: restarts the statistics
    void reset();
    // Engine thread: every block of the radio, as delivered (before DC / IQ correction and resampling)
    void process(const std::complex<float>* x, size_t n);
    // Everything since the last take (any thread). false: no new samples. spec is DC-centred dBFS per bin (total power across bins = rms^2).
    bool take(BlockStats& adc, std::vector<float>& spec);
    // Peek without draining (the display)
    BlockStats peek() const;
    uint64_t samples() const { return total_.load(std::memory_order_relaxed); }

private:
    void window();
    void spectrumBlock();
    float winNorm_ = 1.f;
    double rate_ = 0; int bits_ = 8;
    // accumulators of the engine thread (mutex-guarded copy into pub_ once per 50 ms window)
    float wPeak_ = 0; uint64_t wClip_ = 0, wN_ = 0; double wPow_ = 0; uint32_t wHist_[kHist] = {};
    size_t wTarget_ = 1;
    // hold
    float hold_ = 0; uint64_t holdAge_ = 0;
    // spectrum
    std::vector<float> win_, re_, im_, specAcc_; std::vector<std::complex<float>> buf_;
    size_t bufFill_ = 0, specSinceFft_ = 0, specEvery_ = 1;
    uint32_t specN_ = 0;
    // published (since last take)
    mutable std::mutex mu_;
    float pPeak_ = 0, pHold_ = 0; uint64_t pClip_ = 0, pN_ = 0; double pPow_ = 0; uint32_t pHist_[kHist] = {};
    std::vector<float> pSpecPow_; uint32_t pSpecN_ = 0;
    std::vector<float> lastSpec_;
    std::atomic<uint64_t> total_{0};
};

// The headroom the hold gives, dB under full scale of the peak-hold (120 when none)
inline double holdHeadroomDb(float hold) { return hold > 1e-6f ? -20.0 * std::log10((double)hold) : 120.0; }

} // namespace dect2::ag
