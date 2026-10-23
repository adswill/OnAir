// PACTOR-I packet layer, from the public descriptions (ARRL "PACTOR", SCS / Wavecom / amateur articles). Receive-side monitor only.
//
// What the public descriptions give and this file uses: FSK with 200 Hz shift at 100 or 200 baud, a cycle of 1.25 s (packet 0.96 s, pause
// 0.17 s, 12-bit control signal at 100 baud 0.12 s), packet = header byte (55 hex, inverted with every new information packet, the first data
// packet of a link has AA) + 8 (100 baud) or 20 (200 baud) data bytes + status byte (2 bit packet number, text format, break-in, QRT) +
// CRC-16 (CCITT, x^16 + x^12 + x^5 + 1) over everything except the header; control signals CS1 = 4D5, CS2 = AB2, CS3 = 34B, CS4 = D2C (hex,
// least significant bit right); Huffman compressed ASCII or plain 8-bit ASCII; connect (sync) packets carry the called station's
// callsign padded with 0F hex.
//
// What they do not give, and is therefore an ASSUMPTION here (docs/pactor.md lists them): the bit positions inside the status byte, the
// CRC start value and bit order, which bit of a byte goes first, the complete Huffman table. PACTOR is not verified on a real recording.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace pactor {

constexpr double kCycleSec = 1.25, kPacketSec = 0.96, kCsSec = 0.12, kCsStartSec = 1.13;
constexpr int kDataSlow = 8, kDataFast = 20;           // data bytes of a 100 and a 200 baud packet
constexpr int kBitsSlow = 96, kBitsFast = 192;         // header + data + status + CRC
constexpr uint8_t kHeaderA = 0x55, kHeaderB = 0xAA;
constexpr uint16_t kCs[4] = {0x4D5, 0xAB2, 0x34B, 0xD2C};   // CS1 acknowledge, CS2 reject / busy, CS3 changeover, CS4 speed change
constexpr uint8_t kCallPad = 0x0F;

uint16_t crc16(const uint8_t* p, size_t n);            // CCITT, start value FFFF

struct Status { int count = 0; bool huffman = false, breakIn = false, qrt = false; };
uint8_t statusByte(const Status& s);                   // bits 0-1 count, 2 Huffman, 3 break-in, 4 QRT (assumed layout)
Status statusParse(uint8_t b);

struct Packet {
    uint8_t header = kHeaderA;
    std::vector<uint8_t> data;                         // 8 or 20 bytes
    uint8_t status = 0;
};
bool isFast(const Packet& p);                          // 20 data bytes
// The bits as sent: every byte least significant bit first, the CRC high byte first.
std::vector<uint8_t> packetBits(const Packet& p);
// Check a received bit string (96 or 192 bits, hard decisions). headerErr: bit errors of the header against 55 / AA. crcMode: 0 plain, 1 complemented.
bool packetParse(const std::vector<uint8_t>& bits, Packet& out, int* headerErr = nullptr, int* crcMode = nullptr);

// 12-bit control signal, first bit sent = most significant (the hex value is written least significant bit right).
std::vector<uint8_t> csBits(int n);                    // n = 1 .. 4
int csMatch(const uint8_t* bits12, int* dist = nullptr);   // 1 .. 4 of the nearest, 0 when none is within 2 bit errors

// Callsign (connect) packets: up to 7 characters padded with 0F (8 characters have no padding and look like text).
std::vector<uint8_t> callData(const std::string& call, bool reversed = false);
bool callParse(const std::vector<uint8_t>& data8, std::string& call, bool* reversed = nullptr);

// Huffman text. The table ships with the entries the public ARRL description prints as examples; the full table of the same page
// (Annex, section 9) can be put into a file (pactor_huffman.txt: one entry per line, ASCII number or character, tab, code bits in sending
// order) and is then loaded at run time. A code the table does not know stops the decoder with '~'.
bool huffLoad(const std::string& path);
void huffReset();                                      // back to the built-in entries
size_t huffEntries();
bool huffComplete();                                   // the table holds the 95 printable characters
bool huffEncode(const std::string& text, std::vector<uint8_t>& bits);   // false: a character is not in the table
std::string huffDecode(const std::vector<uint8_t>& bits, bool* unknown = nullptr);   // bits in sending order
// Data bytes (LSB first) <-> bit string
std::vector<uint8_t> bytesToBits(const std::vector<uint8_t>& b);
std::vector<uint8_t> bitsToBytes(const std::vector<uint8_t>& bits);
std::string asciiText(const std::vector<uint8_t>& data);   // printable ASCII, others '.'

} // namespace pactor
} // namespace dect2
