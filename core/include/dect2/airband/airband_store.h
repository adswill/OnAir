// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The airband channel list as one line of text for the settings file (#148).
#pragma once
#include "dect2/airband/airband_rx.h"
#include <string>
#include <vector>

namespace dect2 {
// "v2:" then one record per channel, records split by ';' and fields by ',': frequency, 8.33 flag, mute, solo, priority, label. Label
// bytes other than letters, digits, space and a few marks are %XX, so no control character, separator or line break reaches the file.
std::string airbandEncode(const std::vector<AirbandChannel>& v);
// Reads that, and the older forms (0x1e / 0x1f separators, and the '|' / ';' form of the macOS preferences). A record that is damaged
// or off the raster is skipped, the rest is kept.
std::vector<AirbandChannel> airbandDecode(const std::string& s);
}
