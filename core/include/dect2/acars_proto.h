// ACARS (ARINC 618) block layer: check sequence, parity, repair, field parsing, and the same in reverse for the test signal.
// Facts from acarsdec (acars.c, syndrom.h: CRC and repair), libacars (acars.c: field layout, block id rule) and ARINC 618 as the
// decoders implement it. Nothing here touches a radio signal.
#pragma once
#include "acars_tel.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

constexpr uint8_t kAcarsSyn = 0x16, kAcarsSoh = 0x01, kAcarsStx = 0x02, kAcarsDel = 0x7f;
constexpr uint8_t kAcarsEtx = 0x83, kAcarsEtb = 0x97;      // as sent: 0x03 and 0x17 with their odd-parity bit

// Block check sequence: CRC-16 of the "KERMIT" kind (reflected 0x1021, i.e. 0x8408, start 0, no final xor), computed over every byte from
// the mode character to the suffix, then over the two check bytes, sent low byte first: the result is 0 for a good block.
uint16_t acarsCrcUpdate(uint16_t crc, uint8_t byte);
uint16_t acarsCrc(const uint8_t* p, size_t n, uint16_t crc = 0);

inline bool acarsParityOk(uint8_t c) { return (__builtin_popcount(c) & 1) != 0; }               // odd parity over all 8 bits
inline uint8_t acarsWithParity(uint8_t c7) { c7 &= 0x7f; return acarsParityOk(c7) ? c7 : (uint8_t)(c7 | 0x80); }

// Syndrome of a single flipped bit: the CRC residue it leaves. posFromEnd 0 is the last check byte, 1 the first, 2 the last text byte...
uint16_t acarsSyndrome(int posFromEnd, int bit);

// Parity and check-sequence repair as acarsdec does it. txt: the bytes after SOH up to and including the suffix (with their parity bits,
// as received); crc: the two check bytes. Up to maxParityErrors bytes with wrong parity are allowed: each gets one bit flipped so that
// the check comes out (or leaves an error that only the check bytes carry). With no parity error, an error of two bits in one byte
// is tried. Returns true when the block is good (txt is repaired in place); fixedBits counts the flips; parityErrors what was seen.
bool acarsRepairBlock(uint8_t* txt, int len, const uint8_t crc[2], int& fixedBits, int& parityErrors, int maxParityErrors = 3);

// Fields of a good block. txt7: bytes after SOH up to and including the suffix, parity bits removed. Sets everything but the
// receiver's own fields (serial, times, frequency, level, crcOk, parityFixed). Returns false when the layout is not ACARS.
bool acarsParseBlock(const uint8_t* txt7, int len, AcarsMessage& m);

// Meaning of a label (two characters, the DEL of "_<DEL>" written as 'd'), "" when the label is not in the list.
const char* acarsLabelText(char a, char b);

// Short reading of fixed-format text (OOOI times and airports of the Q labels); "" when the label or the text does not fit.
std::string acarsDecodeText(const std::string& label, const std::string& text);

// What to send
struct AcarsBlockSpec {
    char mode = '2';
    std::string reg;             // up to 7 characters, padded on the left with '.'
    uint8_t ack = 0x15;          // NAK (0x15): nothing to acknowledge; ACK is 0x06; otherwise the block id being acknowledged
    char label[2] = {'H', '1'};  // label "_d" is sent as '_' then DEL
    char blockId = '0';
    bool hasText = true;
    std::string text;            // up to 220 characters (ASCII, no parity yet)
    bool lastBlock = true;       // ETX, or ETB when false
};

// The bytes of one transmission, ready to be sent bit by bit (least significant bit first): 16 pre-key bytes of 0xFF (no parity),
// '+' '*', SYN SYN, SOH, the block, check bytes (low first), DEL. preKeyChars 0 leaves the pre-key and the "+*" out.
std::vector<uint8_t> acarsBuildFrame(const AcarsBlockSpec& s, int preKeyChars = 16);

} // namespace dect2
