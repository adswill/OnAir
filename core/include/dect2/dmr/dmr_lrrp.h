// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Motorola LRRP (Location Request/Response Protocol): the GPS position reports a DMR radio sends in
// "unconfirmed data, IP" packets. The payload OnAir assembles is a plain IPv4 + UDP packet; the UDP
// payload is an LRRP (MBXML) document. See dmr_lrrp.cpp for the token layout.
#pragma once
#include "dect2/dmr/dmr_tel.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {

// Builds one IPv4/UDP/LRRP "Triggered-Location-Report" packet (doc 0x0D) for a transmitted position report (dmr_tx.h): a request-id token, a
// circle-2d token (latitude, longitude, radius) and, when asked, speed and heading. The bytes are exactly what parseDmrLrrp reads back. UDP
// ports are 4001 (the Motorola default); the IPv4 header carries a correct header checksum. requestId is the two-octet id echoed in the report.
std::vector<uint8_t> dmrLrrpEncode(double lat, double lon, double radiusM,
                                   bool hasSpeed, double speedMs, bool hasHeading, double headingDeg, uint16_t requestId = 1);
// A NoFix status report (doc 0x0D, request-id, result 16): the radio has no position.
std::vector<uint8_t> dmrLrrpEncodeNoFix(uint16_t requestId = 1);

// Parses one IPv4/UDP/LRRP packet. Returns true when `data` holds a valid IPv4 packet (protocol UDP)
// whose payload is an LRRP document; fills `out` with the position or the NoFix status. False for
// everything else (short data, voice, other IP traffic). Never reads past `size`.
bool parseDmrLrrp(const uint8_t* data, size_t size, DmrLocation& out);

// An LRRP document behind a compressed IP/UDP header (#335: Motorola radios send the position in a packet whose proprietary header carries the
// compressed header and the first octets of the document, SAP 9 "proprietary packet data"). `data` is the proprietary header's octets after the
// DPF/SAP and MFID octets, followed by the packet's data. The document must end exactly at the end of `data` and hold only known tokens.
bool parseDmrLrrpCompressed(const uint8_t* data, size_t size, DmrLocation& out);

// True when `data` starts with an IPv4 header whose own 16-bit checksum is right. A message whose DMR packet CRC failed is still taken as a position
// report when this holds (#335: the IP checksum is an independent check of the same bytes, so a wrong packet CRC alone is not a reason to drop it).
bool dmrIpv4HeaderOk(const uint8_t* data, size_t size);

} // namespace dect2
