// ATSC 3.0 baseband packets (A/322 section 5.2): the header with the pointer to the first ALP packet, optional and extension fields, and the
// scrambling of the packet. A baseband packet fills one FEC frame payload (Kpayload bits).
#pragma once
#include <cstdint>
#include <vector>

namespace dect2 {
namespace atsc3 {

struct BbHeader {
    int pointer = 0;           // offset in bytes of the first ALP packet that begins in this packet; 8191 = none
    bool twoByteBase = false;  // MODE = 1 (13-bit pointer, optional fields allowed)
    int ofi = 0;               // 0 none, 1 short, 2 long, 3 mixed extension mode
    int extType = 0;           // EXT_TYPE of a single extension (0 = counter, 7 = padding)
    int extLen = 0;            // length of the extension field in bytes
    int counter = -1;          // value of the counter extension, -1 if there is none
    int headerBytes = 0;       // total length of the header (base + optional + extension)
};

// Writes the header and returns the bytes (including the extension field, which carries `counter` when extType = 0 and padding otherwise).
std::vector<uint8_t> makeBbHeader(const BbHeader& h);
// Reads a header; false when it is malformed or runs past the end.
bool parseBbHeader(const uint8_t* data, int size, BbHeader& h);

// A complete baseband packet of `bytes` bytes: header, then `payload` bytes (at most bytes - header), then zero padding. `pointer` is the
// offset in `payload` of the first ALP packet starting in it (-1 if none starts). Returns the packet; `used` receives the number of payload bytes taken.
std::vector<uint8_t> makeBbPacket(int bytes, const std::vector<uint8_t>& payload, int pointer, int counter, int* used = nullptr);

// Scrambling with the sequence of A/322 5.2.3 (the same operation scrambles and descrambles). `variant` selects between readings of the
// shift register: 0 is the serial reading used so far; others exist so a receiver can try them against a real signal.
std::vector<uint8_t> bbScrambleSequence(int bits, int variant = 0);
void bbScramble(std::vector<uint8_t>& bits, int variant = 0);

std::vector<uint8_t> bitsToBytes(const std::vector<uint8_t>& bits);
std::vector<uint8_t> bytesToBits(const std::vector<uint8_t>& bytes);

} // namespace atsc3
} // namespace dect2
