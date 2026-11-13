// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VOR / ILS test signal (see vor_gen.h).
#include "dect2/vor/vor_gen.h"
#include "dect2/vor/vor_rx.h"
#include <cmath>
#include <random>
#include <utility>
#include <vector>

namespace dect2 {

namespace {
constexpr double kTwoPi = 6.283185307179586;

// a key schedule in seconds: (on, off) pairs of one identification word, from the word's start
std::vector<std::pair<double, double>> morseSchedule(const std::string& word, double wpm, double& len) {
    std::vector<std::pair<double, double>> v;
    const double u = 1.2 / std::max(1.0, wpm);
    double t = 0;
    for (size_t i = 0; i < word.size(); i++) {
        const char* m = navMorse(word[i]);
        if (!m) { t += 4 * u; continue; }
        for (const char* p = m; *p; p++) {
            const double d = *p == '-' ? 3 * u : u;
            v.push_back({t, t + d});
            t += d + u;
        }
        t += 2 * u;   // 3 units between letters (one already added)
    }
    len = t;
    return v;
}

struct Biquad {   // band-pass, RBJ
    double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void bandpass(double f, double q, double fs) {
        const double w = kTwoPi * f / fs, al = std::sin(w) / (2 * q), a0 = 1 + al;
        b0 = al / a0; b1 = 0; b2 = -al / a0; a1 = -2 * std::cos(w) / a0; a2 = (1 - al) / a0;
    }
    double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};
} // namespace

struct VorGenerator::Impl {
    VorGenConfig c;
    uint64_t n = 0;
    std::complex<double> carrier{1, 0};
    double ph30 = 0, phS = 0, phId = 0, phT90 = 0, phT150 = 0, phMk = 0, phRefl = 1.0;
    float sigma = 0;
    std::vector<std::pair<double, double>> key;
    double keyLen = 0;
    // voice: noise at 16 kHz through two band-passes, held up to the sample rate
    Biquad v1, v2;
    double vPrev = 0, vNext = 0, vPhase = 1, vRms = 0.05;
    explicit Impl(const VorGenConfig& cc) : c(cc) {
        xs ^= (uint64_t)cc.seed * 0xD1B54A32D192ED03ull;
        sigma = (float)VorGenerator::noiseSigma(c);
        if (!c.ident.empty()) key = morseSchedule(c.ident, c.identWpm, keyLen);
        v1.bandpass(700, 0.9, 16000); v2.bandpass(1600, 0.9, 16000);
    }
    bool keyed(double t) const {
        if (key.empty() || t < 0.5) return false;
        const double tm = std::fmod(t - 0.5, std::max(c.identPeriod, keyLen + 0.5));
        for (const auto& k : key) if (tm >= k.first && tm < k.second) return true;
        return false;
    }
    bool markerKeyed(double t) const {
        switch (c.kind) {
        case 4: return std::fmod(t, 0.5) < 0.375;                     // outer: dashes, 2 per second
        case 6: return std::fmod(t, 1.0 / 6) < 1.0 / 12;              // inner: dots, 6 per second
        case 5: { const double m = std::fmod(t, 0.5 + 1.0 / 6); return m < 0.375 || (m >= 0.5 && m < 0.5 + 1.0 / 12); }   // middle: dot - dash
        default: return false;
        }
    }
    // fast normal pairs (xorshift and Box-Muller): the generator must run in real time at 2 Msps
    uint64_t xs = 0x9E3779B97F4A7C15ull;
    bool haveSpare = false;
    double spare = 0;
    double u01() { xs ^= xs >> 12; xs ^= xs << 25; xs ^= xs >> 27; return ((double)((xs * 2685821657736338717ull) >> 11) + 0.5) * (1.0 / 9007199254740992.0); }
    double gauss() {
        if (haveSpare) { haveSpare = false; return spare; }
        const double r = std::sqrt(-2 * std::log(u01())), a = kTwoPi * u01();
        spare = r * std::sin(a); haveSpare = true;
        return r * std::cos(a);
    }
    double syl = 0;   // the syllable rhythm, updated every 64 samples
    double voiceSample() {   // 16 kHz noise shaped to speech, linearly interpolated, with a syllable rhythm
        vPhase += 16000.0 / c.rate;
        while (vPhase >= 1) {
            vPhase -= 1;
            vPrev = vNext;
            const double w = gauss();
            const double y = v1.run(w) + 0.6 * v2.run(w);
            vRms = 0.9995 * vRms + 0.0005 * y * y;
            vNext = y / std::sqrt(vRms + 1e-12);
        }
        return (vPrev + (vNext - vPrev) * vPhase) * syl * 0.45;
    }
    // the envelope (1 = carrier) seen from bearing `radial`
    double envelope(double radial, double vox, bool id, bool mk) const {
        const double r = radial * kTwoPi / 360;
        double e = 1;
        switch (c.kind) {
        case 0:   // conventional: variable AM lags the FM reference by the radial
            e += 0.3 * std::cos(ph30 - r) + 0.3 * std::cos(phS + 16 * std::sin(ph30));
            break;
        case 1:   // Doppler: AM is the reference; the upper and lower sidebands (0.15 each, antennas turning anticlockwise) put the radial
                  // on the FM: 0.15 e^{j phi} + 0.15 e^{-j phi} = 0.3 cos(phi)
            e += 0.3 * std::cos(ph30) + 0.3 * std::cos(phS + 16 * std::sin(ph30 + r));
            break;
        case 2: e += (0.2 + c.ddm / 2) * std::cos(phT90) + (0.2 - c.ddm / 2) * std::cos(phT150); break;
        case 3: e += (0.4 + c.ddm / 2) * std::cos(phT90) + (0.4 - c.ddm / 2) * std::cos(phT150); break;
        default: break;
        }
        if (c.kind >= 4) {
            const double f = c.kind == 4 ? 400 : c.kind == 5 ? 1300 : 3000;
            if (mk) e += 0.95 * std::cos(phMk * f);
        } else {
            if (id) e += 0.1 * std::cos(phId);
            if (c.kind <= 1 && c.voice) e += 0.2 * vox;
        }
        return std::max(0.0, e);
    }
};

VorGenerator::VorGenerator(const VorGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
VorGenerator::~VorGenerator() = default;
double VorGenerator::timeSec() const { return (double)p_->n / p_->c.rate; }

double VorGenerator::noiseSigma(const VorGenConfig& c) {
    if (c.snrDb >= 99) return 0;
    // carrier power amp^2 against noise 2 sigma^2 * 25e3 / rate
    return std::sqrt(c.amp * c.amp / std::pow(10.0, c.snrDb / 10) * c.rate / (2 * 25e3));
}

void VorGenerator::generate(cf32* out, size_t cnt) {
    Impl& m = *p_;
    const VorGenConfig& c = m.c;
    const double wC = kTwoPi * (c.dialOffsetHz + c.cfoHz) / c.rate, w30 = kTwoPi * c.tone30Hz / c.rate, wS = kTwoPi * c.sub9960Hz / c.rate;
    const double wId = kTwoPi * c.identHz / c.rate, w90 = kTwoPi * 90 / c.rate, w150 = kTwoPi * 150 / c.rate, wMk = kTwoPi / c.rate;
    const double wRefl = kTwoPi * c.reflFadeHz / c.rate, aRefl = c.reflDb > -98 ? std::pow(10.0, c.reflDb / 20) : 0;
    auto wrap = [](double& p, double w, double top) { p += w; if (p >= top) p -= top; };
    const std::complex<double> rotC = std::polar(1.0, wC);
    bool id = false, mk = false;
    for (size_t i = 0; i < cnt; i++) {
        if ((m.n & 63) == 0) {   // the slow parts: keying and the syllable rhythm
            const double t = (double)m.n / c.rate;
            id = m.keyed(t);
            mk = m.markerKeyed(t);
            m.syl = std::pow(std::max(0.0, std::sin(kTwoPi * 3.1 * t + 2 * std::sin(kTwoPi * 0.37 * t))), 2.0);
            m.carrier /= std::abs(m.carrier);
        }
        const double vox = c.kind <= 1 && c.voice ? m.voiceSample() : 0;
        std::complex<double> s = c.amp * m.envelope(c.radialDeg, vox, id, mk) * m.carrier;
        if (aRefl > 0) s += c.amp * aRefl * m.envelope(c.reflRadialDeg, vox, id, mk) * m.carrier * std::polar(1.0, m.phRefl);
        if (m.sigma > 0) s += std::complex<double>(m.sigma * m.gauss(), m.sigma * m.gauss());
        out[i] = cf32((float)s.real(), (float)s.imag());
        m.n++;
        m.carrier *= rotC;
        wrap(m.ph30, w30, kTwoPi);
        wrap(m.phS, wS, kTwoPi);
        wrap(m.phId, wId, kTwoPi);
        wrap(m.phT90, w90, kTwoPi);     // 90 and 150 Hz locked: both restart together every 1/30 s
        wrap(m.phT150, w150, kTwoPi);
        wrap(m.phMk, wMk, kTwoPi * 10);  // 2 pi t, wrapped every 10 s: the marker tones (whole Hz) stay continuous
        wrap(m.phRefl, wRefl, kTwoPi);
    }
}

namespace {
class VorSynth : public ModeSynth {
public:
    VorSynth(const SynthConfig& s, double rate) : rate_(rate) {
        VorGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -vorTuning().tuneOffsetHz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        c.kind = std::max(0, std::min(6, s.modeOpt[0]));
        if (c.kind <= 1) c.radialDeg = s.modeVal[0];
        else c.ddm = s.modeVal[0];
        if (c.kind >= 2) { c.ident = c.kind == 2 ? "IDXB" : ""; c.voice = false; }
        gen_ = std::make_unique<VorGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<VorGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeVorSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<VorSynth>(cfg, sampleRate);
}

} // namespace dect2
