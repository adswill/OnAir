// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// What OnAir knows about each LimeSDR board, from the info string LMS_GetDeviceList gives ("LimeSDR Mini, media=USB 3.0, module=FT601,
// addr=24607:1027, serial=1D40CA8B2ED29A"). Pure functions, no library calls, so the tests can check them without a radio.
#pragma once
#include <cctype>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {
namespace native {

struct LimeBoard {
    bool mini = false;      // LimeSDR Mini (1.x or 2.x), not the LimeSDR-USB or a LimeNET board
    bool v2 = false;        // Mini 2.x (the info string says v2 / 2.x)
    bool usb3 = true;       // "media=USB 3.0" (a Mini on a USB 2 port says USB 2.0)
    std::string model;      // "LimeSDR Mini", "LimeSDR Mini 2", or the library's own name ("LimeSDR-USB")
    std::string serial;     // "serial=" value, may be empty
};

inline std::string limeLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline LimeBoard limeBoard(const std::string& info) {
    LimeBoard b;
    const size_t comma = info.find(',');
    const std::string head = comma == std::string::npos ? info : info.substr(0, comma);
    const std::string low = limeLower(head);
    b.mini = low.find("mini") != std::string::npos;
    // LimeSuite names a Mini 2 "LimeSDR Mini v2.x" (older strings: "LimeSDR-Mini_v2"); a 1.x board has no version or says v1.x
    b.v2 = b.mini && (low.find("v2") != std::string::npos || low.find("mini 2") != std::string::npos || low.find("mini2") != std::string::npos);
    b.model = b.mini ? (b.v2 ? "LimeSDR Mini 2" : "LimeSDR Mini") : head;
    b.usb3 = limeLower(info).find("media=usb 2") == std::string::npos;
    const size_t s = info.find("serial=");
    if (s != std::string::npos) {
        const size_t e = info.find(',', s);
        b.serial = info.substr(s + 7, e == std::string::npos ? std::string::npos : e - s - 7);
    }
    return b;
}

// Which boards take an external reference clock through LMS_SetClockFreq(LMS_CLOCK_EXTREF). The LimeSDR-USB (and the LimeNET boards) have a
// CLK IN connector. The LimeSDR Mini (1.x and 2.x) has no reference input socket, so the option is not offered for it. A Soapy-listed XTRX
// or LimeSDR-USB is covered by the generic SoapySDR clock-source setting instead.
inline bool limeHasExtRef(const LimeBoard& b) { return !b.mini; }

// The rates the Mini offers. The ADC runs at 30.72 Msps at most (its FT601 USB 3 link is the limit too); LimeSuite reaches the lower ones by
// decimation. A pair per rate: the driver picks the smallest one at or above what a mode asks for.
inline std::vector<std::pair<double, double>> limeMiniRates() {
    std::vector<std::pair<double, double>> r;
    for (double m : {1.0, 2.0, 4.0, 5.0, 8.0, 10.0, 15.0, 20.0, 30.72}) r.emplace_back(m * 1e6, m * 1e6);
    return r;
}
// What a Mini carries without losing samples on a USB 2 port (over the 12-bit packed link); on USB 3 there is no limit below 30.72 Msps
constexpr double kLimeMiniUsb2RateHz = 15e6;

// Where the receive input changes. The Mini's second low input is not wired (its list says "LNAL_NC"): LNAW below 1.7 GHz and LNAH above, the
// crossover LimeSuite's own band switching uses (LMS7_LimeSDR_mini::AutoRFPath). The LimeSDR-USB: LNAL below 1.5 GHz, LNAH above.
inline double limeCrossoverHz(const LimeBoard& b) { return b.mini ? 1.7e9 : 1.5e9; }

// The index to pick in the library's antenna list for a frequency, -1 = none usable. Names are matched exactly, so "LNAL_NC" is never taken;
// a board that does list a working LNAL gets it below the crossover, the wideband input is the fallback.
inline int limeAutoAntenna(const LimeBoard& b, double hz, const std::vector<std::string>& names) {
    const double x = limeCrossoverHz(b);
    int pick = -1, wide = -1;
    for (int i = 0; i < (int)names.size(); i++) {
        if (hz < x && names[i] == "LNAL") pick = i;
        if (hz >= x && names[i] == "LNAH") pick = i;
        if (names[i] == "LNAW") wide = i;
    }
    return pick >= 0 ? pick : wide;
}

// Overall RX gain the UI hands over (0-73 dB) as LMS_SetGaindB wants it. LimeSuite splits it over LNA (0-30 dB), TIA (0-12 dB) and PGA
// (-12 to 19 dB) itself, so one slider covers all three stages.
inline unsigned limeGainDb(double db) { return (unsigned)(db < 0 ? 0 : db > 73 ? 73 : (int)(db + 0.5)); }

// Said when the radio is found but will not open or initialise: the gateware / firmware in the board has to match the library
// (LimeUtil --update loads the images that came with that library); on Windows the Mini needs FTDI's driver, not WinUSB (Zadig).
inline std::string limeOpenHint(const LimeBoard& b, bool windows) {
    std::string s = ". If it is plugged in, its gateware or firmware may not match this LimeSuite library: run \"LimeUtil --update\" to load the matching images";
    if (b.mini && windows) s += ". On Windows the LimeSDR Mini needs FTDI's FT60x USB 3 driver (Windows Update or ftdichip.com), not a WinUSB driver from Zadig";
    else if (windows) s += ". On Windows the LimeSuite USB driver must be installed";
    return s;
}

} // namespace native
} // namespace dect2
