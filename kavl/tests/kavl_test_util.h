// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared helpers of the KAVL tests: seeded random draws and a channel with the impairments of a real link (HackRF transmitter, RTL-SDR receiver):
// noise, carrier offset, phase noise, sample clock offset, fractional delay, IQ imbalance, DC offset, 8-bit quantisation and the receiver's sample rate.
#pragma once
#include "dect2/dsp/exact_resampler.h"
#include "kavl/kavl_phy.h"
#include "kavl/kavl_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace kavltest {
using kavl::cf;

struct Rng {
    dect2::kris::XorShift64 x;
    bool have = false;
    double spare = 0.0;
    explicit Rng(uint64_t seed) : x{seed * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull} {}
    double uniform() { return static_cast<double>(x.next() >> 11) * (1.0 / 9007199254740992.0); }
    uint64_t below(uint64_t n) { return x.next() % n; }
    double gauss() {
        if (have) {
            have = false;
            return spare;
        }
        double u1 = uniform();
        const double u2 = uniform();
        if (u1 < 1e-300) u1 = 1e-300;
        const double r = std::sqrt(-2.0 * std::log(u1)), a = 6.283185307179586 * u2;
        spare = r * std::sin(a);
        have = true;
        return r * std::cos(a);
    }
};

inline std::vector<uint8_t> randomBytes(Rng& r, size_t n) {
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = static_cast<uint8_t>(r.below(256));
    return v;
}

struct Channel {
    double symRate = kavl::kDefaultSymRate;
    double esn0Db = 100.0;        // data Es/N0 (100 = no noise)
    double cfoHz = 0.0;
    double phase = 0.3;
    double clockPpm = 0.0;        // transmitter clock against the receiver's
    double delaySamples = 0.0;    // extra fractional delay (4-sps samples)
    double phaseNoiseDeg = 0.0;   // random-walk phase noise, rms degrees per symbol
    double iqGainDb = 0.0;        // receiver IQ imbalance: I/Q gain ratio
    double iqPhaseDeg = 0.0;      //                         quadrature error
    double dc = 0.0;              // DC offset relative to the signal rms
    bool quant8 = false;          // 8-bit ADC (signal + noise at about 25 counts rms)
    double outRate = 0.0;         // receiver sample rate (0 = 4 x symRate)
    size_t padSamples = 2000;     // noise in front and behind (4-sps samples)
};

inline cf cubicAt(const std::vector<cf>& x, double t) {
    const long i = static_cast<long>(std::floor(t));
    const float m = static_cast<float>(t - static_cast<double>(i));
    auto v = [&](long k) { return (k >= 0 && k < static_cast<long>(x.size())) ? x[static_cast<size_t>(k)] : cf(0, 0); };
    return v(i - 1) * (-m * (m - 1) * (m - 2) / 6.0f) + v(i) * ((m + 1) * (m - 1) * (m - 2) / 2.0f) + v(i + 1) * (-(m + 1) * m * (m - 2) / 2.0f) +
           v(i + 2) * ((m + 1) * m * (m - 1) / 6.0f);
}

