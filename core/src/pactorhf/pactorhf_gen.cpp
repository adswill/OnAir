// PACTOR-II / -III / -IV test signals (see pactorhf_gen.h).
#include "dect2/pactorhf/pactorhf_gen.h"
#include "dect2/pactorhf/pactorhf_dsp.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <random>

namespace dect2 {

namespace {
using dect2::math::kPi;
using cd = std::complex<double>;
}

std::vector<int> pactor3Carriers(int level) {
    // The SCS table marks which of the 18 carriers (0 .. 17) a level uses; the text of the description gives the counts: 2, 6, 14, 14, 16, 18.
    // Level 1 sits in the middle, the others are centred around it (the crossed carriers are not recoverable from the text of the table, so the
    // sets here are the centred ones; the receiver does not depend on which of them it is, only on the 120 Hz grid).
    int count = 2;
    switch (level) { case 1: count = 2; break; case 2: count = 6; break; case 3: case 4: count = 14; break; case 5: count = 16; break; default: count = 18; break; }
    std::vector<int> v;
    const int first = (18 - count) / 2;
    for (int i = 0; i < count; i++) v.push_back(first + i);
    return v;
}

struct PactorHfGenerator::Impl {
    PactorHfGenConfig cfg;
    std::mt19937 rng;
    std::normal_distribution<double> nd{0.0, 1.0};
    std::vector<double> carriers;
    std::vector<std::vector<uint8_t>> symbols;     // PACTOR-II / III: phase step indices
    std::vector<uint8_t> sync;                     // the fixed pattern
    std::vector<cd> psk4;                          // PACTOR-IV symbols
    size_t n = 0;
    double sigma = 0;
    // PACTOR-IV pulse
    std::vector<double> rrc;
    std::vector<cd> iv;
    // fading
    double fph[4] = {0, 1.3, 2.9, 4.1}, ffr[4] = {0, 0, 0, 0};
    size_t cache = 0;

