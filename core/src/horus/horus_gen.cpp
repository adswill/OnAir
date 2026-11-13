// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Project Horus test signals (horus_gen.h): phase-continuous 4FSK / 2FSK at the dial, noise in 3 kHz, frequency and clock errors.
#include "dect2/horus/horus_gen.h"
#include "dect2/horus/horus_rx.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <cmath>

namespace dect2::horus {

double synthDialHz(double rate) {
    const double off = horusTuning().tuneOffsetHz;
    return rate >= 4 * off ? -off : 0.0;
}

std::vector<uint8_t> airBits(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> bytes(8, kPreamble);
    const auto f = encodeFrame(payload.data(), (int)payload.size());
    bytes.insert(bytes.end(), f.begin(), f.end());
    std::vector<uint8_t> bits;
    for (uint8_t b : bytes) for (int k = 7; k >= 0; k--) bits.push_back(uint8_t((b >> k) & 1));
    return bits;
}

Packet simPacket(int kind, int k, double spp) {
    Packet p;
    p.kind = kind;
    const double t = k * spp;
    p.payloadId = kind == 2 ? 256 : kind == 1 ? 0 : -1;
    p.callsign = kind == 2 ? "4FSKTEST-V2" : kind == 1 ? "ID 0" : "ONAIR1";
    p.seq = (uint32_t)(100 + k);
    const int s = 12 * 3600 + (int)t;
    p.hour = (s / 3600) % 24; p.minute = (s / 60) % 60; p.second = s % 60;
    p.lat = 54.8985 + 0.00003 * t + (kind == 1 ? 0.05 : kind == 0 ? -0.05 : 0);
    p.lon = 23.9036 + 0.00016 * t;
    p.altM = std::min(33000.0, 250 + 5.0 * t);
    p.speedKmh = 38; p.sats = 9;
    p.tempC = (int)std::lround(std::max(-60.0, 15 - p.altM * 0.0065));
    p.battV = 3.0;
    return p;
}

} // namespace dect2::horus

