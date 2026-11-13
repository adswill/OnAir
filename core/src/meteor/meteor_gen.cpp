// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor-M LRPT test signal (see meteor_gen.h).
#include "dect2/meteor/meteor_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/dsp/rrc.h"
#include "dect2/meteor/meteor_proto.h"
#include "dect2/meteor/meteor_rx.h"
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

uint8_t meteorTestPixel(int c, int x, int y) {
    const double fx = x / 1568.0, fy = (y % 400) / 400.0;
    double v;
    switch (c % 3) {
    case 0: v = 40 + 170 * fx + 30 * std::sin(fy * 6.283); break;
    case 1: v = 60 + 150 * fy + 25 * std::cos(fx * 12.566); break;
    default: { const double dx = x - 784.0, dy = (y % 400) - 200.0; v = 128 + 90 * std::cos(std::sqrt(dx * dx + dy * dy) / 40.0); } break;
    }
    if (c >= 3) v = 255 - v;
    if (x % 196 < 2 || y % 64 < 2) v = 230;
    return (uint8_t)std::clamp((int)std::lround(v), 0, 255);
}

struct MeteorGenerator::Impl {
    MeteorGenConfig c;
    double rs = 72000;
    bool oqpsk = true, diff = true, inter = false;
    meteor::PacketMux mux;
    int row = 0, seq = 0;
    std::deque<uint8_t> bits;        // channel bits, two a symbol
    meteor::ConvEncoder enc;
    meteor::NrzmEncoder nrzm;        // N2-x differential code, on the bits before the convolutional encoder
    std::vector<uint8_t> ring;       // interleaver: the code bits of the last 36 x 73728
    int64_t interP = 0;
    int markPos = 0;
    std::vector<float> I, Q;         // symbol ring
    int64_t symN = 0;
    int64_t outN = 0;
    std::vector<float> tab;
    double scale = 1, sigma = 0;
    std::mt19937 rng;
    dect2::normal_distribution<float> nd{0.f, 1.f};
    std::complex<double> rot{1, 0}, rstep{1, 0}, dstep{1, 0};

    explicit Impl(const MeteorGenConfig& cfg) : c(cfg), rng(cfg.seed) {
        rs = c.modulation == 3 ? 80000 : 72000;
        oqpsk = c.modulation != 2; diff = c.modulation != 2; inter = c.modulation == 3;
        if (inter) {   // a running interleaver is full: start it with noise-like bits, not a long run of zeros
            ring.resize((size_t)meteor::kInterBranches * meteor::kInterBranches * meteor::kInterDelay);
            for (auto& b : ring) b = (uint8_t)(rng() & 1);
        }
        tab.resize((size_t)(2 * kSpan * kTabRes + 1));
        double e = 0;
        for (size_t i = 0; i < tab.size(); i++) {
            const double t = (double)i / kTabRes - kSpan;
            const double w = 0.5 + 0.5 * std::cos(kPi * t / kSpan);
            tab[i] = (float)(rrc(t, 0.6) * w);
            e += (double)tab[i] * tab[i] / kTabRes;
        }
        scale = c.amp / std::sqrt(2 * e);
        if (c.snrDb < 99) { const double n0 = c.amp * c.amp / (std::pow(10.0, c.snrDb / 10) * rs); sigma = std::sqrt(n0 * c.rate / 2); }
        rstep = std::polar(1.0, 2 * kPi * c.cfoHz / c.rate);
        dstep = std::polar(1.0, 2 * kPi * c.cfoDriftHzPerSec / (c.rate * c.rate));
        I.assign(64, 0); Q.assign(64, 0);
    }

