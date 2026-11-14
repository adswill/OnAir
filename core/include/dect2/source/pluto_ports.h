// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// PlutoSDR antenna ports: the radio settings "rxport" (rx1 / rx2) and "txport" (tx1 / tx2) as a 0-based channel index. A Pluto+ (and a Pluto
// with the second channel enabled, 2R2T) has RX1/RX2 and TX1/TX2 SMA ports; everything else has the first only. Anything but the exact value
// "rx2" / "tx2" means the first port, so a missing or garbled setting keeps the behaviour every Pluto user has today. Pure, header-only.
#pragma once
#include <map>
#include <string>

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

} // namespace dect2
