// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF ALE test signal (see ale_gen.h).
#include "dect2/ale/ale_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/ale/ale_proto.h"
#include "dect2/ale/ale_rx.h"
#include "dect2/dsp/gen_util.h"
#include <cmath>
#include <complex>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;

struct Tx {
    double start = 0;
    std::vector<uint8_t> bits;           // 147 a word
    double dur() const { return (double)(bits.size() / 3) / ale::kBaud; }
};

void addWords(std::vector<uint32_t>& v, const std::vector<uint32_t>& w, int times = 1) { for (int i = 0; i < times; i++) v.insert(v.end(), w.begin(), w.end()); }

// the transmissions of one call (the call, then its answers), as words
std::vector<std::vector<uint32_t>> callWords(const AleGenCall& c) {
    using namespace ale;
    std::vector<std::vector<uint32_t>> out;
    std::vector<uint32_t> w;
    switch (c.kind) {
    case AleGenCall::Sounding:
        addWords(w, addressWords(c.tws ? Tws : Tis, c.from), c.reps);
        out.push_back(w);
        break;
    case AleGenCall::Individual: case AleGenCall::Amd: case AleGenCall::AllCall: case AleGenCall::Net: {
        const std::string to = c.kind == AleGenCall::AllCall ? "@?@" : c.to;
        addWords(w, addressWords(To, to), c.scanWords);
        if (c.lqa) w.push_back(lqaWord(2, 24, 1));
        if (c.kind == AleGenCall::Amd) addWords(w, amdWords(c.text));
        addWords(w, addressWords(c.kind == AleGenCall::AllCall ? Tws : Tis, c.from));
        out.push_back(w);
        if (c.kind == AleGenCall::Net) {
            for (const auto& m : c.members) {   // each member answers in its slot
                std::vector<uint32_t> r;
                addWords(r, addressWords(To, c.from), 2);
                addWords(r, addressWords(Tis, m));
                out.push_back(r);
            }
            std::vector<uint32_t> k;
            addWords(k, addressWords(To, c.to), 2);
            addWords(k, addressWords(Tws, c.from));
            out.push_back(k);
        } else if (c.answer) {
            std::vector<uint32_t> r, k;
            addWords(r, addressWords(To, c.from), 2);
            if (c.lqa) r.push_back(lqaWord(1, 26, 0));
            addWords(r, addressWords(Tis, c.to));
            out.push_back(r);
            addWords(k, addressWords(To, c.to), 2);
            addWords(k, addressWords(Tws, c.from));
            out.push_back(k);
        }
        break;
    }
    case AleGenCall::Group: {
        for (int i = 0; i < std::max(1, c.scanWords / 2); i++) for (const auto& m : c.members) addWords(w, addressWords(Thru, m));
        addWords(w, addressWords(Tis, c.from));
        out.push_back(w);
        break;
    }
    }
    return out;
}
} // namespace

struct AleGenerator::Impl {
    AleGenConfig cfg;
    genutil::NoiseSource noise;
    std::mt19937 rng;
    dect2::uniform_real_distribution<double> uni{0.0, 1.0};
    std::vector<Tx> tx;
    double amp = 0, ph = 0, period = 0;
    int64_t n = 0;
    size_t cur = 0;
    int64_t lastSym = -1, curCycle = -1;
    int curTone = 0;
    explicit Impl(const AleGenConfig& c) : cfg(c), noise(c.seed), rng(c.seed * 7919u + 13u) {
        const double sigma = c.noiseSigma > 0 ? c.noiseSigma : 0.02;
        amp = std::sqrt(2 * sigma * sigma * 3000 / c.rate * std::pow(10.0, c.snrDb / 10));
        double t = c.leadSec;
        for (const auto& call : c.calls)
            for (const auto& words : callWords(call)) {
                Tx x;
                x.start = t;
                for (uint32_t w : words) { uint8_t b[147]; ale::wordBits(w, b); x.bits.insert(x.bits.end(), b, b + 147); }
                t += x.dur() + c.gapSec;
                tx.push_back(std::move(x));
            }
        period = t - c.gapSec + 3.0;
    }
};

