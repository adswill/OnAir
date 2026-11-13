// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DECT test signal (see dect_gen.h).
#include "dect2/dect/dect_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/dect/dect_rx.h"
#include <cmath>
#include <complex>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kDev = 288000.0;
}

struct DectGenerator::Impl {
    DectGenConfig c;
    std::mt19937 rng;
    dect2::normal_distribution<float> nd{0.f, 1.f};
    size_t frameLen = 0, pos = 0;
    uint64_t frameNo = 0;
    std::vector<std::complex<float>> frame;
    double sigma = 0, gaussSigma = 0;
    double phase = 0;                 // mixer phase

    explicit Impl(const DectGenConfig& cfg) : c(cfg), rng(cfg.seed) {
        frameLen = (size_t)std::llround(dect::kFrameSec * c.rate);
        frame.assign(frameLen, {0, 0});
        // Gaussian filter, BT 0.5, over 3 bit periods; the pulse of one bit has height 'area 1 in bit periods' (sums to 1 over the bits) so a run of equal bits sits at
        // exactly +-1 (a deviation of +-288 kHz at every sample rate), and it is evaluated at the exact bit time, not at the nearest sample
        const double T = c.rate / dect::kBitRate;   // samples per bit
        gaussSigma = std::sqrt(std::log(2.0)) / (2 * kPi * 0.5);   // in bit periods
        sigma = std::sqrt(c.amp * c.amp / std::pow(10.0, c.snrDb / 10.0) * c.rate / dect::kBitRate);
        buildFrame();
    }

    static void bitsOf(std::vector<uint8_t>& b, uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1)); }

    void addBurst(int slot, const std::vector<uint8_t>& bits, double amp, double cfo) {
        const double T = c.rate / dect::kBitRate;
        const size_t start = (size_t)std::llround(slot * (dect::kFrameSec / dect::kSlots) * c.rate);
        const int half = (int)std::ceil(1.5 * T);
        const size_t len = (size_t)std::ceil((double)bits.size() * T) + 2 * (size_t)half;
        std::vector<float> f(len, 0.f);
        const double norm = 1.0 / (gaussSigma * std::sqrt(2 * kPi));
        for (size_t k = 0; k < bits.size(); k++) {
            const double centre = ((double)k + 0.5) * T + half;
            const int c0 = (int)std::llround(centre);
            const float sgn = bits[k] ? 1.f : -1.f;
            for (int i = -half; i <= half; i++) {
                const int idx = c0 + i;
                if (idx < 0 || idx >= (int)len) continue;
                const double t = ((double)idx - centre) / T;
                f[(size_t)idx] += sgn * (float)(norm * std::exp(-t * t / (2 * gaussSigma * gaussSigma)));
            }
        }
        double ph = 0;
        for (size_t i = 0; i < len; i++) {
            ph += 2 * kPi * (kDev * f[i] + cfo) / c.rate;
            const size_t at = start + i - (size_t)half;
            if (start + i < (size_t)half || at >= frameLen) continue;
            // a short ramp at both ends of the burst
            const double edge = (double)std::min<size_t>(std::min(i, len - 1 - i), 16) / 16.0;
            frame[at] += std::polar((float)(amp * edge), (float)ph);
        }
    }

    void buildFrame() {
        std::fill(frame.begin(), frame.end(), std::complex<float>(0, 0));
        if (c.noiseOnly) return;
        for (const DectGenBase& b : c.bases) {
            std::vector<uint8_t> bits;
            bitsOf(bits, dect::kSyncRfp, 32);
            const bool nt = frameNo % 2 == 0;
            uint64_t a = nt ? dect::makeAField(dect::TaNt, 1, 0, 0, b.rfpi, b.inverted) : dect::makeAField(dect::TaQt, 1, 0, 0, 0x0123456789ULL, b.inverted);
            if (b.badCrc) a ^= 0x5;
            bitsOf(bits, a, 64);
            for (int i = 0; i < 324; i++) bits.push_back((uint8_t)(rng() & 1));
            addBurst(b.slot, bits, c.amp * b.ampScale, c.cfoHz);
            if (c.handset) {
                std::vector<uint8_t> pb;
                bitsOf(pb, dect::kSyncPp, 32);
                bitsOf(pb, dect::makeAField(dect::TaCt0, 0, 0, 0, rng() & 0xFFFFFFFFFFULL), 64);
                for (int i = 0; i < 324; i++) pb.push_back((uint8_t)(rng() & 1));
                addBurst((b.slot + 12) % dect::kSlots, pb, c.amp * b.ampScale * 0.7, c.cfoHz);
            }
        }
    }

    void generate(cf32* out, size_t n) {
        const double w = 2 * kPi * c.dialOffsetHz / c.rate;
        for (size_t i = 0; i < n; i++) {
            std::complex<float> v = frame[pos];
            const std::complex<float> m = std::polar(1.f, (float)phase);
            phase += w; if (phase > kPi) phase -= 2 * kPi; else if (phase < -kPi) phase += 2 * kPi;
            v *= m;
            const float ns = (float)(sigma / std::sqrt(2.0));
            out[i] = cf32{v.real() + ns * nd(rng), v.imag() + ns * nd(rng)};
            if (++pos >= frameLen) { pos = 0; frameNo++; buildFrame(); }
        }
    }
};

DectGenerator::DectGenerator(const DectGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
DectGenerator::~DectGenerator() = default;
void DectGenerator::generate(cf32* out, size_t n) { p_->generate(out, n); }

std::vector<DectGenBase> dectTestBases(int option) {
    std::vector<DectGenBase> v;
    DectGenBase a; a.rfpi = 0x0100A5C3D1ULL; a.slot = 0; v.push_back(a);
    if (option != 1) { DectGenBase b; b.rfpi = 0x0100F31A22ULL; b.slot = 5; b.ampScale = 0.6; v.push_back(b); }
    return v;
}

namespace {
class DectSynth : public ModeSynth {
public:
    DectSynth(const SynthConfig& s, double rate) : rate_(rate) {
        DectGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -dectTuning().tuneOffsetHz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        const int o = s.modeOpt[0];
        c.noiseOnly = o == 2;
        c.handset = o == 0;
        c.bases = dectTestBases(o == 1 ? 1 : 0);
        gen_ = std::make_unique<DectGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<DectGenerator> gen_;
};
}  // namespace

std::unique_ptr<ModeSynth> makeDectSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<DectSynth>(cfg, sampleRate);
}

}  // namespace dect2
