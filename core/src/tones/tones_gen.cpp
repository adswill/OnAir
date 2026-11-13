// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tone signalling test signals: see tones_gen.h.
#include "dect2/tones/tones_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/source/source.h"
#include "dect2/tones/tones_proto.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

namespace dect2 {

using dect2::math::kPi;

void ToneScene::pad(double sec) {
    const size_t n = (size_t)std::lround(sec * rate_);
    if (a_.size() < n) a_.resize(n, 0.f);
}
void ToneScene::tone(double hz, double t0, double sec, double amp) {
    pad(t0 + sec);
    const size_t i0 = (size_t)std::lround(t0 * rate_), n = (size_t)std::lround(sec * rate_);
    for (size_t i = 0; i < n; i++) a_[i0 + i] += (float)(amp * std::sin(2 * kPi * hz * (double)(i0 + i) / rate_));
}
void ToneScene::ctcss(double hz, double t0, double sec, double amp) { tone(hz, t0, sec, amp); }

void ToneScene::dcs(int code, bool inverted, double t0, double sec, double amp, double startBit) {
    pad(t0 + sec);
    const uint32_t w = tones::dcsWord(code);
    const size_t i0 = (size_t)std::lround(t0 * rate_), n = (size_t)std::lround(sec * rate_);
    // NRZ, then a two-pole low-pass at 250 Hz like the transmitters' deviation filter
    std::vector<float> d(n);
    for (size_t i = 0; i < n; i++) {
        const double bit = startBit + (double)i / rate_ * tones::kDcsBaud;
        int b = (int)((w >> ((long)bit % 23)) & 1);
        if (inverted) b ^= 1;
        d[i] = b ? 1.f : -1.f;
    }
    const double al = 1 - std::exp(-2 * kPi * 250.0 / rate_);
    double y1 = d.empty() ? 0 : d[0], y2 = y1;
    for (size_t i = 0; i < n; i++) {
        y1 += al * (d[i] - y1);
        y2 += al * (y1 - y2);
        a_[i0 + i] += (float)(amp * y2);
    }
}
void ToneScene::dtmf(const std::string& keys, double t0, double toneSec, double gapSec, double amp) {
    double t = t0;
    for (char k : keys) {
        for (int i = 0; i < 16; i++) if (tones::kDtmfKeys[i] == k) {
            tone(tones::kDtmfRows[i / 4], t, toneSec, amp * 0.5);
            tone(tones::kDtmfCols[i % 4], t, toneSec, amp * 0.5);
        }
        t += toneSec + gapSec;
    }
}
void ToneScene::sequential(const char* setId, const std::string& digits, double t0, double toneMs, double amp) {
    const tones::SeqSet* st = nullptr;
    for (int i = 0; i < tones::kSeqSetN; i++) if (!std::strcmp(tones::kSeqSets[i].id, setId)) st = &tones::kSeqSets[i];
    if (!st) return;
    double t = t0;
    char prev = 0;
    for (char c : digits) {
        const double f = (c == prev) ? st->rep : st->digit[c - '0'];
        const double sec = toneMs / 1000.0;
        // a keyed tone: phase continuous per tone, 1 ms raised edges
        pad(t + sec);
        const size_t i0 = (size_t)std::lround(t * rate_), n = (size_t)std::lround(sec * rate_);
        const size_t ramp = (size_t)std::max(1.0, rate_ * 0.001);
        for (size_t i = 0; i < n; i++) {
            const double g = std::min({1.0, (double)i / ramp, (double)(n - i) / ramp});
            a_[i0 + i] += (float)(amp * g * std::sin(2 * kPi * f * (double)i / rate_));
        }
        t += sec;
        prev = c;
    }
}
void ToneScene::selcal(const std::string& code, double t0, double amp, double pairSec, double gapSec) {
    auto idx = [](char c) { for (int i = 0; i < tones::kSelcalN; i++) if (tones::selcalLetter(i) == c) return i; return 0; };
    for (size_t p = 0; p + 1 < code.size(); p += 2) {
        const double t = t0 + (double)(p / 2) * (pairSec + gapSec);
        tone(tones::selcalHz(idx(code[p])), t, pairSec, amp * 0.5);
        tone(tones::selcalHz(idx(code[p + 1])), t, pairSec, amp * 0.5);
    }
}
void ToneScene::twoTone(double hzA, double hzB, double t0, double secA, double secB, double amp) {
    tone(hzA, t0, secA, amp);
    tone(hzB, t0 + secA, secB, amp);
}
void ToneScene::speech(double t0, double sec, double amp) {
    pad(t0 + sec);
    std::mt19937 rng(seed_ + (uint32_t)(t0 * 1000));
    dect2::uniform_real_distribution<double> u(0, 1);
    const size_t i0 = (size_t)std::lround(t0 * rate_), n = (size_t)std::lround(sec * rate_);
    double ph = 0, f0 = 100 + 40 * u(rng), lp = 0;
    double target = f0, syl = 0;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / rate_;
        if (i % (size_t)(rate_ * 0.08) == 0) target = 85 + 120 * u(rng);   // the pitch wanders every 80 ms
        f0 += (target - f0) * 0.0006 * (48000.0 / rate_) * 8;
        ph += 2 * kPi * f0 / rate_;
        syl = 0.5 + 0.5 * std::sin(2 * kPi * 3.7 * t + 1.3 * std::sin(2 * kPi * 0.9 * t));   // syllable rhythm
        double v = 0;
        for (int h = 1; h <= 24; h++) {
            const double fh = f0 * h;
            if (fh > 3400) break;
            // formants near 500, 1500 and 2500 Hz
            const double env = std::exp(-std::pow((fh - 500) / 350, 2)) + 0.6 * std::exp(-std::pow((fh - 1500) / 500, 2)) + 0.3 * std::exp(-std::pow((fh - 2500) / 600, 2)) + 0.25 / h;
            v += env * std::sin(h * ph);
        }
        lp += 0.2 * ((u(rng) - 0.5) - lp);
        const double fric = (std::fmod(t, 0.35) < 0.07) ? (u(rng) - 0.5 - lp) * 1.2 : 0.0;
        a_[i0 + i] += (float)(amp * 0.22 * (syl * v + fric));
    }
}
void ToneScene::noise(double t0, double sec, double rms) {
    pad(t0 + sec);
    std::mt19937 rng(seed_ * 7919u + (uint32_t)(t0 * 10));
    dect2::normal_distribution<double> g(0, rms);
    const size_t i0 = (size_t)std::lround(t0 * rate_), n = (size_t)std::lround(sec * rate_);
    for (size_t i = 0; i < n; i++) a_[i0 + i] += (float)g(rng);
}
void ToneScene::normalize(double peak) {
    float m = 0;
    for (float v : a_) m = std::max(m, std::fabs(v));
    if (m <= 0) return;
    const float k = (float)(peak / m);
    for (float& v : a_) v *= k;
}