    void addRow() {
        const uint32_t day = 20000, ms = (uint32_t)(36000000 + row * 1220);
        uint8_t strip[8][meteor::kMcuPerPacket * 8];
        for (int a : c.apids) {
            const int ch = a - meteor::kFirstApid;
            for (int k = 0; k < meteor::kMcuPerLine / meteor::kMcuPerPacket; k++) {
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < meteor::kMcuPerPacket * 8; x++) strip[y][x] = meteorTestPixel(ch, k * 112 + x, row * 8 + y);
                mux.add(a, seq, meteor::encodeImagePacket(strip, k * meteor::kMcuPerPacket, c.quality, day, ms));
                seq = (seq + 1) & 0x3FFF;
            }
        }
        std::vector<uint8_t> tlm(8 + 100, 0);
        mux.add(70, seq, tlm);
        seq = (seq + 1) & 0x3FFF;
        row = (row + 1) % std::max(1, c.rows);
    }

    void pushChannelBit(uint8_t b) {
        if (!inter) { bits.push_back(b); return; }
        while (markPos < 8) { bits.push_back((uint8_t)(meteor::kMarker >> (7 - markPos) & 1)); markPos++; }
        // the data bit: t[p] = code[p - (p mod 36) * 36 * 2048]
        const int64_t S = (int64_t)ring.size(), per = (int64_t)meteor::kInterBranches * meteor::kInterDelay;
        ring[(size_t)(interP % S)] = b;
        const int64_t src = interP - (interP % meteor::kInterBranches) * per;
        bits.push_back(ring[(size_t)(((src % S) + S) % S)]);
        interP++;
        if (++markPos == meteor::kMarkerStride) markPos = 0;
    }

    void nextFrame() {
        uint8_t v[meteor::kVcduBytes];
        while (!mux.next(v, 0x9C, 5)) addRow();
        uint8_t cadu[meteor::kCaduBytes];
        meteor::encodeCadu(v, cadu, false);
        for (int i = 0; i < meteor::kCaduBytes * 8; i++) {
            uint8_t a, b;
            uint8_t bit = (uint8_t)(cadu[i >> 3] >> (7 - (i & 7)) & 1);
            if (diff) bit = nrzm.push(bit);
            enc.push(bit, a, b);
            pushChannelBit(a); pushChannelBit(b);
        }
    }

    void nextSymbol() {
        while (bits.size() < 2) nextFrame();
        const uint8_t a = bits.front(); bits.pop_front();
        const uint8_t b = bits.front(); bits.pop_front();
        I[(size_t)(symN & 63)] = a ? 1.f : -1.f;
        Q[(size_t)(symN & 63)] = b ? 1.f : -1.f;
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
                re += I[(size_t)(k & 63)] * h(t - (double)k);
                im += Q[(size_t)(k & 63)] * h(t - (double)k - (oqpsk ? 0.5 : 0.0));
            }
            std::complex<double> s((double)re * scale, (double)im * scale);
            s *= rot;
            rot *= rstep;
            rstep *= dstep;
            if ((outN & 4095) == 0) { rot /= std::abs(rot); rstep /= std::abs(rstep); }
            if (sigma > 0) s += std::complex<double>{sigma * nd(rng), sigma * nd(rng)};
            out[i] = cf32((float)s.real(), (float)s.imag());
        }
    }
};

MeteorGenerator::MeteorGenerator(const MeteorGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
MeteorGenerator::~MeteorGenerator() = default;
void MeteorGenerator::generate(cf32* out, size_t n) { p_->generate(out, n); }

namespace {
class MeteorSynth : public ModeSynth {
public:
    MeteorSynth(const SynthConfig& s, double rate) : rate_(rate) {
        MeteorGenConfig c;
        c.rate = rate;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz - meteorTuning().tuneOffsetHz;   // where the receiver expects it: the radio is tuned above the channel
        c.modulation = s.modeOpt[0] == 2 ? 2 : s.modeOpt[0] == 3 ? 3 : 1;
        if (s.modeOpt[1] == 1) c.apids = {64, 65, 68};
        c.rows = 200;
        gen_ = std::make_unique<MeteorGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<MeteorGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeMeteorSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<MeteorSynth>(cfg, sampleRate);
}

} // namespace dect2
