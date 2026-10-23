// PACTOR-I test signal (see pactor_gen.h).
#include "dect2/pactor/pactor_gen.h"
#include "dect2/pactor/pactor_rx.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace dect2 {

using dect2::math::kPi;

struct PactorGenerator::Impl {
    PactorGenConfig cfg;
    genutil::NoiseSource noise;
    std::vector<PactorGenEvent> ev;
    double amp = 0.4, sigma = 0, scriptSec = 0;
    int64_t n = 0;
    size_t idx = 0;
    double phase = 0;
    uint32_t cnt = 0;

    explicit Impl(const PactorGenConfig& c) : cfg(c), noise(c.seed), ev(c.events) {
        std::sort(ev.begin(), ev.end(), [](const PactorGenEvent& a, const PactorGenEvent& b) { return a.startSec < b.startSec; });
        double last = 0;
        for (const auto& e : ev) last = std::max(last, e.startSec + (double)e.bits.size() / e.baud);
        scriptSec = (c.lengthSec > 0 ? c.lengthSec : last + 0.3) + (c.repeat ? c.pauseSec : 0);
        if (c.snrDb >= 99 || c.noiseSigma <= 0) { amp = c.amp; sigma = 0; }
        else {
            sigma = c.noiseSigma;
            amp = std::sqrt(std::pow(10.0, c.snrDb / 10) * 2.0 * 2500.0 * sigma * sigma / c.rate);
        }
    }

    cf32 next() {
        double t = (double)n / cfg.rate;
        n++;
        if (cfg.repeat && scriptSec > 0) {
            const double wrap = std::fmod(t, scriptSec);
            t = wrap;
        }
        const double ppm = 1.0 + cfg.clockPpm * 1e-6;
        // find the event active at time t
        while (idx > 0 && ev[idx - 1].startSec * ppm > t) idx--;
        while (idx < ev.size() && ev[idx].startSec * ppm + (double)ev[idx].bits.size() / ev[idx].baud * ppm <= t) idx++;
        double f = 0;
        double a = 0;
        if (idx < ev.size() && ev[idx].startSec * ppm <= t) {
            const PactorGenEvent& e = ev[idx];
            const size_t bi = std::min(e.bits.size() - 1, (size_t)((t - e.startSec * ppm) * e.baud / ppm));
            const int bit = e.bits[bi] ^ (e.invert ? 1 : 0);
            f = cfg.audioCentreHz + (bit ? 100.0 : -100.0);
            a = amp;
        }
        phase += 2 * kPi * (f + cfg.dialOffsetHz + cfg.cfoHz) / cfg.rate;
        if (phase > kPi) phase -= 2 * kPi;
        if (a == 0) return cf32(0, 0);
        return cf32((float)(a * std::cos(phase)), (float)(a * std::sin(phase)));
    }
};

PactorGenerator::PactorGenerator(const PactorGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
PactorGenerator::~PactorGenerator() = default;
double PactorGenerator::scriptSeconds() const { return p_->scriptSec; }

void PactorGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    for (size_t i = 0; i < n; i++) out[i] = m.next();
    if (m.sigma > 0) m.noise.add(out, n, (float)m.sigma);
}

pactor::Packet pactorTextPacket(const std::string& text, bool huffman, uint8_t header, int count, bool fast, bool bk, bool qrt) {
    pactor::Packet p;
    p.header = header;
    const size_t cap = fast ? pactor::kDataFast : pactor::kDataSlow;
    pactor::Status s;
    s.count = count; s.huffman = huffman; s.breakIn = bk; s.qrt = qrt;
    p.status = pactor::statusByte(s);
    if (huffman) {
        std::vector<uint8_t> bits;
        pactor::huffEncode(text, bits);
        if (bits.size() > cap * 8) bits.resize(cap * 8);
        while (bits.size() + 2 <= cap * 8) { bits.push_back(1); bits.push_back(0); }   // padded with spaces
        while (bits.size() < cap * 8) bits.push_back(1);
        p.data = pactor::bitsToBytes(bits);
    } else {
        p.data.assign(text.begin(), text.end());
        p.data.resize(cap, ' ');
    }
    return p;
}

std::vector<PactorGenEvent> pactorTestScript(bool fec) {
    using namespace pactor;
    PactorScriptBuilder b;
    if (fec) {   // unproto: every packet three times in a row, no control signals, header inverted with every new packet
        uint8_t h = kHeaderA;
        const char* msg[] = {"CQ CQ DE LY2ABC LY2ABC PSE K", "eat tea set at sea sea"};
        for (int m = 0; m < 2; m++) {
            const bool huff = m == 1;
            const std::string& full = std::string(msg[m]);
            const size_t cap = huff ? 6 : 20;
            for (size_t o = 0; o < full.size(); o += cap) {
                const Packet p = pactorTextPacket(full.substr(o, cap), huff, h, (int)(o / cap) & 3, !huff);
                for (int r = 0; r < 3; r++) { b.ev.push_back({b.ts(), packetBits(p), isFast(p) ? 200 : 100, b.inv()}); b.next(); }
                h = h == kHeaderA ? kHeaderB : kHeaderA;
            }
        }
        return b.ev;
    }
    // ARQ: connect, speed change to 200, data both ways with a repeat and a changeover, QRT
    { Packet p; p.header = kHeaderA; p.data = callData("LY1XYZ"); p.status = 0; b.packet(p, false); }
    b.cs(4); b.next();
    b.packet(pactorTextPacket("LY2ABC de LY1XYZ", false, kHeaderB, 1, true), true); b.cs(1); b.next();
    b.packet(pactorTextPacket("tea set at sea", true, kHeaderA, 2, false), false); b.cs(2); b.next();
    b.packet(pactorTextPacket("tea set at sea", true, kHeaderA, 2, false), false); b.cs(1); b.next();   // repeated after the reject
    b.packet(pactorTextPacket("ASCII at 200 baud 73", false, kHeaderB, 3, true), true); b.cs(3); b.next();      // changeover requested
    b.packet(pactorTextPacket("a tea set", true, kHeaderA, 0, false, true), false); b.cs(1); b.next();
    b.packet(pactorTextPacket("", false, kHeaderB, 1, false, false, true), false); b.cs(1);
    return b.ev;
}

namespace {
class PactorSynth : public ModeSynth {
public:
    PactorSynth(const SynthConfig& s, double rate) : rate_(rate) {
        PactorGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -pactorTuning().tuneOffsetHz;
        c.cfoHz = s.cfoHz;
        c.snrDb = s.snrDb;
        c.events = pactorTestScript(s.modeOpt[0] == 1);
        gen_ = std::make_unique<PactorGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<PactorGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makePactorSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<PactorSynth>(cfg, sampleRate);
}

} // namespace dect2
