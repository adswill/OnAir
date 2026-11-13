// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The demodulator of the ham decoders' shared audio front end (HF digital, HF ALE): upper or lower sideband, narrow FM or AM, and "Auto",
// which picks the upper sideband below 30 MHz and narrow FM at and above it (VHF / UHF SSTV and data, e.g. the ISS on 145.800 MHz).
#pragma once

namespace dect2 {

enum HfDemod { HfDemodUsb = 0, HfDemodLsb, HfDemodNfm, HfDemodAm, HfDemodAuto, HfDemodCount };

inline const char* hfDemodName(int d) {
    static const char* const n[] = {"USB", "LSB", "NFM", "AM", "Auto"};
    return d >= 0 && d < HfDemodCount ? n[d] : "USB";
}

// Auto (or anything out of range) to the one used at dialMhz: USB below 30 MHz, NFM at and above
inline int hfDemodResolve(int d, double dialMhz) {
    if (d >= HfDemodUsb && d <= HfDemodAm) return d;
    return dialMhz >= 30.0 ? HfDemodNfm : HfDemodUsb;
}

} // namespace dect2
