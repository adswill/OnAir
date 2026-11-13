// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 3G-ALE (MIL-STD-188-141B Appendix C, the link setup of STANAG 4538): BW0 burst receiver, 3G-ALE PDU reader and test generator.
// Written from the published text of MIL-STD-188-141B Appendix C: C.5.1.2 (interleaver), C.5.1.3 (BW0: 256-symbol TLC/AGC guard, 384-symbol
// acquisition preamble, 26 payload bits, tail-biting K = 7 rate 1/2 code, 4 x 13 interleaver, 16-ary Walsh symbols repeated 4 times, PN
// spreading), C.5.1.8 (8-PSK on 1800 Hz at 2400 Bd) and C.5.2.2 (LE PDUs, CRCs of C.4.12). The receiver also reads the traffic bursts
// BW1 to BW4 (hfu_ale3gbw.h) and reports them through the same PDU sink. Receive only. Input: complex baseband at 24 kHz with the sub-carrier at +1800 Hz.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::hfu {

// one 3G-ALE PDU (26 bits, the first bit sent is bit 25)
struct Ale3gPdu {
    uint32_t bits = 0;
    bool crcOk = false;
    std::string type;          // "LE_Call", "LE_Handshake", "LE_Notification", "LE_Broadcast", "3G-ALE PDU"; BW1 to BW4: see Ale3gTraffic
    int waveform = 0;          // the burst waveform, 0 .. 4
    std::string text;          // the fields, in words
    double snrDb = 0, cfoHz = 0;
};
uint8_t ale3gCrc(uint32_t bits, int nBits, int crcBits);     // C.4.12 over the first nBits (MSB first); 4-bit or 8-bit CRC
Ale3gPdu ale3gParse(uint32_t bits26);
// builders for tests (CRC added)
uint32_t ale3gCall(int calledMember, int callType, int callerMember, int callerGroup);
uint32_t ale3gHandshake(int linkId, int command, int argument);
uint32_t ale3gNotification(int status, int callerMember, int callerGroup);
uint32_t ale3gBroadcast(int countdown, int callType, int channel);
int ale3gLinkId(int callerAddr, int responderAddr);           // C.5.2.2.2

// the BW0 coding of 26 bits into 832 raw tribits (before the PN sequence), and back from soft Walsh metrics
std::vector<uint8_t> bw0Encode(uint32_t bits26);              // 52 coded bits in interleaver fetch order
uint32_t bw0Decode(const std::vector<float>& soft52);         // soft values in fetch order, positive favours 0

class Ale3gRx {
public:
    Ale3gRx();
    ~Ale3gRx();
    void reset();
    void push(const cf32* x, size_t n);
    void setPduSink(std::function<void(const Ale3gPdu&)> cb);
    uint64_t bursts() const;         // BW0 to BW4 bursts detected
    uint64_t good() const;           // PDUs and packets with a good CRC
    double lastCfoHz() const;
    double lastSnrDb() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// tables C-V / C-X (TLC/AGC guard sequence) and C-IX / C-XIII (the BW0 and BW1 PN sequence), 256 tribits each
const uint8_t* ale3gTlcTable();
const uint8_t* ale3gPnTable();

// one BW0 burst (24 kHz complex, sub-carrier at +1800 Hz, no noise; nothing is transmitted)
std::vector<cf32> genBw0(uint32_t bits26, double leadSec = 0.05, double tailSec = 0.05);

}  // namespace dect2::hfu
