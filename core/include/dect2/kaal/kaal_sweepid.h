// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL station ID carried in the sweep signature (port of kaal_phy_id.py, see scratchpad KAAL-V3-NOTES.md).
// The sweep period is kSwL = 24240 samples at 48 kHz (0.505 s). Each period carries one of four chirp variants = 2 bits:
//   0 up    full-period up chirp -5000 .. +5000 Hz, unit amplitude
//   1 down  full-period down chirp
//   2 fast1 twice the slope in the first half of the period, silent in the second, amplitude sqrt(2)
//   3 fast2 the same in the second half, silent in the first
// All four have equal energy; the phase restarts at 0 at every period start. An ID cycle is 24 periods (12.12 s): the sync pair
// (fast1, fast2) and 22 dibits = 36 payload bits (a 6-character callsign in base 40 = 32 bits, a 4-bit suffix class) + CRC-8, MSB first.
// A carrier offset f moves the correlation peak of the variants by u * d samples with d = -f * kSwShiftPerHz and u = (1, -1, 1/2, 1/2).
#pragma once
#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace kaal {

constexpr int kSwL = 24240;                         // samples per sweep period at 48 kHz = 0.505 s (the legacy sweep had 24000)
constexpr double kSwPeriodS = kSwL / 48000.0;
constexpr double kSwRate = 10000.0 / kSwPeriodS;    // Hz/s of the slow chirps
constexpr double kSwShiftPerHz = 48000.0 / kSwRate; // samples of peak shift per Hz of carrier offset (2.424)
constexpr int kIdSymbols = 22, kIdCycle = 24;
constexpr double kIdGateMinDb = 5.0;                // sweep-present gate of the ID decoder, see KAAL-V3-NOTES.md for the calibration
enum SweepVariant { kSwUp = 0, kSwDown = 1, kSwFast1 = 2, kSwFast2 = 3 };
constexpr double kSwU[4] = {1.0, -1.0, 0.5, 0.5};   // peak shift factor per variant

extern const char* const kIdAlphabet;               // 40 characters, code 0 = space
extern const char* const kIdSuffix[16];             // "", "/P", "/M", "/MM", "/A", "/QRP", "/B", "/AM", "/R", "/S", "/T", "/X", "/1" .. "/4"

uint8_t crc8(const uint8_t* data, size_t n);        // poly 0x07, init 0xFF, MSB first
// base: up to 6 characters of the alphabet (shorter ones are padded with spaces), suffix 0 .. 15. 44 bits, one per byte, false when unusable.
bool packSweepId(const std::string& base, int suffix, uint8_t bits[44]);
// CRC and range check; base without trailing spaces. False for a bad CRC, a code >= 40^6 or an all-blank callsign.
bool unpackSweepId(const uint8_t bits[44], std::string& base, int& suffix);
// 24 variant numbers of one ID cycle: sync, sync, 22 dibits.
std::array<uint8_t, kIdCycle> sweepIdSymbols(const std::string& base, int suffix);
// "LY2ABC/P" -> base LY2ABC, suffix 1. Lower case is raised. False when the callsign cannot be sent (a base of more than 6 characters,
// a character outside the alphabet, an unknown suffix).
bool splitCallsign(const std::string& callsign, std::string& base, int& suffix);
std::string joinCallsign(const std::string& base, int suffix);

// The variant at time tt seconds since its period start, analytic and UNCLIPPED in time for up/down (the chirp formula continues beyond
// [0, P)); for fast1/fast2 it is the formula of the chirp with u counted from the start of the half, without a window. support(): the
// part of the period [0, kSwL) in which the variant is not zero.
std::complex<double> sweepVariantFormula(int v, double tt);
void sweepVariantSupport(int v, int& from, int& to);   // sample range inside the period
const std::vector<std::complex<float>>& sweepVariantTemplate(int v);   // kSwL samples, zero outside the support
// Value of the transmitted sweep at sample m of a period (0 .. kSwL-1), zero outside the support.
std::complex<double> sweepSampleAt(int v, int m);

struct SweepIdResult {
    bool valid = false;
    std::string base;      // callsign without the suffix
    int suffix = 0;
    std::string callsign;  // joined
    float gateDb = 0;      // mean peak-to-floor of the sweep detection over the last cycle
    int confirmations = 0;
};

// Accumulates the per-period variant energies of a receiver and decodes the ID cycle. Periods are numbered by a running counter k.
class SweepIdDecoder {
public:
    SweepIdDecoder();
    void reset();
    // s: correlation energies of the four variants in period k; gateDb: winner energy over the mean of the three others, in dB.
    void addPeriod(uint64_t k, const double s[4], double gateDb);
    // True when a new valid and confirmed ID is available (once per change); result() holds it.
    bool poll();
    const SweepIdResult& result() const { return res_; }
    uint64_t periods() const { return n_; }
    double gateMean() const { return n_ ? gateSum_ / (double)std::min<uint64_t>(n_, kIdCycle) : 0.0; }   // mean peak-to-others over the last cycle, dB
    void setGateMin(double db) { gateMin_ = db; }   // an ID is only accepted when gateMean() >= this

private:
    void tryDecode();
    double acc_[kIdCycle][4];
    int visits_[kIdCycle];
    double gateSum_ = 0;
    std::array<float, kIdCycle> gateRing_;
    uint64_t n_ = 0, lastK_ = 0, candK_ = 0;
    SweepIdResult res_, cand_;
    double gateMin_ = kIdGateMinDb;
    bool candValid_ = false, fresh_ = false;
    int candCount_ = 0;
};

}  // namespace kaal
}  // namespace dect2
