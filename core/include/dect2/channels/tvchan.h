// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// One row of the multi-channel TV table (several adjacent channels decoded from one wide capture, multitv.h).
#pragma once
#include <cstdint>
#include <string>

namespace dect2 {

struct TvChannelRow {
    int slot = 0;                 // position on the channel raster
    double offsetHz = 0;          // centre of the channel relative to the centre of the capture
    double levelDb = -200;        // level of the channel in the averaged spectrum (dB)
    double aboveDb = 0;           // over the quietest part of the span
    bool occupied = false;        // a signal in this slot
    bool running = false;         // a receiver is decoding it
    bool selected = false;        // the one that plays and shows in detail
    bool locked = false;          // transport stream flowing
    int state = 0;                // the receiver's own state (0 searching, 1 partly, 2 locked)
    float cpuPct = 0;             // the receiver's load: time spent against the signal time it handled
    uint64_t dropped = 0;         // samples this receiver lost (a live radio that is too slow)
    int services = 0;             // services in its list
    double snrDb = 0;
    std::string network;          // network name from its NIT
    // satellite ("Whole span" DVB-S/S2): the transponder
    bool sat = false;
    double bwHz = 0;              // occupied bandwidth
    double symbolRate = 0;        // from the spectrum, then the receiver's own
    std::string detail;           // "DVB-S2 QPSK 2/3"
};

} // namespace dect2
