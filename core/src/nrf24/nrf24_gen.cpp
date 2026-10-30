// nRF24L01 Enhanced ShockBurst test signal, see nrf24_gen.h
#include "dect2/nrf24/nrf24_gen.h"
#include "dect2/nrf24/nrf24_rx.h"
#include "dect2/common/mathconst.h"
#include <cmath>
#include <random>

namespace dect2 {

namespace {
double devFor(int kbps) { return kbps >= 2000 ? 320e3 : 160e3; }

// the frequency of every sample of one transmission: NRZ bits through a Gaussian filter (BT 0.5), times the deviation; 2 bits of carrier at each end
std::vector<float> shape(const std::vector<uint8_t>& bits, int kbps, double rate, double clockPpm) {
    const double bps = kbps * 1e3 * (1 + clockPpm * 1e-6);
    const double sps = rate / bps;
    const int pad = 2;
    const size_t n = (size_t)std::ceil((double)(bits.size() + 2 * pad) * sps);
    std::vector<float> nrz(n, 0.f);
    for (size_t i = 0; i < n; i++) {
        const long k = (long)std::floor((double)i / sps) - pad;
        const int bit = (k < 0) ? bits[0] : (k >= (long)bits.size() ? bits.back() : bits[(size_t)k]);
        nrz[i] = bit ? 1.f : -1.f;
    }
    // Gaussian impulse response, BT 0.5: sigma = sqrt(ln 2) / (2 pi BT) symbols
    const double sigma = std::sqrt(std::log(2.0)) / (2 * math::kPi * 0.5) * sps;
    const int half = (int)std::ceil(3 * sigma);
    std::vector<float> g((size_t)(2 * half + 1));
    double sum = 0;
    for (int i = -half; i <= half; i++) { g[(size_t)(i + half)] = (float)std::exp(-0.5 * (i / sigma) * (i / sigma)); sum += g[(size_t)(i + half)]; }
    for (auto& v : g) v = (float)(v / sum);
    std::vector<float> f(n, 0.f);
    const double dev = devFor(kbps);
    for (size_t i = 0; i < n; i++) {
        double a = 0;
        for (int k = -half; k <= half; k++) {
            const long j = (long)i + k;
            const float v = j < 0 ? nrz[0] : (j >= (long)n ? nrz[n - 1] : nrz[(size_t)j]);
            a += g[(size_t)(k + half)] * v;
        }
        f[i] = (float)(a * dev);
    }
    return f;
}
} // namespace

struct Nrf24Generator::Impl {
    Nrf24GenConfig c;
    std::mt19937 rng;
    std::normal_distribution<double> nd{0.0, 1.0};
    double phase = 0, sigma = 0;
    size_t idx = 0;
    std::vector<float> cur;
    size_t pos = 0;
    bool inFrame = false;
    double gapLeft = 0;
    int curRate = 1000;
    uint32_t rateRot = 0;

