// Meteosat HRDCP test signal (see hrdcp_gen.h).
#include "dect2/goesdcs/hrdcp_gen.h"
#include "dect2/goesdcs/hrdcp_proto.h"
#include "dect2/goesdcs/goesdcs_proto.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/rrc.h"
#include "dect2/source/source.h"
#include <algorithm>
#include <cmath>
#include <random>
#include <string>

namespace dect2 {

namespace {
constexpr int kSpan = 6;          // symbols each side
constexpr int kOver = 64;

struct Burst {
    int Kc = 0, K0 = 0, N = 0;    // carrier symbols, first data symbol, data symbols
    std::vector<int8_t> pre;      // the 192 preamble and marker symbols (+-1)
    std::vector<int8_t> di, dq;   // data symbols (+-1)
    double start = 0;             // seconds
    double lenSym = 0;
};
} // namespace

struct HrdcpGenerator::Impl {
    HrdcpGenConfig c;
    std::vector<float> table;
    std::mt19937 rng;
    std::normal_distribution<float> gauss{0.f, 1.f};
    float sigma = 0;
    uint64_t n = 0;
    struct Tx { HrdcpGenTx t; Burst b; int rep = 0; double ph = 0, pn = 0; };
    std::vector<Tx> tx;

    explicit Impl(const HrdcpGenConfig& cfg) : c(cfg), rng(cfg.seed) {
        table.resize((size_t)(2 * kSpan * kOver + 2));
        for (size_t i = 0; i < table.size(); i++) table[i] = (float)dsp::rrc((double)i / kOver - kSpan, 0.5);
        if (c.ebn0Db < 99) {
            const double P = 0.1 * 0.1;
            sigma = (float)std::sqrt(P * c.rate / (hrdcp::kSymbolRate * std::pow(10.0, c.ebn0Db / 10)) / 2);
        }
        for (const auto& t : c.tx) { Tx x; x.t = t; build(x, 0); tx.push_back(std::move(x)); }
    }

    static Burst make(const HrdcpGenTx& t, int rep) {
        Burst b;
        b.Kc = (int)std::lround(t.carrierSec * hrdcp::kSymbolRate);
        for (int r = 0; r < 4; r++) for (int k = 31; k >= 0; k--) b.pre.push_back((hrdcp::kPreambleWord >> k & 1) ? -1 : 1);
        for (int k = 63; k >= 0; k--) b.pre.push_back((hrdcp::kAsm >> k & 1) ? -1 : 1);
        b.K0 = b.Kc + (int)b.pre.size();
        hrdcp::Header h;
        h.address = dcs::addressWord(t.address21);
        h.sequence = (uint16_t)(t.sequence + rep);
        h.type = t.type; h.health = t.health;
        const auto msg = hrdcp::buildMessage(h, t.data);
        std::vector<uint8_t> ib, qb;
        hrdcp::convEncode(hrdcp::codedBytes(msg, std::max(1, t.tailBytes)), ib, qb);
        for (size_t i = 0; i < ib.size(); i++) { b.di.push_back(ib[i] ? -1 : 1); b.dq.push_back(qb[i] ? -1 : 1); }
        b.N = (int)b.di.size();
        b.lenSym = b.K0 + b.N + 1.0;
        return b;
    }
    void build(Tx& x, int rep) {
        x.rep = rep;
        x.b = make(x.t, rep);
        x.b.start = x.t.startSec + rep * x.t.repeatSec;
    }

