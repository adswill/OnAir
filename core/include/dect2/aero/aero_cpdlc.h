// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FANS-1/A CPDLC (ARINC 622 IMI AT1 / CR1 / CC1 / DR1) inside ACARS text: the ASN.1 PER body decoded to the plain message text
// ("CLIMB TO AND MAINTAIN FL350", "REQUEST DIRECT TO [position]", ...). The decoder is the one of libacars (Tomasz Lemiech, MIT,
// third_party/libacars): asn1c unaligned PER tables of the FANS-1/A ATC uplink and downlink messages, see THIRD_PARTY.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct AeroCpdlcMessage {
    bool found = false;        // the text carries an ARINC 622 CPDLC application (IMI AT1 / CR1 / CC1 / DR1)
    std::string imi;           // "AT1", "CR1", "CC1" or "DR1"
    std::string groundAddr;    // the 7 (or 4) character ground facility address before the IMI
    std::string reg;           // the aircraft registration after the IMI
    bool crcOk = false;        // the ARINC CRC-16 over IMI + registration + body
    bool empty = false;        // no body (a connect confirm or disconnect without a PDU)
    bool parsed = false;       // the PER body decoded completely
    std::string text;          // decoded message, one element per line, "" when not parsed
};

// `uplink`: ground to air (decides uplink or downlink message set). False when the text has no CPDLC application.
bool aeroDecodeCpdlc(const std::string& text, bool uplink, AeroCpdlcMessage& out);

// One line for logs: "FANS-1/A CPDLC message (AT1) ...". Includes the decoded text when parsed.
std::string aeroDescribeCpdlc(const AeroCpdlcMessage& m);

// Builders for the test signal: ARINC 622 text from a PER body (adds ground address, IMI, registration and the CRC-16).
std::string aeroBuildCpdlcText(const std::string& groundAddr, const std::string& imi, const std::string& registration, const std::vector<uint8_t>& perBody);

} // namespace dect2