    explicit Impl(const Nrf24GenConfig& cfg) : c(cfg), rng(cfg.seed) {
        gapLeft = 0.001;
        noiseFor(c.rateKbps);
    }
    void noiseFor(int kbps) {
        if (c.ebn0Db < 99) {
            const double n0 = c.amp * c.amp / (kbps * 1e3 * std::pow(10.0, c.ebn0Db / 10.0));
            sigma = std::sqrt(n0 * c.rate / 2);
        } else sigma = 0;
    }
    bool silent() const { return c.frames.empty() && !c.randomBits; }   // no frames and no random bits: noise only
    void nextFrame() {
        std::vector<uint8_t> bits;
        int kbps = c.rateKbps;
        if (c.randomBits || c.frames.empty()) {
            bits.assign(120, 0);
            for (auto& b : bits) b = (uint8_t)(rng() & 1);
        } else {
            const nrf24::Frame& f = c.frames[idx % c.frames.size()];
            if (!c.rates.empty()) kbps = c.rates[idx % c.rates.size()];
            idx++;
            bits = nrf24::encode(f);
        }
        curRate = kbps;
        noiseFor(kbps);
        cur = shape(bits, kbps, c.rate, c.clockPpm);
        pos = 0;
        inFrame = true;
    }
    void generate(cf32* out, size_t n) {
        for (size_t i = 0; i < n; i++) {
            double re = 0, im = 0;
            if (!inFrame) {
                gapLeft -= 1.0 / c.rate;
                if (gapLeft <= 0) { if (silent()) gapLeft = 1e9; else nextFrame(); }
            }
            if (inFrame) {
                const double f = c.dialOffsetHz + c.cfoHz + cur[pos];
                phase += 2 * math::kPi * f / c.rate;
                if (phase > math::kPi) phase -= 2 * math::kPi; else if (phase < -math::kPi) phase += 2 * math::kPi;
                re = c.amp * std::cos(phase); im = c.amp * std::sin(phase);
                if (++pos >= cur.size()) { inFrame = false; gapLeft = c.gapSec; }
            }
            if (sigma > 0) { re += sigma * nd(rng); im += sigma * nd(rng); }
            out[i] = cf32((float)re, (float)im);
        }
    }
};

Nrf24Generator::Nrf24Generator(const Nrf24GenConfig& c) : p_(std::make_unique<Impl>(c)) {}
Nrf24Generator::~Nrf24Generator() = default;
void Nrf24Generator::generate(cf32* out, size_t n) { p_->generate(out, n); }

std::vector<cf32> Nrf24Generator::burst(const nrf24::Frame& f, int rateKbps, double rate, double dialOffsetHz, double amp) {
    const std::vector<float> fr = shape(nrf24::encode(f), rateKbps, rate, 0);
    std::vector<cf32> out(fr.size());
    double ph = 0;
    for (size_t i = 0; i < fr.size(); i++) {
        ph += 2 * math::kPi * (dialOffsetHz + fr[i]) / rate;
        if (ph > math::kPi) ph -= 2 * math::kPi; else if (ph < -math::kPi) ph += 2 * math::kPi;
        out[i] = cf32((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
    }
    return out;
}

std::vector<nrf24::Frame> nrf24TestFrames(int crcMode) {
    std::vector<nrf24::Frame> v;
    auto mk = [](std::vector<uint8_t> a, int pid, bool noAck, const std::string& text, int crc) {
        nrf24::Frame f;
        f.address = std::move(a); f.pid = pid; f.noAck = noAck; f.payload.assign(text.begin(), text.end()); f.crcBits = crc;
        return f;
    };
    const bool c16 = crcMode == 0 || crcMode == 2, c8 = crcMode == 1 || crcMode == 2;
    if (c16) {
        v.push_back(mk({0xE7, 0xE7, 0xE7, 0xE7, 0xE7}, 0, false, "Hello nRF24", 16));
        v.push_back(mk({0xC2, 0xC2, 0xC2, 0xC2, 0xC2}, 1, false, "temp=21.5C hum=40%", 16));
        v.push_back(mk({0x12, 0x34, 0x56, 0x78}, 2, true, "mouse dx=3 dy=-2", 16));
        v.push_back(mk({0xA5, 0x5A, 0x3C}, 3, false, "ping", 16));
    }
    if (c8) {
        v.push_back(mk({0x01, 0x23, 0x45, 0x67, 0x89}, 0, false, "light on", 8));
        v.push_back(mk({0x01, 0x23, 0x45, 0x67, 0x89}, 1, false, "light off", 8));
        v.push_back(mk({0xB3, 0xB4, 0xB5, 0xB6}, 2, false, std::string("\x00\x01\x02\xFF\x10", 5), 8));
    }
    return v;
}

namespace {
class Nrf24Synth : public ModeSynth {
public:
    Nrf24Synth(const SynthConfig& s, double rate) : rate_(rate) {
        Nrf24GenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -nrf24Tuning().tuneOffsetHz;
        c.cfoHz = s.cfoHz;
        c.ebn0Db = s.snrDb;
        const int k = s.modeOpt[0];
        c.rateKbps = k == 1 ? 2000 : k == 2 ? 250 : 1000;
        c.frames = nrf24TestFrames(s.modeOpt[1]);
        if (k == 3) c.rates = {1000, 2000, 250};
        c.randomBits = s.modeOpt[2] == 1;
        gen_ = std::make_unique<Nrf24Generator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<Nrf24Generator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeNrf24Synth(const SynthConfig& cfg, double sampleRate) { return std::make_unique<Nrf24Synth>(cfg, sampleRate); }

} // namespace dect2
