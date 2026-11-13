// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Iridium ACARS: the ACARS blocks inside short burst data packets (iridium_sbd.h) turned into messages for the same tables, search, label
// decoder and aircraft map as the VHF / VDL2 / HFDL ACARS modes: registration kinds, duplicate retransmissions dropped, blocks joined,
// positions read with the shared position parsers (aero_pos.h), one aircraft list.
// Clean-room: the block layout is the public ARINC 618 one (acars_proto.h) inside the SBD wrapper that iridium_sbd.h describes.
// NOT verified against a real Iridium recording: how the check bytes and parity bits are carried inside SBD is read from public
// descriptions only, so a block that has neither a matching check nor odd parity is kept and flagged, never discarded.
#pragma once
#include "iridium_sbd.h"
#include "iridium_tel.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct IridiumReg {
    int kind = 0;                // 0 none, 1 tail number ("N893QS", "A6-EDA"), 2 24-bit ICAO address in hex, 3 other
    std::string norm;            // upper case, leading dots removed
    uint32_t icao = 0;           // kind 2
};
// The 7 characters of the address field: leading '.' padding removed; six hex digits only = an ICAO address.
IridiumReg iridiumAcarsReg(const std::string& raw);

enum class IridiumAcarsOutcome { None, New, Repeat, Joined };

// Puts the ACARS message of one SBD packet into t.acars (newest last, at most 200) and t.aircraft: a retransmission of a block already
// listed (same direction, address, label, block id, sequence, flight and text within 3 minutes) only counts in `repeats`; a block that
// follows an ETB block of the same address, label and direction within a minute is joined to it. Returns what happened;
// serialOut gets the serial of the message touched.
IridiumAcarsOutcome iridiumAcarsIngest(IridiumTelemetry& t, const IridiumIdaPacket& pk, const IridiumSbd& sbd, int64_t wall, uint64_t* serialOut = nullptr);

// Test signal builder: like iridiumBuildSbdAcars with the options of a real block.
struct IridiumAcarsBuild {
    char mode = '2';
    std::string reg;             // padded with '.' to 7 characters; six hex digits for an ICAO address
    char ack = 0x15;             // NAK; 0x06 ACK or a block id
    std::string label = "H1";
    char blockId = '0';
    std::string text;
    bool parity = false;         // odd parity bits on every byte of the block
    bool crc = false;            // the two check bytes (and DEL) after the suffix
    bool more = false;           // ETB instead of ETX
    bool fromAircraft = false;   // Iridium uplink: message sequence (4) and flight (6) first
    std::string seq = "M01A", flight = "XX0000";
    int type = 0x7608;           // 0x7608 (to the aircraft) or 0x7609 (from it)
};
std::vector<uint8_t> iridiumBuildSbdAcarsEx(const IridiumAcarsBuild& b);

// Packet dump helpers (also used by the screen): "xx xx ..." hex and a printable column.
std::string iridiumHexDump(const std::vector<uint8_t>& v, size_t maxBytes = 256);
std::string iridiumAsciiDump(const std::vector<uint8_t>& v, size_t maxBytes = 256);
// Bytes of an IridiumFrame::hex string.
std::vector<uint8_t> iridiumHexBytes(const std::string& hex);

} // namespace dect2
