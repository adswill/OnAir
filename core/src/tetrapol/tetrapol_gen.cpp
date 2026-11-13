// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRAPOL test signal: PAS 0001-2 frames (control or traffic channel) on a GMSK (BT 0.25, h 0.5) 8 kbit/s carrier.
#include "dect2/tetrapol/tetrapol_gen.h"
#include "dect2/tetrapol/tetrapol_codec.h"
#include "dect2/tetrapol/tetrapol_link.h"
#include "dect2/tetrapol/tetrapol_rx.h"
#include "dect2/tetrapol/tetrapol_voice.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
namespace tp = tetrapol;
constexpr double kBaud = 8000.0;
constexpr int kSps = 8;
constexpr double kWork = kBaud * kSps;   // 64 kHz
constexpr double kChanBw = 12500.0;

struct Builder {
    int scr, band;
    std::vector<uint8_t> x;   // frames before the transmission differential
    void frame(const uint8_t* b) { x.insert(x.end(), b, b + tp::kFrameBits); }
    static std::vector<std::vector<uint8_t>> frames(const std::vector<uint8_t>& block, int scr, int band, int asb0) {
        std::vector<std::vector<uint8_t>> out;
        for (const tp::DataFrame& f : tp::blockFrames(block, scr, asb0)) {
            std::vector<uint8_t> b(tp::kFrameBits);
            tp::encodeDataFrame(f, band, b.data());
            out.push_back(b);
        }
        return out;
    }
    void raw(const std::vector<uint8_t>& block, int asb0 = 0) {
        for (const auto& f : frames(block, scr, band, asb0)) frame(f.data());
    }
    void block(const std::vector<uint8_t>& tsdu, int asb0 = 0) {
        const int nb = tp::uiBlockBytes(tsdu.size());
        raw(tp::buildHdlc(0, 7, 0xfff, tp::kCmdUi, tp::uiInfo(tsdu, nb), nb), asb0);
    }
    void voice(uint32_t n) {
        tp::VoiceFrame v;
        v.scramble = scr;
        tp::knownVoiceBits(n, v.codec);
        uint8_t b[tp::kFrameBits];
        tp::encodeVoiceFrame(v, band, b);
        frame(b);
    }
};

// a fill frame (PAS 0001-3-3 7.4.1.9): "no ST" address 0.7.0x000, UI with P = 0, then 5 octets of the 31-bit sequence x0..x3 = 0,
// x4 = 1, xn = xn-3 + xn-5; these 5 octets are one phase as heard on the issue #408 recording
std::vector<uint8_t> stuffing() { return {0x70, 0x00, 0x03, 0x48, 0x57, 0x63, 0xe6, 0x90}; }

// TP_ADDRESS of the talking party (UI_CD): RFSI 290-2-84-509
std::vector<uint8_t> tpAddress() {
    const std::vector<uint8_t> info{0x01, 0xa0, 0x12, 0x90, 0x28, 0x45, 0x09, 0xff, 0xff, 0xff, 0xff};
    return tp::buildHdlc(0, 7, 0xfff, tp::kCmdUiCd, info, 16);
}
std::vector<uint8_t> vch(int codop) { return tp::buildHdlc(0, 7, 0xfff, tp::kCmdUiVch, {(uint8_t)codop, 0, 1}, 8); }

std::vector<uint8_t> neighbours() {
    // two adjacent cells (BN nibble, channel 12 bits, adjacent parameters) then the cell id list
    return {0x94, 0x02, 0x00, (uint8_t)(0x20 | (kTetrapolTestNeighbourCh >> 8)), (uint8_t)(kTetrapolTestNeighbourCh & 255), 0x83,
            0x20, 41, 0x83, 0x02, 0x04, 19, 0x30, 20, 0x30};
}

