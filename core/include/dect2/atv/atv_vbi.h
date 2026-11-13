// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Data in the vertical blanking interval of an analogue picture: teletext (EN 300 706, system B), wide screen signalling (EN 300 294, line 23) and line 21 closed captions (EIA-608,
// its timing from memory of the standard, not checked against it). Works on the video of one line as a function of the time after the sync's leading edge, black 0, white 1.
#pragma once
#include "dect2/tv/teletext.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct AtvVbiStatus {
    uint64_t ttxPackets = 0;
    bool wssValid = false; int wssAspect = -1;      // the 4 aspect bits b0 (msb here) .. b3
    std::string wssText;
    std::string captions;                           // the last 200 characters of line 21 field 1
};

class AtvVbi {
public:
    AtvVbi();
    using Level = std::function<float(double us)>;
    void reset();
    bool line625(const Level& v);                   // a blanking line of a 625-line picture: teletext or WSS (true: data found)
    bool line525(const Level& v);                   // a blanking line of a 525-line picture: captions
    const AtvVbiStatus& status() const { return st_; }
    std::shared_ptr<TeletextDecoder> teletext() const { return ttx_; }
private:
    bool tryTeletext(const Level& v);
    bool tryWss(const Level& v);
    AtvVbiStatus st_;
    std::shared_ptr<TeletextDecoder> ttx_;
};

const char* atvWssAspectName(int bits);             // the 4 bits b0 b1 b2 b3 as written in EN 300 294 table 1
// for the test transmitter: the transmitted bits of a teletext packet (clock run-in, framing code, 42 bytes LSB first); row 0: a header of page number; rows 1..23: 40 characters
std::vector<uint8_t> atvTtxPacketBits(int mag, int row, int page, const std::string& text);
std::vector<uint8_t> atvWssElements(int aspectBits);    // 5 MHz elements of line 23
std::vector<uint8_t> atvCcBits(uint8_t c1, uint8_t c2); // start bits and two bytes with odd parity

} // namespace dect2
