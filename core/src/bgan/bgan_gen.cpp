// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// BGAN forward-bearer test signal (bgan_gen.h).
#include "dect2/bgan/bgan_gen.h"
#include "dect2/source/source.h"
#include <cmath>
#include <algorithm>

namespace dect2::bgan {

namespace {
const double kPi = 3.141592653589793;
const int kTabPerSym = 256;
const int kHalf = 8;

double rrc(double t, double alpha) {
    const double eps = 1e-6;
    if (std::fabs(t) < eps) return 1.0 - alpha + 4.0 * alpha / kPi;
    if (std::fabs(std::fabs(t) - 1.0 / (4.0 * alpha)) < eps) {
        const double a = kPi / (4.0 * alpha);
        return alpha / std::sqrt(2.0) * ((1.0 + 2.0 / kPi) * std::sin(a) + (1.0 - 2.0 / kPi) * std::cos(a));
    }
    const double n = std::sin(kPi * t * (1.0 - alpha)) + 4.0 * alpha * t * std::cos(kPi * t * (1.0 + alpha));
    return n / (kPi * t * (1.0 - 16.0 * alpha * alpha * t * t));
}
}

Generator::Generator(const GenConfig& c) : c_(c), b_(bearer(c.bearer)), lay_(frameLayout(bearer(c.bearer))) {
    rrc_.resize((2 * kHalf + 1) * kTabPerSym + 1);
    for (size_t i = 0; i < rrc_.size(); i++) rrc_[i] = float(rrc(double(i) / kTabPerSym - kHalf, b_.rolloff));
    fn_ = int(c.seed * 37) & 0xFFF;
    frame_ = 0;
    nextFrame();
}

void Generator::nextFrame() {
    if (stream_.size() > 64) stream_.erase(stream_.begin(), stream_.end() - 32);
    const int mos = b_.mos;
    size_t base = stream_.size();
    stream_.resize(base + mos, cf32(0, 0));
    const uint64_t word = uniqueWords()[c_.subtype];
    const cf32 p(b_.uwAxis, b_.uwAxis);
    for (int k = 0; k < 40; k++) {
        const int bit = (word >> (39 - k)) & 1;
        const float s = (bit ^ (b_.uwZeroIsPlus ? 1 : 0)) ? 1.f : -1.f;
        stream_[base + k] = p * s;
    }
    for (int pos : lay_.pilotPos) stream_[base + pos] = p;
    // blocks
    std::vector<std::vector<cf32>> blockSyms(b_.blocks);
    int fam = b_.family, sub = c_.subtype;
    const Table* t = table(fam, sub);
    if (!t) {             // no published table (F80T1Q-1B): random QPSK as data, the frame structure is real
        int pts; const cf32* cc = constellation(b_.mod, pts);
        uint32_t rng = c_.seed * 2654435761u + frame_;
        for (int d : lay_.dataPos) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; stream_[base + d] = cc[rng % pts]; }
    } else {
        for (int b = 0; b < b_.blocks; b++) {
            std::vector<uint8_t> bytes;
            if (c_.signalling) buildBlock(c_.sig, fn_, b, bytes, t->dataBytes());
            else bytes.assign(t->dataBytes(), 0);
            blockSyms[b].resize(t->symbols);
            encodeBlock(*t, b_.mod, bytes.data(), blockSyms[b].data());
        }
        for (int d = 0; d < int(lay_.dataPos.size()); d++) {
            const DataSym ds = dataSymbolOf(b_, d);
            stream_[base + lay_.dataPos[d]] = blockSyms[ds.block][ds.sym];
        }
    }
    frame_++;
    fn_ = (fn_ + 1) & 0xFFF;
}

