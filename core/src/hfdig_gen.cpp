// HF digital test signal (see hfdig_gen.h): a decoder's test audio on the upper sideband, over noise. The test audio itself comes from
// the decoders' own files.
#include "dect2/hfdig_gen.h"
#include "dect2/hfdig_rx.h"
#include "dect2/gen_util.h"
#include "hfdig_usb.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <memory>
#include <vector>

namespace dect2 {

std::unique_ptr<HfdigTestAudio> makeHfdigTestAudio(int which, const SynthConfig& cfg) {
    switch (which) {
    case 0: return makeRttyTestAudio(cfg);
    case 1: return makeSstvTestAudio(cfg);
    case 2: return makeFreedvTestAudio(cfg);
    default: return nullptr;
    }
}

namespace {

constexpr float kNoiseSigma = 0.02f;   // per component: about -31 dBFS of noise over the whole band

class HfdigSynth : public ModeSynth {
public:
    HfdigSynth(const SynthConfig& c, double rate) : rate_(rate), audio_(makeHfdigTestAudio(c.modeOpt[0], c)) {
        if (!audio_) return;
        const auto h = hfdig::usbTaps(kTaps, kHfdigAudioRate);
        taps_.resize(kTaps);
        for (int j = 0; j < kTaps; j++) taps_[(size_t)j] = h[(size_t)(kTaps - 1 - j)];   // the history is kept oldest first
        hist_.assign(2 * kTaps, 0.f);
        // the noise in 3 kHz against audio with an rms of 0.35, whose upper sideband has twice its power
        const double noise3k = 2.0 * kNoiseSigma * kNoiseSigma * 3000.0 / rate_;
        amp_ = (float)std::min(0.9, std::sqrt(std::pow(10.0, c.snrDb / 10.0) * noise3k / (2 * 0.35 * 0.35)));
        // the dial frequency sits at -tuneOffsetHz in the samples (the radio is tuned above it), plus the carrier offset
        const double w = 2 * hfdig::kPi * (c.cfoHz - hfdigTuning().tuneOffsetHz) / rate_;
        step_ = cf32((float)std::cos(w), (float)std::sin(w));
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        for (size_t i = 0; i < n; i++) out[i] = cf32(0.f, 0.f);
        if (audio_) {
            const double adv = kHfdigAudioRate / rate_;
            for (size_t i = 0; i < n; i++) {
                while (frac_ >= 1.0) { frac_ -= 1.0; prev_ = next_; next_ = nextSideband(); }
                const cf32 v = prev_ + (next_ - prev_) * (float)frac_;   // linear interpolation: the images land outside the receiver's 200 .. 3800 Hz
                out[i] = v * rot_ * amp_;
                rot_ *= step_;
                frac_ += adv;
            }
            rot_ /= std::abs(rot_);
        }
        noise_.add(out, n, kNoiseSigma);
    }

private:
    static constexpr int kTaps = 255;   // about 270 Hz of transition at 8 kHz
    // the next 8 kHz sample of the upper sideband of the test audio
    cf32 nextSideband() {
        if (bufPos_ >= buf_.size()) { buf_.assign(256, 0.f); audio_->generate(buf_.data(), buf_.size()); bufPos_ = 0; }
        const float a = buf_[bufPos_++];
        hist_[(size_t)pos_] = a; hist_[(size_t)(pos_ + kTaps)] = a;
        if (++pos_ >= kTaps) pos_ = 0;
        const float* w = &hist_[(size_t)pos_];
        cf32 s(0.f, 0.f);
        for (int k = 0; k < kTaps; k++) s += taps_[(size_t)k] * w[k];
        return s * 2.f;   // a real tone is two halves, at plus and minus its frequency; the band-pass keeps one: back to the tone's amplitude
    }
    double rate_;
    std::unique_ptr<HfdigTestAudio> audio_;
    std::vector<cf32> taps_;
    std::vector<float> hist_, buf_;
    size_t bufPos_ = 0;
    int pos_ = 0;
    double frac_ = 1.0;
    cf32 prev_{0.f, 0.f}, next_{0.f, 0.f}, rot_{1.f, 0.f}, step_{1.f, 0.f};
    float amp_ = 0;
    genutil::NoiseSource noise_{1};
};

} // namespace

std::unique_ptr<ModeSynth> makeHfdigSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<HfdigSynth>(cfg, sampleRate);
}

} // namespace dect2