    explicit Impl(const PactorHfGenConfig& c) : cfg(c), rng(c.seed) {
        if (c.variant == 2) carriers = {c.audioCentreHz - 100, c.audioCentreHz + 100};
        else if (c.variant == 3) for (int i : pactor3Carriers(c.level)) carriers.push_back(480.0 + 120.0 * i);
        else carriers = {1500.0};
        sync.resize((size_t)c.syncSyms);
        std::mt19937 sr(12345);
        for (auto& s : sync) s = (uint8_t)(sr() % (unsigned)std::max(2, c.order));
        const size_t order = (size_t)std::max(2, c.order);
        if (c.variant != 4) {
            const size_t total = (size_t)(c.rate > 0 ? 400 * 100 : 0);   // 400 s of symbols, then they wrap
            symbols.assign(carriers.size(), std::vector<uint8_t>(total));
            for (size_t k = 0; k < carriers.size(); k++) {
                for (size_t i = 0; i < total; i++) {
                    const size_t pos = i % (size_t)c.frameSyms;
                    symbols[k][i] = pos < (size_t)c.syncSyms ? (uint8_t)((sync[pos] + k) % order) : (uint8_t)(rng() % order);
                }
            }
        } else {
            const size_t total = 1800 * 60;
            iv.resize(total);
            for (size_t i = 0; i < total; i++) {
                const size_t pos = i % (size_t)c.frameSyms;
                const unsigned r = pos < (size_t)c.syncSyms ? sync[pos] : (unsigned)(rng() % order);
                if (c.order <= 8) iv[i] = std::polar(1.0, 2 * kPi * (double)r / (double)std::max(2, c.order) + (c.order == 2 ? 0 : kPi / (double)c.order));
                else {   // 16-QAM
                    const double a[4] = {-3, -1, 1, 3};
                    iv[i] = cd(a[(r >> 2) & 3] / std::sqrt(10.0), a[r & 3] / std::sqrt(10.0));
                    if (pos >= (size_t)c.syncSyms) { const unsigned q = (unsigned)(rng() % 16); iv[i] = cd(a[(q >> 2) & 3] / std::sqrt(10.0), a[q & 3] / std::sqrt(10.0)); }
                }
            }
            const int span = 8; const double alpha = 0.35, T = 1.0, ts = 1.0 / 4.0;   // 4 samples per symbol grid for the table, interpolated
            rrc.assign((size_t)(2 * span * 16 + 1), 0);
            for (size_t i = 0; i < rrc.size(); i++) {
                const double t = ((double)i - span * 16.0) / 16.0 * T;
                double v;
                if (std::abs(t) < 1e-9) v = 1 - alpha + 4 * alpha / kPi;
                else if (std::abs(std::abs(4 * alpha * t) - 1) < 1e-6) v = alpha / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * alpha)) + (1 - 2 / kPi) * std::cos(kPi / (4 * alpha)));
                else v = (std::sin(kPi * t * (1 - alpha)) + 4 * alpha * t * std::cos(kPi * t * (1 + alpha))) / (kPi * t * (1 - std::pow(4 * alpha * t, 2)));
                rrc[i] = v;
            }
            (void)ts;
        }
        // noise from the signal power
        if (c.snrDb < 99) {
            const double pSig = c.variant == 4 ? c.amp * c.amp : (double)carriers.size() * c.amp * c.amp * 0.75;
            const double snr = std::pow(10.0, c.snrDb / 10);
            sigma = std::sqrt(pSig / snr * c.rate / 2500.0 / 2.0);   // per component (I and Q)
        }
        for (int i = 0; i < 4; i++) ffr[i] = c.fadeHz * (0.6 + 0.35 * i) * 0.5;
    }

    // complex amplitude of carrier k at time t (seconds, signal time): raised-cosine pulses, phase accumulated from the symbol steps
    cd carrierAt(size_t k, double t) {
        const double x = t * 100.0;
        const long i = (long)std::floor(x);
        const double f = x - (double)i;   // 0 .. 1 inside symbol i (its centre at 0.5, its boundaries at 0 and 1)
        // PACTOR-II: raised-cosine pulses of width two symbols (cross-fade over the whole symbol). PACTOR-III: flat top with a cross-fade of +-h around the
        // boundaries (the carriers are only 120 Hz apart, the receiver nulls the neighbours with a rectangular window of 1 / 120 s)
        const double h = cfg.variant == 3 ? 0.18 : 0.5;
        auto fade = [&](double d) { return d <= -h ? 0.0 : d >= h ? 1.0 : std::pow(std::sin(kPi * (d + h) / (4 * h)), 2); };   // rises 0 -> 1 across the boundary
        const cd c0 = std::polar(1.0, cumPhase(k, i));
        if (f < h) {            // cross-fade with the previous symbol
            const double w = fade(f);
            return std::polar(1.0, cumPhase(k, i - 1)) * (1 - w) + c0 * w;
        }
        if (f > 1 - h) {        // cross-fade with the next symbol
            const double w = fade(f - 1);
            return c0 * (1 - w) + std::polar(1.0, cumPhase(k, i + 1)) * w;
        }
        return c0;
    }
    std::vector<std::vector<double>> cum;
    double cumPhase(size_t k, long j) {
        if (cum.empty()) cum.assign(carriers.size(), {});
        if (j < 0) j = 0;
        const size_t N = symbols[k].size();
        auto& c = cum[k];
        if (c.empty()) {
            c.resize(N + 2);
            double p = 0;
            const double step = 2 * kPi / (double)std::max(2, cfg.order);
            for (size_t i = 0; i < N + 2; i++) { p += (double)symbols[k][i % N] * step; c[i] = p; }
        }
        return c[(size_t)j % (N + 2)];
    }

    cd pulseIV(double tSym) {   // PACTOR-IV baseband at symbol time tSym (symbols)
        const int span = 8;
        const long i0 = (long)std::floor(tSym);
        cd acc(0, 0);
        for (long j = i0 - span + 1; j <= i0 + span; j++) {
            const double x = ((tSym - (double)j) + span) * 16.0;
            if (x < 0 || x >= (double)rrc.size() - 1) continue;
            const size_t xi = (size_t)x; const double fr = x - (double)xi;
            const double h = rrc[xi] * (1 - fr) + rrc[xi + 1] * fr;
            acc += iv[(size_t)((j % (long)iv.size()) + (long)iv.size()) % iv.size()] * h;
        }
        return acc;
    }

    cd baseSample(double t) {
        cd out(0, 0);
        if (cfg.variant == 4) {
            const double tSym = t * 1800.0;
            const cd b = pulseIV(tSym) * 0.45;
            out = b * std::polar(cfg.amp, 2 * kPi * (carriers[0] + cfg.cfoHz) * t);
        } else {
            for (size_t k = 0; k < carriers.size(); k++)
                out += carrierAt(k, t) * std::polar(cfg.amp, 2 * kPi * (carriers[k] + cfg.cfoHz) * t);
        }
        return out;
    }

    cd fade(double t) {
        if (cfg.fadeHz <= 0) return cd(1, 0);
        cd g(0, 0);
        for (int i = 0; i < 4; i++) g += std::polar(0.5, 2 * kPi * ffr[i] * t * (i % 2 ? 1 : -1) + fph[i]);
        return g;
    }

    cd next() {
        const double t = (double)n / cfg.rate * (1.0 + cfg.clockPpm * 1e-6);
        n++;
        cd s = baseSample(t);
        if (cfg.twoPath) s += 0.5 * baseSample(t - 0.0015);
        s *= fade(t);
        const double ph = 2 * kPi * cfg.dialOffsetHz * (double)(n - 1) / cfg.rate;
        s *= std::polar(1.0, ph);
        if (sigma > 0) s += cd(sigma * nd(rng), sigma * nd(rng));
        return s;
    }
};

