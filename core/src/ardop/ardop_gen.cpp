// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ARDOP test signal (see ardop_gen.h).
#include "dect2/ardop/ardop_gen.h"
#include "dect2/ardop/ardop_proto.h"
#include "dect2/ardop/ardop_rx.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace dect2 {

namespace {
using dect2::math::kPi;

struct Sym { double f1, f2; int sign; int64_t len; };   // f2 = 0: one tone; len in samples

std::vector<Sym> renderFrame(const ArdopGenFrame& fr, double rate, double ppm) {
    std::vector<Sym> v;
    const double scale = rate * (1.0 + ppm * 1e-6);
    const int64_t sym50 = (int64_t)std::llround(0.02 * scale), sym100 = (int64_t)std::llround(0.01 * scale);
    const int ls = std::max(5, std::min(50, fr.leaderMs / 20));
    int sign = 1;
    if (ls & 1) sign = -1;
    for (int i = 0; i < ls; i++) {   // the leader: two tones, the sign alternating; the last symbol (sync) breaks the alternation
        v.push_back({1475, 1525, i == ls - 1 ? -sign : sign, sym50});
        sign = -sign;
    }
    const std::vector<uint8_t> bytes = ardop::makeFrameBytes(fr.code, fr.sid, fr.body);
    for (int j = 0; j < 2; j++)
        for (int k = 0; k < 5; k++) {
            const int s = k < 4 ? (bytes[(size_t)j] >> (6 - 2 * k)) & 3 : ardop::typeParity(bytes[0]);
            v.push_back({1425 + 50.0 * s, 0, ((5 * j + k) & 1) ? -1 : 1, sym50});
        }
    ardop::FrameDef d;
    const bool fsk100 = ardop::frameDef(fr.code, d) && d.kind == ardop::KData && d.baud == 100;
    for (size_t i = 2; i < bytes.size(); i++)
        for (int k = 0; k < 4; k++) {
            const int s = (bytes[i] >> (6 - 2 * k)) & 3;
            v.push_back({fsk100 ? 1350 + 100.0 * s : 1425 + 50.0 * s, 0, (k & 1) ? -1 : 1, fsk100 ? sym100 : sym50});
        }
    return v;
}
} // namespace

struct ArdopGenerator::Impl {
    ArdopGenConfig cfg;
    genutil::NoiseSource noise;
    std::vector<std::vector<Sym>> rendered;
    std::vector<int64_t> gapSamples;
    size_t frame = 0;
    size_t sym = 0;
    int64_t left = 0;      // samples left in the current symbol
    bool inGap = true;
    int64_t gapLeft = 0;
    std::complex<double> p1{1, 0}, p2{1, 0}, w1, w2, carrier{1, 0}, cw;
    double amp = 0.4, sigma = 0;
    double scriptSec = 0;

    explicit Impl(const ArdopGenConfig& c) : cfg(c), noise(c.seed) {
        for (const auto& f : c.frames) {
            rendered.push_back(renderFrame(f, c.rate, c.clockPpm));
            gapSamples.push_back((int64_t)std::llround(f.gapSec * c.rate));
        }
        double tot = c.startSec + c.pauseSec;
        for (size_t i = 0; i < rendered.size(); i++) {
            int64_t n = gapSamples[i];
            for (const Sym& s : rendered[i]) n += s.len;
            tot += (double)n / c.rate;
        }
        scriptSec = tot;
        if (c.snrDb >= 99 || c.noiseSigma <= 0) { amp = c.amp; sigma = 0; }
        else {
            sigma = c.noiseSigma;
            amp = std::sqrt(std::pow(10.0, c.snrDb / 10) * 2.0 * 2500.0 * sigma * sigma / c.rate);
        }
        cw = std::polar(1.0, 2 * kPi * (c.dialOffsetHz + c.cfoHz) / c.rate);
        gapLeft = (int64_t)std::llround(c.startSec * c.rate);
        inGap = true;
        frame = 0; sym = 0;
        if (rendered.empty()) { gapLeft = (int64_t)1 << 60; }
    }

