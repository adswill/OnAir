#include "dect2/railway/railway_gen.h"
#include "dect2/railway/railway_rx.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <cmath>

namespace dect2::railway {
std::vector<uint8_t> demoFrame(Protocol protocol) {
    std::vector<uint8_t> f = {0x0C, 0, 0, 0x3F, 0x1F, 0, 0, 0x1F, 0x8C, 0x30, 32};
    if (protocol == Protocol::Tdcs) {
        f.resize(43, 0); auto* p = f.data() + 11;
        auto put = [&](int at, unsigned n, int bytes) { for (int i = 0; i < bytes; i++) p[at + i] = uint8_t(n >> (8 * i)); };
        p[0] = p[1] = 3; p[2] = 1; p[3] = 'T';
        put(7, 112, 3); put(10, 242, 2); put(12, 521, 2); put(14, 1161011, 3); put(17, 124, 3);
        put(20, 800, 2); put(22, 410, 2); p[24] = 18; put(25, 130, 2); p[27] = 4; p[28] = 5; put(29, 123456, 3);
    } else {
        f[2] = 0x41; f[9] = 0xA5; f[10] = 11;
        const uint8_t payload[] = {0x23, 0x90, 0x03, 0x75, 0, 0x92, 0x12, 0x34, 0x56, 0x04, 0x86};
        f.insert(f.end(), std::begin(payload), std::end(payload));
    }
    f[1] = uint8_t(f.size()); const uint16_t c = crc16(f.data(), f.size()); f.push_back(uint8_t(c >> 8)); f.push_back(uint8_t(c)); return f;
}
std::vector<uint8_t> airBits(const std::vector<uint8_t>& frame, bool inverted) {
    std::vector<uint8_t> bits;
    auto append = [&](uint32_t v, int n) { for (int k = n - 1; k >= 0; k--) bits.push_back(uint8_t(((v >> k) & 1) ^ inverted)); };
    for (int i = 0; i < 160; i++) bits.push_back(uint8_t((i & 1) ^ inverted));
    append(0x0DD4259F, 32);
    for (size_t i = 0; i < frame.size(); i += 2) {
        const uint32_t value = unsigned(frame[i]) << 8 | (i + 1 < frame.size() ? frame[i + 1] : 0);
        uint32_t remainder = value << 10;
        for (int k = 15; k >= 0; k--) if (remainder & (1u << (k + 10))) remainder ^= 0x5B9u << k;
        append(value << 10 | remainder, 26);
    }
    return bits;
}
} // namespace dect2::railway
namespace dect2 {
namespace {
constexpr double pi = 3.14159265358979323846;
class RailwaySynth final : public ModeSynth {
public:
    RailwaySynth(const SynthConfig& cfg, double rate, railway::Protocol protocol)
        : rate_(rate), cfo_(cfg.cfoHz), bits_(railway::airBits(railway::demoFrame(protocol), cfg.modeOpt[1] == 1)), noise_(cfg.modeOpt[3] > 0 ? unsigned(cfg.modeOpt[3]) : 1) {
        deviation_ = cfg.modeOpt[4] > 0 ? cfg.modeOpt[4] : 2500;
        if (cfg.snrDb > -100) sigma_ = float(0.4 * std::sqrt(rate / (2 * 25000 * std::pow(10.0, cfg.snrDb / 10))));
        burst_ = (uint64_t)std::ceil(bits_.size() * rate / 1200); period_ = burst_ + (uint64_t)(rate * 0.3);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            if (pos_ < burst_) {
                const size_t bit = std::min(bits_.size() - 1, size_t(double(pos_) * 1200 / rate_));
                tone_ = std::remainder(tone_ + 2 * pi * (bits_[bit] ? 1200 : 1800) / rate_, 2 * pi);
                phase_ = std::remainder(phase_ + 2 * pi * (cfo_ - 50000 + deviation_ * std::sin(tone_)) / rate_, 2 * pi);
                out[i] = cf32(float(0.4 * std::cos(phase_)), float(0.4 * std::sin(phase_)));
            } else out[i] = {};
            if (++pos_ >= period_) pos_ = 0;
        }
        if (sigma_ > 0) noise_.add(out, n, sigma_);
    }
private:
    double rate_, cfo_, deviation_ = 2500, phase_ = 0, tone_ = 0;
    std::vector<uint8_t> bits_;
    genutil::NoiseSource noise_;
    float sigma_ = 0;
    uint64_t burst_ = 0, period_ = 0, pos_ = 0;
};
}
std::unique_ptr<ModeSynth> makeRailwaySynth(const SynthConfig& cfg, double rate, railway::Protocol protocol) {
    if (!std::isfinite(rate) || rate < klwTuning().minSampleRate) return nullptr;
    return std::make_unique<RailwaySynth>(cfg, rate, protocol);
}
} // namespace dect2
