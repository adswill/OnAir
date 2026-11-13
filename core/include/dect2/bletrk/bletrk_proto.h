// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// BLE tracker advertisements: recognising the item trackers (Apple Find My network accessories such as AirTag, Tile, Chipolo, Samsung
// SmartTag, Google Find My Device network tags) from the advertising data of a Bluetooth LE packet. Only the public structure of the
// advertisement is looked at (company identifier, service UUID, the type and status bytes); public key bytes and the owner's data are
// neither read nor kept. Receive only.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace dect2 {
namespace bletrk {

enum Kind { None = 0, FindMy = 1, Tile = 2, Chipolo = 3, SmartTag = 4, GoogleFmd = 5, kKinds = 6 };

const char* kindName(int k);       // "Apple Find My (AirTag or compatible)" ...
const char* kindShort(int k);      // "Find My", "Tile", "Chipolo", "SmartTag", "Google"

struct Detection {
    Kind kind = None;
    bool separated = false;        // Find My: the offline-finding frame, sent by an accessory that has lost contact with its owner's devices
    int status = -1;               // Find My: the status byte of the frame (-1: none); shown, not interpreted
    bool unwantedProtection = false;   // Google: the frame type that says the tag is away from its owner (unwanted tracking protection)
};

// Looks at the AD structures (length, type, data) of an AdvData / ScanRspData block.
bool detect(const uint8_t* ad, size_t n, Detection& out);

constexpr int kAppleCompany = 0x004C;
constexpr int kAppleOfflineFinding = 0x12;   // the type byte of Apple's manufacturer data for the Find My network frame
constexpr int kFindMyLen = 25;               // its length byte: status, 22 key bytes, 2 bytes (key bits, hint)

} // namespace bletrk
} // namespace dect2
