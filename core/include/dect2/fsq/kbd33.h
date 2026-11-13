// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The 33-tone keyboard modes of the fldigi family: FSQ (Fast Simple QSO) and IFKP (Incremental Frequency Keying Plus).
//
// Both send one of 33 tones per symbol, the tones 3 FFT bins of a 4096-point spectrum apart (FSQ at 12 kHz: 8.789 Hz, IFKP at 16 kHz: 11.719 Hz).
// The tone is not the data: the data is the step from the previous tone, tone = (previous + symbol + 1) mod 33 with symbol 0 .. 31, so a
// receiver that is a few Hz off, or has no idea where the band starts, still reads the steps. A character is one or two symbols (a
// base-29 / base-32 varicode: the 29 commonest take one symbol, the rest take a symbol and a marker 29 / 30 / 31).
//   FSQ   speeds named 1.5 / 2 / 3 / 4.5 / 6 (symbols of 8192 / 6144 / 4096 / 3072 / 2048 samples at 12 kHz: 1.46 .. 5.86 a second), band 290 Hz
//   IFKP  speeds 0.5 / 1.0 / 2.0 (symbols of 8192 / 4096 / 2048 samples at 16 kHz: 1.95 / 3.9 / 7.8 baud), band 387 Hz
// Parameters, the varicode and the CRC are those of fldigi's FSQ and IFKP modems (W1HKJ, Dave Freese and contributors, GPL-3.0-or-later),
// see THIRD_PARTY.md. The receiver and the generator are new code.
//
// Kbd33Rx is the signal part: complex 8 kHz audio (the upper sideband, dial at 0 Hz) in, characters out. It finds the band itself,
// the tone grid, the speed and the symbol timing, and follows the tuning and the symbol clock.
#pragma once
#include "dect2/common/ring.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace kbd33 {

constexpr int kTones = 33;
constexpr int kFsqSpeeds = 5, kIfkpSpeeds = 3;
constexpr double kFsqRate = 12000, kIfkpRate = 16000;   // the modems' own sample rates (they fix the tone spacing)

struct Spec {
    bool ifkp = false;
    int speed = 0;
    double spacingHz = 0;     // between tones
    double symSec = 0;        // one symbol
    double baud() const { return 1.0 / symSec; }
    double bandHz() const { return kTones * spacingHz; }
};
Spec fsqSpec(int speed);      // 0 .. 4: 1.5, 2, 3, 4.5, 6 baud
Spec ifkpSpec(int speed);     // 0 .. 2: 0.5, 1.0, 2.0
inline Spec specOf(bool ifkp, int speed) { return ifkp ? ifkpSpec(speed) : fsqSpec(speed); }
inline int speedCount(bool ifkp) { return ifkp ? kIfkpSpeeds : kFsqSpeeds; }
const char* speedName(bool ifkp, int speed);   // "3", "6" ... (baud as the programs name them)

// ---- the varicode: a character is one or two symbols. encode() gives the symbols (s2 = -1 when one), false when the character has no code.
bool encode(bool ifkp, int ch, int& s1, int& s2);
int idleSymbol(bool ifkp, int& s2);   // the idle character: FSQ 28 then 30, IFKP 0 (s2 = -1)

// Symbols in, characters out. push() returns the character completed by this symbol, 0 for none (idle / NUL are not returned), -1 for a symbol that cannot be
// (the decoder starts over).
class CharDecoder {
public:
    explicit CharDecoder(bool ifkp) : ifkp_(ifkp) {}
    void reset() { have_ = false; }
    int push(int sym);
    int flush();                // the character waiting for a possible second symbol
private:
    bool ifkp_, have_ = false;
    int prev_ = 0;
    int single(int s) const;
    int pair(int a, int b) const;
};

// The 2-hex-digit CRC (CRC-8, polynomial 0x07) that FSQ puts after a callsign: "ly2abc" -> two lower case hex digits
std::string crc8hex(const std::string& s);

// The tone sequence of a text. The first tone sent is `startTone`; returns the tone (0..32) of each symbol. idleTail idle symbols follow.
std::vector<int> toneSequence(bool ifkp, const std::string& text, int startTone = 0, int idleTail = 2);

// ---- receiver (8 kHz complex audio, dial at 0 Hz, upper sideband)
class Rx {
public:
    static constexpr double kFs = 8000;
    static constexpr double kLoHz = 100, kHiHz = 3900;     // the passband searched
    static constexpr int kPos = 73;                         // tone positions of the grid that is watched (the 33 of the band, with room each side)
    explicit Rx(bool ifkp);
    ~Rx();
    void reset();
    void setSpeed(int speed);                  // -1 automatic
    void feed(const cf32* x, size_t n);        // 8 kHz
    std::function<void(int ch, double timeSec)> onChar;   // a character (never 0)

    bool locked() const;
    int speed() const;                         // of the lock, -1 when searching
    double timeSec() const;
    double snrDb() const;                      // symbol energy against the noise per tone, scaled to the 33-tone band
    double afcHz() const;                      // how far the grid moved from where it was found
    double quality() const;                    // share of the energy in the strongest tone, smoothed (about 0.07 for noise, 0.5 and more for a clean signal)
    double gridHz() const;                     // frequency of grid position 0
    double spacingHz() const;
    const std::vector<float>& toneUse() const; // kPos entries: how often each position was the strongest tone, 0 .. 1
    void occupied(double& loHz, double& hiHz) const;   // the tones heard so far
    bool takeSpectrum(std::vector<float>& dbPerBin, float& floorDb);   // new waterfall line, kLoHz .. kHiHz; false when none
    double spectrumLoHz() const;
    uint64_t symbols() const;
    uint64_t droppedSymbols() const;           // too weak to trust, left out
    int syncs() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace kbd33
} // namespace dect2