std::vector<cf32> toneSceneFm(const ToneScene& s, double rate, double offsetHz, double snrDb, uint32_t seed) {
    const size_t n = (size_t)std::lround(s.seconds() * rate);
    std::vector<cf32> out(n);
    const std::vector<float>& a = s.audio();
    const double adv = s.rate() / rate;
    double pos = 0, th = 0;
    for (size_t i = 0; i < n; i++) {
        const size_t i0 = (size_t)pos;
        const float fr = (float)(pos - (double)i0);
        const float v0 = i0 < a.size() ? a[i0] : 0.f, v1 = i0 + 1 < a.size() ? a[i0 + 1] : 0.f;
        const double v = v0 + (v1 - v0) * fr;
        th += 2 * kPi * (offsetHz + 5000.0 * v) / rate;
        if (th > kPi) th -= 2 * kPi; else if (th < -kPi) th += 2 * kPi;
        out[i] = cf32((float)std::cos(th), (float)std::sin(th));
        pos += adv;
    }
    if (snrDb < 100) {
        const double pn = rate / 25000.0 / std::pow(10.0, snrDb / 10.0);
        genutil::NoiseSource noise(seed);
        noise.add(out.data(), out.size(), (float)std::sqrt(pn / 2));
    }
    return out;
}

namespace {

ToneScene demoScene(int variant) {
    ToneScene s(48000, 5);
    const bool noiseOnly = variant == 1;
    if (noiseOnly) { s.pad(4); return s; }
    if (variant == 2) {   // SELCAL mode: voice, then three calls (a code heard twice) with voice between
        s.speech(0.5, 3.0, 0.4);
        s.selcal("AMCP", 4.0, 0.5);
        s.speech(8.0, 2.0, 0.4);
        s.selcal("DHKS", 10.5, 0.5);
        s.speech(14.5, 2.0, 0.4);
        s.selcal("AMCP", 17.0, 0.5);
        s.pad(22);
        return s;
    }
    // 0 - 10 s: CTCSS 123.0 Hz (12% of the deviation) under speech from 2 s on
    s.ctcss(123.0, 0, 10, 0.12);
    s.speech(2.0, 2.2, 0.4); s.speech(5.0, 3.0, 0.4);
    // 10 - 18 s: DCS D023N
    s.dcs(23, false, 10.2, 7.6, 0.15);
    // DTMF 159#, ZVEI1 12345, SELCAL AM-CP, two-tone page
    s.dtmf("159#", 18.5, 0.12, 0.1, 0.5);
    s.sequential("zvei1", "12345", 22.0, 70, 0.5);
    s.selcal("AMCP", 24.5, 0.5);
    s.twoTone(682.5, 1000.0, 29.5, 1.0, 3.0, 0.5);
    s.pad(40);
    return s;
}

class TonesSynth : public ModeSynth {
public:
    TonesSynth(const SynthConfig& c, double rate) : rate_(rate), cfg_(c) {
        const ToneScene s = demoScene(c.modeOpt[0]);
        audio_ = s.audio();
        const double pn = rate / 25000.0 / std::pow(10.0, c.snrDb / 10.0);
        sigma_ = c.snrDb < -100 ? 0.f : (float)std::sqrt(pn / 2);
        scale_ = sigma_ > 0.15f ? 0.15f / sigma_ : 0.4f;
        if (sigma_ == 0.f) scale_ = 0.4f;
        nz_ = std::make_unique<genutil::NoiseSource>(11);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        const double adv = 48000.0 / rate_;
        const double w = 2 * kPi * (cfg_.cfoHz - 50000.0) / rate_;
        const size_t len = audio_.size();
        for (size_t i = 0; i < n; i++) {
            const size_t i0 = (size_t)pos_ % len, i1 = (i0 + 1) % len;
            const float fr = (float)(pos_ - std::floor(pos_));
            const double v = audio_[i0] + (audio_[i1] - audio_[i0]) * fr;
            th_ += 2 * kPi * 5000.0 * v / rate_ + w;
            if (th_ > kPi) th_ -= 2 * kPi; else if (th_ < -kPi) th_ += 2 * kPi;
            out[i] = cf32((float)std::cos(th_), (float)std::sin(th_)) * scale_;
            pos_ += adv;
            if (pos_ >= (double)len) pos_ -= (double)len;
        }
        if (sigma_ > 0.f) nz_->add(out, n, sigma_ * scale_);
    }
private:
    double rate_;
    SynthConfig cfg_;
    std::vector<float> audio_;
    std::unique_ptr<genutil::NoiseSource> nz_;
    float sigma_ = 0, scale_ = 0.4f;
    double pos_ = 0, th_ = 0;
};

} // namespace

std::unique_ptr<ModeSynth> makeTonesSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<TonesSynth>(cfg, sampleRate);
}

} // namespace dect2

namespace dect2 {
std::unique_ptr<ModeSynth> makeSelcalSynth(const SynthConfig& cfg, double sampleRate) {
    SynthConfig c = cfg; c.modeOpt[0] = 2;
    return makeTonesSynth(c, sampleRate);
}
}
