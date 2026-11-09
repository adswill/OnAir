// Test signals for the aviation, rail and road receivers. These generators exist only in the tests: none of them is reachable from the transmit side of the application.
#pragma once
#include "dect2/common/ring.h"
#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace avgen {
using dect2::cf32;

inline constexpr double kPi = 3.14159265358979323846;

inline void addNoise(std::vector<cf32>& x, double snrDb, double sigPower, unsigned seed) {
    std::mt19937 r(seed);
    std::normal_distribution<float> n(0, (float)std::sqrt(sigPower / std::pow(10.0, snrDb / 10) / 2));
    for (auto& v : x) v += cf32(n(r), n(r));
}

inline void mix(std::vector<cf32>& x, double hz, double fs) {
    double p = 0; const double d = 2 * kPi * hz / fs;
    for (auto& v : x) { v *= cf32((float)std::cos(p), (float)std::sin(p)); p += d; }
}

// Tone-keyed FM (AFSK): bits at rate, tones f0 / f1, peak deviation dev
inline void appendAfskFm(std::vector<cf32>& out, const std::vector<uint8_t>& bits, double bitRate, double fs, double f0, double f1, double dev, double& tonePhase, double& carrierPhase, double clockPpm = 0) {
    const double spb = fs / bitRate * (1 + clockPpm * 1e-6);
    double pos = 0; size_t i = 0;
    while (i < bits.size()) {
        const double f = bits[i] ? f1 : f0;
        tonePhase += 2 * kPi * f / fs;
        const float a = (float)std::sin(tonePhase);
        carrierPhase += 2 * kPi * dev * a / fs;
        out.push_back(cf32((float)std::cos(carrierPhase), (float)std::sin(carrierPhase)));
        if (++pos >= spb) { pos -= spb; i++; }
    }
}

// ---- AAR end of train frame (PyEOT field positions)
struct EotFields { int chaining = 0, batt = 3, type = 0; uint32_t unit = 12345; int psi = 90, charge = 100, spare = 0, valve = 1, conf = 0, turbine = 0, motion = 1, mkrBatt = 0, mkrLight = 1; };
inline std::vector<uint8_t> eotDataBits(const EotFields& f) {
    std::vector<uint8_t> d(45);
    auto lsb = [&](int a, int n, uint32_t v) { for (int i = 0; i < n; i++) d[a + i] = v >> i & 1; };
    lsb(0, 2, (uint32_t)f.chaining);
    d[2] = f.batt & 1; d[3] = f.batt >> 1 & 1;
    d[4] = f.type >> 2 & 1; d[5] = f.type >> 1 & 1; d[6] = f.type & 1;
    lsb(7, 17, f.unit); lsb(24, 7, (uint32_t)f.psi); lsb(31, 7, (uint32_t)f.charge);
    d[38] = f.spare; d[39] = f.valve; d[40] = f.conf; d[41] = f.turbine; d[42] = f.motion; d[43] = f.mkrBatt; d[44] = f.mkrLight;
    return d;
}
inline std::vector<uint8_t> eotFrameBits(const std::vector<uint8_t>& d, int preamble = 69) {
    const char* gen = "1111001101000001111", *key = "101011011101110000";
    std::vector<uint8_t> r(63, 0);
    for (int i = 0; i < 45; i++) r[i] = d[44 - i];
    for (int i = 0; i < 45; i++) if (r[i]) for (int j = 0; j < 19; j++) r[i + j] ^= (uint8_t)(gen[j] - '0');
    std::vector<uint8_t> b;
    for (int i = 0; i < preamble; i++) b.push_back((uint8_t)((i ^ (preamble - 1)) & 1));      // ends in 0 1 0 1 ... so that the sync below continues the pattern
    for (const char* s = "11100010010"; *s; s++) b.push_back((uint8_t)(*s - '0'));
    b.insert(b.end(), d.begin(), d.end());
    for (int i = 0; i < 18; i++) b.push_back((uint8_t)(r[45 + i] ^ (key[i] - '0')));
    return b;
}

} // namespace avgen
