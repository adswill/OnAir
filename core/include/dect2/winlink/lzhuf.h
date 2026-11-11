// LZHUF compression as Winlink's B2 forwarding protocol uses it (LZSS with a 2 kB window and adaptive Huffman codes of Okumura, Yoshizaki
// and Rikitake), in the "B2" framing: a CRC-16 (XMODEM CRC-CCITT, two zero bytes appended, stored low byte first) over what follows, the
// uncompressed length (4 bytes, little endian), then the code bits (most significant bit first).
//
// Ported from wl2k-go's lzhuf package (Copyright 2016 Martin Hebnes Pedersen LA5NTA, MIT licence), itself a port of lzhuf.c of JNOS 2;
// the lzhuf.c authors allow use "freely" (see THIRD_PARTY.md). The output is byte for byte the same as wl2k-go's and the JNOS code's.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace winlink {

// CRC of the B2 header: CRC-CCITT (polynomial 0x1021, start 0) over d, then two zero bytes
uint16_t b2Crc16(const uint8_t* d, size_t n);

// Compresses data. b2 = true: the CRC-16 header in front (what B2F sends); false: length + bits only.
std::vector<uint8_t> lzhufCompress(const uint8_t* d, size_t n, bool b2 = true);
inline std::vector<uint8_t> lzhufCompress(const std::vector<uint8_t>& v, bool b2 = true) { return lzhufCompress(v.data(), v.size(), b2); }

// Decompresses. False (and err) when the data are short, the CRC does not match or the length is implausible (over maxLen).
bool lzhufDecompress(const uint8_t* d, size_t n, std::vector<uint8_t>& out, bool b2 = true, std::string* err = nullptr, size_t maxLen = 64u << 20);

} // namespace winlink
} // namespace dect2
