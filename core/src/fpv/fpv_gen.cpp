// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Analog FPV video test signal, see fpv_gen.h. The composite video comes from the analog TV generator (atv_gen.h) at 12 Msps, is brought to the
// output rate, gets the sound subcarrier added and frequency modulates the carrier; noise last.
#include "dect2/fpv/fpv_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/fpv/fpv_chan.h"
#include "dect2/atv/atv_gen.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace dect2 {

struct FpvGenerator::Impl {
    FpvGenConfig c;
    std::unique_ptr<AtvGenerator> g;
    std::vector<float> comp;           // composite samples at kInternalRate, filled by the generator's tap
    ExactResampler rs;
    std::vector<cf32> pend;            // composite at the output rate, waiting to be modulated
    size_t pendPos = 0;
    double ph = 0, subPh = 0, hpY = 0, hpX = 0;
    std::complex<double> tone{1.0, 0.0};
    bool hpInit = false;
    std::mt19937_64 rng;
    dect2::normal_distribution<float> nd{0.f, 1.f};
    double noiseSigma = 0;
    uint64_t nOut = 0, gapPos = 0;   // gapPos: nOut modulo the 2 s of the sound tone's cycle (counted, not divided, per sample)

    explicit Impl(const FpvGenConfig& cfg) : c(cfg), rng(cfg.seed) {
        AtvGenConfig a;
        a.sys = c.ntsc ? kAtvM : kAtvG;
        a.colour = c.ntsc ? kAtvNtsc : kAtvPal;
        a.pattern = c.pattern;
        a.cnrDb = 100;
        a.rate = AtvGenerator::kInternalRate;
        a.sound = 3;   // no sound carrier of the broadcast signal
        g = std::make_unique<AtvGenerator>(a);
        g->setCompositeTap([this](const float* v, size_t n) { comp.insert(comp.end(), v, v + n); });
        rs.configure(AtvGenerator::kInternalRate, c.rate);
        const double nPow = c.amp * c.amp / std::pow(10.0, c.cnrDb / 10) * (c.rate / 17e6);
        noiseSigma = c.cnrDb >= 99 ? 0.0 : std::sqrt(nPow / 2);
    }

    void refill(size_t want) {
        while (pend.size() - pendPos < want) {
            if (pendPos > 0 && pendPos == pend.size()) { pend.clear(); pendPos = 0; }
            const size_t m = std::max<size_t>(4096, (size_t)((double)want * AtvGenerator::kInternalRate / c.rate) + 64);
            std::vector<cf32> sink(m);
            comp.clear();
            g->generate(sink.data(), m);
            if (c.acTauMs > 0) {
                const double a = std::exp(-1.0 / (c.acTauMs * 1e-3 * AtvGenerator::kInternalRate));
                if (!hpInit) { hpX = comp.empty() ? 0.0 : comp[0]; hpInit = true; }
                for (float& v : comp) { const double x = v; hpY = a * (hpY + x - hpX); hpX = x; v = (float)hpY; }
            }
            std::vector<cf32> in(comp.size());
            for (size_t i = 0; i < comp.size(); i++) in[i] = cf32(comp[i], 0.f);
            rs.process(in.data(), in.size(), pend);
        }
    }

    void generate(cf32* out, size_t n) {
        refill(n);
        const double sgn = c.syncLow ? 1.0 : -1.0;
        const double fsub = c.sound == 2 ? 6.0e6 : 6.5e6;
        const double fcar = c.offsetHz + c.cfoHz;
        // per-sample constants out of the loop, and the three oscillators from one sine table (genutil::fastPolar): the double-precision
        // sin / cos calls were two thirds of the time, and 20 Msps ran slower than real time
        const double k = 2 * M_PI / c.rate, devK = k * sgn * c.devHz / 0.715, carK = k * fcar - devK * 0.285;
        const uint64_t period = (uint64_t)std::llround(2.0 * c.rate), gapFrom = (uint64_t)std::llround(1.8 * c.rate);
        const float a = c.off ? 0.f : (float)c.amp, subLevel = (float)(c.soundLevel * 0.715);
        const std::complex<double> toneRot = std::polar(1.0, k * 1000.0);
        for (size_t i = 0; i < n; i++) {
            float v = pend[pendPos++].real();
            if (c.sound) {
                // a 1 kHz tone with a short break every 2 s
                if (++gapPos >= period) gapPos = 0;
                const bool gap = gapPos > gapFrom;
                tone *= toneRot;   // the 1 kHz tone: a rotating phasor (renormalised once a block)
                const double fi = fsub + (gap ? 0.0 : c.soundDevHz * tone.imag());
                subPh += k * fi; if (subPh > 2 * M_PI) subPh -= 2 * M_PI;
                v += subLevel * genutil::fastPolar(subPh).real();
            }
            ph += carK + devK * v;
            if (ph > M_PI) ph -= 2 * M_PI; else if (ph < -M_PI) ph += 2 * M_PI;
            const cf32 e = genutil::fastPolar(ph);
            float re = a * e.real(), im = a * e.imag();
            if (noiseSigma > 0) { re += (float)noiseSigma * nd(rng); im += (float)noiseSigma * nd(rng); }
            out[i] = cf32(re, im);
            nOut++;
        }
        tone /= std::abs(tone);
        if (pendPos > 65536) { pend.erase(pend.begin(), pend.begin() + (long)pendPos); pendPos = 0; }
    }
};

FpvGenerator::FpvGenerator(const FpvGenConfig& c) : p_(new Impl(c)) {}
FpvGenerator::~FpvGenerator() = default;
void FpvGenerator::generate(cf32* out, size_t n) { p_->generate(out, n); }

namespace {
class FpvSynth : public ModeSynth {
public:
    FpvSynth(const SynthConfig& s, double rate) : rate_(rate) {
        FpvGenConfig c;
        c.rate = rate;
        c.cnrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        c.offsetHz = -s.modeVal[0] * 1e6;   // the capture is centred modeVal[0] MHz from the channel: the channel is that far from the centre
        c.ntsc = s.modeOpt[0] == 1;
        c.pattern = s.modeOpt[1] == 1 ? 1 : 0;
        c.sound = s.modeOpt[2] == 2 ? 0 : s.modeOpt[2] == 1 ? 2 : 1;
        c.off = s.modeOpt[3] == 1;
        gen_ = std::make_unique<FpvGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<FpvGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeFpvSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < 16e6) return nullptr;   // a link is about 17 MHz wide
    return std::make_unique<FpvSynth>(cfg, sampleRate);
}

} // namespace dect2
