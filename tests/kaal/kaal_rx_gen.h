// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test transmitter for the KAAL receiver tests (v3 format): codec v3 frames -> rate-matched LDPC words of 480 bits (240 or 320 information
// bits) -> 4-word pseudo-random interleave -> 16-QAM with preamble and pilots (160-symbol frames) -> RRC -> the sweep at -10 dB with the
// station ID in its variants, plus channel helpers. Built from the PHY functions on purpose, not from VoiceTx.
#pragma once
#include "dect2/kaal/kaal_codec3.h"
#include "dect2/kaal/kaal_fec.h"
#include "dect2/kaal/kaal_phy.h"
#include "dect2/kaal/kaal_sweepid.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace kaalrx {
using namespace dect2::kaal;
using cd = std::complex<double>;

// Test speech-like signal: gliding tones with a slow amplitude envelope and a little deterministic noise. n samples at 16 kHz.
inline std::vector<int16_t> testPcm(size_t n) {
    std::vector<int16_t> x(n);
    uint64_t st = 0x1234567ULL;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / 16000.0;
        const double env = 0.6 + 0.4 * std::sin(2 * 3.141592653589793 * 1.7 * t);
        const double f1 = 300.0 + 120.0 * std::sin(2 * 3.141592653589793 * 0.8 * t);
        const double nz = ((double)(xorshift64(st) >> 40) / 16777216.0 - 0.5) * 800.0;
        x[i] = (int16_t)(env * (5000.0 * std::sin(2 * 3.141592653589793 * f1 * t) + 2500.0 * std::sin(2 * 3.141592653589793 * 2.7 * f1 * t)) + nz);
    }
    return x;
}

inline int wordK(int kbit) { return (kbit == 12 || kbit == 24) ? 240 : 320; }
inline int wordsPerFrame(int kbit) { return (kbit == 12 || kbit == 16) ? 1 : 2; }

// Codec frames at kbit, padded with silent frames until the words fill whole 4-word blocks.
inline std::vector<uint8_t> frameBits(const std::vector<int16_t>& pcm, int kbit) {
    dect2::KaalCodec3 c(kbit);
    std::vector<uint8_t> b = c.encode(pcm);
    const size_t fb = c.frameBits();
    const std::vector<int16_t> z(640, 0);
    const std::vector<uint8_t> sil = c.encode(z);   // three frames, the middle one is silence
    while ((b.size() / fb * (size_t)wordsPerFrame(kbit)) % 4 != 0) b.insert(b.end(), sil.begin() + fb, sil.begin() + 2 * fb);
    return b;
}

// Baseband at 48 kHz: data part plus the sweep at -10 dB (idCall empty: plain up sweeps). dataPower receives the mean power of the data part.
inline std::vector<cd> transmit(const std::vector<uint8_t>& bits, int kbit, const std::string& idCall, double& dataPower, double sweepDb = kSweepPowerDb) {
    const size_t fb = (size_t)kbit * 20, nfr = bits.size() / fb;
    const int k = wordK(kbit);
    std::vector<std::vector<uint8_t>> words;
    for (size_t f = 0; f < nfr; f++)
        for (size_t o = 0; o < fb; o += (size_t)k) {
            std::vector<uint8_t> w(kRtCodedBits);
            ldpcEncodeRm(&bits[f * fb + o], k, w.data());
            words.push_back(std::move(w));
        }
    std::vector<cf> pre = preamble();
    const cf pil = pilotSymbol();
    std::vector<cd> syms;
    for (size_t blk = 0; blk < words.size() / 4; blk++) {
        std::vector<uint8_t> coded;
        for (int f = 0; f < 4; f++) coded.insert(coded.end(), words[blk * 4 + (size_t)f].begin(), words[blk * 4 + (size_t)f].end());
        coded = interleave(coded);
        for (int f = 0; f < 4; f++) {
            const std::vector<cf> data = qam16Map(&coded[(size_t)f * kRtCodedBits], kRtCodedBits);
            for (const cf& p : pre) syms.emplace_back(p.real(), p.imag());
            for (size_t j = 0; j < data.size(); j += kPilotEvery - 1) {
                const size_t e = std::min(data.size(), j + (size_t)(kPilotEvery - 1));
                for (size_t q = j; q < e; q++) syms.emplace_back(data[q].real(), data[q].imag());
                if (e - j == (size_t)(kPilotEvery - 1)) syms.emplace_back(pil.real(), pil.imag());
            }
        }
    }
    const std::vector<double> taps = rrcTaps();
    std::vector<cd> sig(syms.size() * kSps + taps.size() - 1, cd(0, 0));
    for (size_t m = 0; m < syms.size(); m++)
        for (size_t q = 0; q < taps.size(); q++) sig[m * kSps + q] += syms[m] * taps[q];
    double p = 0;
    for (const cd& v : sig) p += std::norm(v);
    dataPower = p / (double)sig.size();
    std::array<uint8_t, kIdCycle> ids{};
    std::string base;
    int sfx = 0;
    if (!idCall.empty() && splitCallsign(idCall, base, sfx)) ids = sweepIdSymbols(base, sfx);
    const double a = std::sqrt(dataPower * std::pow(10.0, sweepDb / 10.0));
    for (size_t i = 0; i < sig.size(); i++) sig[i] += sweepSampleAt(ids[(i / (size_t)kSwL) % kIdCycle], (int)(i % (size_t)kSwL)) * a;
    return sig;
}

struct Gauss {
    uint64_t st = 0x9E3779B97F4A7C15ULL;
    double uni() { return (double)((xorshift64(st) >> 11) + 1) / 9007199254740994.0; }
    void pair(double& a, double& b) {
        const double r = std::sqrt(-2.0 * std::log(uni())), ph = 2 * 3.141592653589793 * uni();
        a = r * std::cos(ph); b = r * std::sin(ph);
    }
};

// Adds white noise with the given per-sample SNR (data power / noise power per 48 kHz sample), then rotates by cfoHz.
inline std::vector<std::complex<float>> channelSnr(const std::vector<cd>& sig, double dataPower, double snrDb, double cfoHz, uint64_t seed) {
    const double sigma2 = dataPower / std::pow(10.0, snrDb / 10.0);
    Gauss g;
    g.st ^= seed * 0x2545F4914F6CDD1DULL;
    std::vector<std::complex<float>> out(sig.size());
    const double sc = std::sqrt(sigma2 / 2);
    for (size_t i = 0; i < sig.size(); i++) {
        double a, b;
        g.pair(a, b);
        cd v = sig[i] + cd(a * sc, b * sc);
        if (cfoHz != 0) {
            const double ph = 2 * 3.141592653589793 * std::fmod(cfoHz * (double)i / kFs, 1.0);
            v *= cd(std::cos(ph), std::sin(ph));
        }
        out[i] = std::complex<float>((float)v.real(), (float)v.imag());
    }
    return out;
}

// The same for an Eb/N0 of the information bits at the code rate k / 480 (16-QAM, 6 samples per symbol).
inline std::vector<std::complex<float>> channel(const std::vector<cd>& sig, double dataPower, double ebn0Db, double cfoHz, uint64_t seed, int kbit = 12) {
    const double rate = (double)wordK(kbit) / 480.0;
    const double esn0 = ebn0Db + 10 * std::log10(rate) + 10 * std::log10(4.0);
    return channelSnr(sig, dataPower, esn0 - 10 * std::log10((double)kSps), cfoHz, seed);
}
}  // namespace kaalrx
