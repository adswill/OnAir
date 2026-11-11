// CP16 test signal and transmitter (cp16_gen.h).
#include "dect2/cp16/cp16_gen.h"
#include "dect2/cp16/cp16_rx.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

namespace {
constexpr double kFg = 16000;   // rate the carriers are made at
constexpr double kPiD = 3.141592653589793;
} // namespace

const char* cp16DemoText(int which) {
    return which == 1 ? "CQ CQ DE ONAIR CP16 TEST 73" : "CQ CP16 DE ONAIR 业余无线电汉字通信试验 73";
}

struct Cp16Generator::Impl {
    Cp16GenOptions o;
    std::vector<uint32_t> cells;
    std::vector<uint16_t> rows;
    std::unique_ptr<cp16::Modulator> mod;
    genutil::NoiseSource noise;
    double amp = 0.055, sigma = 0;
    // 16 kHz stream: lead silence, the rows, the gap
    int64_t leadN = 0, gapN = 0, idle = 0;   // idle: silent samples still to make
    bool done = false;
    std::complex<float> h[4];
    double frac = 0, step = 1, ph = 0;
    explicit Impl(const Cp16GenOptions& opt) : o(opt), noise(opt.seed) {
        cells = cp16::textCells(o.text.empty() ? cp16DemoText(0) : o.text);
        rows = cp16::messageRows(cells);
        double on = 0;
        for (uint16_t r : rows) on += __builtin_popcount(r);
        on = rows.empty() ? 1 : std::max(0.5, on / (double)rows.size());
        if (o.snrDb < 150) {
            sigma = 0.02;
            const double pn = 2 * sigma * sigma * 2500.0 / o.rate;          // noise power in 2500 Hz
            amp = std::sqrt(std::pow(10.0, o.snrDb / 10) * pn / on);
        } else amp = o.ampPerCarrier;
        leadN = (int64_t)(o.leadSec * kFg);
        gapN = (int64_t)(o.gapSec * kFg);
        startPass();
        idle = leadN;
        step = kFg * (1 + o.sroPpm * 1e-6) / o.rate;
        for (auto& v : h) v = next16();
    }
    void startPass() {
        cp16::Params p = o.params;
        mod = std::make_unique<cp16::Modulator>(p, kFg * 1.0, amp, o.seed);
        for (uint16_t r : rows) mod->push(r);
    }
    std::complex<float> next16() {
        if (idle > 0) { idle--; return {0, 0}; }
        std::complex<float> v(0, 0);
        if (mod && mod->generate(&v, 1) == 1) return v;
        // the pass has ended
        if (!o.repeat) { done = true; return {0, 0}; }
        startPass();
        idle = gapN;
        return {0, 0};
    }
};

Cp16Generator::Cp16Generator(const Cp16GenOptions& o) : p_(std::make_unique<Impl>(o)) {}
Cp16Generator::~Cp16Generator() = default;
const std::vector<uint32_t>& Cp16Generator::cells() const { return p_->cells; }
double Cp16Generator::messageSec() const { return (double)p_->rows.size() * p_->o.params.rowSec(); }
bool Cp16Generator::finished() const { return p_->done; }

void Cp16Generator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    // the carriers are made by a clock that is (1 + ppm) fast: their frequencies scale with it as well
    const double w = 2 * kPiD * (m.o.dialOffsetHz + m.o.cfoHz) / m.o.rate;
    for (size_t i = 0; i < n; i++) {
        const float f = (float)m.frac;
        const std::complex<float> p0 = m.h[0], p1 = m.h[1], p2 = m.h[2], p3 = m.h[3];
        const std::complex<float> v = 0.5f * (2.f * p1 + (p2 - p0) * f + (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * f * f + (3.f * (p1 - p2) + p3 - p0) * f * f * f);
        m.ph += w;
        if (m.ph > kPiD) m.ph -= 2 * kPiD; else if (m.ph < -kPiD) m.ph += 2 * kPiD;
        out[i] = v * std::polar(1.f, (float)m.ph);
        m.frac += m.step;
        while (m.frac >= 1) { m.frac -= 1; m.h[0] = m.h[1]; m.h[1] = m.h[2]; m.h[2] = m.h[3]; m.h[3] = m.next16(); }
    }
    if (m.sigma > 0) m.noise.add(out, n, (float)m.sigma);
}

namespace {
class Cp16Synth : public ModeSynth {
public:
    Cp16Synth(const SynthConfig& s, double rate) : rate_(rate) {
        Cp16GenOptions o;
        o.rate = rate;
        o.dialOffsetHz = -cp16Tuning().tuneOffsetHz;
        o.cfoHz = s.cfoHz;
        o.sroPpm = s.sroPpm;
        o.snrDb = s.snrDb;
        const int si = std::clamp(s.modeOpt[0], 0, cp16::kNumCharSec - 1), sp = std::clamp(s.modeOpt[1], 0, cp16::kNumSpacing - 1);
        o.params.charSec = cp16::kCharSecChoices[si];
        o.params.spacingHz = cp16::kSpacingChoices[sp];
        o.params.lsb = s.modeOpt[2] == 1;
        if (s.modeVal[0] > 0) o.params.baseHz = s.modeVal[0];
        o.text = cp16DemoText(s.modeOpt[3] == 1 ? 1 : 0);
        gen_ = std::make_unique<Cp16Generator>(o);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<Cp16Generator> gen_;
};

class Cp16Tx : public TxGenerator {
public:
    Cp16Tx(const Cp16GenOptions& o) : rate_(o.rate), gen_(o) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_.generate(out, n); }
    bool finished() const override { return gen_.finished(); }
private:
    double rate_;
    Cp16Generator gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeCp16Synth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < 16000) return nullptr;
    return std::make_unique<Cp16Synth>(cfg, sampleRate);
}

std::unique_ptr<TxGenerator> makeCp16TxGenerator(const TxRequest& r, double rateHz, std::string& err) {
    if (rateHz < 16000) { err = "CP16 needs a transmit rate of at least 16 kHz"; return nullptr; }
    Cp16GenOptions o;
    o.rate = rateHz;
    o.params.charSec = r.o("cp16.char", 2.0);
    o.params.spacingHz = r.o("cp16.spacing", 15);
    o.params.baseHz = r.o("cp16.base", 1000);
    o.params.lsb = r.o("cp16.lsb", 0) >= 0.5;
    std::string why;
    if (!cp16::validParams(o.params, &why)) { err = "CP16: " + why; return nullptr; }
    o.text = r.text.empty() ? std::string(cp16DemoText(0)) : r.text;
    if (cp16::textCells(o.text).empty()) { err = "CP16: the text has no characters to send"; return nullptr; }
    o.repeat = false;
    o.snrDb = 200;
    return std::make_unique<Cp16Tx>(o);
}

} // namespace dect2
