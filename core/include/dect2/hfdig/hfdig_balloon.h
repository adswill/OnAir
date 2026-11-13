// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// WSPR balloon telemetry spots (the S4 / U4B / Traquito convention): a special callsign starting 0, 1 or Q with a digit third, where callsign, locator and power carry altitude, the 5th and 6th locator
// letters, temperature, battery, speed and GPS status. Formulas from the QRP Labs S4 page; simulated only. The page's own worked example prints 36 C and 3.8 V where these formulas give 38 C and 3.85 V for the same spot.
#pragma once
#include <cstdio>
#include <string>

namespace dect2 {

struct WsprBalloon { bool ok = false; std::string sub; int altM = 0; int tempC = 0; double batV = 0; int speedKt = 0; bool gpsOk = false, sats8 = false; };

inline WsprBalloon wsprBalloonDecode(const std::string& call, const std::string& grid, int dbm) {
    WsprBalloon b;
    if (call.size() != 6 || grid.size() != 4) return b;
    if (call[0] != '0' && call[0] != '1' && call[0] != 'Q') return b;
    if (call[2] < '0' || call[2] > '9') return b;
    auto v36 = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : -1; };
    const int d2 = v36(call[1]);
    if (d2 < 0) return b;
    int n = d2;
    for (int i = 3; i < 6; i++) { const int v = call[(size_t)i] - 'A'; if (v < 0 || v >= 26) return b; n = n * 26 + v; }
    const int S = n / 1068, A = n % 1068;
    if (S >= 576) return b;
    b.sub = std::string(1, (char)('A' + S / 24)) + (char)('A' + S % 24);
    b.altM = A * 20;
    static const int kPow[19] = {0, 3, 7, 10, 13, 17, 20, 23, 27, 30, 33, 37, 40, 43, 47, 50, 53, 57, 60};
    int p = -1;
    for (int i = 0; i < 19; i++) if (kPow[i] == dbm) p = i;
    if (p < 0) return b;
    const int g1 = grid[0] - 'A', g2 = grid[1] - 'A', g3 = grid[2] - '0', g4 = grid[3] - '0';
    if (g1 < 0 || g1 >= 18 || g2 < 0 || g2 >= 18 || g3 < 0 || g3 > 9 || g4 < 0 || g4 > 9) return b;
    int m = ((((g1 * 18 + g2) * 10 + g3) * 10 + g4) * 19) + p;
    b.sats8 = m & 1; m >>= 1;
    b.gpsOk = m & 1; m >>= 1;
    b.speedKt = (m % 42) * 2; m /= 42;
    const int bat = m % 40;
    b.batV = 3.0 + bat * 0.05;
    m /= 40;
    b.tempC = m - 50;
    b.ok = true;
    return b;
}

inline std::string wsprBalloonText(const WsprBalloon& b) {
    char s[128];
    snprintf(s, sizeof s, "balloon: %s, %d m, %d C, %.2f V, %d kt%s", b.sub.c_str(), b.altM, b.tempC, b.batV, b.speedKt, b.gpsOk ? "" : ", no GPS");
    return s;
}

} // namespace dect2
