// Analog voice for TX: NFM, AM, USB/LSB of a microphone, a file, a 1 kHz tone or silence. Broadcast FM (stereo + RDS) lives in fm_gen.cpp.
#include "dect2/tx/tx.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/audio/audio_control.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;

// 48 kHz stereo from the session → one sample of mono at `rate` (linear).
struct PullMono {
    std::function<int(float*, int)> src;
    int srcRate = 48000;
    double pos = 1;
    float prev = 0, next = 0;
    float peek() {
        while (pos >= 1.0) {
            pos -= 1.0;
            prev = next;
            float st[2] = {0, 0};
            if (src) src(st, 1);
            next = 0.5f * (st[0] + st[1]);
        }
        const float y = prev + (next - prev) * (float)pos;
        return y;
    }
    void advance(double adv) { pos += adv; }
};

// A 2nd-order Butterworth section (high-pass or low-pass) at the 48 kHz audio rate
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    bool on = false;
    void set(double fs, double fc, bool high) {
        on = fc > 0 && fc < fs * 0.45;
        if (!on) return;
        const double w = 2 * kPi * fc / fs, c = std::cos(w), sn = std::sin(w), al = sn / (2 * 0.70710678), a0 = 1 + al;
        if (high) { b0 = (float)((1 + c) / 2 / a0); b1 = (float)(-(1 + c) / a0); b2 = b0; }
        else { b0 = (float)((1 - c) / 2 / a0); b1 = (float)((1 - c) / a0); b2 = b0; }
        a1 = (float)(-2 * c / a0); a2 = (float)((1 - al) / a0);
    }
    float run(float x) {
        if (!on) return x;
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// Catmull-Rom interpolation between the 2nd and 3rd of four values (the audio and the SSB baseband are made at 48 kHz and brought up to the radio's rate)
inline float cubic(const float* v, float t) {
    return v[1] + 0.5f * t * (v[2] - v[0] + t * (2.f * v[0] - 5.f * v[1] + 4.f * v[2] - v[3] + t * (3.f * (v[1] - v[2]) + v[3] - v[0])));
}

// Analog voice. The audio is made at 48 kHz (microphone / file through r.pullAudio, or the test tone): high-pass, low-pass, compressor, pre-emphasis (FM), VOX gate;
// then modulated at the radio's rate: AM (full carrier or DSB suppressed carrier), USB / LSB (Hilbert, optional carrier injection) or FM with an optional CTCSS tone.
// The keys it reads from TxRequest::opt: audio.hpHz, audio.lpHz, audio.compOn, audio.compThreshDb, audio.compRatio, audio.compMakeupDb, audio.compReleaseMs,
// audio.toneLevelPct, fm.preemphUs, audio.tone2Hz, am.type, am.depthPct, am.carrierLevel, ssb.level, ssb.carrierPct, fm.level, fm.ctcssHz, fm.ctcssPct, vox.on, vox.threshDb, vox.hangMs.
class AnalogVoice : public ModeSynth {
public:
    AnalogVoice(double rate, double offHz, int kind, const TxRequest& r)
        : rate_(rate), off_(offHz), kind_(kind), pull_(r.pullAudio) {
        tone_ = r.toneHz > 0 ? r.toneHz : 1000;
        tone2_ = r.o("audio.tone2Hz", 0);
        toneAmp_ = (float)(r.o("audio.toneLevelPct", 50) / 100.0);
        dev_ = r.deviationHz > 0 ? r.deviationHz : (kind == 1 ? 5000.0 : 2500.0);
        pre_ = r.o("fm.preemphUs", r.preemphUs > 0 ? r.preemphUs : 50);   // 0 = off
        silence_ = !pull_ && r.audioSrc == 3;
        hp_.set(kFs, r.o("audio.hpHz", 0), true);
        const double lp = r.o("audio.lpHz", 0);
        lp1_.set(kFs, lp, false);
        lp2_.set(kFs, lp, false);
        compOn_ = r.o("audio.compOn", 0) > 0.5;
        compThr_ = (float)r.o("audio.compThreshDb", -18);
        compRatio_ = (float)std::max(1.0, r.o("audio.compRatio", 4));
        compMake_ = (float)std::pow(10.0, r.o("audio.compMakeupDb", 0) / 20.0);
        compRel_ = (float)std::exp(-1.0 / (kFs * std::max(5.0, r.o("audio.compReleaseMs", 200)) * 1e-3));
        compAtt_ = (float)std::exp(-1.0 / (kFs * 5e-3));
        voxOn_ = r.o("vox.on", 0) > 0.5;
        voxThr_ = (float)std::pow(10.0, r.o("vox.threshDb", -35) / 20.0);
        voxHang_ = (int)(r.o("vox.hangMs", 500) * 1e-3 * kFs);
        amType_ = (int)r.o("am.type", 0);
        amDepth_ = (float)(r.o("am.depthPct", 85) / 100.0);
        amLevel_ = (float)r.o("am.carrierLevel", 0.55);
        ssbLevel_ = (float)r.o("ssb.level", 0.4);
        ssbCar_ = (float)(r.o("ssb.carrierPct", 0) / 100.0);
        fmLevel_ = (float)r.o("fm.level", 0.40);
        ctcss_ = r.o("fm.ctcssHz", 0);
        if (const int ix = (int)r.o("fm.ctcssIdx", 0); ix > 0) ctcss_ = txCtcssToneHz(ix);   // the panel's pull-down of the standard tones (#373)
        ctcssPct_ = r.o("fm.ctcssPct", 12) / 100.0;   // the usual 600 Hz at 5 kHz deviation
        step_ = kFs / rate_;
        acc_ = 1.0;
        // Hilbert taps (odd, zeros on even), Hamming-windowed: delay of kHilN samples on the real path
        hil_.assign((size_t)(2 * kHilN + 1), 0.f);
        for (int k = -kHilN; k <= kHilN; k++)
            if (k % 2) hil_[(size_t)(k + kHilN)] = (float)(2.0 / (kPi * k) * (0.54 + 0.46 * std::cos(kPi * k / (kHilN + 1))));
        hist2_.assign(2 * hil_.size(), 0.f);
        for (int i = 0; i < 4; i++) bI_[i] = bQ_[i] = bA_[i] = 0;
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        const double w0 = 2 * kPi * off_ / rate_;
        for (size_t i = 0; i < n; i++) {
            acc_ += step_;
            while (acc_ >= 1.0) { acc_ -= 1.0; nextAudio(); }
            const float t = (float)acc_;
            const float a = cubic(bA_, t);
            if (kind_ == 2) {   // AM
                const float m = amType_ == 1 ? amLevel_ * a * amDepth_ : amLevel_ * (1.f + amDepth_ * a) * gate_;
                out[i] = cf32(m * (float)std::cos(cph_), m * (float)std::sin(cph_));
                cph_ += w0;
            } else if (kind_ == 3 || kind_ == 4) {   // USB / LSB, carrier suppressed (or reduced when ssb.carrierPct is set) at `off`
                const float I = ssbLevel_ * (cubic(bI_, t) + ssbCar_ * gate_), Q = ssbLevel_ * cubic(bQ_, t);
                const float cs = (float)std::cos(cph_), sn = (float)std::sin(cph_);
                out[i] = cf32(I * cs - Q * sn, I * sn + Q * cs);
                cph_ += w0;
            } else {   // NFM (kind 1) or generic FM
                double mod = a;
                if (ctcss_ > 0) { mod += ctcssPct_ * std::sin(cph2_); cph2_ += 2 * kPi * ctcss_ / rate_; if (cph2_ > 2 * kPi) cph2_ -= 2 * kPi; }
                cph_ += w0 + 2 * kPi * (dev_ * mod) / rate_;
                out[i] = cf32(fmLevel_ * (float)std::cos(cph_) * gate_, fmLevel_ * (float)std::sin(cph_) * gate_);
            }
            if (cph_ > kPi) cph_ -= 2 * kPi; else if (cph_ < -kPi) cph_ += 2 * kPi;
        }
    }
private:
    static constexpr int kHilN = 127;
    static constexpr double kFs = 48000.0;
    // one 48 kHz audio sample through the chain, then the baseband for the sideband modes; the VOX gate is smoothed over about 5 ms
    void nextAudio() {
        float x;
        if (pull_) { float st[2] = {0, 0}; pull_(st, 1); x = 0.5f * (st[0] + st[1]); }
        else if (silence_) x = 0;
        else {
            x = toneAmp_ * (float)std::sin(tph_);
            tph_ += 2 * kPi * tone_ / kFs;
            if (tone2_ > 0) { x = 0.5f * (x + toneAmp_ * (float)std::sin(tph2_)); tph2_ += 2 * kPi * tone2_ / kFs; if (tph2_ > 2 * kPi) tph2_ -= 2 * kPi; }
            if (tph_ > 2 * kPi) tph_ -= 2 * kPi;
        }
        x = lp2_.run(lp1_.run(hp_.run(x)));
        const float ax = std::fabs(x);
        if (compOn_) {
            env_ = ax > env_ ? compAtt_ * env_ + (1 - compAtt_) * ax : compRel_ * env_ + (1 - compRel_) * ax;
            const float db = 20.f * std::log10(std::max(env_, 1e-6f));
            const float over = db - compThr_;
            const float g = over > 0 ? -over * (1.f - 1.f / compRatio_) : 0.f;
            x *= std::pow(10.f, g / 20.f) * compMake_;
        }
        if (kind_ == 1 && pull_ && pre_ > 0) {   // pre-emphasis of FM voice
            const float k = (float)(pre_ * 1e-6 * kFs);
            const float p = x + k * (x - preX_);
            preX_ = x;
            x = p;
        }
        x = std::max(-1.f, std::min(1.f, x));
        if (voxOn_) {
            if (ax >= voxThr_) hang_ = voxHang_; else if (hang_ > 0) hang_--;
            const float target = hang_ > 0 ? 1.f : 0.f;
            gate_ += (target - gate_) * 0.04f;
            x *= gate_;
        } else gate_ = 1.f;
        for (int i = 0; i < 3; i++) bA_[i] = bA_[i + 1];
        bA_[3] = x;
        if (kind_ == 3 || kind_ == 4) {
            const size_t N = hil_.size();
            hist2_[hpos_] = x; hist2_[hpos_ + N] = x;
            const float* w = hist2_.data() + hpos_;
            float im = 0;
            for (size_t k = 0; k < N; k += 2) im += hil_[k] * w[k];   // only the taps with k - kHilN odd are not zero: they sit at even indices
            const float re = w[(size_t)kHilN];
            if (++hpos_ >= N) hpos_ = 0;
            for (int i = 0; i < 3; i++) { bI_[i] = bI_[i + 1]; bQ_[i] = bQ_[i + 1]; }
            bI_[3] = re;
            bQ_[3] = (kind_ == 4 ? 1.f : -1.f) * im;   // USB (3) puts the tone above the dial frequency, LSB (4) below it (checked by tests/tx/test_tx_params.cpp)
        }
    }
    double rate_, off_, tone_, tone2_ = 0, dev_, pre_, cph_ = 0, cph2_ = 0, tph_ = 0, tph2_ = 0, step_ = 1, acc_ = 1, ctcss_ = 0, ctcssPct_ = 0.15;
    int kind_ = 1, amType_ = 0, voxHang_ = 0, hang_ = 0;
    std::function<int(float*, int)> pull_;
    bool silence_ = false, compOn_ = false, voxOn_ = false;
    float toneAmp_ = 0.5f, preX_ = 0, env_ = 0, compThr_ = -18, compRatio_ = 4, compMake_ = 1, compRel_ = 0, compAtt_ = 0, voxThr_ = 0.018f, gate_ = 1;
    float amDepth_ = 0.85f, amLevel_ = 0.55f, ssbLevel_ = 0.4f, ssbCar_ = 0, fmLevel_ = 0.4f;
    Biquad hp_, lp1_, lp2_;
    float bA_[4], bI_[4], bQ_[4];
    std::vector<float> hil_, hist2_;
    size_t hpos_ = 0;
};

class MixSynth : public ModeSynth {
public:
    MixSynth(std::unique_ptr<ModeSynth> a, std::unique_ptr<ModeSynth> b, double rate)
        : a_(std::move(a)), b_(std::move(b)), rate_(rate) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        if (a_) a_->generate(out, n);
        else for (size_t i = 0; i < n; i++) out[i] = cf32(0.f, 0.f);
        if (!b_) return;
        tmp_.resize(n);
        b_->generate(tmp_.data(), n);
        for (size_t i = 0; i < n; i++) out[i] += tmp_[i];
    }
private:
    std::unique_ptr<ModeSynth> a_, b_;
    double rate_;
    std::vector<cf32> tmp_;
};
}