void Generator::generate(cf32* out, size_t n) {
    const double fs = c_.fs;
    const double rs = b_.symRate * (1.0 + c_.clockPpm * 1e-6);
    const double dT = rs / fs;
    for (size_t i = 0; i < n; i++) {
        t_ += dT;
        while (t_ >= double(frame_) * b_.mos - kHalf) nextFrame();
        // stream_[0] corresponds to symbol index s0 = frames_built * mos - stream_.size()
        const double s0 = double(frame_) * b_.mos - double(stream_.size());
        const double r = t_ - s0;
        const int ci = int(std::floor(r));
        const float frac = float(r - ci);
        cf32 v(0, 0);
        for (int k = -kHalf + 1; k <= kHalf; k++) {
            const int si = ci + k;
            if (si < 0 || si >= int(stream_.size())) continue;
            const double d = frac - k;
            const double pos = (d + kHalf) * kTabPerSym;
            const int ip = int(pos);
            if (ip < 0 || ip + 1 >= int(rrc_.size())) continue;
            const float f = float(pos - ip);
            v += stream_[si] * (rrc_[ip] * (1 - f) + rrc_[ip + 1] * f);
        }
        const double t = sinceStart_ / fs;
        const double fHz = c_.offsetHz + c_.driftHzPerSec * t;
        ph_ += 2 * kPi * fHz / fs;
        if (ph_ > 2 * kPi) ph_ -= 2 * kPi; else if (ph_ < -2 * kPi) ph_ += 2 * kPi;
        cf32 g(float(std::cos(ph_) * c_.amplitude), float(std::sin(ph_) * c_.amplitude));
        if (c_.fadeHz > 0 && c_.fadeDepth > 0) {
            phFade_ += 2 * kPi * c_.fadeHz / fs;
            const float a = float(1.0 - c_.fadeDepth * 0.5 * (1 + std::sin(phFade_)));
            g *= cf32(a * float(std::cos(1.2 * std::sin(phFade_ * 0.7))), a * float(std::sin(1.2 * std::sin(phFade_ * 0.7))));
        }
        out[i] += v * g;
        sinceStart_ += 1;
    }
}

namespace {
class Synth : public ModeSynth {
public:
    Synth(const SynthConfig& cfg, double rate) : fs_(rate) {
        const double cn0 = cfg.modeVal[0] != 0 ? cfg.modeVal[0] : 70.0;
        const int single = cfg.modeOpt[0];
        const double sigma2Total = std::pow(10.0, -cn0 / 10.0) * rate;    // for amplitude 1 carriers
        sigma_ = std::sqrt(sigma2Total * 0.5);
        const double scale = 0.08;
        sigma_ *= scale;
        auto add = [&](int b, int sub, double off, uint32_t seed, int spot, int beamType) {
            GenConfig g; g.fs = rate; g.bearer = b; g.subtype = sub; g.offsetHz = off; g.seed = seed; g.amplitude = scale;
            g.sig.spotBeam = spot; g.sig.beamType = beamType; g.sig.rnc = 3; g.sig.primaryMhz = 1545.5 + off * 1e-6;
            g.sig.satId = 2; g.sig.lonDeg = 24.9; g.sig.fBearer = 0;
            gens_.push_back(std::make_unique<Generator>(g));
        };
        if (single > 0 && single <= bearerCount()) add(single - 1, 8, 12000.0, 3, 7, 2);
        else {
            add(0, 8, -280000.0, 1, 0, 0);            // the global beam bulletin board bearer
            add(2, 6, -150000.0, 2, 12, 1);           // a regional QPSK bearer
            add(3, 8, 25000.0, 3, 77, 2);
            add(8, 8, 180000.0, 4, 112, 2);
            add(10, 9, 380000.0, 5, 113, 2);
        }
        rng_ = 88172645463325252ull ^ uint64_t(cfg.modeOpt[1] + 1);
    }
    double sampleRate() const override { return fs_; }
    void generate(cf32* out, size_t n) override {
        std::fill(out, out + n, cf32(0, 0));
        for (auto& g : gens_) g->generate(out, n);
        for (size_t i = 0; i < n; i++) {
            out[i] += cf32(float(sigma_ * gauss()), float(sigma_ * gauss()));
        }
    }
private:
    double gauss() {
        // Box-Muller from xorshift
        const auto u = [&]() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 7; rng_ ^= rng_ << 17; return (rng_ >> 11) * (1.0 / 9007199254740992.0); };
        const double a = std::max(1e-12, u()), b = u();
        return std::sqrt(-2.0 * std::log(a)) * std::cos(2 * kPi * b);
    }
    double fs_, sigma_ = 0;
    uint64_t rng_ = 88172645463325252ull;
    std::vector<std::unique_ptr<Generator>> gens_;
};
}

std::unique_ptr<ModeSynth> makeSynth(const SynthConfig& cfg, double sampleRate) { return std::make_unique<Synth>(cfg, sampleRate); }

} // namespace dect2::bgan

namespace dect2 {
std::unique_ptr<ModeSynth> makeBganSynth(const SynthConfig& cfg, double sampleRate) { return bgan::makeSynth(cfg, sampleRate); }
}
