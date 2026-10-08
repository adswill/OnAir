// Signal processing of the marine receiver: channel front end (any input rate -> 24 kHz complex), FSK tone slicer, spectrum and FSK
// centre finder, USB audio for the fax decoder. No decoding here.
#pragma once
#include "ring.h"
#include <cstdint>
#include <vector>

namespace dect2 {
namespace marine {

constexpr double kBaseRate = 24000.0;      // complex rate after the front end

// Input at any rate >= 250 kHz: mix the channel to 0 Hz, CIC decimation to about 100 - 190 kHz, then a polyphase windowed-sinc
// resampler to exactly 24 kHz (passband 8 kHz, stop band from 14.5 kHz).
class Front {
public:
    Front();
    void configure(double inputRate);
    void setOffsetHz(double hz);                 // where the channel sits in the input: it is mixed by -hz
    void reset();
    void process(const cf32* x, size_t n, std::vector<cf32>& out);   // appends
    double inputRate() const { return fs_; }
private:
    void resample(std::vector<cf32>& out);
    double fs_ = 0, off_ = 0;
    int dec_ = 1;
    double r1_ = 0, step_ = 0;           // rate after the CIC; input samples per output sample
    cf32 rot_{1.f, 0.f}, rotStep_{1.f, 0.f};
    int rotCount_ = 0;
    uint64_t integ_[2][4] = {};
    uint64_t comb_[2][4] = {};
    int cnt_ = 0;
    double cicScale_ = 1;
    std::vector<cf32> buf_;
    double pos_ = 0;
    static constexpr int kTaps = 192, kPhases = 256;
    std::vector<float> table_;           // (kPhases + 1) x kTaps
};

// Two tones, integrate and dump over one symbol, symbol clock from the energy difference.
// Input: complex samples at fs; tone frequencies relative to 0 Hz; sps = samples per symbol (an integer).
class ToneSlicer {
public:
    ToneSlicer(double fs, double f1, double f2, int sps);
    void reset();
    // Returns true when a symbol decision is ready: soft in [-1, 1], positive when tone 1 is stronger
    bool push(cf32 x, float& soft);
    double symbolRate() const { return rate_; }       // measured from the decisions per second of input
    float lastE1() const { return e1_; }
    float lastE2() const { return e2_; }
private:
    double fs_; int sps_;
    double w1_, w2_;
    double ph1_ = 0, ph2_ = 0;
    std::vector<cf32> r1_, r2_;
    cf32 s1_{0, 0}, s2_{0, 0};
    int idx_ = 0;
    uint64_t n_ = 0, lastEmit_ = 0;
    std::vector<float> avg_;
    int cntSym_ = 0, period_ = 0;
    float e1_ = 0, e2_ = 0;
    uint64_t emits_ = 0; double rate_ = 0; uint64_t rateN_ = 0, rateEmits_ = 0;
};

// FSK on 100 baud, +-85 Hz: centre the channel (NCO), low-pass and decimate to 2400 Hz, then the tone slicer.
class FskSlicer {
public:
    FskSlicer();
    void reset();
    void setCentreHz(double hz) { centreTarget_ = hz; }
    double centreHz() const { return centre_; }
    // Feeds 24 kHz samples; emits one soft decision (positive: the higher tone) per symbol.
    template <class F> void process(const cf32* x, size_t n, F&& onBit) {
        for (size_t i = 0; i < n; i++) {
            cf32 y;
            if (step(x[i], y)) { float s; if (slicer_.push(y, s)) onBit(s); }
        }
    }
    double baud() const { return slicer_.symbolRate(); }
private:
    bool step(cf32 x, cf32& y);
    double centre_ = 0, centreTarget_ = 0, phase_ = 0;
    std::vector<float> taps_;
    std::vector<cf32> ring_;
    int pos_ = 0, phaseDec_ = 0;
    ToneSlicer slicer_;
};

// FM discriminator and 1300 / 2100 Hz AFSK at 1200 baud (VHF channel 70). Positive soft = the lower tone (Y = 1).
class VhfSlicer {
public:
    VhfSlicer();
    void reset();
    template <class F> void process(const cf32* x, size_t n, F&& onBit) {
        for (size_t i = 0; i < n; i++) { float a = disc(x[i]); float s; if (slicer_.push(cf32(a, 0.f), s)) onBit(s); }
    }
    double baud() const { return slicer_.symbolRate(); }
private:
    float disc(cf32 x);
    cf32 prev_{0, 0};
    float lp1_ = 0, lp2_ = 0;
    ToneSlicer slicer_;
};

// 8192-point spectrum of the 24 kHz stream, averaged; finds the FSK pair and gives the noise level
class Spectrum {
public:
    Spectrum();
    void reset();
    // returns true when a new spectrum is ready (every 4096 samples)
    bool push(const cf32* x, size_t n);
    struct Fsk { bool found = false; double centreHz = 0, levelDb = 0, snrDb = 0, highHz = 0, lowHz = 0; };
    Fsk findFsk(double shiftHz, double searchHz) const;
    void audioDb(std::vector<float>& out, double loHz, double hiHz, int bins) const;   // dB, max of the FFT bins inside each output bin
    double noisePerBin() const;                     // median power, linear
    double binHz() const { return kBaseRate / kN; }
    double freqPower(double hz) const;
private:
    static constexpr int kN = 8192;
    std::vector<float> avg_;
    std::vector<cf32> buf_;
    std::vector<float> win_;
    std::vector<cf32> tmp_;
    double win2_ = 0;
    int fill_ = 0;
    int frames_ = 0;
    bool primed_ = false;
    void* fft_ = nullptr;
public:
    ~Spectrum();
    Spectrum(const Spectrum&) = delete;
    Spectrum& operator=(const Spectrum&) = delete;
};

// USB audio for the weather fax: 24 kHz complex -> real audio at 12 kHz, band 50 .. 3950 Hz of the upper sideband, with a slow AGC
class FaxAudio {
public:
    FaxAudio();
    void reset();
    void process(const cf32* x, size_t n, std::vector<float>& out);   // appends
private:
    std::vector<cf32> taps_;
    std::vector<cf32> ring_;
    int pos_ = 0, phase_ = 0;
    float env_ = 0;
};

} // namespace marine
} // namespace dect2
