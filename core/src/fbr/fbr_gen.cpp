// Fleet Broadcast test signal (simulated, see fbr_gen.h).
#include "dect2/fbr/fbr_gen.h"
#include "dect2/fbr/fbr_rx.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;

uint64_t mix64(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// m-sequence x^5 + x^3 + 1, period 31
int pn31(uint64_t frame) {
    static int seq[31];
    static bool init = false;
    if (!init) {
        unsigned s = 0x1F;
        for (int i = 0; i < 31; i++) { seq[i] = (int)(s & 1); unsigned nb = ((s >> 4) ^ (s >> 2)) & 1; s = ((s << 1) | nb) & 0x1F; }
        init = true;
    }
    return seq[frame % 31];
}
} // namespace

int fbrTestBit(int scenario, int channel, uint64_t frame) {
    const auto rnd = [&](int c) { return (int)(mix64(((uint64_t)c << 48) ^ frame ^ 0x5151ull) & 1); };
    switch (scenario) {
    case 0:
        if (channel == 0) return pn31(frame);
        if (channel == 1 || channel == 4) return (int)(frame & 1);
        if (channel == 2) return 1;
        if (channel == 3) return 0;
        return rnd(channel);
    case 1:
        if (channel == 0) return pn31(frame);
        return channel & 1;
    case 4:
        return rnd(channel);
    default:
        return 0;
    }
}

struct FbrGenerator::Impl {
    FbrGenConfig c;
    uint64_t rng;
    double tpos = 0;               // position inside the current symbol, 0 .. 1
    uint64_t symIdx = 0;           // index of the current symbol
    uint64_t nextIdx = 0;          // index of the next symbol to build
    int state = 0;                 // differential state after the last built symbol
    int prevA = 1, curA = 1, nextA = 1;   // amplitudes (+1 / -1) of the previous, current and next symbol
    double carPh = 0, t = 0;
    double amp = 0.1, sigma = 0;

    explicit Impl(const FbrGenConfig& cc) : c(cc), rng(cc.seed * 0x9E3779B97F4A7C15ull + 7) {
        const double snr = std::pow(10.0, c.snrDb / 10.0);
        // total power 0.04 (rms 0.2); the noise power in 1.2 kHz is 2 sigma^2 x 1200 / rate
        amp = 0.2 / std::sqrt(1.0 + c.rate / (1200.0 * snr));
        sigma = amp * std::sqrt(c.rate / (2.0 * 1200.0 * snr));
        if (c.scenario == 2) { amp = 0; sigma = 0.15; }
        curA = advance();
        prevA = curA;
        nextA = advance();
    }

    // the amplitude of the next symbol: a bit 1 flips the phase (differential BPSK)
    int advance() {
        const int bit = fbrTestBit(c.scenario, (int)(nextIdx % 16), nextIdx / 16) ^ (c.invert ? 1 : 0);
        state ^= bit;
        nextIdx++;
        return state ? -1 : 1;
    }

    double rnd01() {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        return ((rng >> 11) + 0.5) * (1.0 / 9007199254740992.0);
    }
    std::complex<double> gauss() {
        const double r = std::sqrt(-2.0 * std::log(rnd01())), a = 2 * kPi * rnd01();
        return {r * std::cos(a), r * std::sin(a)};
    }
    double pulse() const {
        const double w = 0.25;
        if (tpos < w) {
            const double x = (tpos + w) / (2 * w);
            return prevA + (curA - prevA) * (1 - std::cos(kPi * x)) / 2;
        }
        if (tpos > 1 - w) {
            const double x = (tpos - (1 - w)) / (2 * w);
            return curA + (nextA - curA) * (1 - std::cos(kPi * x)) / 2;
        }
        return curA;
    }
};

FbrGenerator::FbrGenerator(const FbrGenConfig& c) : p_(new Impl(c)) {}
FbrGenerator::~FbrGenerator() = default;

void FbrGenerator::generate(cf32* out, size_t n) {
    Impl& s = *p_;
    const FbrGenConfig& c = s.c;
    const double symPerSample = 1200.0 * (1.0 + c.clockPpm * 1e-6) / c.rate;
    const double gmin = std::pow(10.0, -std::max(0.0, c.fadeDepthDb) / 20.0);
    for (size_t i = 0; i < n; i++) {
        const double f = c.offsetHz + c.dopplerHzPerS * s.t;
        s.carPh += 2 * kPi * f / c.rate;
        if (s.carPh > 1e6) s.carPh = std::fmod(s.carPh, 2 * kPi);
        double g = 1.0;
        if (c.fadeDepthDb > 0) g = (1 + gmin) / 2 + (1 - gmin) / 2 * std::cos(2 * kPi * 0.5 * s.t);
        std::complex<double> sig(0, 0);
        if (c.scenario == 3) sig = s.amp * g * std::polar(1.0, s.carPh);
        else if (c.scenario != 2) sig = s.amp * g * s.pulse() * std::polar(1.0, s.carPh);
        sig += s.sigma * s.gauss();
        out[i] = cf32((float)sig.real(), (float)sig.imag());
        s.t += 1.0 / c.rate;
        s.tpos += symPerSample;
        while (s.tpos >= 1.0) {
            s.tpos -= 1.0;
            s.symIdx++;
            s.prevA = s.curA; s.curA = s.nextA; s.nextA = s.advance();
        }
    }
}

namespace {
class FbrSynth : public ModeSynth {
public:
    FbrSynth(const SynthConfig& cfg, double fs) : fs_(fs) {
        FbrGenConfig c;
        c.rate = fs;
        c.snrDb = cfg.snrDb;
        c.scenario = std::max(0, std::min(4, cfg.modeOpt[0]));
        c.invert = cfg.modeOpt[1] == 1;
        // the radio sits tuneOffsetHz above the channel (fbrTuning): the carrier is that far below the centre of the capture, as the other generators do
        c.offsetHz = (cfg.modeVal[0] != 0 ? cfg.modeVal[0] : 1500) - fbrTuning().tuneOffsetHz;
        c.clockPpm = cfg.modeVal[1];
        c.fadeDepthDb = cfg.modeVal[2];
        c.dopplerHzPerS = cfg.modeVal[3];
        gen_ = std::make_unique<FbrGenerator>(c);
    }
    double sampleRate() const override { return fs_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    std::unique_ptr<FbrGenerator> gen_;
    double fs_;
};
} // namespace

std::unique_ptr<ModeSynth> makeFbrSynth(const SynthConfig& cfg, double sampleRate) { return std::make_unique<FbrSynth>(cfg, sampleRate); }

} // namespace dect2
