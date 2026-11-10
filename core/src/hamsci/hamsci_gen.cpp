// Test signals of the ham science modes (hamsci_gen.h).
#include "dect2/hamsci/hamsci_gen.h"
#include "dect2/hamsci/hamsci_dsp.h"
#include "dect2/hamsci/hamsci_tel.h"
#include "hamsci_int.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace dect2 {

namespace {
using hamsci::detail::kPi;

class HamsciSynth final : public ModeSynth {
public:
    HamsciSynth(const SynthConfig& c, double rate, int kind) : cfg_(c), rate_(rate), kind_(kind), rng_(1234u + (unsigned)kind) { prepare(); }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        if (kind_ == kHsPulsar) { genPulsar(out, n); return; }
        if (kind_ == kHsSid) { genSid(out, n); return; }
        if (kind_ == kHsRiometer) { genRio(out, n); return; }
        genAudio(out, n);
    }
private:
    void prepare() {
        const double fs = hamsci::kAudioRate;
        const double snr = cfg_.snrDb >= 99 ? 30.0 : cfg_.snrDb;
        const double off = cfg_.cfoHz;
        switch (kind_) {
        case kHsQrss: audio_ = hamsci::qrssGenerate("CQ", 3.0, fs, 1000 + off, false, 0, 0.3, 5.0); break;
        case kHsDfcw: audio_ = hamsci::qrssGenerate("CQ", 6.0, fs, 1000 + off, true, 1.5, 0.3, 5.0); break;
        case kHsFsk441: {
            audio_.assign((size_t)(24 * fs), 0.f);
            const double starts[] = {2.0, 8.5, 14.0, 19.5};
            for (double s : starts) {
                auto b = hamsci::fsk441Generate("W9XYZ K1JT 26", fs, 1 + (int)(s) % 3, off, 0.3);
                b.resize(std::min(b.size(), (size_t)(0.5 * fs)));
                for (size_t i = 0; i < b.size(); i++) audio_[(size_t)(s * fs) + i] += b[i];
            }
            break;
        }
        case kHsIscat: audio_ = hamsci::iscatGenerate("CQ DX K1JT FN20", std::clamp(cfg_.modeOpt[0], 0, 1), fs, 30.0, off, 0.3, 0.0); break;
        case kHsPi4: { audio_ = hamsci::pi4Generate("PI4ABC", fs, 1200 + off, 0.3, 0.0, 30.0); break; }
        case kHsThrob: audio_ = hamsci::throbGenerate("CQ CQ DE W1AW K", std::clamp(cfg_.modeOpt[0], 0, 5), fs, 1500 + off, 0.3, 4); break;
        default: break;
        }
        if (audio_.empty() && kind_ <= kHsThrob) audio_.assign((size_t)fs, 0.f);
        double p = 0;
        for (float v : audio_) p += (double)v * v;
        p /= std::max<size_t>(1, audio_.size());
        // the signal power in the 2500 Hz reference band is the audio power over two (the real signal has two sidebands, the receiver keeps one); the keyed modes are off part of
        // the time, so the figure is a mean power, not the key-down power
        double on = 0;
        size_t cnt = 0;
        for (float v : audio_) if (std::fabs(v) > 1e-4f) { on += (double)v * v; cnt++; }
        const double sigPow = cnt ? on / (double)cnt / 2.0 : 0.05;
        sigma_ = std::sqrt(sigPow / std::pow(10.0, snr / 10.0) * (rate_ / 2.0) / 2500.0);
        (void)p;
    }

    void genAudio(cf32* out, size_t n) {
        std::normal_distribution<float> g(0.f, 1.f);
        const double step = hamsci::kAudioRate / rate_;
        for (size_t i = 0; i < n; i++) {
            size_t i0 = (size_t)pos_;
            const double fr = pos_ - (double)i0;
            const size_t i1 = (i0 + 1) % audio_.size();
            i0 %= audio_.size();
            const float a = (float)((1.0 - fr) * audio_[i0] + fr * audio_[i1]);
            out[i] = cf32(a + (float)sigma_ * g(rng_) / 1.4142f, (float)sigma_ * g(rng_) / 1.4142f);
            pos_ += step;
            if (pos_ >= (double)audio_.size()) pos_ -= (double)audio_.size();
        }
    }

    void genSid(cf32* out, size_t n) {
        // stations around a dial at 22 kHz: NAA +2 kHz, NLK +2.8 kHz, NPM -0.6 kHz, DHO38 +1.4 kHz; noise 1.0 per complex sample, carriers 20 / 16 / 12 / 10 dB over
        // the noise per sample; the flare option steps NAA by 3 dB at 17 minutes
        struct C { double off, amp; };
        static const C cs[] = {{2000, 10.0}, {2800, 6.3}, {-600, 4.0}, {1400, 3.2}};
        std::normal_distribution<float> g(0.f, 1.f);
        for (size_t i = 0; i < n; i++) {
            cf32 v(g(rng_) / 1.4142f, g(rng_) / 1.4142f);
            const double t = (double)total_ / rate_;
            for (size_t c = 0; c < 4; c++) {
                double a = cs[c].amp * 0.05;
                if (c == 0 && cfg_.modeOpt[0] == 1 && t >= 17 * 60) a *= 1.41;
                const double ph = 2 * kPi * cs[c].off * t;
                v += cf32((float)(a * std::cos(ph)), (float)(a * std::sin(ph)));
            }
            out[i] = v;
            total_++;
        }
    }

    void genRio(cf32* out, size_t n) {
        std::normal_distribution<float> g(0.f, 1.f);
        std::uniform_real_distribution<double> u(0, 1);
        const size_t blk = std::max<size_t>(1, (size_t)(rate_ * 0.02));
        for (size_t i = 0; i < n; i++) {
            if (total_ % blk == 0) burst_ = u(rng_) < 0.03;
            const double t = (double)total_ / rate_;
            double a = 0.1;
            if (std::fmod(t, 600.0) > 300.0) a *= std::pow(10.0, -0.5 / 20.0);   // 0.5 dB steps down every five minutes: shows in the power trace
            if (burst_) a *= 3.0;
            out[i] = cf32((float)(a * g(rng_)), (float)(a * g(rng_)));
            total_++;
        }
    }

    void genPulsar(cf32* out, size_t n) {
        for (size_t i = 0; i < n; i++) {
            if (pos2_ >= chunk_.size()) {
                hamsci::PulsarConfig c;
                c.rateHz = rate_; c.centreHz = 408e6; c.channels = 64; c.periodS = 0.714520; c.dm = 26.76;
                const double snr = cfg_.modeVal[0] > 0 ? cfg_.modeVal[0] : 0.1;
                chunk_ = hamsci::pulsarRender(c, 5.0, snr, 99 + (uint32_t)block_, 0.03, 26.76, 0.714520);
                block_++;
                pos2_ = 0;
            }
            out[i] = chunk_[pos2_++] * 0.2f;
        }
    }

    SynthConfig cfg_;
    double rate_;
    int kind_;
    std::mt19937 rng_;
    std::vector<float> audio_;
    double pos_ = 0, sigma_ = 0.1;
    uint64_t total_ = 0;
    bool burst_ = false;
    std::vector<cf32> chunk_;
    size_t pos2_ = 0;
    uint64_t block_ = 0;
};
}

std::unique_ptr<ModeSynth> makeHamsciSynth(const SynthConfig& cfg, double sampleRate, int kind) { return std::make_unique<HamsciSynth>(cfg, sampleRate, kind); }

}  // namespace dect2
