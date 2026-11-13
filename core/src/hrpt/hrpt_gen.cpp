// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HRPT / AHRPT test signal (see hrpt_gen.h).
#include "dect2/hrpt/hrpt_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/dsp/rrc.h"
#include "dect2/hrpt/hrpt_proto.h"
#include "dect2/hrpt/hrpt_rx.h"
#include "dect2/meteor/meteor_proto.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr int kSpan = 8, kTabRes = 256;

using dect2::dsp::rrc;
} // namespace

uint16_t hrptTestPixel(int c, int x, int y) {
    const double fx = x / 2048.0, fy = (y % 64) / 64.0;
    double v;
    switch (c % 3) {
    case 0: v = 100 + 700 * fx + 100 * std::sin(fy * 6.283); break;
    case 1: v = 200 + 600 * fy + 100 * std::cos(fx * 25.13); break;
    default: { const double dx = x - 1024.0, dy = (y % 64) * 8 - 256.0; v = 512 + 350 * std::cos(std::sqrt(dx * dx + dy * dy) / 60.0); } break;
    }
    if (c >= 3) v = 1023 - v;
    if (x % 256 < 3 || y % 16 < 1) v = 940;
    return (uint16_t)std::clamp((int)std::lround(v), 0, 1023);
}

struct HrptGenerator::Impl {
    HrptGenConfig c;
    double rs = 2.33e6;
    bool bpsk = false;
    meteor::PacketMux mux;
    int line = 0, seq = 0;
    std::deque<uint8_t> bits;        // channel bits (QPSK: two a symbol; BPSK: one a chip)
    hrpt::CcsdsEncoder enc;
    std::vector<float> I, Q;
    int64_t symN = 0, outN = 0;
    std::vector<float> tab;
    double scale = 1, sigma = 0;
    std::mt19937 rng;
    dect2::normal_distribution<float> nd{0.f, 1.f};
    std::complex<double> rot{1, 0}, rstep{1, 0};
    uint32_t noaaMs = 36000000;

    explicit Impl(const HrptGenConfig& cfg) : c(cfg), rng(cfg.seed) {
        rs = hrptSymbolRate(c.variant);
        bpsk = c.variant == 3;
        const double alpha = bpsk ? 1.0 : 0.6;
        tab.resize((size_t)(2 * kSpan * kTabRes + 1));
        double e = 0;
        for (size_t i = 0; i < tab.size(); i++) {
            const double t = (double)i / kTabRes - kSpan;
            const double w = 0.5 + 0.5 * std::cos(kPi * t / kSpan);
            tab[i] = (float)(rrc(t, alpha) * w);
            e += (double)tab[i] * tab[i] / kTabRes;
        }
        scale = c.amp / std::sqrt(bpsk ? e : 2 * e);
        if (c.snrDb < 99) { const double n0 = c.amp * c.amp / (std::pow(10.0, c.snrDb / 10) * rs); sigma = std::sqrt(n0 * c.rate / 2); }
        rot = std::polar(1.0, c.phaseDeg * kPi / 180);
        rstep = std::polar(1.0, 2 * kPi * c.cfoHz / c.rate);
        I.assign(64, 0); Q.assign(64, 0);
    }

    hrpt::Line makeLine(int n) const {
        hrpt::Line l;
        l.day = 20000 & 0x1FF; l.ms = (uint32_t)(36000000 + std::llround(n * hrpt::kLineMs));
        if (!bpsk) l.day = 20000;
        for (int ch = 0; ch < kHrptChannels; ch++)
            for (int x = 0; x < kHrptWidth; x++) l.px[(size_t)ch * kHrptWidth + (size_t)x] = hrptTestPixel(ch, x, n);
        return l;
    }

    void addLine() {
        if (c.variant == 1) {
            mux.add(c.apid, seq, hrpt::encodeAvhrrPacket(makeLine(line)));
            seq = (seq + 1) & 0x3FFF;
            mux.add(30, seq, std::vector<uint8_t>(100, 0x5A));   // a housekeeping-sized packet of another APID
        } else {
            std::vector<uint8_t> u(4000);
            for (auto& b : u) b = (uint8_t)rng();
            mux.add(0x100 + (line & 7), seq, u);
        }
        seq = (seq + 1) & 0x3FFF;
        line = (line + 1) % std::max(1, c.lines);
    }

