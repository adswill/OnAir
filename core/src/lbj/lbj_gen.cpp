// LBJ test signal (see lbj_gen.h).
#include "dect2/lbj/lbj_gen.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/lbj/lbj_proto.h"
#include "dect2/lbj/lbj_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace dect2::lbj {

namespace {

uint32_t digitsToData(const std::string& s5) {
    static const char kNum[] = "0123456789*U -)(";
    uint32_t d = 0;
    for (int i = 0; i < 5; i++) {
        const char* p = std::strchr(kNum, s5[i]);
        const uint32_t v = p && s5[i] ? (uint32_t)(p - kNum) : 0;
        const uint32_t rev = ((v & 1) << 3) | ((v & 2) << 1) | ((v & 4) >> 1) | ((v & 8) >> 3);
        d = (d << 4) | rev;
    }
    return d;
}

} // namespace

std::vector<uint32_t> burstWords(const std::vector<GenPage>& pages) {
    std::vector<uint32_t> w;
    for (const GenPage& p : pages) {
        const int frame = (int)(p.ric & 7);
        while ((w.size() % 16) / 2 != (size_t)frame) w.push_back(kIdleWord);
        w.push_back(addressWord(p.ric, p.function));
        std::string d = p.digits;
        while (d.size() % 5) d.push_back(' ');
        for (size_t i = 0; i < d.size(); i += 5) w.push_back(messageWord(digitsToData(d.substr(i, 5))));
    }
    while (w.size() % 16 || w.empty()) w.push_back(kIdleWord);
    return w;
}

std::vector<uint8_t> burstBits(const std::vector<GenPage>& pages, int preambleBits) {
    std::vector<uint8_t> b;
    for (int i = 0; i < preambleBits; i++) b.push_back((uint8_t)(~i & 1));
    const std::vector<uint32_t> w = burstWords(pages);
    for (size_t i = 0; i < w.size(); i++) {
        if (i % 16 == 0) for (int k = 31; k >= 0; k--) b.push_back((kSyncWord >> k) & 1);
        for (int k = 31; k >= 0; k--) b.push_back((w[i] >> k) & 1);
    }
    return b;
}

GenPage basicPage(const std::string& train, int speedKmh, int kmTenths, int function) {
    GenPage p;
    p.ric = kAddrBasic;
    p.function = function;
    p.digits = formatBasic(train, speedKmh, kmTenths);
    return p;
}

std::vector<TestMessage> testMessages() {
    return {{"19533", "67", "38.2", "down"}, {"10482", "112", "1204.6", "up"}, {"23017", "0", "5.0", "down"}, {"41206", "86", "311.9", "up"}};
}

} // namespace dect2::lbj

namespace dect2 {

namespace {

using dect2::math::kPi;
constexpr double kGapSec = 0.6;
constexpr double kEdge = 0.3;

class LbjSynth : public ModeSynth {
public:
    LbjSynth(const SynthConfig& cfg, double rate)
        : rate_(rate), cfo_(cfg.cfoHz), inv_(cfg.modeOpt[1] == 1), dev_(cfg.modeOpt[4] > 0 ? cfg.modeOpt[4] : 4200.0), noise_(cfg.modeOpt[3] > 0 ? (uint32_t)cfg.modeOpt[3] : 1u) {
        for (const auto& m : lbj::testMessages()) {
            std::vector<lbj::GenPage> pages{lbj::basicPage(m.train, std::atoi(m.speed.c_str()), (int)std::lround(std::atof(m.km.c_str()) * 10), m.direction == "up" ? 3 : 1)};
            bits_.push_back(lbj::burstBits(pages));
        }
        amp_ = 0.45f;
        sigma_ = 0.f;
        if (cfg.snrDb > -100) {
            const double sigRel = std::sqrt(rate / (2.0 * 25000.0 * std::pow(10.0, cfg.snrDb / 10.0)));
            amp_ = (float)std::min(0.45, 0.85 / (4.0 * sigRel));
            sigma_ = (float)(sigRel * amp_);
        }
        enter();
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        const double w0 = 2 * kPi * (cfo_ - lbjTuning().tuneOffsetHz) / rate_;
        for (size_t i = 0; i < n; i++) {
            cf32 v(0.f, 0.f);
            const std::vector<uint8_t>& b = bits_[idx_];
            if (pos_ < len_) {
                const double ts = (double)pos_ / rate_;
                const double u = ts * lbj::kBaud;
                int64_t k = (int64_t)u;
                const int64_t nb = (int64_t)b.size();
                if (k >= nb) k = nb - 1;
                const double fr = u - (double)k;
                auto lev = [&](int64_t j) { return b[(size_t)j] ? -1.0 : 1.0; };   // a 1 is the lower frequency
                double l = lev(k);
                if (fr < kEdge && k > 0) l = lev(k - 1) + (l - lev(k - 1)) * (0.5 + 0.5 * std::sin(kPi / 2 * fr / kEdge));
                else if (fr > 1 - kEdge && k + 1 < nb) l = l + (lev(k + 1) - l) * (0.5 - 0.5 * std::sin(kPi / 2 * (1 - fr) / kEdge));
                const double f = (inv_ ? -1.0 : 1.0) * l * dev_;
                phase_ += w0 + 2 * kPi * f / rate_;
                if (phase_ > kPi) phase_ -= 2 * kPi; else if (phase_ < -kPi) phase_ += 2 * kPi;
                const double ramp = std::min({1.0, ts / 0.001, ((double)len_ / rate_ - ts) / 0.001});
                const float a = amp_ * (float)std::max(0.0, ramp);
                v = cf32(a * (float)std::cos(phase_), a * (float)std::sin(phase_));
            }
            out[i] = v;
            if (++pos_ >= len_ + (int64_t)std::llround(kGapSec * rate_)) { idx_ = (idx_ + 1) % bits_.size(); enter(); }
        }
        if (sigma_ > 0) noise_.add(out, n, sigma_);
    }

private:
    void enter() { pos_ = 0; len_ = (int64_t)std::llround((double)bits_[idx_].size() / lbj::kBaud * rate_); }
    double rate_, cfo_;
    bool inv_;
    double dev_;
    genutil::NoiseSource noise_;
    std::vector<std::vector<uint8_t>> bits_;
    int64_t pos_ = 0, len_ = 0;
    size_t idx_ = 0;
    double phase_ = 0;
    float amp_ = 0.45f, sigma_ = 0.f;
};

} // namespace

std::unique_ptr<ModeSynth> makeLbjSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<LbjSynth>(cfg, sampleRate);
}

} // namespace dect2
