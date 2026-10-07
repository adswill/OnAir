// DMR error control codes, checksums and bit interleavers (ETSI TS 102 361-1 annex B), shared by the receiver and the test signal.
// Bits are one per element (0 or 1) in the order they go on the air; words held in integers have their first bit in the top position.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace dmr {

using Bits = std::vector<uint8_t>;

// ---- bit helpers
void putBits(Bits& b, uint64_t v, int n);                                   // append the n low bits of v, most significant first
uint64_t getBits(const Bits& b, size_t pos, int n);
void bytesToBits(const uint8_t* d, size_t n, Bits& out);                    // appends, most significant bit of each byte first
void bitsToBytes(const Bits& b, size_t pos, size_t nbits, uint8_t* out);    // nbits must be a multiple of 8
int popcount32(uint32_t v);

// ---- short block codes (B.3.1 to B.3.5). `info` holds the information bits, the parity word the check bits (first one on top).
unsigned golay2008Parity(unsigned info8);                    // slot type: 4 bit colour code and 4 bit data type -> 12 parity bits
int golay2008Decode(unsigned word20, unsigned& info8);       // nearest codeword within 3 bit errors: number of errors, or -1
unsigned qr1676Parity(unsigned info7);                       // EMB: 4 bit colour code, PI, 2 bit LCSS -> 9 parity bits
int qr1676Decode(unsigned word16, unsigned& info7);          // within 2 bit errors
unsigned hamming743Parity(unsigned info4);                   // TACT: access type, channel, 2 bit LCSS -> 3 parity bits
int hamming743Decode(unsigned word7, unsigned& info4);       // single error correction

// Hamming codes used by the block product codes. The word holds k information bits followed by the check bits.
enum HammingKind { kHam1393, kHam15113, kHam16114, kHam17123 };
unsigned hammingParity(HammingKind k, unsigned info);
int hammingCorrect(HammingKind k, unsigned& word);           // 0 clean, 1 one bit corrected, -1 not correctable (word untouched)
int hammingInfoBits(HammingKind k);
int hammingParityBits(HammingKind k);

// ---- Reed-Solomon (12,9) over GF(256) for the link control of headers and terminators (B.3.6)
void rs129Parity(const uint8_t msg[9], uint8_t parity[3]);
int rs129Correct(uint8_t word[12]);                          // 0 clean, 1 symbol corrected, -1 failed; the parity bytes must be unmasked

// ---- checksums
uint16_t crcCcitt(const uint8_t* d, size_t n);               // B.3.8: CRC-CCITT with the final inversion; the result still has to be masked per data type
uint8_t crc8Short(uint32_t bits28);                          // B.3.7: short LC and hash, over 28 bits
uint16_t crc9(const uint8_t* d, size_t nbytes, unsigned dbsn);   // B.3.10 before the data type mask: the data octets then the 7 bit serial number
uint32_t crc32Msg(const uint8_t* d, size_t n);               // B.3.9: message CRC; send/expect the four bytes least significant first
uint8_t checksum5(const uint8_t lc[9]);                      // B.3.11: embedded LC checksum

constexpr uint16_t kMaskPi = 0x6969, kMaskVoiceLc = 0x9696, kMaskTermLc = 0x9999, kMaskCsbk = 0xA5A5, kMaskMbcHeader = 0xAAAA,
                   kMaskDataHeader = 0xCCCC, kMaskUsbd = 0x3333;     // 16 bit masks (the Reed-Solomon masks repeat the byte three times)
constexpr uint16_t kMaskRate12 = 0x0F0, kMaskRate34 = 0x1FF, kMaskRate1 = 0x10F;   // 9 bit masks of the confirmed data blocks

// ---- block product turbo code (196,96) of control blocks and rate 1/2 data (B.1.1)
void bptc196Encode(const Bits& info96, Bits& tx196);         // info96[0] is I(95); tx196[0] is the first bit on the air
int bptc196Decode(const Bits& tx196, Bits& info96);          // number of bits corrected, or -1 when the row and column checks do not clear

// ---- variable length BPTC of the embedded signalling (B.2.1): 72 bit link control + 5 bit checksum in four 32 bit fragments
void embLcEncode(const uint8_t lc[9], Bits frag[4]);
int embLcDecode(const Bits frag[4], uint8_t lc[9]);          // corrected bits, -1 for an uncorrectable matrix, -2 when the checksum is wrong

// ---- Short LC in the CACH (B.2.3): 28 bits + CRC-8 in four 17 bit pieces
void shortLcEncode(uint32_t lc28, Bits piece[4]);
int shortLcDecode(const Bits piece[4], uint32_t& lc28);      // corrected bits, -1 uncorrectable, -2 CRC mismatch

// ---- CACH burst (B.4.1): TACT (access type, channel, LCSS and Hamming parity) interleaved with 17 payload bits
void cachEncode(int at, int tc, int lcss, unsigned payload17, Bits& out24);
int cachDecode(const Bits& in24, int& at, int& tc, int& lcss, unsigned& payload17);   // TACT errors corrected, or -1

// ---- rate 3/4 trellis code (B.2.4): 18 data bytes <-> 98 dibits (196 bits) in transmit order
void trellis34Encode(const uint8_t data[18], Bits& tx196);
// symbols: the 98 received dibit values in transmit order as 4-level amplitudes (-3, -1, +1, +3 nominal); soft values are fine.
// Returns the number of dibits that differ from the best path's re-encoding, or -1 when no path ends in the zero state.
int trellis34Decode(const float symbols[98], uint8_t data[18]);

// ---- rate 1 (B.2.5): 192 data bits placed in 196 with four zero pad bits
void rate1Encode(const uint8_t data[24], Bits& tx196);
void rate1Decode(const Bits& tx196, uint8_t data[24], bool* padOk = nullptr);

// ---- 4FSK dibit mapping (table 10.3): dibit 01 -> +3, 00 -> +1, 10 -> -1, 11 -> -3
inline int dibitToSymbol(unsigned d) { return d == 1 ? 3 : d == 0 ? 1 : d == 2 ? -1 : -3; }
inline unsigned symbolToDibit(int s) { return s == 3 ? 1u : s == 1 ? 0u : s == -1 ? 2u : 3u; }

} // namespace dmr
} // namespace dect2
