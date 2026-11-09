// UAT message decode: ADS-B reports, FIS-B text, TIS-B relay (C decode from dump978).
#pragma once
#include "dect2/uat/uat_nexrad.h"
#include "uat_tel.h"
#include "uat_demod.h"
#include <string>

namespace dect2 {

struct UatDecoded {
    enum class Kind { Unknown, Adsb, Tisb, Fisb, Other };
    Kind kind = Kind::Unknown;
    UatAircraft ac;
    UatFisbProduct fisb;
    std::vector<UatNexradBlock> nexrad;   // NEXRAD blocks in this uplink
    std::string summary;
    std::string hex;
};

// Apply a demodulated downlink or uplink frame to produce UI-facing fields.
bool uatDecodeFrame(const UatRawFrame& raw, UatDecoded& out);

std::string uatFisbProductName(uint16_t productId);

} // namespace dect2
