// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DCR, Japan's digital convenience radio (ARIB STD-T98 Volume 3, 4-level FSK): the channel plan of Tables 3-4, 3-5 and 3-6, 6.25 kHz apart.
//   154 MHz   licensed, channels 1 - 28 from 154.44375 MHz (20 - 28 are data only)
//   467 MHz   licensed, channels 1 - 65 from 467.00000 MHz, 66 - 75 from 465.096875 MHz
//   351 MHz   registered stations: channels 1 - 70 from 351.20000 MHz, 71 - 82 from 351.03125 MHz (1 W / 5 W), S1 - S5 from 351.16875 MHz and
//             S6 - S15 from 351.10625 MHz (1 W, may be used in the air); channel 15 (351.2875 MHz) is the calling channel
//   relay     licensed repeaters D1 - D10: input 465.034375 MHz + 6.25 kHz * (n - 1), output 468.796875 MHz + 6.25 kHz * (n - 1)
// Header only: the receiver, the tests and the generator share it.
#pragma once
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {
namespace dcr {

struct Channel {
    std::string label;      // "351 MHz ch 15", "351 MHz S5", "467 MHz ch 3", "154 MHz ch 20 (data)", "relay D3 input"
    double hz = 0;
    int band = 351;         // 154, 351, 467 (465 MHz channels 66 - 75 and the relay inputs count as 467), 468 (relay outputs)
    bool sky = false;       // S1 - S15
    bool dataOnly = false;  // 154 MHz channels 20 - 28
    bool calling = false;   // 351.2875 MHz
};

inline std::string lab(const char* f, int n) {
    char b[64];
    snprintf(b, sizeof b, f, n);
    return b;
}

// every channel of the plan (351 MHz registered, 467 / 465 MHz and 154 MHz licensed, the relay pairs)
inline const std::vector<Channel>& plan() {
    static const std::vector<Channel> p = [] {
        std::vector<Channel> v;
        auto add = [&](const std::string& l, double hz, int band, bool sky = false, bool data = false, bool calling = false) {
            Channel c; c.label = l; c.hz = hz; c.band = band; c.sky = sky; c.dataOnly = data; c.calling = calling; v.push_back(c);
        };
        for (int n = 1; n <= 82; n++) {
            const double hz = n <= 70 ? 351.2e6 + (n - 1) * 6250.0 : 351.03125e6 + (n - 71) * 6250.0;
            add(lab("351 MHz ch %d", n), hz, 351, false, false, n == 15);
        }
        for (int n = 1; n <= 5; n++) add(lab("351 MHz S%d", n), 351.16875e6 + (n - 1) * 6250.0, 351, true);
        for (int n = 6; n <= 15; n++) add(lab("351 MHz S%d", n), 351.10625e6 + (n - 6) * 6250.0, 351, true);
        for (int n = 1; n <= 75; n++) add(lab("467 MHz ch %d", n), n <= 65 ? 467.0e6 + (n - 1) * 6250.0 : 465.096875e6 + (n - 66) * 6250.0, 467);
        for (int n = 1; n <= 28; n++) add(lab(n >= 20 ? "154 MHz ch %d (data)" : "154 MHz ch %d", n), 154.44375e6 + (n - 1) * 6250.0, 154, false, n >= 20);
        for (int n = 1; n <= 10; n++) {
            add(lab("relay D%d input", n), 465.034375e6 + (n - 1) * 6250.0, 467);
            add(lab("relay D%d output", n), 468.796875e6 + (n - 1) * 6250.0, 468);
        }
        return v;
    }();
    return p;
}

// the plan channel within tolHz of hz, or nullptr
inline const Channel* findChannel(double hz, double tolHz = 1500.0) {
    const Channel* best = nullptr; double bd = tolHz;
    for (const Channel& c : plan()) { const double d = std::fabs(c.hz - hz); if (d <= bd) { bd = d; best = &c; } }
    return best;
}

// The 351 MHz channels only (the "scan the band" list): ch 1 - 82 and S1 - S15, 97 channels from 351.03125 to 351.63125 MHz
inline std::vector<const Channel*> channelsOfBand(int band) {
    std::vector<const Channel*> r;
    for (const Channel& c : plan()) if (c.band == band) r.push_back(&c);
    return r;
}

} // namespace dcr
} // namespace dect2