    void startSym() {
        const Sym& s = rendered[frame][sym];
        left = s.len;
        w1 = std::polar(1.0, 2 * kPi * s.f1 / cfg.rate);
        w2 = std::polar(1.0, 2 * kPi * s.f2 / cfg.rate);
        p1 = p2 = std::complex<double>(0, -1);   // the analytic signal of sin(): -j e^{j phi}
    }

    void advanceFrame() {
        sym = 0;
        inGap = true;
        gapLeft = gapSamples[frame];
        if (++frame >= rendered.size()) {
            frame = 0;
            if (!cfg.repeat) { gapLeft = (int64_t)1 << 60; }
            else gapLeft += (int64_t)std::llround(cfg.pauseSec * cfg.rate);
        }
    }

    cf32 next() {
        std::complex<double> z(0, 0);
        if (inGap) {
            if (--gapLeft <= 0) { inGap = false; sym = 0; startSym(); }
        } else {
            const Sym& s = rendered[frame][sym];
            if (s.f2 > 0) z = (double)s.sign * 0.5 * amp * (p1 + p2);
            else z = (double)s.sign * amp * p1;
            p1 *= w1; p2 *= w2;
            if (--left <= 0) {
                if (++sym >= rendered[frame].size()) advanceFrame(); else startSym();
            }
        }
        carrier *= cw;
        if ((++cnt & 1023) == 0) carrier /= std::abs(carrier);
        const std::complex<double> o = z * carrier;
        return cf32((float)o.real(), (float)o.imag());
    }
    uint32_t cnt = 0;
};

ArdopGenerator::ArdopGenerator(const ArdopGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
ArdopGenerator::~ArdopGenerator() = default;
double ArdopGenerator::scriptSeconds() const { return p_->scriptSec; }

void ArdopGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    for (size_t i = 0; i < n; i++) out[i] = m.next();
    if (m.sigma > 0) m.noise.add(out, n, (float)m.sigma);
}

std::vector<ArdopGenFrame> ardopTestFrames() {
    using namespace ardop;
    std::vector<ArdopGenFrame> v;
    auto add = [&](int code, uint8_t sid, std::vector<uint8_t> body, double gap = 0.5) {
        ArdopGenFrame f;
        f.code = code; f.sid = sid; f.body = std::move(body); f.gapSec = gap;
        v.push_back(std::move(f));
    };
    auto bytes = [](const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); };
    std::vector<uint8_t> blk;
    add(0x30, 0xFF, makeIdFrame("LY2ABC", "KO24HL"));
    add(0x32, 0xFF, makeCallFrame(0x32, "LY2ABC", "LY1XYZ"));
    add(0x3A, 0xFF, makeConAck(0x3A, 160, 0xFF));
    add(0x3E, 0xFF, makeCallFrame(0x3E, "LY1XYZ", "LY2ABC"));
    add(0x3D, 0xFF, makePingAck(12, 80));
    makeDataBlock(0x48, bytes("CQ CQ DE LY2ABC"), blk);
    add(0x48, 0x5A, blk);
    add(0xF0, 0x5A, {});                       // ACK, quality 38 + 2 * 16 = 70
    makeDataBlock(0x4C, bytes("HELLO FROM LY2ABC"), blk);
    add(0x4C, 0x5A, blk);
    add(0xF4, 0x5A, {});
    makeDataBlock(0x4B, bytes("ARDOP 4FSK 500 Hz monitor test, 100 baud, odd frame."), blk);
    add(0x4B, 0x5A, blk);
    add(0x05, 0x5A, {});                       // NAK, quality 48
    add(0x29, 0x5A, {});                       // DISC
    add(0x2C, 0x5A, {});                       // END
    add(0x44, 0x5A, {}, 4.2);                  // 8PSK.200.100: the header only, then the silence a real frame would fill
    return v;
}

namespace {
class ArdopSynth : public ModeSynth {
public:
    ArdopSynth(const SynthConfig& s, double rate) : rate_(rate) {
        ArdopGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -ardopTuning().tuneOffsetHz;
        c.cfoHz = s.cfoHz;
        c.snrDb = s.snrDb;
        c.frames = ardopTestFrames();
        if (s.modeOpt[0] == 1) c.frames.resize(1);
        gen_ = std::make_unique<ArdopGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<ArdopGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeArdopSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<ArdopSynth>(cfg, sampleRate);
}

} // namespace dect2
