// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Issue #659: "Custom" in the Mesh (MeshCore) radio preset list did not unlock the custom LoRa setting, so a narrow network (62.5 kHz SFNarrow) could not be
// entered. Choosing Custom turns the custom setting on, on its own frequency; every bandwidth of the list (62.5 kHz among them) and every built-in MeshCore
// preset then reaches the receiver and decodes a frame. In memory: no hardware.
#include "dect2/dsp/gen_util.h"
#include "dect2/mesh/mesh_custom.h"
#include "dect2/mesh/mesh_lora.h"
#include "dect2/mesh/mesh_presets.h"
#include "dect2/mesh/mesh_proto.h"
#include "dect2/mesh/mesh_rx.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;

#include "../common/test_util.h"

// renders one frame (carrier error cfoHz) at freqHz with the radio tuned to tuned, feeds it, returns the report
static MeshTelemetry run(MeshReceiver& rx, const lora::Params& p, const std::vector<uint8_t>& pkt, double freqHz, double tuned, double rate,
                         double cfoHz, double snrDb) {
    const double off = -meshTuning().tuneOffsetHz, noise = 0.03;
    lora::TxFrame fr;
    fr.p = p;
    fr.data = lora::encode(p, pkt.data(), pkt.size());
    fr.startSec = 0.05;
    fr.freqHz = freqHz - tuned + off + cfoHz;
    fr.sroPpm = 3;
    fr.amp = (float)std::sqrt(std::pow(10.0, snrDb / 10.0) * noise * noise * p.bwHz / rate);
    std::vector<cf32> x((size_t)((fr.endSec() + 0.2 + 64 * lora::symbolSeconds(p.sf, p.bwHz)) * rate), cf32(0, 0));
    fr.render(x.data(), x.size(), 0, rate);
    genutil::NoiseSource ns(7);
    ns.add(x.data(), x.size(), (float)(noise / std::sqrt(2.0)));
    rx.configure(rate);
    rx.setSignalOffset(off);
    rx.setTunedHz(tuned);
    for (size_t i = 0; i < x.size(); i += 65536) rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
    MeshTelemetry t;
    rx.telemetry(t, 0);
    return t;
}


static double rateFor(double bwHz) { return bwHz > 500e3 ? 4e6 : 2e6; }   // as mesh_ui.cpp meshSampleRate()

// the screen's receiving chain for a custom setting: frame at the custom frequency, receiver set from meshCustomToLora
static bool decodes(const MeshCustomLora& c, const MeshtasticNode& node, int seq, const char* what) {
    const MeshLoraSettings s = meshCustomToLora(c);
    lora::Params p;
    p.sf = s.sf; p.bwHz = s.bwHz; p.cr = s.cr; p.preamble = s.preamble; p.syncWord = s.syncWord; p.ldro = s.ldro;
    char text[64];
    snprintf(text, sizeof text, "659 %s", what);
    const std::vector<uint8_t> pkt = meshtasticText(node, 0xFFFFFFFFu, 0x6590 + seq, 3, 3, text);
    MeshReceiver rx;
    rx.setCustom(c.on, s);
    const double tuned = c.mhz * 1e6;
    const MeshTelemetry t = run(rx, p, pkt, tuned, tuned, rateFor(c.bwHz), std::min(800.0, 0.05 * c.bwHz), 8);
    // a frame heard on the custom setting, its CRC good (a sync word 0x12 network is not Meshtastic: its text is not shown as one)
    return t.custom && t.blocksOk == 1 && t.blocksBad == 0;
}

int main() {
    std::vector<uint8_t> key;
    CHECK(meshtasticExpandPsk({1}, key), "default key");
    MeshtasticNode node;
    node.num = 0x1234abcd; node.longName = "659"; node.shortName = "659";

    // choosing Custom: off before, on after, on its own frequency; the receiver restarts only when something changed
    {
        MeshCustomLora c;
        c.mhz = 869.618; c.bwHz = 250000;
        double tuned = 869.525;
        CHECK(!c.on, "the custom setting starts off");
        CHECK(meshChooseCustomPreset(c, tuned), "choosing Custom changes the reception");
        CHECK(c.on, "choosing Custom turns the custom setting on (its fields open)");
        CHECK(std::fabs(tuned - 869.618) < 1e-9, "the tuned frequency is the custom one: %.4f", tuned);
        CHECK(!meshChooseCustomPreset(c, tuned), "choosing it again changes nothing");
        c.bwHz = 62500;   // the field is editable now: 62.5 kHz reaches the demodulator setting
        const MeshLoraSettings s = meshCustomToLora(c);
        CHECK(s.bwHz == 62500 && s.freqHz == 0 && s.syncWord == 0x2B, "62.5 kHz in the receiver's setting: %.1f", s.bwHz);
        double other = 868.0;
        MeshCustomLora d; d.on = true; d.mhz = 869.618;
        CHECK(meshChooseCustomPreset(d, other) && std::fabs(other - 869.618) < 1e-9, "Custom while on, on another frequency, returns to the custom frequency");
    }

    // the Spanish SFNarrow setting: SF7, 62.5 kHz, 4/5 on 869.618 MHz, through the receiver
    {
        MeshCustomLora c; c.on = true; c.mhz = 869.618; c.bwHz = 62500; c.sf = 7; c.cr = 5; c.sync = 0x2B; c.preamble = 16;
        CHECK(decodes(c, node, 0, "SFNarrow SF7 62.5 kHz"), "SFNarrow decoded");
    }

    // every bandwidth of the list, at two spreading factors
    int seq = 1;
    for (double bw : meshCustomBandwidthsHz()) {
        for (int sf : {7, 10}) {
            if (bw >= 1e6 && sf == 10) continue;   // a long frame at 4 Msps: the short one is enough there
            MeshCustomLora c; c.on = true; c.mhz = 869.4; c.bwHz = bw; c.sf = sf; c.cr = 5; c.sync = 0x2B; c.preamble = 16;
            char what[48]; snprintf(what, sizeof what, "bw %.1f sf %d", bw / 1e3, sf);
            CHECK(decodes(c, node, seq++, what), "%s not decoded", what);
        }
    }

    // every built-in MeshCore preset, entered as the custom setting
    const auto presets = meshCoreBuiltinPresets();
    CHECK(!presets.empty(), "built-in presets");
    for (const auto& p : presets) {
        const MeshLoraSettings l = meshCorePresetToLora(p);
        MeshCustomLora c; c.on = true; c.mhz = p.mhz; c.bwHz = l.bwHz; c.sf = std::min(l.sf, 8); c.cr = l.cr; c.sync = l.syncWord; c.preamble = l.preamble;
        char what[96]; snprintf(what, sizeof what, "preset %s", p.title.c_str());
        CHECK(decodes(c, node, seq++, what), "%s not decoded", what);
    }
    printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
