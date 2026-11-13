// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wireshark output: the byte formats, as pure functions (classic pcap, pcapng, GSMTAP, IPv4 + UDP, and the link-layer pseudo headers).
// Layouts checked against the Wireshark sources (packet-gsmtap.h, packet-loratap.c, packet-btle.c, wiretap/pcap-common.c) and the tcpdump link type list.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::wire {

using Bytes = std::vector<uint8_t>;

// classic pcap, little endian, microseconds
Bytes pcapFileHeader(uint32_t linktype, uint32_t snaplen = 262144);
void appendPcapRecord(Bytes& out, int64_t unixUsec, const uint8_t* p, size_t n);

// pcapng: one section, one interface (microsecond timestamps, the format's default)
Bytes pcapngSectionHeader(const std::string& application);
Bytes pcapngInterface(uint32_t linktype, const std::string& name, const std::string& description, uint32_t snaplen = 262144);
Bytes pcapngPacket(uint32_t interfaceId, int64_t unixUsec, const uint8_t* p, size_t n);

// an IPv4 + UDP packet from 127.0.0.1 to 127.0.0.1 around a payload (the addresses are placeholders: nothing is sent)
Bytes udpInIpv4(uint16_t dstPort, const uint8_t* p, size_t n, uint16_t srcPort = 49152);

// GSMTAP version 1 (16-byte header, then the payload)
namespace gsmtap {
constexpr uint8_t kTypeUm = 0x01, kTypeTetraI1 = 0x05, kTypeLteRrc = 0x0d;
constexpr uint8_t kChanBcch = 0x01;                                                   // GSM Um sub types
constexpr uint8_t kTetraBsch = 0x01, kTetraSchHd = 0x04, kTetraSchF = 0x05, kTetraBnch = 0x06;
constexpr uint8_t kLteBcchBch = 4, kLteBcchDlSch = 5;                                 // GSMTAP_LTE_RRC_SUB_*
}
Bytes makeGsmtap(uint8_t type, uint8_t subType, const uint8_t* p, size_t n, uint16_t arfcn = 0, uint8_t timeslot = 0, uint32_t frameNumber = 0, int8_t signalDbm = 0, int8_t snrDb = 0);

// bits (one per byte, 0 or 1), first bit in the top of the first byte, as osmo-tetra and the GSMTAP dissectors expect
Bytes packBitsMsbFirst(const uint8_t* bits, size_t n);

// a radiotap header (flags, and channel and signal when known) and an 802.11 frame without its FCS
Bytes radiotap80211(const uint8_t* frame, size_t n, int freqMhz, float signalDbm);

// a LoRaTap version 0 header (15 bytes) and a LoRa PHY payload. syncWord 0x34 is LoRaWAN (public network), which sends the frame to the lorawan dissector.
Bytes loraTap(const uint8_t* phy, size_t n, double freqHz, double bwHz, int sf, float snrDb, uint8_t syncWord = 0x34);

// LINKTYPE_BLUETOOTH_LE_LL: the access address (little endian), the PDU and the 3 CRC bytes in the order they come off the air
Bytes bleLl(const uint8_t* pduAndCrc, size_t n, uint32_t accessAddress = 0x8E89BED6u);

// the USER1 link type of Z-Wave: one byte for the rate (0: 9.6, 1: 40, 2: 100 kbit/s), then the MPDU with its checksum or CRC (docs/WIRESHARK-3.8.md). rate: zwave rate 1, 2 or 3
Bytes zwaveUser(const uint8_t* mpdu, size_t n, int rate);

// the Lua file for what Wireshark has no dissector for, or does not read by default (installed as onair.lua)
const char* luaDissectors();

} // namespace dect2::wire
