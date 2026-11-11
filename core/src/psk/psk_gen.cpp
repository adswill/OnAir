// PSK31 / PSK63 test signal (see psk_gen.h).
#include "dect2/psk/psk_gen.h"
#include "dect2/psk/psk_rx.h"
#include "dect2/psk/psk_tel.h"
#include "dect2/psk/psk_varicode.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;

struct Sender {
    PskGenSignal s;
    double amp = 0, ph = 0;       // carrier phase
    std::vector<int> turns;       // the phase steps of one sending (eighth turns), then the carrier goes off
    size_t idx = 0;
    double u = 0;                 // position in the current symbol, 0 .. 1
    std::complex<double> prev{0, 0}, cur{0, 0};
    bool on = false, done = false;
    double offLeft = 0;           // carrier off: seconds left
    void build() {
        turns.clear();
        const double baud = pskBaud(s.mode);
        const int idle = std::max(8, (int)(s.idleSec * baud));
        if (psk8psk(s.mode)) {
            turns = psk::psk8Steps(s.text, idle, idle);
            idx = 0; u = 0;
            prev = 0; cur = std::polar(1.0, s.phase0);
            return;
        }
        std::vector<uint8_t> bits(idle, 0);
        const std::vector<uint8_t> tb = psk::varicodeBits(s.text);
        bits.insert(bits.end(), tb.begin(), tb.end());
        bits.insert(bits.end(), idle, 0);
        if (pskQpsk(s.mode)) {
            psk::ConvEncoder enc;
            for (int k = 0; k < 8; k++) bits.push_back(0);   // flush the code
            for (uint8_t b : bits) turns.push_back(2 * psk::qpskTurns(enc.encode(b)));
        } else {
            for (uint8_t b : bits) turns.push_back(b ? 0 : 4);
        }
        idx = 0; u = 0;
        prev = 0; cur = std::polar(1.0, s.phase0);
    }
};
} // namespace

struct PskGenerator::Impl {
    PskGenConfig cfg;
    genutil::NoiseSource noise;
    std::vector<Sender> snd;
    int64_t n = 0;
    explicit Impl(const PskGenConfig& c) : cfg(c), noise(c.seed) {
        const double sigma = c.noiseSigma > 0 ? c.noiseSigma : 0.02;
        for (const auto& g : c.signals) {
            Sender s;
            s.s = g;
            s.amp = std::sqrt(2 * sigma * sigma * pskBaud(g.mode) / c.rate * std::pow(10.0, g.snrDb / 10));
            s.offLeft = g.startSec;
            snd.push_back(s);
        }
    }
};

PskGenerator::PskGenerator(const PskGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
PskGenerator::~PskGenerator() = default;

void PskGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    const double dt = 1.0 / m.cfg.rate;
    for (size_t i = 0; i < n; i++) out[i] = cf32(0, 0);
    for (Sender& g : m.snd) {
        if (g.done) continue;
        const double T = (1 + g.s.clockPpm * 1e-6) / pskBaud(g.s.mode);
        for (size_t i = 0; i < n; i++) {
            const double t = (double)(m.n + (int64_t)i) * dt;
            const double f = m.cfg.dialOffsetHz + m.cfg.cfoHz + g.s.offsetHz + g.s.driftHzPerMin * t / 60;
            g.ph = std::remainder(g.ph + 2 * kPi * f * dt, 2 * kPi);
            if (!g.on) {
                if (g.done) break;
                g.offLeft -= dt;
                if (g.offLeft > 0) continue;
                g.on = true; g.build();
            }
            g.u += dt / T;
            if (g.u >= 1) {
                g.u -= 1;
                g.prev = g.cur;
                if (++g.idx > g.turns.size()) {   // the last symbol has faded out
                    g.on = false;
                    if (!g.s.repeat) { g.done = true; continue; }
                    g.offLeft = g.s.pauseSec;
                    continue;
                }
                g.cur = g.idx < g.turns.size() ? g.prev * std::polar(1.0, g.turns[g.idx] * kPi / 4) : std::complex<double>(0, 0);
                if (g.idx == 0) g.prev = 0;
            }
            const double w = 0.5 + 0.5 * std::cos(kPi * g.u);
            const std::complex<double> b = g.prev * w + g.cur * (1 - w);
            const std::complex<double> v = g.amp * b * std::polar(1.0, g.ph);
            out[i] += cf32((float)v.real(), (float)v.imag());
        }
    }
    m.n += (int64_t)n;
    if (m.cfg.noiseSigma > 0) m.noise.add(out, n, (float)m.cfg.noiseSigma);
}

std::vector<PskGenSignal> pskTestSignals() {
    std::vector<PskGenSignal> v(4);
    v[0].offsetHz = 700; v[0].text = "CQ CQ CQ DE LY2ABC LY2ABC KO24 PSE K\n"; v[0].snrDb = 0;
    v[1].offsetHz = 1050; v[1].text = "DL1XYZ DE G4ABC UR RST 599 NAME JOHN QTH LONDON\nBTU DL1XYZ DE G4ABC KN\n"; v[1].snrDb = -3; v[1].startSec = 1.1;
    v[1].driftHzPerMin = 3;
    v[2].offsetHz = 1500; v[2].text = "CQ TEST DE OK1XX OK1XX TEST\n"; v[2].snrDb = -6; v[2].startSec = 0.6; v[2].clockPpm = 80;
    v[3].offsetHz = 2200; v[3].text = "SP5AB DE EA3XYZ TNX FER QSO 73 SK\n"; v[3].snrDb = -2; v[3].startSec = 2;
    return v;
}

namespace {
class PskSynth : public ModeSynth {
public:
    PskSynth(const SynthConfig& s, double rate) : rate_(rate) {
        PskGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -pskTuning().tuneOffsetHz;
        c.cfoHz = s.cfoHz;
        c.noiseSigma = 0.05;
        c.signals = pskTestSignals();
        if (s.modeOpt[0] == 1) c.signals.resize(1);
        if (s.snrDb >= 150) {
            // a transmitter (cleanSynth asks for 200 dB): no noise, and the strongest station at amplitude 0.25 with the others at their relative levels.
            // Adding 200 dB to the stations made the demo 10^10 too strong: the output limiter flattened the PSK31 envelope and it splattered.
            c.noiseSigma = 0;
            double top = -1e9;
            for (const auto& g : c.signals) top = std::max(top, g.snrDb + 10 * std::log10(pskBaud(g.mode)));
            const double off = 20 * std::log10(0.25) - 10 * std::log10(2 * 0.02 * 0.02 / rate) - top;   // amp = sqrt(2 sigma^2 baud / rate 10^(snr/10)), sigma 0.02
            for (auto& g : c.signals) g.snrDb += off;
        } else
            for (auto& g : c.signals) g.snrDb += s.snrDb;
        gen_ = std::make_unique<PskGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<PskGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makePskSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<PskSynth>(cfg, sampleRate);
}

} // namespace dect2
