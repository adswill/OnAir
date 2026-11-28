// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The country of a callsign as an ISO 3166 two-letter code, for the flag chip next to a call in the decode lists (#708). Built on the DXCC
// lookup (ham_dxcc.h: portable /P /M, prefixed calls like EA8/G4ABC): the entity is mapped to the country it belongs to (England and
// Scotland are GB, Hawaii is US, Corsica is FR). Nothing is downloaded and no image set is used.
#pragma once
#include <string>
#include "dect2/ham/ham_dxcc.h"

namespace dect2::ham {

// "DE" for DXCC entity "Germany", "GB" for "England", "" when the entity is not in the table (a user country file with unusual names)
std::string entityIso(const std::string& entityName);
// the code for a callsign; "" when the call is not found, or is /MM or /AM. A user's country file is tried first, then the built-in table.
std::string callIso(const std::string& call, const DxccDb& db = DxccDb::builtin());
// the Unicode flag (two regional indicator symbols, UTF-8) for an ISO code; "" for anything that is not two letters A-Z
std::string isoFlagEmoji(const std::string& iso);

}  // namespace dect2::ham
