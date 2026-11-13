// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Lightning position (network) simulation and the mode's test signal (ltpos_gen.h).
#include "dect2/sferics/ltpos_gen.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace dect2::ltpos {

namespace {
constexpr double kPi = 3.14159265358979323846;
int bitOf(int64_t k, uint32_t seed) {   // the reference's data bits: a fixed pseudo-random sequence
    uint64_t z = (uint64_t)k * 0x9E3779B97F4A7C15ULL + seed * 0xBF58476D1CE4E5B9ULL;
    z ^= z >> 31; z *= 0x94D049BB133111EBULL; z ^= z >> 29;
    return (z & 1) ? 1 : -1;
}
}

double simArrivalS(const SimScene& sc, const SimStrike& k, const SimStation& st) {
    return k.tS + propagationS(sc.pm, geodesicM(k.lat, k.lon, st.lat, st.lon));
}
double simSampleTime(const SimStation& st, double trueS) { return (trueS - st.startS) * (1 + st.ppm * 1e-6); }
double simTrueTime(const SimStation& st, double sampleS) { return st.startS + sampleS / (1 + st.ppm * 1e-6); }

void simRender(const SimScene& sc, const SimStation& st, double rate, uint64_t first, size_t n, std::complex<float>* out, MskState* msk) {
    std::mt19937 rng(st.seed * 2654435761u + (uint32_t)(first / std::max<uint64_t>(1, n)) * 40503u + (uint32_t)first);
    std::normal_distribution<float> nd(0.f, 1.f);
    const float sg = (float)(st.noiseRms / std::sqrt(2.0));
    for (size_t i = 0; i < n; i++) out[i] = std::complex<float>(nd(rng) * sg, nd(rng) * sg);
    // sferics: a raised-cosine rise then an exponential decay, oscillating at oscHz in the band; evaluated at each sample's true time
    const double tau = sc.tauUs * 1e-6, rise = sc.riseUs * 1e-6;
    for (size_t si = 0; si < sc.strikes.size(); si++) {
        const SimStrike& k = sc.strikes[si];
        const double d = std::max(1e3, geodesicM(k.lat, k.lon, st.lat, st.lon));
        const double amp = std::min(sc.maxAmp, k.ampAt100km * 100e3 / d);
        const double ta = simArrivalS(sc, k, st);
        const double s0 = simSampleTime(st, ta) * rate;   // fractional sample index of the arrival
        const double len = (rise + 8 * tau) * rate * (1 + st.ppm * 1e-6) + 2;
        const double ph0 = 2 * kPi * std::fmod(0.6180339887 * (double)(si + 1) * 7.0, 1.0);
        const int64_t a = std::max<int64_t>((int64_t)first, (int64_t)std::ceil(s0));
        const int64_t b = std::min<int64_t>((int64_t)(first + n), (int64_t)std::ceil(s0 + len));
        for (int64_t m = a; m < b; m++) {
            const double u = simTrueTime(st, (double)m / rate) - ta;
            if (u < 0) continue;
            const double env = (u < rise ? 0.5 * (1 - std::cos(kPi * u / rise)) : std::exp(-(u - rise) / tau));
            const double ph = ph0 + 2 * kPi * sc.oscHz * u;
            out[(size_t)(m - (int64_t)first)] += std::complex<float>((float)(amp * env * std::cos(ph)), (float)(amp * env * std::sin(ph)));
        }
    }
    // the MSK reference: f = offset +- baud / 4, continuous phase, symbol boundaries at refEpochS + k T + its delay to this station
    if (st.refAmp > 0 && sc.ref.baud > 0 && msk) {
        const double T = 1.0 / sc.ref.baud;
        const double delay = propagationS(sc.pm, geodesicM(sc.ref.lat, sc.ref.lon, st.lat, st.lon));
        for (size_t i = 0; i < n; i++) {
            const double t = simTrueTime(st, (double)(first + i) / rate);
            const double tp = t - delay - sc.refEpochS;
            const int64_t k = (int64_t)std::floor(tp / T);
            if (!msk->init) { msk->k = k; msk->theta = 0; msk->init = true; }
            while (msk->k < k) { msk->theta += bitOf(msk->k, sc.refSeed) * kPi / 2; msk->k++; }
            const double u = tp - (double)k * T;
            const double ph = msk->theta + bitOf(k, sc.refSeed) * kPi * u / (2 * T) + 2 * kPi * std::fmod((st.refOffsetHz + st.cfoHz) * t, 1.0);
            out[i] += std::complex<float>((float)(st.refAmp * std::cos(ph)), (float)(st.refAmp * std::sin(ph)));
        }
        msk->theta = std::fmod(msk->theta, 2 * kPi);
    }
    // the radio's own carrier offset (cfoHz) is applied to the reference above; on a sferic it only turns the phase, which the timer ignores
}

namespace {
class LtposSynth : public ModeSynth {
public:
    LtposSynth(const SynthConfig& c, double rate) : rate_(rate), cfg_(c), rng_(23) {
        sc_.ref = RefTx{250, "test reference", 0, 200, 52.0, 10.0};
        st_.lat = 52.5; st_.lon = 13.4;
        st_.noiseRms = 0.01 * std::pow(10.0, -(std::clamp(c.snrDb >= 99 ? 30.0 : c.snrDb, 0.0, 60.0) - 30.0) / 20.0);
        st_.ppm = c.sroPpm;
        st_.cfoHz = c.cfoHz;
        if (c.modeOpt[0] == 1) {
            st_.refAmp = 0.05;
            st_.refOffsetHz = c.modeVal[1] != 0 ? c.modeVal[1] : 50e3;
            if (std::fabs(st_.refOffsetHz) > rate / 2.5) st_.refOffsetHz = rate / 4;
        }
        sc_.oscHz = std::min(25e3, rate / 8);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        while (n > 0) {
            const size_t chunk = std::min<size_t>(n, (size_t)std::max(1.0, rate_ * 0.1));
            planStrikes((double)(pos_ + chunk) / rate_ + 0.01);
            simRender(sc_, st_, rate_, pos_, chunk, out, &msk_);
            pos_ += chunk; out += chunk; n -= chunk;
            const double now = (double)pos_ / rate_;
            sc_.strikes.erase(std::remove_if(sc_.strikes.begin(), sc_.strikes.end(), [&](const SimStrike& k) { return k.tS < now - 0.05; }), sc_.strikes.end());
        }
    }
private:
    void planStrikes(double until) {
        const double perS = cfg_.modeVal[0] > 0 ? cfg_.modeVal[0] : 2.0;
        std::exponential_distribution<double> ed(perS);
        std::uniform_real_distribution<double> ud(0.0, 1.0);
        while (nextT_ < until) {
            SimStrike k;
            k.tS = nextT_;
            k.lat = st_.lat + (ud(rng_) - 0.5) * 6; k.lon = st_.lon + (ud(rng_) - 0.5) * 8;
            k.ampAt100km = 0.3 + 0.5 * ud(rng_);
            sc_.strikes.push_back(k);
            nextT_ += std::max(0.002, ed(rng_));
        }
    }
    double rate_;
    SynthConfig cfg_;
    std::mt19937 rng_;
    SimScene sc_;
    SimStation st_;
    MskState msk_;
    uint64_t pos_ = 0;
    double nextT_ = 0.3;
};
}

std::unique_ptr<ModeSynth> makeLtposSynth(const SynthConfig& cfg, double sampleRate) { return std::make_unique<LtposSynth>(cfg, sampleRate); }

} // namespace dect2::ltpos
