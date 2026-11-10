// Yaesu System Fusion GPS in the slow data of V/D mode 1 and 2: tags 0x22 0x62 or 0x47 0x64, packed digit latitude and longitude, the radio model byte.
// Layout from Jonathan Naylor's YSFGateway (GPL-2+). Simulated only: no off-air recording of this block was available.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace dect2::fusion {

inline std::string ysfRadioName(uint8_t b) {
    switch (b) {
        case 0x20: return "DR-2X";
        case 0x24: return "FT-1D";
        case 0x25: return "FTM-400D";
        case 0x26: return "DR-1X";
        case 0x27: return "FT-991A";
        case 0x28: return "FT-2D";
        case 0x29: return "FTM-100D";
        case 0x30: return "FT-3D";
        case 0x31: return "FTM-300D";
        case 0x33: return "FT-5D";
        default: { char t[8]; snprintf(t, sizeof t, "0x%02X", b); return t; }
    }
}

struct YsfGps { bool ok = false; double lat = 0, lon = 0; std::string radio; };

// buf holds n assembled bytes (offsets as in YSFGateway). Finds the end marker 0x03 scanning backward, checks the 8-bit sum, then decodes.
inline YsfGps ysfGpsParse(const uint8_t* b, int n) {
    YsfGps g;
    if (n < 15) return g;
    if (!((b[1] == 0x22 && b[2] == 0x62) || (b[1] == 0x47 && b[2] == 0x64))) return g;
    int end = -1;
    for (int i = n - 2; i > 0; i--) {
        if (b[i] != 0x03) continue;
        uint8_t c = 0;
        for (int k = 0; k <= i; k++) c = (uint8_t)(c + b[k]);
        if (c == b[i + 1]) { end = i; break; }
    }
    if (end < 14) return g;
    for (int i = 5; i <= 10; i++) { const int hi = b[i] >> 4; if (hi != 5 && hi != 3) return g; }
    const int laD = (b[5] & 15) * 10 + (b[6] & 15), laM = (b[7] & 15) * 10 + (b[8] & 15), laF = (b[9] & 15) * 10 + (b[10] & 15);
    if (laD > 89 || laM > 59 || laF > 99) return g;
    const double latSign = (b[8] >> 4) == 5 ? 1.0 : (b[8] >> 4) == 3 ? -1.0 : 0.0;
    const double lonSign = (b[10] >> 4) == 3 ? 1.0 : (b[10] >> 4) == 5 ? -1.0 : 0.0;
    if (latSign == 0.0 || lonSign == 0.0) return g;
    int loD = -1;
    const int v = b[11];
    if ((b[9] >> 4) == 5) {
        if (v >= 0x76 && v <= 0x7F) loD = v - 0x76;
        else if (v >= 0x6C && v <= 0x75) loD = 100 + v - 0x6C;
        else if (v >= 0x26 && v <= 0x6B) loD = 110 + v - 0x26;
    } else if ((b[9] >> 4) == 3 && v >= 0x26 && v <= 0x7F) loD = 10 + v - 0x26;
    const int m = b[12];
    int loM = -1;
    if (m >= 0x58 && m <= 0x61) loM = m - 0x58;
    else if (m >= 0x26 && m <= 0x57) loM = 10 + m - 0x26;
    const int fr = b[13] >= 0x1C && b[13] <= 0x7F ? b[13] - 0x1C : -1;
    if (loD < 0 || loM < 0 || fr < 0 || fr > 99 || loD > 179 || loM > 59) return g;
    g.lat = latSign * (laD + (laM + laF * 0.01) / 60.0);
    g.lon = lonSign * (loD + (loM + fr * 0.01) / 60.0);
    g.radio = ysfRadioName(b[4]);
    g.ok = true;
    return g;
}

} // namespace dect2::fusion
