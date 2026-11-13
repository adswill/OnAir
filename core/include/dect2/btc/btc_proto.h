// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Bluetooth Classic (BR/EDR) discovery: the baseband pieces needed to find devices from their access codes and inquiry responses
// (Bluetooth Core Specification vol 2 part B: access code, packet header, FHS packet; the class of device is from the Assigned Numbers).
// Receive only: nothing here pages, inquires or answers.
//
// The algorithms follow libbtbb (Ubertooth, GPL-2.0-or-later, https://github.com/greatscottgadgets/libbtbb, bluetooth_packet.c): the sync word
// generator matrix, the packet header and FHS unpacking, HEC and CRC. Bit arrays are in air order (first transmitted bit first, one bit per byte).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace btc {

constexpr uint32_t kGiac = 0x9E8B33;   // General Inquiry Access Code: every device in inquiry scan answers it
constexpr uint32_t kLiac = 0x9E8B00;   // Limited Inquiry Access Code
constexpr int kAcBits = 72;            // preamble 4 + sync word 64 + trailer 4 (an ID packet has no trailer: 68)

// The sync word of a LAP, as libbtbb holds it: bit i of the result is the i-th bit sent
uint64_t syncWord(uint32_t lap);

struct AcHit { size_t pos = 0; uint32_t lap = 0; int errors = 0; bool trailer = false; };
// Looks for a sync word in air-order bits from `start`: the 7 Barker and LAP-top bits first, then the LAP's own sync word with at most maxErr bit
// errors, then the 4-bit preamble before it (alternating, ending opposite to the first sync bit). pos = index of the first preamble bit.
// `trailer` says the 4 bits after the sync word are the trailer pattern (otherwise it is an ID packet or the burst ended).
bool findAccessCode(const uint8_t* bits, size_t n, size_t start, int maxErr, AcHit& hit);

// The FHS packet an inquiry response carries
struct Fhs {
    uint32_t lap = 0;
    uint8_t uap = 0;
    uint16_t nap = 0;
    uint32_t cod = 0;            // class of device, 24 bits
    uint8_t ltAddr = 0;
    uint32_t clk = 0;            // CLK27-2 (units of 1.25 ms)
    int scanRepMode = 0, pageScanMode = 0;
    int hdrClk = 0;              // the whitening seed (CLK1-6) that decoded it
};
// Decodes the packet header and FHS payload that follow the 72-bit access code of an inquiry response (UAP 0, the default check initialisation).
// `bits` points just after the trailer; needs 54 + 240 bits. False when the header (FEC 1/3, HEC) or the payload (FEC 2/3, CRC-16) does not check.
bool decodeFhs(const uint8_t* bits, size_t n, Fhs& out);

std::string bdAddr(const Fhs& f);                 // "00:11:22:33:44:55" (NAP:UAP:LAP)
std::string lapText(uint32_t lap);                // "9E8B33"
const char* acKind(uint32_t lap);                 // "GIAC (inquiry)", "LIAC (limited inquiry)", "" for a device or piconet
std::string codText(uint32_t cod);                // "Phone: smartphone (Telephony, Object Transfer)"
const char* codMajor(uint32_t cod);               // "Phone"

// The transmit side, for the test signal and the tests: air-order bit arrays
std::vector<uint8_t> makeAccessCode(uint32_t lap, bool trailer);
std::vector<uint8_t> makeFhsPacket(const Fhs& f, int hdrClk);   // access code of the GIAC + header + payload, UAP 0 as in an inquiry response

} // namespace btc
} // namespace dect2
