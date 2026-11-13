// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/hfbeacon/hfbeacon_gen.h"
#include "dect2/hfbeacon/hfbeacon_dsp.h"
#include "dect2/hfbeacon/hfbeacon_list.h"
#include "dect2/hfbeacon/hfbeacon_rx.h"
#include <cmath>
#include <cstring>

namespace dect2::hfb {

static const char* morseChar(char c) {
    static const char* const L[26] = {".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
                                      "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--.."};
    static const char* const D[10] = {"-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----."};
    if (c >= 'A' && c <= 'Z') return L[c - 'A'];
    if (c >= '0' && c <= '9') return D[c - '0'];
    return "";
}

// on/off pattern of a text: dot 1 unit, dash 3, 1 unit between elements, 3 between characters
static void morsePattern(const char* text, double u, std::vector<std::pair<bool, double>>& p) {
    for (const char* c = text; *c; c++) {
        for (const char* e = morseChar(*c); *e; e++) {
            p.push_back({true, (*e == '.' ? 1 : 3) * u});
            p.push_back({false, (e[1] ? 1 : 3) * u});
        }
    }
}

Gen::Gen(double rate, double centreHz, double startUtc, double noiseVar, uint64_t seed) : rate_(rate), centre_(centreHz), t_(startUtc), var_(noiseVar), rng_(seed * 0x9E3779B97F4A7C15ull + 1) {}

double Gen::gauss() {
    auto uni = [this]() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 7; rng_ ^= rng_ << 17; return ((rng_ >> 11) + 0.5) * (1.0 / 9007199254740992.0); };
    if (haveSpare_) { haveSpare_ = false; return spare_; }
    const double u = uni(), v = uni(), r = std::sqrt(-2 * std::log(u));
    spare_ = r * std::sin(2 * M_PI * v); haveSpare_ = true;
    return r * std::cos(2 * M_PI * v);
}

void Gen::add(const GenSrc& s) {
    Src x; x.s = s;
    x.amp = std::sqrt(std::pow(10.0, s.snr100Db / 10) * var_ * 100.0 / rate_);
    if (s.kind == GenSrc::Letter) {
        const char t[2] = {s.letter, 0};
        morsePattern(t, s.unit, x.pat);
        x.pat.back().second = s.gapUnits * s.unit;   // the gap after the letter
        for (auto& e : x.pat) x.patLen += e.second;
    }
    src_.push_back(std::move(x));
}

double Gen::envelope(const Src& x, double t) const {
    const GenSrc& s = x.s;
    if (t < s.from || t > s.to) return 0;
    double a = 0;
    switch (s.kind) {
    case GenSrc::Carrier: a = 1; break;
    case GenSrc::Letter: {
        double tm = std::fmod(t - (s.from > -1e29 ? s.from : 0.0), x.patLen);
        if (tm < 0) tm += x.patLen;
        for (const auto& e : x.pat) { if (tm < e.second) { a = e.first ? 1 : 0; break; } tm -= e.second; }
        break;
    }
    case GenSrc::Dashes: {
        double c = std::fmod(t - s.cycle0, 27.0);
        if (c < 0) c += 27;
        const bool on0 = c >= 3.0 * s.dashIdx && c < 3.0 * s.dashIdx + 2.9;
        const bool on1 = s.dashIdx == 0 && c >= 3.0 && c < 5.9;
        a = (on0 || on1) ? 1 : 0;
        break;
    }
    case GenSrc::Ncdxf: {
        double m = std::fmod(t, (double)kNcdxfCycleSec);
        if (m < 0) m += kNcdxfCycleSec;
        const int slot = (int)(m / kNcdxfSlotSec);
        if (slot != (s.ncdxfIdx + s.band) % kNcdxfBeacons) break;
        const double tau = m - slot * kNcdxfSlotSec;
        if (tau >= s.t4) {
            for (int k = 0; k < 4; k++) if (tau >= s.t4 + k && tau < s.t4 + k + 1 - s.dashGap) a = std::pow(10.0, -k * 10 / 20.0);
        } else if (tau >= 0.1) {
            static thread_local std::vector<std::pair<bool, double>> pat; static thread_local int cachedIdx = -1;
            if (cachedIdx != s.ncdxfIdx) { pat.clear(); morsePattern(kNcdxfCall[s.ncdxfIdx], 60.0 / (50 * 22), pat); cachedIdx = s.ncdxfIdx; }
            double q = tau - 0.1;
            for (const auto& e : pat) { if (q < e.second) { a = e.first ? 1 : 0; break; } q -= e.second; }
        }
        break;
    }
    }
    if (s.fadeDepthDb > 0) a *= std::pow(10.0, -s.fadeDepthDb / 20 * (0.5 - 0.5 * std::cos(2 * M_PI * t / s.fadePeriodSec)));
    return a;
}

