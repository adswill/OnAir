// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL voice codec 4 (kaalvoice): internal building blocks shared by the encoder, the decoder and the evaluator.
// Everything on the bit-stream path is deterministic: only + - * / and sqrt of IEEE doubles/floats (the files are built with
// -ffp-contract=off), and the transcendental functions below are our own series, so every platform gives the same bits and samples.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace kaalvoice {
namespace detail {

constexpr double kPi = 3.14159265358979323846;

double kvSin(double x);
double kvCos(double x);
double kvLog2(double x);   // x > 0
double kvExp2(double x);
inline double kvLog10(double x) { return kvLog2(x) * 0.30102999566398119521; }
inline double kvPow10(double x) { return kvExp2(x * 3.32192809488736234787); }
inline double kvExp(double x) { return kvExp2(x * 1.44269504088896340736); }
inline double kvDb(double p) { return 10.0 * kvLog10(p > 1e-10 ? p : 1e-10); }   // power -> dB
inline double kvFromDb(double db) { return kvPow10(db * 0.1); }                  // dB -> power

// Linear congruential generator: the noise sources of the decoder (concealment, comfort noise, high band) are reproducible.
struct Rng {
    uint32_t s = 0x4b41414cu;
    uint32_t next() { s = s * 1664525u + 1013904223u; return s; }
    float uni() { return (float)(int32_t)next() * (1.0f / 2147483648.0f); }               // [-1, 1)
    float gauss() { return (uni() + uni() + uni() + uni()) * 0.8660254f; }                // about unit variance
};

// ---- LPC (kv_lpc.cpp) ----
constexpr int kP = 10;           // LPC order of the 8 kHz core
void autocorr(const float* x, const float* win, int n, int p, double* r);
void lagWindow(double* r, int p);                     // 60 Hz Gaussian lag window + -40 dB white-noise correction
bool levinson(const double* r, int p, float* a);      // a[0] = 1; false when unstable (a then holds a usable copy of the last stable order)
bool lpcToLsf(const float* a, double* lsf);           // order kP; lsf in radians, ascending; false when the roots were not all found
void lsfToLpc(const double* lsf, float* a);           // order kP
void weightLpc(const float* a, double g, float* out); // out[i] = a[i] * g^i
void lsfStabilise(double* lsf, double minGap);        // ascending, inside (minGap, pi - minGap), at least minGap apart
// y = A(z) x (FIR, a[0] = 1). mem[i] = x[-1 - i]
void firA(const float* a, const float* x, float* y, int n, float* mem);
// y = x / A(z) (all-pole). mem[i] = y[-1 - i]
void iirA(const float* a, const float* x, float* y, int n, float* mem);

// ---- pyramid VQ enumeration (kv_pvq.cpp): integer vectors of length n with sum |y| = k ----
uint64_t pvqCount(int n, int k);   // saturates at UINT64_MAX
int pvqBits(int n, int k);         // bits of an index (ceil log2 count), 99 when it does not fit 63 bits
uint64_t pvqIndex(const int* y, int n, int k);
void pvqVector(uint64_t idx, int n, int k, int* y);   // idx is reduced modulo the count (bit errors stay inside the code)
int pvqMaxK(int n, int bits);      // the largest k whose index fits `bits`
// Greedy + refine search of k unit pulses maximising (d.y)^2 / (y' Phi y), signs follow d. phi is n x n row-major.
void pvqSearch(const double* d, const double* phi, int n, int k, int* y);

// ---- small FFT for the noise suppressor and the evaluator (kv_math.cpp) ----
void fft(float* re, float* im, int n, bool inverse);   // n power of two, in place, unscaled

// ---- band split 16 kHz <-> 8 kHz (kv_split.cpp) ----
constexpr int kSplitTaps = 95;                         // linear phase, delay 47 samples at 16 kHz for each direction
const float* splitTaps();
struct Down2 {                                         // 16 kHz -> 8 kHz low band
    float hist[kSplitTaps + 1] = {};
    void run(const float* x, int n16, float* y8);      // n16 even
};
struct Up2 {                                           // 8 kHz -> 16 kHz (gain 2 so the pass band is unity)
    float hist[kSplitTaps / 2 + 2] = {};
    void run(const float* x8, int n8, float* y16);
};

}  // namespace detail
}  // namespace kaalvoice
}  // namespace dect2
