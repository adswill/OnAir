// Spreading codes of the satellite navigation signals.
// GPS L1 C/A: IS-GPS-200 section 3.3.2.3 (two 10 stage registers, the G2 output taken from two stages per satellite, Table 3-I).
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

} // namespace dect2