// tx: 4 samples per symbol at power 1 per sample (Transmitter::modulate). Returns the samples the receiver sees, at c.outRate (or 4 x symRate).
inline std::vector<cf> applyChannel(const std::vector<cf>& tx, const Channel& c, Rng& rng) {
    const double fs = kavl::kSps * c.symRate;
    // clock offset + delay: the receiver samples the transmitter's signal at t = (n - delay) (1 + ppm)
    std::vector<cf> x(tx.size() + 2 * c.padSamples);
    const double r = 1.0 + c.clockPpm * 1e-6;
    for (size_t n = 0; n < x.size(); n++) {
        const double t = (static_cast<double>(n) - static_cast<double>(c.padSamples) - c.delaySamples) * r;
        x[n] = cubicAt(tx, t);
    }
    // carrier offset, phase noise
    double ph = c.phase;
    const double pnStep = c.phaseNoiseDeg * 3.141592653589793 / 180.0 / std::sqrt(static_cast<double>(kavl::kSps));
    for (size_t n = 0; n < x.size(); n++) {
        if (pnStep > 0) ph += pnStep * rng.gauss();
        const double a = ph + 6.283185307179586 * c.cfoHz / fs * static_cast<double>(n);
        x[n] *= cf(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    }
    // noise: Es = kSps (power 1 per sample, kSps samples per symbol) -> complex noise variance per sample kSps / (Es/N0)
    if (c.esn0Db < 99.0) {
        const double sd = std::sqrt(kavl::kSps / std::pow(10.0, c.esn0Db / 10.0) / 2.0);
        for (auto& v : x) v += cf(static_cast<float>(sd * rng.gauss()), static_cast<float>(sd * rng.gauss()));
    }
    // receiver: IQ imbalance, DC
    const double gI = std::pow(10.0, c.iqGainDb / 40.0), gQ = 1.0 / gI, phi = c.iqPhaseDeg * 3.141592653589793 / 180.0;
    for (auto& v : x) {
        const double i = v.real(), q = v.imag();
        v = cf(static_cast<float>(gI * i + c.dc), static_cast<float>(gQ * (q * std::cos(phi) - i * std::sin(phi)) + c.dc));
    }
    // receiver sample rate
    std::vector<cf> y;
    if (c.outRate > 0 && std::fabs(c.outRate - fs) > 1e-3) {
        dect2::ExactResampler rs;
        if (rs.configure(fs, c.outRate)) rs.process(x.data(), x.size(), y);
    } else {
        y = std::move(x);
    }
    if (c.quant8) {
        double p = 0;
        for (const auto& v : y) p += std::norm(v);
        const double rms = std::sqrt(p / std::max<size_t>(1, y.size()) / 2.0);
        const double scale = 25.0 / std::max(1e-12, rms);
        for (auto& v : y) {
            auto qz = [&](double a) { return std::clamp(std::round(a * scale), -128.0, 127.0) / scale; };
            v = cf(static_cast<float>(qz(v.real())), static_cast<float>(qz(v.imag())));
        }
    }
    return y;
}

// Sends `frames` random frames of modcod m through the channel and counts the code words received intact.
struct RunResult {
    int framesSent = 0, framesFound = 0, blocks = 0, blocksOk = 0;
    double esn0Est = 0, merDb = 0, cfoHz = 0, clockPpm = 0;
};
inline RunResult runLink(int m, int frames, const Channel& c, uint64_t seed, int chunk = 65536) {
    Rng rng(seed);
    const kavl::Modcod& mc = *kavl::modcod(m);
    std::vector<std::vector<uint8_t>> pay;
    for (int i = 0; i < frames; i++) pay.push_back(randomBytes(rng, static_cast<size_t>(kavl::framePayloadBytes(mc))));
    kavl::Transmitter tx(m);
    const auto iq = applyChannel(tx.modulate(pay, 0, 0, 100), c, rng);
    kavl::Receiver::Options o;
    o.symRate = c.symRate;
    o.inRate = c.outRate;
    kavl::Receiver rx(o);
    RunResult res;
    res.framesSent = frames;
    res.blocks = frames * mc.blocks;
    std::vector<kavl::RxFrame> got;
    for (size_t at = 0; at < iq.size(); at += static_cast<size_t>(chunk)) {
        auto f = rx.push(iq.data() + at, std::min(static_cast<size_t>(chunk), iq.size() - at));
        got.insert(got.end(), f.begin(), f.end());
    }
    const std::vector<cf> tail(static_cast<size_t>(kavl::kSps * 400), cf(0, 0));
    auto f = rx.push(tail.data(), tail.size());
    got.insert(got.end(), f.begin(), f.end());
    for (const auto& fr : got) {
        const int idx = fr.hdr.seq - 100;
        if (idx < 0 || idx >= frames || fr.mc != &mc) continue;
        res.framesFound++;
        res.esn0Est += fr.esn0Db;
        res.merDb += fr.merDb;
        res.cfoHz += fr.cfoHz;
        res.clockPpm += fr.clockPpm;
        for (int b = 0; b < mc.blocks; b++) {
            const auto& blk = fr.blocks[static_cast<size_t>(b)];
            if (blk.ok && std::equal(blk.bytes.begin(), blk.bytes.end(), pay[static_cast<size_t>(idx)].begin() + static_cast<long>(b) * mc.payloadBytes)) res.blocksOk++;
        }
    }
    if (res.framesFound) {
        res.esn0Est /= res.framesFound;
        res.merDb /= res.framesFound;
        res.cfoHz /= res.framesFound;
        res.clockPpm /= res.framesFound;
    }
    return res;
}

// RTL-SDR-like receiver impairments on top of the given noise.
inline Channel rtlChannel(double esn0Db, double symRate = kavl::kDefaultSymRate) {
    Channel c;
    c.symRate = symRate;
    c.esn0Db = esn0Db;
    c.cfoHz = 23700.0;        // about 20 ppm at 1.2 GHz
    c.clockPpm = 37.0;
    c.delaySamples = 0.37;
    c.phaseNoiseDeg = 0.05;
    c.iqGainDb = 0.4;
    c.iqPhaseDeg = 1.5;
    c.dc = 0.05;
    c.quant8 = true;
    c.outRate = 2.4e6;
    return c;
}

}  // namespace kavltest
