// Inside the 4.0 signal receivers: the decoder interface and the small DSP pieces they share.
#pragma once
#include "dect2/x40/x40_rx.h"
#include "dect2/x40/x40_tel.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace dect2::x40 {

using dect2::math::kPi;

// What a decoder reports to the receiver
struct Sink {
    virtual ~Sink() = default;
    virtual void row(const char* kind, const std::string& id, const std::string& text, float levelDb) = 0;
    virtual void good(float snrDb = 0, double cfoHz = 0) = 0;
    virtual void bad() = 0;
    virtual void energy() = 0;               // something is on the channel
    virtual double now() const = 0;
};

class Decoder {
public:
    virtual ~Decoder() = default;
    virtual bool configure(double fs) = 0;
    virtual void reset() = 0;
    virtual void feed(const cf32* x, size_t n) = 0;
    virtual void fill(X40Telemetry& t) const = 0;      // headline and facts
    virtual void setCenterHz(double) {}
    void attach(Sink* s) { out = s; }
protected:
    Sink* out = nullptr;
};

std::unique_ptr<Decoder> makeDecoder(int variant);
std::unique_ptr<Decoder> makeRcPpm();
std::unique_ptr<Decoder> makeUvb76();
std::unique_ptr<Decoder> makeGraves();
std::unique_ptr<Decoder> makeJove();
std::unique_ptr<Decoder> makeBeacon();
std::unique_ptr<Decoder> makeEm4100();
std::unique_ptr<Decoder> makeUhfRfid();
std::unique_ptr<Decoder> makeQi();
std::unique_ptr<Decoder> makeFmMon();
std::unique_ptr<Decoder> makeAmMon();
std::unique_ptr<Decoder> makePlNoise();
std::unique_ptr<Decoder> makeRadar();
std::unique_ptr<Decoder> makeGnssJam();
std::unique_ptr<Decoder> makePassband();
std::unique_ptr<Decoder> makeVlfMsk();
std::unique_ptr<Decoder> makeOccupancy();
std::unique_ptr<Decoder> makeEvm();
std::unique_ptr<Decoder> makeFreqAcc();
std::unique_ptr<Decoder> makePttLog();
std::unique_ptr<Decoder> makeIntermod();
std::unique_ptr<Decoder> makeSpur();
std::unique_ptr<Decoder> makeDopplerLog();
std::unique_ptr<Decoder> makeHopAnalyser();
std::unique_ptr<Decoder> makeBirdie();
std::unique_ptr<Decoder> makeBurst();
std::unique_ptr<Decoder> makeFading();
std::unique_ptr<Decoder> makeInputHealth();
std::unique_ptr<Decoder> makePhaseNoise();
std::unique_ptr<Decoder> makeSsbTest();
std::unique_ptr<Decoder> makeSinad();
std::unique_ptr<Decoder> makeFskAnalyser();
std::unique_ptr<Decoder> makeYFactor();
std::unique_ptr<Decoder> makeCwKeying();
std::unique_ptr<Decoder> makeNoiseFloor();
std::unique_ptr<Decoder> makeNfmDev();
std::unique_ptr<Decoder> makeAfsk();
std::unique_ptr<Decoder> makeOokPulse();
std::unique_ptr<Decoder> makeTxStability();
std::unique_ptr<Decoder> makeOccBw();
std::unique_ptr<Decoder> makePapr();
std::unique_ptr<Decoder> makeSpecKurt();

// Windowed-sinc low-pass FIR with decimation by an integer factor. Complex in, complex out. Self-contained so the decoders can be tested alone.
class Decimator {
public:
    // cutoffHz: the pass edge; the decimated rate is fs / d. Returns false when fs / d < 2 * cutoff.
    bool configure(double fs, int d, double cutoffHz) {
        d_ = std::max(1, d); fs_ = fs;
        if (fs / d_ < 2 * cutoffHz * 0.999) return false;
        const int taps = std::max(31, (int)(4.0 * fs / std::max(50.0, fs / d_ * 0.5 - cutoffHz + 1.0)) | 1);
        const int n = std::min(taps, 2047) | 1;
        h_.assign(n, 0.f);
        const double fc = (cutoffHz + (fs / d_ * 0.5 - cutoffHz) * 0.5) / fs;   // transition centre
        double s = 0;
        for (int i = 0; i < n; i++) {
            const double m = i - (n - 1) / 2.0;
            const double sinc = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
            const double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (n - 1)) + 0.08 * std::cos(4 * kPi * i / (n - 1));
            h_[i] = (float)(sinc * w); s += h_[i];
        }
        for (auto& v : h_) v /= (float)s;
        hist_.assign(n, cf32(0, 0)); pos_ = 0; phase_ = 0;
        return true;
    }
    void reset() { std::fill(hist_.begin(), hist_.end(), cf32(0, 0)); pos_ = 0; phase_ = 0; }
    double outRate() const { return fs_ / d_; }
    template <class F> void process(const cf32* x, size_t n, F&& emit) {
        const int L = (int)h_.size();
        for (size_t i = 0; i < n; i++) {
            hist_[pos_] = x[i];
            if (++pos_ == L) pos_ = 0;
            if (++phase_ >= d_) {
                phase_ = 0;
                cf32 a(0, 0); int k = pos_;
                for (int j = 0; j < L; j++) { a += hist_[k] * h_[j]; if (++k == L) k = 0; }
                emit(a);
            }
        }
    }
private:
    std::vector<float> h_; std::vector<cf32> hist_;
    int d_ = 1, pos_ = 0, phase_ = 0; double fs_ = 0;
};

inline std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
inline std::string fmt(const char* f, ...) {
    char b[512];
    va_list ap; va_start(ap, f); vsnprintf(b, sizeof b, f, ap); va_end(ap);
    return b;
}

inline float toDb(float p) { return 10 * std::log10(std::max(p, 1e-14f)); }

} // namespace dect2::x40