void Gen::generate(cf32* out, size_t n) {
    const double sg = std::sqrt(var_ / 2), dt = 1.0 / rate_;
    // braced: the real and imaginary draws are in a fixed order (two calls in one argument list are unordered in C++)
    if (noise_) for (size_t i = 0; i < n; i++) out[i] = cf32{(float)(sg * gauss()), (float)(sg * gauss())};
    else std::fill(out, out + n, cf32(0.f, 0.f));
    const size_t blk = 64;
    for (Src& x : src_) {
        const double w = 2 * M_PI * (x.s.freqHz - centre_) / rate_;
        double t = t_, ph = x.phase;
        for (size_t i = 0; i < n; i += blk) {
            const double a = x.amp * envelope(x, t + 0.5 * blk * dt);
            const size_t m = std::min(blk, n - i);
            if (a > 0) for (size_t k = 0; k < m; k++) out[i + k] += cf32((float)(a * std::cos(ph + w * (double)k)), (float)(a * std::sin(ph + w * (double)k)));
            ph = std::fmod(ph + w * (double)m, 2 * M_PI);
            t += (double)m * dt;
        }
        x.phase = ph;
    }
    t_ += (double)n * dt;
}

} // namespace dect2::hfb

namespace dect2 {

namespace {
class HfbSynth : public ModeSynth {
public:
    HfbSynth(const SynthConfig& c, double fs) : fs_(fs), g_(fs, (c.modeVal[0] > 0 ? c.modeVal[0] : kHfbSynthDefMhz) * 1e6, 0, 0.04, 7) {
        const double fc = (c.modeVal[0] > 0 ? c.modeVal[0] : kHfbSynthDefMhz) * 1e6;
        if (c.snrDb >= 100) g_.setNoise(false);   // the transmitter's clean signal (TxSession): the beacons without the receiver's noise floor
        if (c.modeOpt[0] == 1) return;
        // the cluster at 3594.7 + 0.1 i kHz (priyom); the receiver is told the marker by its keying only
        auto put = [&](double khz, char letter, double snr) {
            hfb::GenSrc s; s.kind = hfb::GenSrc::Letter; s.freqHz = khz * 1e3; s.letter = letter; s.snr100Db = snr; s.unit = 0.1; s.gapUnits = 7;
            if (std::fabs(s.freqHz - fc) < 0.4 * fs_ - 3000) g_.add(s);
        };
        put(3594.7, 'D', 15);
        put(3595.3, 'K', 15);
        hfb::GenSrc cw; cw.kind = hfb::GenSrc::Carrier; cw.freqHz = 3595.45e3; cw.snr100Db = 15;
        if (std::fabs(cw.freqHz - fc) < 0.4 * fs_ - 3000) g_.add(cw);
        // the same two markers on the 13.5277 MHz cluster, at the offsets they have from the mode's own default frequency (14.5 MHz): the test
        // signal does not know the radio's frequency, and from 14.5 MHz the 3.59 MHz cluster is out of the span. Around 3.65 MHz these sit
        // 970 kHz below, on nothing in the beacon list. (The NCDXF beacons need the computer clock, which a file or the test signal has not.)
        const double off = 13527.7e3 - hfbeaconTuning().defMhz * 1e6;
        if (std::fabs(off) < 0.4 * fs_ - 3000) {
            put(fc / 1e3 + off / 1e3, 'D', 15);
            put(fc / 1e3 + off / 1e3 + 0.6, 'K', 15);
        }
    }
    double sampleRate() const override { return fs_; }
    void generate(cf32* out, size_t n) override { g_.generate(out, n); }
private:
    double fs_;
    hfb::Gen g_;
};
}

std::unique_ptr<ModeSynth> makeHfbeaconSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<HfbSynth>(cfg, sampleRate);
}

} // namespace dect2
