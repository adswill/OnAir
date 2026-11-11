// Test signals for the second block of 4.0 signal modes (families 886 and up). Generators exist only in the tests; none is reachable from the transmit side.
#pragma once
#include "x40_gen.h"
#include <algorithm>
#include <cstdint>
#include <string>

namespace x40gen {

// noise power per sample that gives a carrier of amplitude 1 the stated carrier to noise ratio in a bandwidth bwHz (complex input rate fs)
inline double noiseFor(double cnrDb, double bwHz, double fs) { return std::pow(10.0, -cnrDb / 10) * fs / bwHz; }

// ---- EM4100 -------------------------------------------------------------------------------------------------------------------------------
// 64-bit frame of a 40-bit payload (8-bit version + 32-bit ID): 9 ones, ten rows of 4 bits + even parity, four column parities, stop 0
inline std::vector<int> em4100Bits(uint64_t d40) {
    std::vector<int> b(9, 1); int col[4] = {0, 0, 0, 0};
    for (int r = 0; r < 10; r++) {
        int p = 0;
        for (int c = 0; c < 4; c++) { const int bit = (int)((d40 >> (39 - (4 * r + c))) & 1); b.push_back(bit); p ^= bit; col[c] ^= bit; }
        b.push_back(p);
    }
    for (int c = 0; c < 4; c++) b.push_back(col[c]);
    b.push_back(0);
    return b;
}
struct EmTag { uint64_t id = 0x0123456789ull; double depth = 0.08, rfDiv = 64, carrierHz = 125000, clockPpm = 0, start = 0.05; bool invert = false; };
// Complex baseband at the carrier: amplitude 1 carrier loaded by depth while the Manchester half-bit is high. The edges are smoothed (one pole, 20 kHz).
inline std::vector<cf32> emTag(const EmTag& t, double fs, double seconds) {
    std::vector<cf32> x((size_t)(fs * seconds));
    const auto bits = em4100Bits(t.id);
    const double half = t.rfDiv / t.carrierHz / 2 * (1 + t.clockPpm * 1e-6);
    float y = 0; const float a = (float)(1 - std::exp(-2 * kPi * 20000 / fs));
    for (size_t i = 0; i < x.size(); i++) {
        const double tt = i / fs - t.start;
        int lvl = 0;
        if (tt >= 0) {
            const long hidx = (long)(tt / half); const long bi = (hidx / 2) % 64; const int ph = (int)(hidx % 2);
            lvl = ph == 0 ? bits[bi] : !bits[bi];
            if (t.invert) lvl = !lvl;
        }
        y += a * ((float)lvl - y);
        x[i] = cf32((float)(1 - t.depth * y), 0);
    }
    return x;
}

}
