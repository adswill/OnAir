// Motorola LRRP (Location Request/Response Protocol): the GPS position reports a DMR radio sends in
// "unconfirmed data, IP" packets. The payload OnAir assembles is a plain IPv4 + UDP packet; the UDP
// payload is an LRRP (MBXML) document. See dmr_lrrp.cpp for the token layout.
#pragma once
#include "dect2/dmr/dmr_tel.h"
#include <cstddef>
#include <cstdint>

namespace dect2 {

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