std::vector<uint8_t> controlSuperframe(int scr, int band) {
    using Frames = std::vector<std::vector<uint8_t>>;
    std::vector<std::vector<uint8_t>> sf(200);
    tp::SysInfo si;
    si.country = kTetrapolTestCountry; si.systemId = kTetrapolTestSystem; si.locArea = kTetrapolTestLa; si.bnId = kTetrapolTestBn;
    si.bsId = kTetrapolTestCell; si.rswId = kTetrapolTestRsw; si.uScramble = scr;
    auto ui = [](const std::vector<uint8_t>& t) { const int nb = tp::uiBlockBytes(t.size()); return tp::buildHdlc(0, 7, 0xfff, tp::kCmdUi, tp::uiInfo(t, nb), nb); };
    for (int half = 0; half < 2; half++) {
        si.bch = half;
        const Frames bch = Builder::frames(ui(tp::buildSysInfo(si)), scr, band, 0);
        for (size_t k = 0; k < bch.size(); k++) sf[(size_t)(100 * half) + k] = bch[k];
        std::vector<uint8_t> pch(16, 0);
        pch[0] = 0x28;
        const int x = kTetrapolTestGroup;
        pch[8] = (uint8_t)(x >> 8); pch[9] = (uint8_t)(x & 255);   // 0.0.<group>
        for (int i = 1; i < 4; i++) { pch[(size_t)(8 + 2 * i)] = 0x70; pch[(size_t)(9 + 2 * i)] = 0x00; }
        const Frames pf = Builder::frames(pch, scr, band, 0);
        sf[(size_t)(100 * half + 98)] = pf[0];
        sf[(size_t)(100 * half + 99)] = pf[1];
    }
    for (int f = 14; f < 200; f += 25) {
        std::vector<uint8_t> r{(uint8_t)(0x10 | (kTetrapolTestRchX >> 8)), (uint8_t)(kTetrapolTestRchX & 255), 0x70, 0x00, 0xc0, 0x00};
        tp::appendFcs(r);
        sf[(size_t)f] = Builder::frames(r, scr, band, 0)[0];
    }
    // SDCH: whole blocks only where they fit between the BCH, PCH and RCH frames
    std::vector<Frames> q;
    for (int rep = 0; rep < 3; rep++) {
        q.push_back(Builder::frames(ui(tp::buildGroupActivation(kTetrapolTestGroup, kTetrapolTestChannel, scr, kTetrapolTestKeyClear)), scr, band, 0));
        q.push_back(Builder::frames(ui(tp::buildGroupActivation(kTetrapolTestGroup + 1, kTetrapolTestChannel + 1, scr + 1, kTetrapolTestKeyCiph)), scr, band, 0));
        q.push_back(Builder::frames(ui(neighbours()), scr, band, 0));
    }
    const Frames fill = Builder::frames(stuffing(), scr, band, 0);
    size_t qi = 0;
    for (size_t f = 0; f < 200;) {
        if (!sf[f].empty()) { f++; continue; }
        size_t room = 0;
        while (f + room < 200 && sf[f + room].empty()) room++;
        if (qi < q.size() && q[qi].size() <= room) {
            for (size_t k = 0; k < q[qi].size(); k++) sf[f + k] = q[qi][k];
            f += q[qi].size();
            qi++;
        } else {
            sf[f++] = fill[0];
        }
    }
    std::vector<uint8_t> x;
    for (const auto& fr : sf) x.insert(x.end(), fr.begin(), fr.end());
    return x;
}

std::vector<uint8_t> cycleBits(int scene, int scr, int band) {
    if (scene == 0) return controlSuperframe(scr, band);
    Builder b{scr, band, {}};
    const int key = scene == 1 ? kTetrapolTestKeyClear : scene == 2 ? kTetrapolTestKeyCiph : -1;
    for (int i = 0; i < 3; i++) b.block(tp::buildCallStart(key));
    b.raw(tpAddress(), 1);
    b.raw(vch(3), 1);   // D_START_SPEECH
    for (int i = 0; i < kTetrapolTestVoiceFrames; i++) b.voice((uint32_t)i);
    for (int i = 0; i < 2; i++) b.block(tp::buildCallEnd());
    b.raw(vch(0), 1);   // D_CHANNEL_FREE
    while (b.x.size() < (size_t)tp::kFrameBits * 200) b.raw(stuffing());
    return b.x;
}

