// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Argos message layers, from what is public. Receive only.
//
// Uplink (PTT to satellite, 401.650 MHz, band 401.618 - 401.680 MHz for Argos 2/3):
//   PTT-A2: 160 ms of unmodulated carrier (+-2.5 ms), then 400 bit/s Manchester (bi-phase) phase modulation of +-1.1 rad (+-63 degrees).
//   The message: 24 bits of bit and format synchronisation added by the transmitter (their pattern is in the CNES physical layer
//   specification AS3-SP-516-2098-CNES, which is not public: shown as received, not checked), a 4 bit message length field, a 28 bit
//   platform ID, then 24 + 32 k bits of user data (k = 0 .. 7): 80 to 304 bits, 360 to 920 ms on the air with the carrier.
//   Sources: the ARTIC R2 user datasheet (Arribada / Kineis, Table 9 "ARGOS 2 uplink messages"), the Telonics "Argos System Background"
//   note (160 ms carrier, 48 bit header in older 20 bit ID form, 360 - 920 ms), Campbell Scientific ST-21 data (401.618 - 401.680 MHz,
//   +-1.1 rad), US patent 4,731,870 (0 / +63 / -63 degree phase levels, Manchester data).
//   The coding of the 4 bit length field is not given in those sources: it is shown as received, and the message length is taken from the
//   burst's duration (the only lengths that exist are 80 + 32 k bits).
//   PTT-A3 (QPSK, convolutional code), PTT-ZE, Argos-4 VLD / MD / HD: their layers are in CNES documents that are not public. A burst that
//   starts with the carrier but is not A2 Manchester is reported as "other Argos modulation" with its timing, frequency and level only.
//
// Downlink (satellite to PMT, 465.9875 MHz, MetOp-B / -C and SARAL A-DCS): 800 symbol/s Manchester BPSK = 400 bit/s (SatNOGS transmitter
// entries for MetOp-B), described by ESA as bi-phase PM carrying an HDLC bit stream with filler when there is no message. What the HDLC
// frames hold is not public: the frames are shown as hex, with the standard HDLC FCS (CRC-16/X.25) checked. Whether the bits are NRZ or NRZI
// is not stated: the receiver deframes all three ways (NRZ, inverted NRZ, NRZI) and reports which one shows flags.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::argos {

constexpr double kUplinkMhz = 401.650;
constexpr double kUplinkLowMhz = 401.618, kUplinkHighMhz = 401.680;
constexpr double kDownlinkMhz = 465.9875;
constexpr double kBitRate = 400.0;
constexpr double kCarrierSec = 0.160;
constexpr double kPhaseDevRad = 1.1;
constexpr int kSyncBits = 24, kLenBits = 4, kIdBits = 28;
constexpr int kMinBits = 80, kMaxBits = 304;   // whole A2 messages: 80 + 32 k bits

struct A2Message {
    std::string syncHex;           // the first 24 bits as received (6 hex digits)
    int lenField = 0;              // the 4 bit length field as received
    uint32_t id = 0;               // the 28 bit platform ID
    std::string idHex;             // 7 hex digits
    uint32_t id20 = 0;             // the first 20 bits of the ID field, for platforms with an older 20 bit ID
    int dataBits = 0;              // 24 + 32 k
    std::vector<uint8_t> data;     // the user data, MSB first (dataBits / 8 bytes)
    std::string rawHex;            // every bit of the message (padded with 0 to a whole byte)
    int totalBits = 0;
};

// The A2 message lengths that exist, nearest to a count of demodulated bits (within tol bits); 0 when none is that close
int nearestA2Length(int bits, int tol = 3);
// Reads the fields of an A2 message from its bits (first modulated bit first). bits.size() must be one of the A2 lengths.
bool parseA2(const std::vector<uint8_t>& bits, A2Message& out);
// The bits of an A2 message for the test signal. sync24: the 24 synchronisation bits to put first (the real pattern is not public; the
// test signal uses a stand-in); lenField: the 4 bit field (its real coding is not public: the test signal writes k, the number of extra
// 32 bit words).
std::vector<uint8_t> buildA2(uint32_t sync24, int lenField, uint32_t id28, const std::vector<uint8_t>& data, int dataBits);

std::string hexOfBits(const std::vector<uint8_t>& bits, size_t from, size_t n);   // MSB first, padded with 0
std::string hexOf(const std::vector<uint8_t>& bytes);

// HDLC (ISO/IEC 13239) bit stream deframer: 0x7E flags, zero-bit deletion, seven 1 bits abort; bytes LSB first; FCS CRC-16/X.25.
struct HdlcFrame { std::vector<uint8_t> bytes; bool fcsOk = false; };   // bytes without the FCS when fcsOk, everything otherwise
class HdlcDeframer {
public:
    // one bit; true when a frame ended with it (then frame() holds it)
    bool push(int bit);
    const HdlcFrame& frame() const { return last_; }
    uint64_t flags = 0, aborts = 0;
private:
    uint32_t ones_ = 0, sr_ = 0;
    bool inFrame_ = false;
    std::vector<uint8_t> bits_;   // the data bits of the frame so far (zero bits deleted)
    HdlcFrame last_;
};
// The bits (LSB first, zero-bit insertion, flags around) of one HDLC frame with its FCS, for the test signal
std::vector<uint8_t> hdlcFrameBits(const std::vector<uint8_t>& payload);
uint16_t fcs16(const uint8_t* p, size_t n);

}  // namespace dect2::argos
