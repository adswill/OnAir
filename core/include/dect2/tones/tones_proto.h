// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tone signalling tables and codes: CTCSS, DCS, DTMF, the five-tone sequential standards, SELCAL. Written from the public descriptions of
// each system (EIA / TIA-603 tone lists, the Golay (23,12) DCS word, ITU / ZVEI / EEA / EIA tone plans, the ARINC 714 SELCAL plan); no code
// of other projects is used.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace tones {

// ---- CTCSS: 50 sub-audible tones, ascending. The first 38 of the EIA / TIA-603 list are "standard"; 69.3 Hz and the ones above 150 Hz that
// are not in it are "extended".
constexpr int kCtcssN = 50;
double ctcssHz(int idx);                  // idx 0 .. 49
bool ctcssExtended(int idx);
int ctcssIndex(double hz);                // nearest tone, -1 when none is within 1.2 Hz

// ---- DCS (Digital Coded Squelch): 134.4 bit/s, a 23 bit Golay (23,12) word repeated without a gap. Bits are sent least significant first:
// 9 code bits (three octal digits), the marker bits 1,0,0 (written 100 with the first-sent bit last), then the 11 parity bits.
constexpr double kDcsBaud = 134.4;
uint32_t dcsWord(int code);               // code as written: 23 means octal 023; bit i of the result is the i-th bit on the air
bool dcsValid(uint32_t w23, int& code);   // the word as received (normal polarity): parity and marker right
bool dcsStandard(int code);               // one of the 83 codes of the usual list
struct DcsHit {
    int code = 0;                         // as written (23 for 023)
    bool inverted = false;                // sent with inverted polarity (the "I" codes)
    bool standard = false;
    int aliasCode = -1;                   // the same waveform also reads as this code ...
    bool aliasInverted = false;           // ... with this polarity
};
// All 23 rotations (the word repeats; where it starts is not known) and both polarities of a received word; the best reading, the normal polarity first
bool dcsDecode(uint32_t w23, DcsHit& out);
std::string dcsName(int code, bool inverted);   // "D023N"

// ---- DTMF
constexpr int kDtmfRows[4] = {697, 770, 852, 941};
constexpr int kDtmfCols[4] = {1209, 1336, 1477, 1633};
constexpr char kDtmfKeys[16] = {'1', '2', '3', 'A', '4', '5', '6', 'B', '7', '8', '9', 'C', '*', '0', '#', 'D'};

// ---- five-tone sequential calling: digits 0 .. 9 and the repeat tone "E"
struct SeqSet {
    const char* id;
    const char* name;
    double digit[10];
    double rep;
    double toneMs;                        // the usual tone length (a hint: lengths are vendor specific)
};
extern const SeqSet kSeqSets[];
extern const int kSeqSetN;
// every distinct tone of all sets, ascending
const std::vector<double>& seqPool();

// ---- SELCAL (aviation): 16 tones, two pairs of two tones, each pair 1 s, 0.2 s apart
constexpr int kSelcalN = 16;
double selcalHz(int idx);                 // idx 0 .. 15 for the letters A B C D E F G H J K L M P Q R S
char selcalLetter(int idx);

} // namespace tones
} // namespace dect2
