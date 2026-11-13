// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Spreading codes of the satellite navigation signals.
// GPS L1 C/A: IS-GPS-200 section 3.3.2.3 (two 10 stage registers, the G2 output taken from two stages per satellite, Table 3-I).
// SBAS L1 (PRN 120-158, RTCA DO-229 Appendix A) and QZSS L1 C/A (PRN 193-202, IS-QZSS-PNT): the same Gold code family, each PRN given by its G2 delay
// in chips (the delays and the first ten chips of every PRN as listed in the L1 C/A PRN code assignment table of gps.gov, which collects those ICDs).
// Galileo E1-B: the memory codes of the Galileo OS SIS ICD Annex C (4092 chips, one code period lasts 4 ms).
// BeiDou B1I: BDS SIS ICD section 4.3 (two 11 stage registers G1 = 1+X+X^7+X^8+X^9+X^10+X^11, G2 = 1+X+X^2+X^3+X^4+X^5+X^8+X^9+X^11, both starting
// at 01010101010; the G2 output taken from two stages per satellite, Table 4-2; the 2047 chip Gold code shortened by its last chip to 2046).
#pragma once
#include <cstdint>

namespace dect2 {

constexpr int kGpsCaLen = 1023;
constexpr double kGpsCaChipRate = 1.023e6;
constexpr double kGpsL1Hz = 1575.42e6;

// The 1023 chips of the C/A code of PRN 1..32 as 0/1 (logic 0 is +1 on the carrier). Returns false for another PRN.
bool gpsCaChips(int prn, uint8_t* chips);
// The first ten chips as the octal number of IS-GPS-200 Table 3-I (the first chip is the leading 1 digit: PRN 1 = 01440 octal)
unsigned gpsCaFirst10(int prn);

// Any L1 C/A code: GPS 1..32, SBAS 120..158, QZSS 193..202. Returns false for another PRN.
bool l1caChips(int prn, uint8_t* chips);
unsigned l1caFirst10(int prn);
int l1caG2Delay(int prn);          // the G2 delay in chips, -1 for a PRN without one (the GPS PRNs are made by their tap pairs)
// The system of an L1 C/A PRN: GnssGps, GnssSbas or GnssQzss (gnss_tel.h), -1 for none
int l1caSystem(int prn);

constexpr int kGalE1Len = 4092;
constexpr double kGalE1ChipRate = 1.023e6;
// The 4092 chips of the E1-B primary code of Galileo satellite 1..50 as 0/1 (logic 0 is +1). Returns false for another number.
bool galE1bChips(int prn, uint8_t* chips);

// BeiDou B1I on 1561.098 MHz: 2046 chips at 2.046 Mcps (1 ms). The tap pairs of the ranging codes 1..37 are those of Table 4-2 of the ICD.
constexpr int kBdsLen = 2046;
constexpr double kBdsChipRate = 2.046e6;
constexpr double kBdsB1Hz = 1561.098e6;
constexpr int kBdsMaxPrn = 63;
bool bdsB1iChips(int prn, uint8_t* chips);
// GEO satellites (D2 navigation message at 500 bps, no secondary code): ranging codes 1..5 and 59..63
inline bool bdsIsGeo(int prn) { return (prn >= 1 && prn <= 5) || (prn >= 59 && prn <= 63); }

// GLONASS L1OF (ICD L1/L2 edition 5.1, 3.3.2.1): one 511 chip m-sequence for every satellite, 1 + X^5 + X^9, all ones at the start, the output
// taken from stage 7; 0.511 Mcps (1 ms). The carriers are 1602 MHz + k x 0.5625 MHz, k = -7 .. +6 (3.3.1.1).
constexpr int kGloLen = 511;
constexpr double kGloChipRate = 0.511e6;
constexpr double kGloL1Hz = 1602e6, kGloStepHz = 0.5625e6;
constexpr int kGloMinK = -7, kGloMaxK = 6, kGloSlots = 14;
inline double gloL1Hz(int k) { return kGloL1Hz + k * kGloStepHz; }
void gloCaChips(uint8_t* chips);

} // namespace dect2
