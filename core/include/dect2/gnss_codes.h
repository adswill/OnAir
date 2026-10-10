// Spreading codes of the satellite navigation signals.
// GPS L1 C/A: IS-GPS-200 section 3.3.2.3 (two 10 stage registers, the G2 output taken from two stages per satellite, Table 3-I).
// SBAS L1 (PRN 120-158, RTCA DO-229 Appendix A) and QZSS L1 C/A (PRN 193-202, IS-QZSS-PNT): the same Gold code family, each PRN given by its G2 delay
// in chips (the delays and the first ten chips of every PRN as listed in the L1 C/A PRN code assignment table of gps.gov, which collects those ICDs).
// Galileo E1-B: the memory codes of the Galileo OS SIS ICD Annex C (4092 chips, one code period lasts 4 ms).
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

} // namespace dect2
