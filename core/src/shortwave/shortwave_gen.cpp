// Shortwave test signal (see shortwave_gen.h).
#include "dect2/shortwave/shortwave_gen.h"
#include "dect2/shortwave/shortwave_rx.h"
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_rtty.h"
#include "dect2/cw/cw_gen.h"
#include "dect2/psk/psk_gen.h"
#include "dect2/common/mathconst.h"
#include <cmath>
#include <complex>
#include <vector>

namespace dect2 {

namespace {

class ShortwaveSynth : public ModeSynth {
public:
    ShortwaveSynth(const SynthConfig& s, double rate) : rate_(rate) {
        const int which = s.modeOpt[0];
        const double tune = shortwaveTuning().tuneOffsetHz;
        const double extra = s.modeVal[0];
        const double dialInSamples = -tune + s.cfoHz;
        const bool wantAm = which == 0 || which == 1, wantRtty = which == 0 || which == 2, wantCw = which == 0 || which == 3, wantPsk = which == 0 || which == 4;
        if (wantAm) {
            amHz_ = dialInSamples + kSwSynthAmHz;
            am_ = true;
        }
        // RTTY (also the noise floor: the HF digital generator without audio is noise only)
        SynthConfig rc = s;
        rc.cfoHz = s.cfoHz + (kSwSynthRttyCentreHz - 1360.0) + extra;
        rc.snrDb = s.snrDb >= 99 || s.snrDb <= 0 ? 30 : s.snrDb;
        rc.modeOpt[0] = 0;
        std::unique_ptr<HfdigTestAudio> audio;
        if (wantRtty) audio = makeRttyTxAudio("", 0, 0, false, kSwSynthRttyLowHz);
        base_ = makeHfdigSynthFromAudio(std::move(audio), rc, rate);
        if (wantCw) {
            CwGenConfig c;
            c.rate = rate; c.dialOffsetHz = dialInSamples; c.noiseSigma = 0;
            CwGenSignal g;
            g.offsetHz = kSwSynthRttyCentreHz + extra; g.wpm = 20; g.snrDb = 38; g.text = "CQ CQ DE ONAIR ONAIR K"; g.pauseSec = 3;
            c.signals.push_back(g);
            cw_ = std::make_unique<CwGenerator>(c);
        }
        if (wantPsk) {
            PskGenConfig c;
            c.rate = rate; c.dialOffsetHz = dialInSamples; c.noiseSigma = 0;
            PskGenSignal g;
            g.offsetHz = kSwSynthPskHz + extra; g.mode = 0; g.snrDb = 28; g.text = "CQ CQ DE LY2ABC LY2ABC PSE K\n"; g.pauseSec = 2;
            c.signals.push_back(g);
            psk_ = std::make_unique<PskGenerator>(c);
        }
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        base_->generate(out, n);
        if (cw_ || psk_ || am_) tmp_.resize(n);
        if (cw_) { cw_->generate(tmp_.data(), n); for (size_t i = 0; i < n; i++) out[i] += tmp_[i]; }
        if (psk_) { psk_->generate(tmp_.data(), n); for (size_t i = 0; i < n; i++) out[i] += tmp_[i]; }
        if (am_) {
            const double w = 2 * math::kPi * amHz_ / rate_, wt = 2 * math::kPi * 1000.0 / rate_;
            for (size_t i = 0; i < n; i++) {
                const double env = 0.12 * (1.0 + 0.6 * std::sin(tph_));
                out[i] += cf32((float)(env * std::cos(ph_)), (float)(env * std::sin(ph_)));
                ph_ += w; tph_ += wt;
                if (ph_ > 2 * math::kPi) ph_ -= 2 * math::kPi;
                if (tph_ > 2 * math::kPi) tph_ -= 2 * math::kPi;
            }
        }
    }
private:
    double rate_, amHz_ = 0, ph_ = 0, tph_ = 0;
    bool am_ = false;
    std::unique_ptr<ModeSynth> base_;
    std::unique_ptr<CwGenerator> cw_;
    std::unique_ptr<PskGenerator> psk_;
    std::vector<cf32> tmp_;
};

}  // namespace

std::unique_ptr<ModeSynth> makeShortwaveSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<ShortwaveSynth>(cfg, sampleRate);
}

}  // namespace dect2