// GMSK, BT 0.25, h 0.5, 8 samples a symbol, as one closed loop (the last symbol leads into the first)
std::vector<cf32> gmsk(const std::vector<uint8_t>& e, bool invert) {
    const size_t n = e.size() * kSps;
    std::vector<float> nrz(n);
    for (size_t i = 0; i < n; i++) nrz[i] = ((e[i / kSps] & 1) ^ (invert ? 1 : 0)) ? -1.f : 1.f;
    const double bt = 0.25, sigma = std::sqrt(std::log(2.0)) / (2 * kPi * bt) * kSps;
    const int half = 2 * kSps;
    std::vector<float> g((size_t)(2 * half + 1));
    double sum = 0;
    for (int k = -half; k <= half; k++) { g[(size_t)(k + half)] = (float)std::exp(-0.5 * k * k / (sigma * sigma)); sum += g[(size_t)(k + half)]; }
    for (float& v : g) v = (float)(v / sum);
    std::vector<cf32> out(n);
    double ph = 0;
    for (size_t i = 0; i < n; i++) {
        double f = 0;
        for (int k = -half; k <= half; k++) f += g[(size_t)(k + half)] * nrz[(i + n + (size_t)k) % n];
        ph += (kPi / 2) / kSps * f;
        out[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
    }
    return out;
}

class TetrapolSynth : public ModeSynth {
public:
    TetrapolSynth(const SynthConfig& cfg, double rate) : rate_(rate), noise_(9) {
        const int scr = cfg.modeOpt[0] > 0 ? std::min(127, cfg.modeOpt[0]) : kTetrapolTestScramble;
        const int scene = std::clamp(cfg.modeOpt[1], 0, 3);
        const int band = cfg.modeOpt[3] == 1 ? tp::kVhf : tp::kUhf;
        std::vector<uint8_t> e = cycleBits(scene, scr, band);
        tp::diffEncode(e.data(), (int)e.size(), e.back() & 1);   // the stream loops: the last bit of the cycle leads into the first
        iq_ = gmsk(e, cfg.modeOpt[2] == 1);
        const double pn = rate / kChanBw / std::pow(10.0, cfg.snrDb / 10.0);
        sigma_ = cfg.snrDb < -100 || cfg.snrDb >= 150 ? 0.f : (float)std::sqrt(pn / 2);   // 200 dB: a transmitter, no noise and the clean level below (not full scale)
        scale_ = sigma_ > 0.15f ? 0.15f / sigma_ : 1.f;
        if (sigma_ == 0.f) scale_ = 0.35f;
        const double w = 2 * kPi * cfg.cfoHz / rate;
        step_ = cf32((float)std::cos(w), (float)std::sin(w));
        adv_ = kWork / rate * (1.0 + cfg.sroPpm * 1e-6);   // clock error: stretch the work-rate playback
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        const size_t len = iq_.size();
        for (size_t i = 0; i < n; i++) {
            const size_t i0 = (size_t)pos_;
            const float fr = (float)(pos_ - (double)i0);
            const cf32 a = iq_[i0 % len], b = iq_[(i0 + 1) % len];
            out[i] = (a + (b - a) * fr) * rot_ * scale_;
            rot_ *= step_;
            pos_ += adv_;
            if (pos_ >= (double)len) pos_ -= (double)len;
        }
        rot_ /= std::abs(rot_);
        if (sigma_ > 0.f) noise_.add(out, n, sigma_ * scale_);
    }
private:
    double rate_, pos_ = 0, adv_ = 1;
    std::vector<cf32> iq_;
    genutil::NoiseSource noise_;
    float sigma_ = 0, scale_ = 1;
    cf32 step_{1.f, 0.f}, rot_{1.f, 0.f};
};
} // namespace

std::vector<uint8_t> tetrapolTestBits(int scene, int scramble, int band) { return cycleBits(scene, scramble, band); }

std::unique_ptr<ModeSynth> makeTetrapolSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < tetrapolTuning().minSampleRate - 1) return nullptr;
    return std::make_unique<TetrapolSynth>(cfg, sampleRate);
}

} // namespace dect2
