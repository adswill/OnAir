// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CODAR / HF ocean radar test signal (see codar_gen.h).
#include "dect2/codar/codar_gen.h"
#include "dect2/codar/codar_rx.h"
#include "dect2/cw/cw_morse.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kNoise = 0.1;         // rms of the complex noise
constexpr double kIdLead = 1.2;        // silence at the start of the ID slot
constexpr double kIdSweep = 0.005;     // repetition of the fast sweeps while the key is down
}

struct CodarGenerator::Impl {
    CodarGenConfig c;
    double dt = 0;
    int64_t g = 0;
    uint64_t rng = 1;
    struct R { CodarGenRadar r; double amp = 0, phase = 0; std::vector<uint8_t> key; };   // key: the ID slot at 1 ms
    std::vector<R> rs;
    double junkPhase = 0, carrierPhase = 0, cleanAmp = 0;
    bool clean = false;
    bool haveSpare = false;
    double spare = 0;

    uint64_t next() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
    double uni() { return ((next() >> 11) + 1) * (1.0 / 9007199254740994.0); }
    double gauss() {
        if (haveSpare) { haveSpare = false; return spare; }
        const double u = uni(), v = uni(), r = std::sqrt(-2 * std::log(u));
        spare = r * std::sin(2 * kPi * v);
        haveSpare = true;
        return r * std::cos(2 * kPi * v);
    }
};

CodarGenerator::CodarGenerator(const CodarGenConfig& c) : p_(new Impl) {
    Impl& m = *p_;
    m.c = c;
    m.dt = 1.0 / c.rate;
    m.rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)c.seed * 0x2545F4914F6CDD1Dull;
    for (int i = 0; i < 4; i++) m.next();
    // the amplitude that gives snrDb in a bin of the 1 ms Hann FFT the receiver uses: bin power = amp^2 x (sum w)^2, noise = sigma^2 x sum w^2
    int N = 512;
    while (N < c.rate / 1000.0 * 0.75 && N < 8192) N *= 2;
    const double gain = 2.0 * N / 3.0;
    m.clean = c.snrDb >= 99;   // the transmitter's clean signal (TxSession): no noise, and fixed levels instead of snrDb over the noise
    m.cleanAmp = 0.25 / std::max<size_t>(1, c.radars.size());   // the sweeps add up to at most 0.25 (the sum of their amplitudes)
    for (const auto& r : c.radars) {
        Impl::R x;
        x.r = r;
        x.amp = m.clean ? m.cleanAmp * std::pow(10.0, std::min(0.0, r.relDb) / 20.0) : kNoise * std::sqrt(std::pow(10.0, (c.snrDb + r.relDb) / 10.0) / gain);
        if (!r.callsign.empty()) {
            const double dit = 1.2 / std::max(5.0, r.wpm);
            std::vector<std::pair<bool, double>> el;
            for (const std::string& tok : cw::tokenize(r.callsign)) {
                if (tok == " ") { el.push_back({false, 7 * dit}); continue; }
                const std::string code = cw::encodeSymbol(tok);
                for (size_t i = 0; i < code.size(); i++) {
                    el.push_back({true, (code[i] == '-' ? 3 : 1) * dit});
                    el.push_back({false, i + 1 < code.size() ? dit : 3 * dit});
                }
            }
            x.key.assign((size_t)(r.idCycleSec * 500), 0);   // the second half of the cycle, in ms
            double t = kIdLead;
            for (const auto& e : el) {
                if (e.first) for (double s = t; s < t + e.second; s += 0.001) { const size_t k = (size_t)(s * 1000); if (k < x.key.size()) x.key[k] = 1; }
                t += e.second;
            }
        }
        m.rs.push_back(std::move(x));
    }
}
CodarGenerator::~CodarGenerator() = default;

void CodarGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    const double fs = m.c.rate;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)m.g * m.dt;
        std::complex<double> z = m.clean ? std::complex<double>(0, 0) : std::complex<double>(m.gauss() * kNoise / std::sqrt(2.0), m.gauss() * kNoise / std::sqrt(2.0));
        if (!m.c.noiseOnly) {
            for (auto& x : m.rs) {
                const CodarGenRadar& r = x.r;
                double f = 0;
                bool on = false;
                bool idSlot = !x.key.empty() && std::fmod(t, r.idCycleSec) >= 0.5 * r.idCycleSec;
                if (idSlot) {
                    const double s = std::fmod(t, r.idCycleSec) - 0.5 * r.idCycleSec;
                    const size_t k = (size_t)(s * 1000);
                    if (k < x.key.size() && x.key[k]) {
                        const double u = std::fmod(t, kIdSweep) / kIdSweep;
                        f = r.offsetHz + r.dir * r.bwHz * (u - 0.5);
                        on = true;
                    }
                } else {
                    double u = std::fmod(t - r.startSec, r.periodSec);
                    if (u < 0) u += r.periodSec;
                    const double len = (1 - m.c.blank) * r.periodSec;
                    if (u < len) { f = r.offsetHz + r.dir * r.bwHz * (u / len - 0.5); on = true; }
                }
                if (on) {
                    x.phase += 2 * kPi * f / fs;
                    if (x.phase > kPi) x.phase -= 2 * kPi; else if (x.phase < -kPi) x.phase += 2 * kPi;
                    z += std::polar(x.amp, x.phase);
                }
            }
            if (m.c.junk) {
                const double a = m.clean ? 0.1 : kNoise * std::sqrt(std::pow(10.0, m.c.snrDb / 10.0) / (2.0 * 2048 / 3.0));
                m.carrierPhase += 2 * kPi * 500e3 / fs;
                if (m.carrierPhase > kPi) m.carrierPhase -= 2 * kPi;
                z += std::polar(a, m.carrierPhase);
                const double span = 0.9 * fs;   // an ionosonde-like sweep, 200 kHz/s: crosses the span in several seconds
                double u = std::fmod(t * 200e3, span);
                m.junkPhase += 2 * kPi * (u - 0.5 * span) / fs;
                if (m.junkPhase > kPi) m.junkPhase -= 2 * kPi; else if (m.junkPhase < -kPi) m.junkPhase += 2 * kPi;
                z += std::polar(a, m.junkPhase);
            }
        }
        out[i] = cf32((float)z.real(), (float)z.imag());
        m.g++;
    }
}

std::vector<CodarGenRadar> codarTestRadars(bool withId) {
    std::vector<CodarGenRadar> v(3);
    v[0].offsetHz = -300e3; v[0].bwHz = 50e3; v[0].periodSec = 0.5; v[0].dir = 1; v[0].relDb = 0; v[0].startSec = 0.1;
    if (withId) v[0].callsign = "AB1CD";
    v[1].offsetHz = 250e3; v[1].bwHz = 100e3; v[1].periodSec = 1.0; v[1].dir = -1; v[1].relDb = -4; v[1].startSec = 0.35;
    v[2].offsetHz = 550e3; v[2].bwHz = 25e3; v[2].periodSec = 0.5; v[2].dir = 1; v[2].relDb = -2; v[2].startSec = 0.2;
    return v;
}

namespace {
class CodarSynth : public ModeSynth {
public:
    CodarSynth(const SynthConfig& cfg, double fs) {
        CodarGenConfig c;
        c.rate = fs;
        c.snrDb = cfg.snrDb;
        c.noiseOnly = cfg.modeOpt[0] == 1;
        c.junk = cfg.modeOpt[0] == 3;
        c.radars = codarTestRadars(cfg.modeOpt[0] == 2);
        if (cfg.modeOpt[0] == 3) c.radars.clear();
        gen_ = std::make_unique<CodarGenerator>(c);
        fs_ = fs;
    }
    double sampleRate() const override { return fs_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    std::unique_ptr<CodarGenerator> gen_;
    double fs_ = 2e6;
};
}

std::unique_ptr<ModeSynth> makeCodarSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<CodarSynth>(cfg, sampleRate);
}

} // namespace dect2
