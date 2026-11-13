// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CW language model: a small character trigram and a word list of amateur-radio traffic (callsigns, reports, Q-codes, abbreviations, names,
// prosigns), written for this project and built in code at the first use. The adaptive decoder (cw_decoder.h) adds its scores to the timing
// likelihood so that an ambiguous gap or a doubtful dash resolves to the likelier text.
//
// Symbols: 0-25 A-Z, 26-35 0-9, 36 space, 37 other punctuation, 38 <AR>, 39 <SK>, 40 <BT>, 41 <KN>, 42 other prosigns.
#pragma once
#include <cstdint>

namespace dect2 {
namespace cw {

constexpr int kLmSyms = 43;
constexpr int kLmSpace = 36;

// the symbol of a Morse token ("A", "5", "<AR>", "?")
int lmSymbol(const char* token);
// natural log of P(c | c2 c1)
float lmLogP(int c2, int c1, int c);
// log bonus of a finished word (symbols, no space): in the word list, or shaped like a callsign, or a number
float lmWordBonus(const uint8_t* syms, int n);

} // namespace cw
} // namespace dect2
