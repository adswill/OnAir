// GOES / Meteosat DCS platform message format (public NESDIS / JMA / EUMETSAT descriptions of the DCP radio set standards).
// On the air, in order: an unmodulated carrier, a bit preamble of alternating 1 and 0 (250 bits at 100 bps, 450 at 300 and 1200 bps), the 15-bit
// frame sync (a maximal length sequence), the platform address (31 bits: a BCH (31,21) codeword), for the 300 / 1200 bps format a spare bit and an
// 8-bit flag word, the data as 8-bit characters (7 bits LSB first and an odd parity bit last: ASCII or "pseudo-binary B", six data bits per
// character), and the end-of-transmission code 0x04. The carrier is phase modulated +-60 degrees, the bits NRZ-L or Manchester coded.
// The 100 bps parts and the BCH polynomial are checked against the JMA DCP annex example; the 300 / 1200 bps extension (spare bit, flag word) is from
// the NESDIS summary slides and has not been tried on a real platform.
#pragma once
#include "dect2/goesdcs/goesdcs_tel.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace dcs {

constexpr int kSyncBits = 15;
extern const uint8_t kSync[kSyncBits];            // b1..b15 as sent
constexpr uint8_t kEot = 0x04;                    // the end-of-transmission character (bits 0 0 1 0 0 0 0 0, LSB first)
constexpr double kChannel1Hz = 401701750.0;       // centre of satellite channel 1 (GOES-R DCS 300 bps grid)
constexpr double kChannelStepHz = 1500.0;
constexpr int kPreamble100 = 250, kPreamble300 = 450;

uint32_t bchCheck(uint32_t data21);               // the 10 check bits of a 21-bit address value
uint32_t addressWord(uint32_t data21);            // the 31-bit codeword, b1 as bit 30
bool addressDecode(uint32_t word31, uint32_t& corrected, int& nerr);   // up to 2 bit errors corrected
std::string addressHex(uint32_t word31);          // 8 hex digits as the operators write them: the 31 bits left-aligned in 32 (b1 first)

int channelOf(double absHz);                      // nearest satellite channel, 0 outside 401.7 - 402.1 MHz
double channelHz(int channel);

// The channel plans of the operators that use this platform format (the CGMS international DCP standard):
//   Goes      NESDIS: 1.5 kHz grid from channel 1 at 401.70175 MHz (266 channels to 402.1 MHz)
//   Meteosat  EUMETSAT TD 16 table 11: channel n (1 - 267) at 402.0340 + 0.0015 n MHz, 268 - 289 at 402.0025 + 0.0015 (n - 268) MHz
//             (international channels 402.0355 - 402.0655 MHz, regional 402.0685 - 402.4345 MHz)
//   Himawari  JMA: 402.0685 - 402.4 MHz (100 bps), 402.1 - 402.4 MHz (300 bps); JMA publishes no channel numbers: frequency only (channel 0)
enum class Plan { Goes = 0, Meteosat = 1, Himawari = 2 };
int channelOf(double absHz, Plan plan);           // 0: outside the plan, or a plan without numbers
double channelHz(int channel, Plan plan);         // 0 for an unknown channel
const char* planName(Plan plan);                  // "GOES", "Meteosat", "Himawari"
Plan planForCentre(double centreHz);              // Auto: GOES below 402.0 MHz, Meteosat from there up (the international channels are shared)

uint8_t withParity(uint8_t c7);                   // 7 bits plus odd parity in bit 7
bool parityOk(uint8_t c);

// Text and sensor fields of a received report (parity bits removed): decides ASCII or pseudo-binary B and splits the sensor data
void interpret(DcsMessage& m);

// The bits of a whole report as sent after the preamble: sync, address, extension, data, EOT. `ext`: true adds the spare bit and the flag word.
std::vector<uint8_t> frameBits(uint32_t addressData21, bool ext, uint8_t flags, const std::vector<uint8_t>& data7);
// The same, bytes already holding their parity
std::vector<uint8_t> charBits(const std::vector<uint8_t>& bytes);

// The report body of the received bits after the address: picks the offset of the data (0, or 9 for the extended format) by the parity of the characters,
// reads characters up to the EOT code. Returns false when no character was found.
bool parseBody(const std::vector<uint8_t>& bitsAfterAddress, bool preferExt, DcsMessage& m);

// "Pseudo-binary B": 6 bits per character, the character is 0x40 | value
std::vector<uint8_t> pseudoBinaryB(const std::vector<int>& values6);

} // namespace dcs
} // namespace dect2
