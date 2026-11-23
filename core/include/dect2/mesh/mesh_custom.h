// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The custom LoRa setting of a Mesh entry (bandwidth, spreading factor, coding rate, sync word, preamble, frequency) and the rules of the screen that edits it,
// kept here so a test can check them without a window (issue #659: "Custom" in the MeshCore preset list left the setting off, so its fields never appeared).
#pragma once
#include "dect2/mesh/mesh_proto.h"
#include <vector>

namespace dect2 {

struct MeshCustomLora {
    bool on = false;                    // the custom setting is the one searched (the band plan's presets are not)
    double mhz = 869.525, bwHz = 125000;
    int sf = 9, cr = 5, sync = 0x2B, preamble = 16;
};

// The bandwidths the screen offers: the SX126x set, then the 2.4 GHz ones (SX128x, Meshtastic LORA_24). 62.5 kHz is the Spanish SFNarrow and EU narrow presets.
inline const std::vector<double>& meshCustomBandwidthsHz() {
    static const std::vector<double> v = {7812.5, 10416.667, 15625, 20833.333, 31250, 41666.667, 62500, 125000, 250000, 500000, 812500, 1625000};
    return v;
}

// "Custom" picked in a preset list (the MeshCore radio presets): the custom setting becomes the one in use, on its own frequency, and its fields open.
// Returns true when that changes what is received (it was off, or the tuned frequency is not the custom one): the receiver restarts. tunedMhz is set to
// the custom frequency. Picking it again changes nothing.
inline bool meshChooseCustomPreset(MeshCustomLora& c, double& tunedMhz) {
    const bool changes = !c.on || (c.mhz > 0 && tunedMhz != c.mhz);
    c.on = true;
    if (c.mhz > 0) tunedMhz = c.mhz;
    return changes;
}

// The LoRa setting the receiver gets (MeshReceiver::setCustom): frequency 0, the tuned frequency is the custom one.
inline MeshLoraSettings meshCustomToLora(const MeshCustomLora& c) {
    MeshLoraSettings s;
    s.name = "Custom"; s.freqHz = 0;
    s.sf = c.sf; s.bwHz = c.bwHz; s.cr = c.cr; s.syncWord = (uint8_t)(c.sync & 0xFF); s.preamble = c.preamble;
    s.ldro = meshLoraLdro(c.sf, c.bwHz);
    return s;
}

} // namespace dect2
