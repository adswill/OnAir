// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 3G-ALE traffic bursts (MIL-STD-188-141B Appendix C, the STANAG 4538 waveforms after link setup): BW1, BW2, BW3 and BW4 receivers,
// the TM, HDL and LDL PDU readers and test generators. BW0 (link setup) is in hfu_ale3g.h.
// Written from the published text of MIL-STD-188-141B Appendix C: C.5.1.2 (interleaver), C.5.1.4 (BW1: TM and HDL_ACK / HDL_EOM PDUs,
// 48 bits, tail-biting K = 9 rate 1/3 code, 16 x 9 interleaver, 16-ary Walsh x 4), C.5.1.5 (BW2: HDL_DATA, 3 to 24 packets of 1881 bits
// with a CRC-32, K = 8 rate 1/4 code of which one quarter is sent per forward transmission, 32 data + 16 known 8-PSK symbols, a 2^16 PN
// generator), C.5.1.6 (BW3: LDL_DATA, 8n + 25 bits, K = 7 rate 1/2 code of which one half is sent per forward transmission, the
// interleavers of table C-XVI, 16-ary Walsh), C.5.1.7 (BW4: LDL_ACK / LDL_EOM, 2 bits, 4-ary Walsh x 80), C.5.3.4, C.5.4.4 and C.5.5.4.
// The BW2 PN generator is read from figure C-12 (a drawing); see docs/HF-MIL-3.9.md. Receive only, nothing is decrypted.
// Input: complex baseband at 24 kHz with the sub-carrier at +1800 Hz.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::hfu {

// C.4.12 CRC over bits (first bit first), K = 4 .. 32 with the polynomial's terms below x^K as a mask
uint32_t ale3gCrcBits(const std::vector<uint8_t>& bits, uint32_t poly, int k);
constexpr uint32_t kAle3gCrc12 = 0xBCFu, kAle3gCrc16 = 0x897Bu, kAle3gCrc32 = 0x04C11DB7u;

// ---- BW1: TM, HDL_ACK, HDL_EOM (48 bits, the first bit sent is bit 47) ----
uint64_t tmPdu(int type, int priority, int destAddr, int srcAddr, int argument);   // C.5.3.4, CRC-12 added; type 0 REQ, 1 CONF, 2 TERM
uint64_t hdlAckPdu(uint32_t ackMask24);                                             // C.5.4.4.2, the first ack bit for the first packet
uint64_t hdlEomPdu();                                                               // C.5.4.4.3
struct Bw1Pdu {
    uint64_t bits = 0;
    bool crcOk = false;
    std::string type;          // "TM_REQUEST", "TM_CONFIRM", "TM_TERM", "HDL_ACK", "HDL_EOM", or "BW1 PDU"
    std::string text;
};
Bw1Pdu bw1Parse(uint64_t bits48);
std::vector<uint8_t> bw1Encode(uint64_t bits48);          // 144 coded bits in interleaver fetch order
uint64_t bw1Decode(const std::vector<float>& soft144);    // soft values in fetch order, positive favours 0

// ---- BW2: HDL_DATA ----
constexpr int kHdlPayloadBytes = 233;
// one data packet: 233 payload bytes (zero filled), the 17-bit sequence number, CRC-32: 1913 bits
std::vector<uint8_t> hdlPacket(const std::vector<uint8_t>& data233, uint32_t seq17);
// the sequence numbers of table C-XLVI / C-LVI: SOM, EOM, count
uint32_t seqOnly(int bytes);
uint32_t seqFirst(int packets);
uint32_t seqInterior(int following);
uint32_t seqLast(int bytes);
std::string seqText(uint32_t seq17);
// the BW2 PN spreading generator (C.5.1.5.8): 64 + 960 * numPkts spreading tribits for an FTcount
std::vector<uint8_t> bw2Pn(int ftCount, size_t n);

// ---- BW3: LDL_DATA (n = 64, 128, 256 or 512 payload bytes) ----
std::vector<uint8_t> ldlPacket(const std::vector<uint8_t>& dataN, int n, uint32_t seq17);   // 8n + 25 bits (payload, sequence, control)

// ---- the receiver of the traffic bursts ----
struct Ale3gTraffic {
    int waveform = 1;          // 1 .. 4
    std::string type;          // e.g. "TM_REQUEST", "HDL_DATA", "LDL_DATA", "LDL_ACK", "datagram"
    std::string text;          // in words
    bool crcOk = false;
    std::vector<uint8_t> data; // the payload bytes of a data packet or a whole datagram
    double snrDb = 0, cfoHz = 0;
};
class Ale3gTrafficRx {
public:
    Ale3gTrafficRx();
    ~Ale3gTrafficRx();
    void reset();
    void push(const cf32* x, size_t n);
    void setSink(std::function<void(const Ale3gTraffic&)> cb);
    uint64_t bursts() const;   // BW1 to BW4 bursts found
    uint64_t good() const;     // PDUs and packets with a good CRC (BW4: a clear decision)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// test signals (24 kHz complex, sub-carrier at +1800 Hz, no noise; nothing is transmitted)
std::vector<cf32> genBw1(uint64_t bits48, double leadSec = 0.05, double tailSec = 0.05);
std::vector<cf32> genBw2(const std::vector<std::vector<uint8_t>>& packets1913, int ftCount, double leadSec = 0.05, double tailSec = 0.05);   // 3, 6, 12 or 24 packets
std::vector<cf32> genBw3(const std::vector<uint8_t>& packet, int n, int ftCount, double leadSec = 0.05, double tailSec = 0.05);
std::vector<cf32> genBw4(int twoBits, double leadSec = 0.05, double tailSec = 0.05);   // b1 (sent first) in bit 1

}  // namespace dect2::hfu