namespace dect2 {
namespace {
constexpr double kPi = 3.14159265358979323846;
using namespace horus;

// one balloon: a queue of tones (-1: off), a symbol clock, a phase-continuous oscillator
class Tx {
public:
    Tx(int kind, int baud, int bits, double centreHz, double spacing, bool mirror, double rate, double ppm, double amp)
        : kind_(kind), baud_(baud), bits_(bits), centre_(centreHz), spacing_(spacing), mirror_(mirror), rate_(rate), amp_(amp) {
        symLen_ = rate / (baud * (1 + ppm * 1e-6));
    }
    cf32 next() {
        if (idx_ >= q_.size()) refill();
        while (pos_ >= symLen_) { pos_ -= symLen_; if (++idx_ >= q_.size()) refill(); }
        pos_ += 1;
        const int tone = q_[idx_];
        if (tone < 0) return cf32(0, 0);
        const int m = kind_ >= 1 ? 4 : 2;
        const int ti = mirror_ ? m - 1 - tone : tone;
        const double f = centre_ + (ti - (m - 1) / 2.0) * spacing_;
        ph_ = std::remainder(ph_ + 2 * kPi * f / rate_, 2 * kPi);
        return cf32((float)(amp_ * std::cos(ph_)), (float)(amp_ * std::sin(ph_)));
    }
private:
    int kind_, baud_, bits_, k_ = 0;
    double centre_, spacing_;
    bool mirror_;
    double rate_, amp_, symLen_ = 1, pos_ = 0, ph_ = 0;
    std::vector<int> q_;
    size_t idx_ = 0;
    void refill() {
        q_.clear(); idx_ = 0;
        if (kind_ >= 1) {
            const double spp = (8 + frameBytes(kind_ == 2 ? kV2Bytes : kV1Bytes)) * 4.0 / baud_ + 1.0;
            Packet p = simPacket(kind_, k_++, spp);
            uint8_t custom[9] = {0xF4, 0x01, 0x85, 0xFF, 45, 0x34, 0x21, 0, 0};   // ascent 5.00 m/s, -12.3 C, 45 %, 850.0 hPa (the RS41ng fields)
            const auto bits = airBits(buildBinary(p, kind_, kind_ == 2 ? custom : nullptr));
            for (size_t i = 0; i + 1 < bits.size(); i += 2) q_.push_back(bits[i] * 2 + bits[i + 1]);
            for (int i = 0; i < baud_; i++) q_.push_back(-1);   // a second off between packets
        } else {
            const double chars = 75, spp = chars * (bits_ + 3) / (double)baud_ + 0.8;
            Packet p = simPacket(0, k_++, spp);
            const std::string s = buildSentence(p) + "\n";
            for (int i = 0; i < baud_ / 2; i++) q_.push_back(1);   // half a second of mark
            for (char c : s) {
                q_.push_back(0);
                for (int b = 0; b < bits_; b++) q_.push_back((c >> b) & 1);
                q_.push_back(1); q_.push_back(1);
            }
            for (int i = 0; i < baud_ * 3 / 10; i++) q_.push_back(-1);
        }
    }
};

class HorusSynth final : public ModeSynth {
public:
    HorusSynth(const SynthConfig& cfg, double rate) : rate_(rate), noise_(cfg.modeOpt[3] > 0 ? unsigned(cfg.modeOpt[3]) : 1) {
        const int what = std::clamp(cfg.modeOpt[0], 0, 5);
        const bool mirror = cfg.modeOpt[2] == 1;
        const double dial = synthDialHz(rate) + cfg.cfoHz, ppm = cfg.modeVal[0];
        auto spacing = [&](bool fsk4) { return cfg.modeOpt[1] > 0 ? (double)cfg.modeOpt[1] : fsk4 ? 270.0 : 500.0; };
        const bool multi = what == 5 && rate >= 64000;
        const int n = multi ? 3 : 1;
        // keep the total level for the 8-bit source: the noise of the whole input band counts
        const double snr = std::pow(10.0, cfg.snrDb / 10), rel = cfg.snrDb > -100 ? std::sqrt(rate / (2 * 3000 * snr)) : 0;
        amp_ = std::min(0.35 / n, 0.3 / std::sqrt(n + 2 * rel * rel));
        sigma_ = (float)(amp_ * rel);
        if (what <= 1 || multi) tx_.emplace_back(what == 1 ? 1 : 2, 100, 0, dial, spacing(true), mirror, rate, ppm, amp_);
        if (multi) {
            tx_.emplace_back(1, 100, 0, dial + 12000, spacing(true), mirror, rate, -ppm, amp_);
            tx_.emplace_back(0, 100, 7, dial - 18000, spacing(false), mirror, rate, ppm, amp_);
        } else if (what >= 2) {
            const int baud = what == 3 ? 300 : what == 4 ? 50 : 100, bits = what == 3 ? 8 : 7;
            tx_.emplace_back(0, baud, bits, dial, spacing(false), mirror, rate, ppm, amp_);
        }
        if (what == 5 && !multi) tx_.emplace_back(2, 100, 0, dial, spacing(true), mirror, rate, ppm, amp_);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            cf32 s(0, 0);
            for (auto& t : tx_) s += t.next();
            out[i] = s;
        }
        if (sigma_ > 0) noise_.add(out, n, sigma_);
    }
private:
    double rate_, amp_ = 0.3;
    std::vector<Tx> tx_;
    genutil::NoiseSource noise_;
    float sigma_ = 0;
};
} // namespace

std::unique_ptr<ModeSynth> makeHorusSynth(const SynthConfig& cfg, double rate) {
    if (!std::isfinite(rate) || rate < horusTuning().minSampleRate) return nullptr;
    return std::make_unique<HorusSynth>(cfg, rate);
}
} // namespace dect2
