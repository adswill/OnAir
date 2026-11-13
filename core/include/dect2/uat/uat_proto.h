// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// UAT message decode: ADS-B reports, FIS-B text, TIS-B relay (C decode from dump978).
#pragma once
#include "dect2/uat/uat_nexrad.h"
#include "dect2/uat/uat_twgo.h"
#include "uat_tel.h"
#include "uat_demod.h"
#include <string>

namespace dect2 {

struct UatDecoded {
    enum class Kind { Unknown, Adsb, Tisb, Fisb, Other };
    Kind kind = Kind::Unknown;
    UatAircraft ac;
    UatFisbProduct fisb;
    std::vector<UatOverlay> overlays;     // graphic overlay records in this uplink
    std::vector<UatNexradBlock> nexrad;   // NEXRAD blocks in this uplink
    std::string summary;
    std::string hex;
};

// Apply a demodulated downlink or uplink frame to produce UI-facing fields.
bool uatDecodeFrame(const UatRawFrame& raw, UatDecoded& out);

std::string uatFisbProductName(uint16_t productId);

} // namespace dect2