double txCtcssToneHz(int idx) { return idx >= 1 && idx <= 50 ? (double)actl::kCtcssHz[idx - 1] : 0.0; }

std::unique_ptr<ModeSynth> makeTxVoiceSynth(const TxModeSpec& m, const TxRequest& r, double rate) {
    const std::string id = m.id ? m.id : "";
    if (id == "airband" || m.stdMode == 28) {
        const ModeTuning* mt = modeTuning(28);
        return std::make_unique<AnalogVoice>(rate, mt ? -mt->tuneOffsetHz : 0, 2, r);
    }
    if (id == "fm" || m.stdMode == 7) return nullptr;   // FmGenerator in tx_payload
    if (id == "atv" || m.stdMode == 10) {
        const int k = r.kind == 2 ? 3 : r.kind == 3 ? 4 : r.kind == 1 ? 2 : 1;
        return std::make_unique<AnalogVoice>(rate, 5.5e6, k, r);
    }
    // the radio is told the dial frequency plus the mode's tuning offset (tx.cpp, #351), so the signal sits that far below the radio's centre (#374)
    const double dialOff = -txModeTuneOffsetHz(m);
    if (r.analogVoice || r.kind == 1 || r.kind == 2 || r.kind == 3) {
        const int k = r.kind == 2 ? 3 : r.kind == 3 ? 4 : r.kind == 1 ? 2 : 1;
        return std::make_unique<AnalogVoice>(rate, dialOff, k, r);
    }
    if (id == "qo100") return std::make_unique<AnalogVoice>(rate, dialOff, 3, r);
    (void)id;
    return std::make_unique<AnalogVoice>(rate, dialOff, 1, r);
}

std::unique_ptr<ModeSynth> makeTxMixSynth(std::unique_ptr<ModeSynth> a, std::unique_ptr<ModeSynth> b, double rate) {
    return std::make_unique<MixSynth>(std::move(a), std::move(b), rate);
}

void txPullPcm8k(const TxRequest& r, int16_t* pcm, int n) {
    static thread_local PullMono p;
    p.src = r.pullAudio;
    const double adv = r.pullAudio ? 48000.0 / 8000.0 : 0;
    for (int i = 0; i < n; i++) {
        float a = 0;
        if (r.pullAudio) { a = p.peek(); p.advance(adv); }
        else if (r.audioSrc != 3) a = 0.5f * (float)std::sin(2 * kPi * (r.toneHz > 0 ? r.toneHz : 1000) * i / 8000.0);
        pcm[i] = (int16_t)std::max(-32767, std::min(32767, (int)lrint(a * 20000)));
    }
}

} // namespace dect2