PactorHfGenerator::PactorHfGenerator(const PactorHfGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
PactorHfGenerator::~PactorHfGenerator() = default;
const std::vector<std::vector<uint8_t>>& PactorHfGenerator::sent() const { return p_->symbols; }
const std::vector<double>& PactorHfGenerator::carrierHz() const { return p_->carriers; }
void PactorHfGenerator::generate(cf32* out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const cd s = p_->next();
        out[i] = p_->cfg.analytic ? cf32((float)s.real(), (float)s.imag()) : cf32((float)s.real(), 0.f);
    }
}

namespace {
class HfSynth : public ModeSynth {
public:
    HfSynth(int variant, const SynthConfig& s, double rate) : rate_(rate) {
        PactorHfGenConfig c;
        c.variant = variant; c.rate = rate; c.analytic = true;
        c.dialOffsetHz = -20000;   // the radio sits 20 kHz above the dial (tuneOffsetHz)
        c.cfoHz = s.cfoHz; c.clockPpm = s.sroPpm; c.snrDb = s.snrDb;
        if (variant == 3) c.level = s.modeOpt[0] >= 1 && s.modeOpt[0] <= 6 ? s.modeOpt[0] : 3;
        if (variant == 4) c.order = s.modeOpt[0] == 2 || s.modeOpt[0] == 8 || s.modeOpt[0] == 16 ? s.modeOpt[0] : 4;
        c.twoPath = s.modeOpt[1] == 1;
        c.amp = variant == 4 ? 0.3 : 0.25 / std::sqrt((double)(variant == 3 ? pactor3Carriers(c.level).size() : 2));
        gen_ = std::make_unique<PactorHfGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<PactorHfGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makePactorHfSynth(int variant, const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<HfSynth>(variant, cfg, sampleRate);
}

} // namespace dect2
