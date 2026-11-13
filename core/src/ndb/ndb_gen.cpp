// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/ndb/ndb_gen.h"
#include "dect2/cw/cw_morse.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/ndb/ndb_rx.h"
#include <cmath>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
}

struct NdbGenerator::Impl {
    NdbGenConfig c;
    genutil::NoiseSource noise;
    float sigma = 0;
    int64_t n = 0;
    double carPh = 0, tonePh = 0;
    std::vector<char> ms;       // one ident cycle, one flag per millisecond (1 = key down)

    explicit Impl(const NdbGenConfig& cfg) : c(cfg), noise(cfg.seed) {
        if (c.wpm < 4) c.wpm = 4;
        if (c.wpm > 20) c.wpm = 20;
        if (c.mod < 0) c.mod = 0;
        if (c.mod > 0.95) c.mod = 0.95;
        const double unit = 1.2 / c.wpm;   // seconds; WPM = 1200 / dot in ms, as cw::Timing
        for (char ch : c.ident) {
            std::string code = cw::encodeSymbol(std::string(1, ch));
            if (code.empty()) continue;
            for (size_t i = 0; i < code.size(); i++) {
                push(true, unit * (code[i] == '-' ? 3 : 1));
                push(false, unit * (i + 1 == code.size() ? 3 : 1));
            }
        }
        push(false, unit * 4);            // the letter gap was 3; a word gap is 7, then the ident starts again
        if (ms.empty()) ms.push_back(0);
        if (c.snrDb < 90) {
            const double n1k = (double)c.amp * c.amp / std::pow(10.0, c.snrDb / 10.0);
            sigma = (float)std::sqrt(n1k * c.rate / 1000.0 / 2.0);
        }
    }

    void push(bool on, double sec) {
        int k = (int)std::lround(sec * 1000.0);
        if (k < 1) k = 1;
        ms.insert(ms.end(), (size_t)k, on ? 1 : 0);
    }

    void generate(cf32* out, size_t cnt) {
        const double dCar = 2 * kPi * (c.dialOffsetHz + c.cfoHz) / c.rate;
        const double dTone = c.toneHz > 0 ? 2 * kPi * c.toneHz / c.rate : 0;
        const size_t L = ms.size();
        for (size_t i = 0; i < cnt; i++) {
            const double t = (double)n / c.rate;
            const bool on = ms[(size_t)(t * 1000.0) % L] != 0;
            double a;
            if (c.toneHz > 0) {
                tonePh = std::remainder(tonePh + dTone, 2 * kPi);
                a = 1.0 + c.mod * (on ? 1.0 : 0.0) * std::sin(tonePh);
            } else {
                a = on ? 1.0 : 0.04;       // a keyed carrier; a little left on so the frequency stays measurable
            }
            carPh = std::remainder(carPh + dCar, 2 * kPi);
            const float g = c.amp * (float)a;
            out[i] = cf32(g * (float)std::cos(carPh), g * (float)std::sin(carPh));
            n++;
        }
        if (sigma > 0) noise.add(out, cnt, sigma);
    }
};

NdbGenerator::NdbGenerator(const NdbGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
NdbGenerator::~NdbGenerator() = default;
void NdbGenerator::generate(cf32* out, size_t n) { p_->generate(out, n); }

namespace {
class NdbSynth : public ModeSynth {
public:
    NdbSynth(const SynthConfig& s, double rate) : rate_(rate) {
        NdbGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -ndbTuning().tuneOffsetHz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        c.toneHz = s.modeOpt[0] == 1 ? 1020 : s.modeOpt[0] == 2 ? 0 : 400;
        c.seed = 1;
        gen_ = std::make_unique<NdbGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<NdbGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeNdbSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<NdbSynth>(cfg, sampleRate);
}

} // namespace dect2