    void nextFrame() {
        if (!bpsk) {
            uint8_t v[meteor::kVcduBytes];
            while (!mux.next(v, c.variant == 1 ? 0x0B : 0x2A, c.variant == 1 ? 9 : 5)) addLine();
            uint8_t cadu[meteor::kCaduBytes];
            meteor::encodeCadu(v, cadu);
            for (int i = 0; i < meteor::kCaduBytes * 8; i++) {
                uint8_t a, b;
                enc.push(cadu[i >> 3] >> (7 - (i & 7)) & 1, a, b);
                bits.push_back(a); bits.push_back(b);
            }
            return;
        }
        uint16_t w[hrpt::kNoaaWords];
        hrpt::Line l = makeLine(line);
        hrpt::encodeNoaaFrame(l, w);
        line = (line + 1) % std::max(1, c.lines);
        for (int i = 0; i < hrpt::kNoaaWords; i++)
            for (int k = 9; k >= 0; k--) {
                const bool b = (w[i] >> k & 1) != 0;
                const bool x = b != c.invert;
                bits.push_back(x ? 1 : 0); bits.push_back(x ? 0 : 1);   // Manchester: first chip high for a one
            }
    }

    void nextSymbol() {
        if (bpsk) {
            while (bits.empty()) nextFrame();
            I[(size_t)(symN & 63)] = bits.front() ? 1.f : -1.f; bits.pop_front();
            Q[(size_t)(symN & 63)] = 0;
        } else {
            while (bits.size() < 2) nextFrame();
            const uint8_t a = bits.front(); bits.pop_front();
            const uint8_t b = bits.front(); bits.pop_front();
            I[(size_t)(symN & 63)] = a ? 1.f : -1.f;
            Q[(size_t)(symN & 63)] = b ? 1.f : -1.f;
        }
        symN++;
    }

    float h(double t) const {
        const double u = (t + kSpan) * kTabRes;
        if (u <= 0 || u >= (double)tab.size() - 1) return 0;
        const size_t i = (size_t)u;
        const float f = (float)(u - (double)i);
        return tab[i] * (1 - f) + tab[i + 1] * f;
    }

    void generate(cf32* out, size_t n) {
        const double r = rs * (1 + c.clockPpm * 1e-6) / c.rate;
        for (size_t i = 0; i < n; i++) {
            const double t = (double)outN * r;
            outN++;
            const int64_t k0 = (int64_t)std::floor(t);
            while (symN <= k0 + kSpan + 1) nextSymbol();
            float re = 0, im = 0;
            for (int64_t k = k0 - kSpan; k <= k0 + kSpan + 1; k++) {
                if (k < 0 || k < symN - 64) continue;
                const float hh = h(t - (double)k);
                re += I[(size_t)(k & 63)] * hh;
                im += Q[(size_t)(k & 63)] * hh;
            }
            std::complex<double> s((double)re * scale, (double)im * scale);
            s *= rot;
            rot *= rstep;
            if ((outN & 4095) == 0) {
                rot /= std::abs(rot);
                if (c.driftHzPerS != 0) rstep = std::polar(1.0, 2 * kPi * (c.cfoHz + c.driftHzPerS * (double)outN / c.rate) / c.rate);
            }
            if (sigma > 0) s += std::complex<double>{sigma * nd(rng), sigma * nd(rng)};
            out[i] = cf32((float)s.real(), (float)s.imag());
        }
    }
};

HrptGenerator::HrptGenerator(const HrptGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
HrptGenerator::~HrptGenerator() = default;
void HrptGenerator::generate(cf32* out, size_t n) { p_->generate(out, n); }

namespace {
class HrptSynth : public ModeSynth {
public:
    HrptSynth(const SynthConfig& s, double rate) : rate_(rate) {
        HrptGenConfig c;
        c.rate = rate;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz - hrptTuning().tuneOffsetHz;
        c.variant = s.modeOpt[0] == 2 ? 2 : s.modeOpt[0] == 3 ? 3 : 1;
        if (s.modeOpt[1] == 1) c.apid = 104;
        c.lines = 200;
        gen_ = std::make_unique<HrptGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<HrptGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeHrptSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<HrptSynth>(cfg, sampleRate);
}

} // namespace dect2
