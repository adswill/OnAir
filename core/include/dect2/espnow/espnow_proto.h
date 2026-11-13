// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ESP-NOW (Espressif's connectionless protocol of the ESP8266 / ESP32 chips): the frame layer. An ESP-NOW packet is an IEEE 802.11 vendor-specific
// action frame (management type 0, subtype 13 action; 14 action no-ack is accepted too) from the Espressif documentation:
//   MAC header (24 bytes): frame control, duration, address 1 the destination (ff:ff:ff:ff:ff:ff for a broadcast), address 2 the source,
//     address 3 the broadcast address, sequence control
//   category 127 (vendor specific), organisation identifier 18:FE:34 (Espressif), 4 random bytes
//   one or more vendor-specific elements: element ID 221, length (OUI + type + version + body), OUI (18:FE:34 unless the user set another),
//     type 4 (ESP-NOW), version byte (bits 3 - 0 the version; v2 adds a "more data" bit 4), body 0 - 250 bytes
//   FCS (CRC-32, checked by the PHY)
// ESP-NOW v2 carries up to 1470 bytes: the bodies of all ESP-NOW elements of one frame are joined.
//
// Encryption: a unicast frame to a peer with a local master key (LMK) is protected with CCMP (IEEE 802.11 clause 12.5.3) as a robust management
// frame: the Protected bit set, an 8-byte CCMP header (PN0, PN1, reserved, key ID with ExtIV, PN2 - PN5) after the MAC header, the whole body
// from the category on encrypted, an 8-byte MIC. Such a frame is shown as encrypted; it is decrypted only with a key the user entered (the key
// store, voicekeys::Protocol::EspNow), tried with the management nonce flag (802.11w) and without. Nothing here searches for keys.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::espnow {

constexpr uint8_t kCategoryVendor = 127;
constexpr uint8_t kElementVendor = 221;
constexpr uint8_t kTypeEspNow = 4;
constexpr uint32_t kEspressifOui = 0x18FE34;
constexpr size_t kMaxBodyV1 = 250, kMaxBodyV2 = 1470;

struct Frame {
    bool action = false;                 // a management action frame (subtype 13 or 14)
    bool espnow = false;                 // category 127, OUI 18:FE:34 and at least one ESP-NOW element (after decryption for a protected one)
    bool isProtected = false;            // the Protected bit: CCMP
    bool decrypted = false;              // protected, and a key from the user opened it (MIC good)
    bool broadcast = false;              // address 1 is ff:ff:ff:ff:ff:ff
    bool truncated = false;              // an element runs past the end of the frame
    uint8_t da[6] = {}, sa[6] = {}, a3[6] = {};
    uint16_t seq = 0;                    // sequence number (sequence control >> 4)
    uint32_t random = 0;                 // the 4 random bytes after the OUI, as sent (first byte highest)
    uint64_t pn = 0;                     // CCMP packet number (protected frames)
    int keyId = 0;
    int version = -1;                    // of the first ESP-NOW element; -1 none
    bool moreData = false;               // v2 "more data" bit of the last element
    uint32_t elemOui = kEspressifOui;    // the OUI of the first ESP-NOW element
    int elements = 0;                    // ESP-NOW elements in the frame
    std::vector<uint8_t> body;           // the payload: the bodies of every ESP-NOW element, in order
};

// n: the MPDU without its FCS. false: not an action frame (or too short to be one). keys: 16-byte keys to try on a protected frame.
bool parse(const uint8_t* p, size_t n, Frame& f, const std::vector<std::vector<uint8_t>>& keys = {});

// CCMP (clause 12.5.3) for a frame with a 24-byte header: the nonce flags 0x10 for a management frame when mgmtFlag (802.11w), else 0.
// decrypt: f / n the protected MPDU without FCS (header, CCMP header, ciphertext, MIC); plain gets the frame body. false: the MIC does not match.
bool ccmpDecrypt(const uint8_t key[16], const uint8_t* f, size_t n, bool mgmtFlag, std::vector<uint8_t>& plain);
// encrypt: the header (24 bytes) and the plain body; returns the protected MPDU without FCS (Protected bit set, key ID 0).
std::vector<uint8_t> ccmpEncrypt(const uint8_t key[16], const uint8_t hdr[24], const uint8_t* body, size_t n, uint64_t pn, bool mgmtFlag);

// The test signal and the tests: one ESP-NOW frame, FCS appended. A body over 250 bytes is split into elements of at most 250 (v2).
struct Build {
    uint8_t sa[6] = {0x24, 0x6F, 0x28, 0x11, 0x22, 0x33};
    uint8_t da[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint16_t seq = 0;
    uint32_t random = 0x5A17C0DEu;
    int version = 1;
    std::vector<uint8_t> body;
    bool encrypt = false;                // CCMP with key (management nonce flag)
    uint8_t key[16] = {};
    uint64_t pn = 1;
};
std::vector<uint8_t> makeFrame(const Build& b);

// The address's OUI is one of Espressif's (the ESP8266 / ESP32 chips' own addresses; a best-effort list of the IEEE registry)
bool espressifOui(const uint8_t mac[6]);

std::string hexText(const uint8_t* p, size_t n, size_t maxBytes = 1500);   // "01 02 AB ..." (… past maxBytes)
std::string printable(const uint8_t* p, size_t n, size_t maxBytes = 1500);  // the bytes as text, '.' for what does not print
bool mostlyText(const uint8_t* p, size_t n);                                // at least 3 bytes and 80 % printable ASCII
std::string summary(const Frame& f);                                        // one line for the message log

} // namespace dect2::espnow