    float pulse(double d) const {   // d in symbols
        const double p = (d + kSpan) * kOver;
        if (p < 0 || p >= (double)table.size() - 1) return 0.f;
        const int j = (int)p;
        const float f = (float)(p - j);
        return table[(size_t)j] + f * (table[(size_t)j + 1] - table[(size_t)j]);
    }
    float iSym(const Burst& b, int k) const {
        if (k < 0) return 0.f;
        if (k < b.Kc) return 1.f;
        if (k < b.K0) return b.pre[(size_t)(k - b.Kc)];
        if (k < b.K0 + b.N) return b.di[(size_t)(k - b.K0)];
        return 0.f;
    }
    cf32 base(const Burst& b, double u) const {   // u: symbols since the burst start
        float I = 0, Q = 0;
        const int k0 = (int)std::floor(u);
        for (int k = k0 - kSpan; k <= k0 + kSpan; k++) {
            const float p = pulse(u - k);
            if (p == 0.f) continue;
            const float v = iSym(b, k);
            I += v * p;
            if (k < b.K0) Q += v * p;   // carrier, preamble and marker: the same on both rails
        }
        const double uq = u - 0.5 - b.K0;   // data on Q: half a symbol later
        const int m0 = (int)std::floor(uq);
        for (int m = std::max(0, m0 - kSpan); m <= m0 + kSpan && m < b.N; m++) Q += b.dq[(size_t)m] * pulse(uq - m);
        return cf32(I, Q);
    }

    void generate(cf32* out, size_t cnt) {
        const double kPi = math::kPi;
        for (size_t i = 0; i < cnt; i++, n++) {
            const double t = (double)n / c.rate;
            cf32 s(0, 0);
            for (auto& x : tx) {
                const double tb = t - x.b.start;
                const double fs = x.t.ppm * 1e-6;
                const double u = tb * hrdcp::kSymbolRate * (1 + fs);
                if (u > x.b.lenSym + kSpan) {
                    if (x.t.repeatSec > 0) build(x, x.rep + 1);
                    continue;
                }
                if (u < -kSpan) continue;
                const double f = (x.t.freqHz - c.centreHz) + x.t.extraHz;
                if (x.t.phaseNoiseDeg > 0) x.pn += x.t.phaseNoiseDeg * kPi / 180 * std::sqrt(1.0 / c.rate) * gauss(rng);
                const double ph = 2 * kPi * (f * t + 0.5 * x.t.driftHzPerSec * tb * tb) + x.pn;
                const cf32 v = base(x.b, u) * (float)(x.t.amp / std::sqrt(2.0));
                s += v * cf32((float)std::cos(ph), (float)std::sin(ph));
            }
            if (sigma > 0) s += cf32(sigma * gauss(rng), sigma * gauss(rng));
            out[i] = s;
        }
    }
};

HrdcpGenerator::HrdcpGenerator(const HrdcpGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
HrdcpGenerator::~HrdcpGenerator() = default;
void HrdcpGenerator::generate(cf32* out, size_t n) { p_->generate(out, n); }
double HrdcpGenerator::burstSeconds(size_t i) const { return i < p_->tx.size() ? p_->tx[i].b.lenSym / hrdcp::kSymbolRate : 0.0; }

namespace {
class HrdcpSynth : public ModeSynth {
public:
    HrdcpSynth(const SynthConfig& s, double rate) : rate_(rate) {
        HrdcpGenConfig c;
        c.rate = rate; c.centreHz = 402.3e6; c.ebn0Db = s.snrDb;
        const std::string a = "SYNOP 06590 32970 10125 20087 39982 40213 58004=\r\n";
        HrdcpGenTx t1;
        t1.freqHz = 402.2945e6 + s.cfoHz; t1.address21 = 0x2C412; t1.data.assign(a.begin(), a.end()); t1.startSec = 0.5; t1.repeatSec = 12;
        c.tx.push_back(t1);
        if (s.modeOpt[0] == 0) {
            HrdcpGenTx t2;
            t2.freqHz = 402.3245e6 + s.cfoHz; t2.address21 = 0x5800A; t2.sequence = 400; t2.startSec = 2.0; t2.repeatSec = 20;
            for (int i = 0; i < 900; i++) t2.data.push_back((uint8_t)(i * 37 + 11));
            c.tx.push_back(t2);
        }
        gen_ = std::make_unique<HrdcpGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<HrdcpGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeHrdcpSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<HrdcpSynth>(cfg, sampleRate);
}

} // namespace dect2
