// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// PlutoSDR antenna ports: the radio settings "rxport" (rx1 / rx2) and "txport" (tx1 / tx2) as a 0-based channel index. A Pluto+ (and a Pluto
// with the second channel enabled, 2R2T) has RX1/RX2 and TX1/TX2 SMA ports; everything else has the first only. Anything but the exact value
// "rx2" / "tx2" means the first port, so a missing or garbled setting keeps the behaviour every Pluto user has today. Pure, header-only.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <complex>

namespace dect2 {

inline int plutoPortIndex(const std::map<std::string, std::string>& radio, const std::string& key, const std::string& second) {
    const auto it = radio.find(key);
    return it != radio.end() && it->second == second ? 1 : 0;
}
inline int plutoRxPort(const std::map<std::string, std::string>& radio) { return plutoPortIndex(radio, "rxport", "rx2"); }
// An explicit TxTune::channel above 0 wins; otherwise the "txport" setting
inline int plutoTxPort(const std::map<std::string, std::string>& radio, int explicitChannel) {
    return explicitChannel > 0 ? explicitChannel : plutoPortIndex(radio, "txport", "tx2");
}

// Both receive inputs at once (issue #398): the setting "dualrx" is on only for the exact value "1" / "true" (off by default, one input as always).
// The AD9361's RX1 and RX2 share one LO and one sample clock, so both inputs are on one frequency and one rate. With two inputs the radio's
// buffer holds frames of I1 Q1 I2 Q2 (16 bits each, 12 bits used); this splits them into the two streams. Returns the frames split.
inline bool plutoDualRx(const std::map<std::string, std::string>& radio) {
    const auto it = radio.find("dualrx");
    return it != radio.end() && (it->second == "1" || it->second == "true");
}
inline size_t plutoSplitDual(const int16_t* p, size_t frames, std::complex<float>* a, std::complex<float>* b) {
    for (size_t k = 0; k < frames; k++) {
        a[k] = {p[4 * k] / 2048.f, p[4 * k + 1] / 2048.f};
        b[k] = {p[4 * k + 2] / 2048.f, p[4 * k + 3] / 2048.f};
    }
    return frames;
}
// The second input's list entry is the first one's address with this suffix
inline const char* plutoRx2Suffix() { return "#rx2"; }
inline bool plutoIsRx2Uri(const std::string& uri) { return uri.size() > 4 && uri.compare(uri.size() - 4, 4, "#rx2") == 0; }
inline std::string plutoBaseUri(const std::string& uri) { return plutoIsRx2Uri(uri) ? uri.substr(0, uri.size() - 4) : uri; }

} // namespace dect2
