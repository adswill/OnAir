// nRF24L01(+) Enhanced ShockBurst (ESB) frames, as the Nordic datasheet describes them (public): 2.4 GHz GFSK at 250 kbit/s, 1 or 2 Mbit/s,
// channel N at 2400 + N MHz (N = 0..125), bits sent most significant first:
//
//   preamble (1 byte: 0xAA or 0x55, whichever alternates into the first address bit) | address (3..5 bytes) |
//   packet control field (9 bits: payload length 6, PID 2, NO_ACK 1) | payload (0..32 bytes) | CRC (1 or 2 bytes)
//
// The CRC covers address, control field and payload. CRC-8: x^8 + x^2 + x + 1, start value 0xFF. CRC-16: CCITT x^16 + x^12 + x^5 + 1, start value 0xFFFF.
// The air gives no address length and no frame length: a sniffer finds a frame by trying the candidate positions and address lengths on the
// demodulated bits and accepting those with a valid CRC (the public technique of the nRF24 sniffers). A CRC-16 match is accepted at once; a CRC-8 match is
// only 1 in 256 by chance per candidate, so it needs a third sight of the same frame (or the address already known from a CRC-8 frame).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace nrf24 {

constexpr int kMaxPayload = 32;
constexpr int kMinAddr = 3, kMaxAddr = 5;

struct Frame {
    std::vector<uint8_t> address;   // 3..5 bytes, as sent (first byte first)
    int pid = 0;                    // 0..3
    bool noAck = false;
    std::vector<uint8_t> payload;   // 0..32 bytes
    int crcBits = 16;               // 8 or 16
    // filled by the sniffer
    int startBit = 0;               // where the address begins in the bits it was given
};

// bits of a whole transmission: preamble, address, control field, payload, CRC (one byte per bit, 0 or 1)
std::vector<uint8_t> encode(const Frame& f);
// the CRC of n bits starting at bits[0] (MSB first)
uint16_t crc16(const uint8_t* bits, size_t n);
uint8_t crc8(const uint8_t* bits, size_t n);

struct Candidate {
    Frame frame;
    bool weak = false;              // CRC-8: needs a confirmation
};
// All frames in a burst's bits (bits start at or before the preamble). A frame is found by its CRC: the start is taken from every place that follows
// at least six alternating bits within the first 16 bits (the preamble, which also alternates into the first address bit; the slicer starts where the signal starts), and each address length 3..5 and each CRC length is tried.
// Duplicates (the same frame found from more than one start) are removed; a CRC-16 frame wins over a CRC-8 one at the same place.
std::vector<Candidate> sniff(const std::vector<uint8_t>& bits);

std::string hex(const std::vector<uint8_t>& b);
std::string printable(const std::vector<uint8_t>& b);   // ASCII, other bytes as '.'

} // namespace nrf24
} // namespace dect2
