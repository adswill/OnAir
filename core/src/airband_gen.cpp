// Airband test signal (see airband_gen.h): AM channels with keyed carriers, voice-like audio or a tone, over noise.
#include "dect2/airband_gen.h"
#include "dect2/gen_util.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <random>
#include <vector>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kAudRate = 16000;
constexpr double kRamp = 0.002;

struct Bq {
    double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    Bq(bool high, double f, double q, double fs) {
        const double w = 2 * kPi * f / fs, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
        if (high) { b0 = (1 + c) / 2; b1 = -(1 + c); b2 = (1 + c) / 2; } else { b0 = (1 - c) / 2; b1 = 1 - c; b2 = (1 - c) / 2; }
        b0 /= a0; b1 /= a0; b2 /= a0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
    }
    double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

struct GenChan {
    AirbandGenChannel c;
    double amp = 0;
    double ph = 0, step = 0;
    std::mt19937 rng;
    std::normal_distribution<double> nd{0, 1};
    Bq hp1{true, 300, 0.7071, kAudRate}, lp1{false, 2800, 0.5412, kAudRate}, lp2{false, 2800, 1.3066, kAudRate};
    double prev = 0, next = 0;
    int64_t aIdx = 0;
    double toneAt(double t) const { return std::sin(2 * kPi * 1000 * t); }
    double voice(double t) {
        double v = lp2.run(lp1.run(hp1.run(nd(rng))));
        const double syl = 0.35 + 0.65 * std::fabs(std::sin(2 * kPi * 2.7 * t));
        return std::max(-1.0, std::min(1.0, 2.2 * v * syl));
    }
};
} // namespace

struct AirbandGenerator::Impl {
    AirbandGenConfig cfg;
    std::vector<GenChan> ch;
    int64_t n = 0;
    float sigma = 0;
    genutil::NoiseSource noise;
    explicit Impl(const AirbandGenConfig& c) : cfg(c), noise(c.seed) {}
};

double AirbandGenerator::noiseSigma(const AirbandGenConfig& c) {
    if (c.snrDb >= 99) return 0;
    // carrier power amp^2 over the noise in 6.8 kHz: total complex variance 2 sigma^2 spread over the rate
    const double n68 = c.amp * c.amp / std::pow(10.0, c.snrDb / 10);
    return std::sqrt(n68 * c.rate / 6800 / 2);
}

AirbandGenerator::AirbandGenerator(const AirbandGenConfig& c) : p_(std::make_unique<Impl>(c)) {
    p_->sigma = (float)noiseSigma(c);
    uint32_t s = c.seed * 7919u + 13;
    for (const auto& gc : c.chans) {
        GenChan g;
        g.c = gc;
        g.amp = c.amp * std::pow(10.0, gc.levelDb / 20);
        g.step = 2 * kPi * (c.dialOffsetHz + gc.offsetHz + gc.cfoHz + c.cfoHz) / c.rate;
        g.rng.seed(s++);
        p_->ch.push_back(std::move(g));
    }
}
AirbandGenerator::~AirbandGenerator() = default;

double AirbandGenerator::timeSec() const { return (double)p_->n / p_->cfg.rate; }

bool AirbandGenerator::keyed(int i, double t) const {
    if (i < 0 || i >= (int)p_->ch.size()) return false;
    const AirbandGenChannel& c = p_->ch[(size_t)i].c;
    if (c.continuous) return true;
    if (!c.bursts.empty()) {
        for (const auto& b : c.bursts) if (t >= b.first && t < b.first + b.second) return true;
        return false;
    }
    if (t < c.phase) return false;
    return std::fmod(t - c.phase, c.period) < c.on;
}

void AirbandGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    for (size_t i = 0; i < n; i++) out[i] = cf32(0.f, 0.f);
    const double rate = m.cfg.rate;
    for (size_t k = 0; k < m.ch.size(); k++) {
        GenChan& g = m.ch[k];
        for (size_t i = 0; i < n; i++) {
            const double t = (double)(m.n + (int64_t)i) / rate;
            // the audio at 16 kHz, linearly interpolated
            const double at = t * kAudRate;
            while ((double)g.aIdx <= at) {
                g.prev = g.next;
                const double ta = (double)(g.aIdx + 1) / kAudRate;
                g.next = g.c.audio == 1 ? g.toneAt(ta) : g.c.audio == 0 ? g.voice(ta) : 0.0;
                g.aIdx++;
            }
            const double a = g.prev + (g.next - g.prev) * (at - (double)(g.aIdx - 1));
            // keying with 2 ms ramps
            double env;
            if (g.c.continuous) env = 1;
            else {
                const bool on = keyed((int)k, t), onA = keyed((int)k, t - kRamp), onB = keyed((int)k, t + kRamp);
                env = on && onA ? 1.0 : 0.0;
                if (on != onA || on != onB) {   // near an edge: measure the distance
                    double d = 0;
                    for (int s = 0; s <= 20; s++) d += keyed((int)k, t - kRamp + s * kRamp / 10) ? 1 : 0;
                    env = d / 21.0;
                }
            }
            if (env <= 0) { g.ph = std::remainder(g.ph + g.step, 2 * kPi); continue; }
            double e = 1 + g.c.mod * a;
            if (e < 0) e = 0;   // overmodulation: the transmitter clips the envelope at zero
            const double v = g.amp * env * e;
            out[i] += cf32((float)(v * std::cos(g.ph)), (float)(v * std::sin(g.ph)));
            g.ph = std::remainder(g.ph + g.step, 2 * kPi);
        }
    }
    m.n += (int64_t)n;
    if (m.sigma > 0) m.noise.add(out, n, m.sigma);
}

std::vector<AirbandGenChannel> airbandTestLayout() {
    std::vector<AirbandGenChannel> v;
    AirbandGenChannel c;
    c = {}; c.offsetHz = 0; c.label = "Tower"; c.cfoHz = 120; c.period = 6; c.on = 2.5; c.phase = 0.5; v.push_back(c);
    c = {}; c.offsetHz = kAirband833; c.is833 = true; c.label = "Approach"; c.levelDb = -6; c.cfoHz = -250; c.period = 5; c.on = 1.8; c.phase = 2.0; v.push_back(c);
    c = {}; c.offsetHz = 250e3; c.label = "Ground"; c.levelDb = 3; c.cfoHz = 60; c.period = 7; c.on = 2.0; c.phase = 4.0; v.push_back(c);
    c = {}; c.offsetHz = -300e3; c.label = "ATIS"; c.levelDb = -3; c.continuous = true; c.mod = 0.6; v.push_back(c);
    c = {}; c.offsetHz = 191666.6666667; c.is833 = true; c.label = "Tone"; c.audio = 1; c.cfoHz = 300; c.period = 4; c.on = 1.0; c.phase = 1.0; v.push_back(c);
    return v;
}

std::vector<AirbandChannel> airbandChannelsFor(const std::vector<AirbandGenChannel>& layout, double dialHz) {
    std::vector<AirbandChannel> out;
    for (const auto& g : layout) {
        AirbandChannel c;
        c.freqHz = dialHz + g.offsetHz;
        c.is833 = g.is833;
        c.label = g.label;
        out.push_back(c);
    }
    return out;
}

namespace {
class AirbandSynth : public ModeSynth {
public:
    AirbandSynth(const SynthConfig& s, double rate) : rate_(rate) {
        AirbandGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -airbandTuning().tuneOffsetHz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        if (s.modeOpt[0] == 1) {
            AirbandGenChannel t; t.is833 = true; t.label = "Tone"; t.audio = 1; t.period = 5; t.on = 2; t.phase = 0.5;
            c.chans.push_back(t);
        } else c.chans = airbandTestLayout();
        gen_ = std::make_unique<AirbandGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<AirbandGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeAirbandSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<AirbandSynth>(cfg, sampleRate);
}

} // namespace dect2