AleGenerator::AleGenerator(const AleGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
AleGenerator::~AleGenerator() = default;

double AleGenerator::durationSec() const { return p_->tx.empty() ? 0 : p_->tx.back().start + p_->tx.back().dur(); }

void AleGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    const double dt = 1.0 / m.cfg.rate;
    for (size_t i = 0; i < n; i++) {
        out[i] = cf32(0, 0);
        if (m.tx.empty()) continue;
        double t = (double)(m.n + (int64_t)i) * dt;
        int64_t cycle = 0;
        if (m.cfg.repeat) { cycle = (int64_t)std::floor(t / m.period); t -= (double)cycle * m.period; }
        if (cycle != m.curCycle) { m.curCycle = cycle; m.cur = 0; m.lastSym = -1; }
        while (m.cur < m.tx.size() && t >= m.tx[m.cur].start + m.tx[m.cur].dur()) { m.cur++; m.lastSym = -1; }
        if (m.cur >= m.tx.size()) continue;
        const Tx& x = m.tx[m.cur];
        const double tt = t - x.start;
        if (tt < 0) continue;
        const int64_t sym = (int64_t)(tt * ale::kBaud);
        if (sym != m.lastSym) {   // a new tone: its three bits, some turned over
            m.lastSym = sym;
            int tb = 0;
            for (int k = 0; k < 3; k++) {
                int b = x.bits[(size_t)(3 * sym + k)];
                if (m.cfg.bitErrorRate > 0 && m.uni(m.rng) < m.cfg.bitErrorRate) b ^= 1;
                tb = (tb << 1) | b;
            }
            m.curTone = ale::tone(tb);
        }
        const double f = m.cfg.dialOffsetHz + m.cfg.cfoHz + ale::kTone0 + ale::kToneStep * m.curTone;
        m.ph = std::remainder(m.ph + 2 * kPi * f * dt, 2 * kPi);
        const double edge = std::min(1.0, std::min(tt, x.dur() - tt) / 0.002);   // 2 ms ramps
        const double a = m.amp * (0.5 - 0.5 * std::cos(kPi * edge));
        out[i] = cf32((float)(a * std::cos(m.ph)), (float)(a * std::sin(m.ph)));
    }
    m.n += (int64_t)n;
    if (m.cfg.noiseSigma > 0) m.noise.add(out, n, (float)m.cfg.noiseSigma);
}

std::vector<AleGenCall> aleTestCalls() {
    std::vector<AleGenCall> v;
    AleGenCall c;
    c.kind = AleGenCall::Sounding; c.from = "LY2ABC"; c.tws = true; c.reps = 4;
    v.push_back(c);
    c = AleGenCall(); c.kind = AleGenCall::Individual; c.from = "LY2ABC"; c.to = "OH2XYZ"; c.answer = true; c.lqa = true;
    v.push_back(c);
    c = AleGenCall(); c.kind = AleGenCall::Net; c.from = "K1ABC"; c.to = "HFN"; c.members = {"W2XYZ", "VE3ABC"};
    v.push_back(c);
    c = AleGenCall(); c.kind = AleGenCall::Amd; c.from = "G4XYZ"; c.to = "LY2ABC"; c.text = "QSY 14109 USB PSE";
    v.push_back(c);
    c = AleGenCall(); c.kind = AleGenCall::Group; c.from = "EA1ABC"; c.members = {"F5XYZ", "I2ABC"}; c.scanWords = 4;
    v.push_back(c);
    c = AleGenCall(); c.kind = AleGenCall::Sounding; c.from = "DL5ABCDEF"; c.tws = false; c.reps = 3;   // a long address: TIS, DATA, REP
    v.push_back(c);
    return v;
}

namespace {
class AleSynth : public ModeSynth {
public:
    AleSynth(const SynthConfig& s, double rate) : rate_(rate) {
        AleGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -aleTuning().tuneOffsetHz;
        c.cfoHz = s.cfoHz;
        const bool clean = s.snrDb >= 99;   // the transmitter's clean signal (TxSession): no noise, and the tones at the 20 dB level, not 190 dB above it
        c.noiseSigma = clean ? 0 : 0.05;
        c.snrDb = clean ? 20 : s.snrDb - 10;   // the default 30 gives 20 dB in 3 kHz
        c.repeat = true;
        c.calls = aleTestCalls();
        if (s.modeOpt[0] == 1) c.calls.resize(1);
        gen_ = std::make_unique<AleGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<AleGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeAleSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<AleSynth>(cfg, sampleRate);
}

} // namespace dect2
