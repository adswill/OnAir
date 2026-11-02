// LTR word format and test-signal generator, clean-room from public descriptions:
//  - Word: 40 bits, MSB first: 9-bit sync 0x158 (a "10" start and a 7-bit Barker code), then 24 information bits
//    AREA 1, GO-TO 5, HOME 5, ID 8, FREE 5, then 7 check bits (US 6,529,486 B1 describes this word for an LTR-compatible system).
//  - 300 bit/s subaudible NRZ below 150 Hz (same patent's background section).
//  - Talkgroup text A-HH-GGG, special ids 240-247 tokens, 253 busy, 254 callsign ID, 255 idle (RadioReference wiki).
//  - Check bits: 7-bit parity code, the XOR of one fixed 7-bit value per set information bit (a CRC-7 with polynomial 0xFD, initial fill 0).
//    The table and the inbound (bit-flipped word, optionally inverted check) behaviour are from DSheirer/sdrtrunk (GPL-3, CRCLTR.java,
//    Copyright Dennis Sheirer); credit in THIRD_PARTY.md. A word is accepted only when its check bits match.
// The meaning of the GO-TO / FREE codes beyond the 1 - 20 repeater numbers is not needed. Polarity of the FM data is not known: both are accepted.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::ltr {

constexpr double kBaud = 300.0;
constexpr uint32_t kSync = 0x158;   // 9 bits
constexpr int kWordBits = 40;

struct Word {
    int area = 0, gotoCh = 0, home = 1, id = 255, freeCh = 31, chk = 0;
};

std::vector<uint8_t> encodeWord(const Word& w);                       // 40 bits, MSB first; chk is written as given
bool parseWord(const uint8_t* bits40, Word& w);                        // false when the sync is not 0x158
bool plausible(const Word& w);                                         // home 1-20, go-to 0-20 or 29-31, free 0-20 or 31
int checksum(const Word& w);                                           // the 7 check bits for the 24 information bits
bool checkOk(const Word& w, bool inverted);                            // chk == checksum, or (inverted words only) chk == ~checksum
std::string talkgroup(const Word& w);                                  // "A-HH-GGG"
std::string kindOf(int id);                                            // "idle", "callsign ID", "busy", "token" or ""

// Test signal: FM at fs with the subaudible data (bit 1 = higher frequency) of the bits, optionally with a voice-band tone and noise.
// devHz: deviation of the data; voiceHz/voiceDevHz: a tone riding on top (0 = none); the carrier sits offsetHz from 0 Hz.
std::vector<cf32> modulate(const std::vector<uint8_t>& bits, double fs, double offsetHz, double devHz, float amp, double voiceHz = 0, double voiceDevHz = 0, bool invert = false);

} // namespace dect2::ltr
