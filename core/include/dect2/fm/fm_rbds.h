// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RBDS (the US flavour of RDS, NRSC-4-B): the programme type names, the call letters hidden in the PI code, and the choice between RDS
// (Europe, the rest of the world) and RBDS. The bit stream, groups and checkwords are identical; only these meanings differ.
#pragma once
#include <string>

namespace dect2 {

enum RdsRegion { kRdsAuto = 0, kRdsEurope = 1, kRdsUs = 2 };

// Programme type name (0 .. 31) in the European RDS table or the US RBDS table
const char* rdsPtyName(int pty, bool rbds);

// US call letters from a PI code: K or W and three letters, for PI 0x1000 .. 0x994F (K = 0x1000 + 26*26*a + 26*b + c for letters
// a, b, c counted from A, W the same from 0x54A8). Empty outside that range (three-letter calls, nationwide and regional codes and
// every non-US PI have no letters here).
std::string rbdsCallSign(int pi);

// The inverse: the PI of a four letter K / W call sign (case does not matter), 0 when the text is not one.
int rbdsPiFromCallSign(const std::string& call);

// Auto: RBDS when the tuned frequency is on the US raster (odd tenths: 87.9, 88.1 ... 107.9 MHz), else RDS. Europe also uses odd tenths, so a
// European station there needs the setting on RDS (Europe); the PI is not used to decide, since its first digit is a country code in Europe
// and the ranges overlap.
bool rdsUseRbds(int region, double tunedMhz);

} // namespace dect2
