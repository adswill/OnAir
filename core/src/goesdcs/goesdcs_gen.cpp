// GOES DCS test signal (see goesdcs_gen.h).
#include "dect2/goesdcs/goesdcs_gen.h"
#include "dect2/goesdcs/goesdcs_proto.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace dect2 {

struct GoesDcsGenerator::Impl {
    GoesDcsGenConfig c;
    genutil::NoiseSource noise;
    float sigma = 0;
    int64_t n = 0;
    struct Burst {
        std::vector<int8_t> chips;      // +1 / -1
        double chipRate = 0, carrierSec = 0, lengthSec = 0, offsetHz = 0;
        double ph = 0;
        GoesDcsGenTx tx;
    };
    std::vector<Burst> b;
    std::vector<double> carrierPh;
    std::vector<double> carrierOff;

    explicit Impl(const GoesDcsGenConfig& cfg) : c(cfg), noise(cfg.seed) {
        double ref = 0.3;
        if (!c.tx.empty()) ref = c.tx[0].amp;
        if (c.snrDb < 99) sigma = (float)std::sqrt(ref * ref * c.rate / (100.0 * std::pow(10.0, c.snrDb / 10)) / 2);
        for (const auto& t : c.tx) {
            Burst u;
            u.tx = t;
            std::vector<uint8_t> bits;
            if (t.preamble) {
                const int pre = t.bps == 100 ? dcs::kPreamble100 : dcs::kPreamble300;
                for (int i = 0; i < pre; i++) bits.push_back((uint8_t)((i & 1) ^ 1));   // 1 0 1 0 ... ending in 0
            }
            const auto fb = dcs::frameBits(t.address21, t.ext, t.flags, t.data);
            bits.insert(bits.end(), fb.begin(), fb.end());
            for (int i = 0; i < 16; i++) bits.push_back(0);   // flush
            for (uint8_t v : bits) {
                if (t.manchester) { u.chips.push_back(v ? 1 : -1); u.chips.push_back(v ? -1 : 1); }
                else u.chips.push_back(v ? 1 : -1);
            }
            u.chipRate = t.bps * (t.manchester ? 2 : 1) * (1.0 + t.ppm * 1e-6);
            u.carrierSec = t.carrierSec;
            u.lengthSec = t.carrierSec + (double)u.chips.size() / u.chipRate;
            u.offsetHz = (t.freqHz > 0 ? t.freqHz - c.centreHz : offsetHz(t.channel)) + t.extraHz;
            b.push_back(std::move(u));
        }
        for (int ch : c.carrierChannels) { carrierOff.push_back(offsetHz(ch)); carrierPh.push_back(0); }
    }
    double offsetHz(int ch) const { return dcs::channelHz(ch) - c.centreHz; }
};

GoesDcsGenerator::GoesDcsGenerator(const GoesDcsGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
GoesDcsGenerator::~GoesDcsGenerator() = default;
double GoesDcsGenerator::offsetHz(int channel) const { return p_->offsetHz(channel); }

void GoesDcsGenerator::generate(cf32* out, size_t cnt) {
    Impl& m = *p_;
    const double rate = m.c.rate;
    const double kMod = math::kPi / 3;   // +-60 degrees
    for (size_t k = 0; k < cnt; k++) {
        const double t = (double)(m.n + (int64_t)k) / rate;
        double re = 0, im = 0;
        for (auto& u : m.b) {
            u.ph += 2 * math::kPi * u.offsetHz / rate;
            if (u.ph > math::kPi) u.ph -= 2 * math::kPi; else if (u.ph < -math::kPi) u.ph += 2 * math::kPi;
            double rel = t - u.tx.startSec;
            if (rel < 0) continue;
            if (u.tx.repeatSec > 0) rel = std::fmod(rel, u.tx.repeatSec);
            if (rel >= u.lengthSec) continue;
            double phi = 0;
            if (rel >= u.carrierSec) {
                const size_t idx = (size_t)((rel - u.carrierSec) * u.chipRate);
                phi = idx < u.chips.size() ? kMod * u.chips[idx] : 0.0;
            }
            re += u.tx.amp * std::cos(u.ph + phi);
            im += u.tx.amp * std::sin(u.ph + phi);
        }
        for (size_t i = 0; i < m.carrierPh.size(); i++) {
            m.carrierPh[i] += 2 * math::kPi * m.carrierOff[i] / rate;
            if (m.carrierPh[i] > math::kPi) m.carrierPh[i] -= 2 * math::kPi; else if (m.carrierPh[i] < -math::kPi) m.carrierPh[i] += 2 * math::kPi;
            re += 0.3 * std::cos(m.carrierPh[i]); im += 0.3 * std::sin(m.carrierPh[i]);
        }
        out[k] = cf32((float)re, (float)im);
    }
    if (m.sigma > 0) m.noise.add(out, cnt, m.sigma);
    m.n += (int64_t)cnt;
}

namespace {
std::vector<uint8_t> ascii(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

class GoesDcsSynth : public ModeSynth {
public:
    GoesDcsSynth(const SynthConfig& s, double rate) : rate_(rate) {
        GoesDcsGenConfig c;
        c.rate = rate;
        c.snrDb = s.snrDb;
        const int mode = s.modeOpt[0];
        GoesDcsGenTx a, b2, d;
        a.channel = 60; a.address21 = 0x0A1B2; a.bps = 100; a.manchester = true; a.ext = false; a.startSec = 1.5; a.repeatSec = 18;
        a.data = ascii("WL 12.34\r\nTW 18.5\r\nBV 12.6\r\n");
        b2.channel = 100; b2.address21 = 0x1C0DE; b2.bps = 300; b2.manchester = mode != 2; b2.ext = true; b2.flags = 0x21; b2.startSec = 3.0; b2.repeatSec = 14;
        b2.data = ascii("RAIN 0.4 TEMP 21.7 WIND 3.2 BATT 13.1\r\n");
        d.channel = 140; d.address21 = 0x03FFF; d.bps = 1200; d.manchester = mode != 2; d.ext = true; d.flags = 0x02; d.startSec = 4.5; d.repeatSec = 16;
        d.data = dcs::pseudoBinaryB({0, 5, 17, 1, 0, 44, 12, 9, 33, 0, 0, 63, 40, 40, 7});
        if (mode == 1) c.tx = {b2};
        else c.tx = {a, b2, d};
        for (auto& t : c.tx) t.extraHz = s.cfoHz;
        if (s.modeOpt[1] == 1) c.carrierChannels = {180};
        gen_ = std::make_unique<GoesDcsGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<GoesDcsGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeGoesDcsSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<GoesDcsSynth>(cfg, sampleRate);
}

} // namespace dect2
