// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mesh (LoRa), custom LoRa settings and the Meshtastic regions (issue #60): frames at non-default spreading factors, bandwidths
// (7.8 - 500 kHz, and the 812.5 kHz of 2.4 GHz LoRa), coding rates, sync words and preamble lengths, through the receiver with
// setCustom() at the rate the app picks for the bandwidth; a frame on a regional Meshtastic slot through setMeshtasticPlan(); the
// slot frequencies of every region and preset.
#include "dect2/dsp/gen_util.h"
#include "dect2/mesh/mesh_lora.h"
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

int main() {
    std::vector<uint8_t> key;
    CHECK(meshtasticExpandPsk({1}, key), "default key");
    MeshtasticNode node;
    node.num = 0x1234abcd; node.longName = "Custom test"; node.shortName = "CT";

    struct Case { int sf; double bwHz; int cr; uint8_t sync; int preamble; double freqHz; int proto; };   // proto 1 Meshtastic, 3 LoRa APRS
    const Case cases[] = {
        {9, 62.5e3, 7, 0x2B, 16, 869.400e6, 1},
        {10, 31.25e3, 6, 0x12, 8, 0, 3},
        {12, 125e3, 8, 0x2B, 16, 0, 1},
        {7, 500e3, 5, 0x2B, 12, 869.300e6, 1},
        {8, 41.7e3, 8, 0x12, 10, 0, 3},
        {11, 20.8e3, 5, 0x12, 8, 0, 3},
        {7, 7.8e3, 5, 0x12, 8, 0, 3},
        {6, 125e3, 5, 0x12, 12, 0, 3},
        {5, 250e3, 6, 0x2B, 12, 0, 1},
        {11, 812.5e3, 5, 0x2B, 16, 0, 1},
    };
    int n = 0;
    for (const Case& c : cases) {
        const double tuned = 869.525e6;
        MeshLoraSettings s;
        s.name = "Custom"; s.freqHz = c.freqHz; s.sf = c.sf; s.bwHz = c.bwHz; s.cr = c.cr; s.syncWord = c.sync; s.preamble = c.preamble;
        s.ldro = meshLoraLdro(c.sf, c.bwHz);
        lora::Params p;
        p.sf = s.sf; p.bwHz = s.bwHz; p.cr = s.cr; p.preamble = s.preamble; p.syncWord = s.syncWord; p.ldro = s.ldro;
        char text[64];
        snprintf(text, sizeof text, "SF%d %.1f kHz 4/%d", c.sf, c.bwHz / 1e3, c.cr);
        std::vector<uint8_t> pkt = c.proto == 1 ? meshtasticText(node, 0xFFFFFFFFu, 0x700 + n, 3, 3, text)
                                                : loraAprsFrame(std::string("LY1TST-7>APLRT1,WIDE1-1:>") + text);
        MeshReceiver rx;
        rx.setCustom(true, s);
        // a carrier error well inside the channel filter's margin (bandwidths under 62.5 kHz leave 40 % of it)
        const double cfo = std::min(800.0, 0.05 * c.bwHz);
        const MeshTelemetry t = run(rx, p, pkt, c.freqHz > 0 ? c.freqHz : tuned, tuned, rateFor(c.bwHz), cfo, 8);
        bool got = false;
        for (const auto& m : t.messages) got |= m.text.find(text) != std::string::npos;
        for (const auto& a : t.packets) got |= a.parsed && a.detail.find(text) != std::string::npos;
        for (const auto& st : t.aprsStations) got |= st.st.call == "LY1TST-7";
        printf("%-22s sync 0x%02X pre %2d%s: %zu decoders, %llu good frames, %llu CRC errors, %s\n", text, c.sync, c.preamble, s.ldro ? " LDRO" : "",
               t.decoders.size(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, got ? "decoded" : "MISSING");
        CHECK(t.custom && t.decoders.size() == 1 && t.decoders[0].inBand, "%s: custom decoder", text);
        CHECK(got && t.blocksOk == 1 && t.blocksBad == 0, "%s: not decoded", text);
        n++;
    }

    // every region and preset: a slot inside the region, the 2.4 GHz bandwidths in LORA_24
    const auto regions = meshtasticRegions(), presets = meshtasticPresetNames();
    CHECK(regions.size() >= 26 && presets.size() == 10, "lists %zu %zu", regions.size(), presets.size());
    {   // Finnish EU2 setup: SF8, 62.5 kHz, 4/8, centre frequency is the override 869.4312744 MHz
        MeshLoraSettings e;
        CHECK(meshtasticRegionPreset("EU_868", "EdgeFastLow", e) && e.sf == 8 && e.bwHz == 62500.0 && e.cr == 8 && e.freqHz == 869431274.0,
              "EdgeFastLow sf %d bw %.0f cr %d f %.1f", e.sf, e.bwHz, e.cr, e.freqHz);
    }
    for (const char* want : {"EU_433", "EU_868", "US", "ANZ", "IN", "KR", "TW", "RU", "CN", "JP", "NZ_865", "TH", "UA_433", "UA_868", "MY_433", "MY_919",
                             "SG_923", "PH_433", "PH_868", "PH_915", "LORA_24"}) {
        bool found = false;
        for (const auto& r : regions) found |= r == want;
        CHECK(found, "region %s", want);
    }
    int slots = 0;
    for (const auto& r : regions)
        for (const auto& pn : presets) {
            MeshLoraSettings s;
            double lo = 0, hi = 0;
            CHECK(meshtasticRegionRange(r, lo, hi), "range %s", r.c_str());
            if (!meshtasticRegionPreset(r, pn, s)) continue;   // a region narrower than the preset (EU_868 and the 500 kHz presets)
            slots++;
            CHECK(s.freqHz - s.bwHz / 2 >= lo - 1 && s.freqHz + s.bwHz / 2 <= hi + 1, "%s %s %.4f MHz outside", r.c_str(), pn.c_str(), s.freqHz / 1e6);
        }
    MeshLoraSettings s;
    CHECK(meshtasticRegionPreset("LORA_24", "LongFast", s) && s.bwHz == 812.5e3 && s.sf == 11, "LORA_24 LongFast");
    CHECK(meshtasticRegionPreset("LORA_24", "ShortTurbo", s) && s.bwHz == 1625e3, "LORA_24 ShortTurbo");
    CHECK(meshtasticRegionPreset("ANZ", "LongFast", s) && std::fabs(s.freqHz - 919.875e6) < 1, "ANZ LongFast %.4f", s.freqHz / 1e6);
    CHECK(!meshtasticRegionPreset("EU_868", "ShortTurbo", s), "EU_868 ShortTurbo has no slot");
    printf("%d region / preset slots\n", slots);

    // a regional plan: Meshtastic MediumSlow in ANZ, on its slot, found by the region plan (not a custom setting)
    {
        MeshLoraSettings ms;
        CHECK(meshtasticRegionPreset("ANZ", "MediumSlow", ms), "ANZ MediumSlow");
        lora::Params p;
        p.sf = ms.sf; p.bwHz = ms.bwHz; p.cr = ms.cr; p.preamble = ms.preamble; p.syncWord = ms.syncWord; p.ldro = ms.ldro;
        const auto data = meshtasticData(1, std::vector<uint8_t>({'a', 'n', 'z'}));
        const auto pkt = meshtasticRaw(0x2a0000aau, 0xFFFFFFFFu, 0x99, 3, 3, false, "MediumSlow", key, data);
        MeshReceiver rx;
        rx.setProtocols(1);
        rx.setMeshtasticPlan("ANZ", "MediumSlow");
        const MeshTelemetry t = run(rx, p, pkt, ms.freqHz, ms.freqHz, 2e6, 300, 8);
        bool got = false;
        for (const auto& m : t.messages) got |= m.text == "anz";
        printf("ANZ MediumSlow %.4f MHz: %s, plan %s\n", ms.freqHz / 1e6, got ? "decoded" : "MISSING", t.mtRegion.c_str());
        CHECK(got && t.mtRegion == "ANZ" && !t.custom, "ANZ MediumSlow");
    }

    // the ANZ band plan (setRegion 2, no Meshtastic region set): LongFast on 919.875 MHz, MeshCore AU (915.8 MHz) in the plan, no LoRaWAN
    {
        MeshLoraSettings lf;
        CHECK(meshtasticRegionPreset("ANZ", "LongFast", lf), "ANZ LongFast");
        lora::Params p;
        p.sf = lf.sf; p.bwHz = lf.bwHz; p.cr = lf.cr; p.preamble = lf.preamble; p.syncWord = lf.syncWord; p.ldro = lf.ldro;
        const auto data = meshtasticData(1, std::vector<uint8_t>({'s', 'c'}));
        const auto pkt = meshtasticRaw(0x2a0000abu, 0xFFFFFFFFu, 0x9a, 3, 3, false, "LongFast", key, data);
        MeshReceiver rx;
        rx.setProtocols(1 | 2 | 16);
        rx.setRegion(2);
        const MeshTelemetry t = run(rx, p, pkt, lf.freqHz, lf.freqHz, 2e6, 300, 8);
        bool got = false, mc = false, lw = false;
        for (const auto& m : t.messages) got |= m.text == "sc";
        for (const auto& d : t.decoders) { mc |= d.protocol == 2 && d.preset.find("Australia") == 0 && std::fabs(d.freqHz - 915.8e6) < 1; lw |= d.protocol == 5; }
        printf("ANZ plan LongFast %.4f MHz: %s, plan %s, MeshCore Australia %s\n", lf.freqHz / 1e6, got ? "decoded" : "MISSING", t.mtRegion.c_str(), mc ? "planned" : "MISSING");
        CHECK(got && t.region == 2 && t.mtRegion == "ANZ" && !t.custom, "ANZ plan LongFast");
        CHECK(mc && !lw, "ANZ plan: MeshCore AU, no LoRaWAN");
        const MeshNode* nd = t.nodes.empty() ? nullptr : &t.nodes[0];
        CHECK(nd && nd->packets == 1 && nd->firstHeard >= 0 && nd->firstHeard == nd->lastHeard && nd->snrMin == nd->snrMax && std::abs(nd->snrSum - nd->snrMin) < 1e-3 &&
              nd->encrypted == 0, "node statistics after one packet");
        // a device telemetry report: the node's last values, and one reading in the history with its time
        MeshtasticNode tn; tn.num = 0x2a0000ab;
        const auto tp = meshtasticTelemetry(tn, 0x9b, 3, 3, 87, 4.05, 3.5, 0.4, 3600);
        const MeshTelemetry t2 = run(rx, p, tp, lf.freqHz, lf.freqHz, 2e6, 300, 8);
        const MeshReading* rd = t2.readings.empty() ? nullptr : &t2.readings.back();
        CHECK(t2.readings.size() == 1 && rd && rd->id == "!2a0000ab" && std::abs(rd->batteryPct - 87) < 0.5 && std::abs(rd->voltage - 4.05) < 0.01 &&
              rd->uptimeS == 3600 && !rd->hasEnv && rd->unixTime > 1.7e9, "telemetry reading kept (%zu)", t2.readings.size());
        bool dev = false;
        for (const auto& n : t2.nodes) dev |= n.id == "!2a0000ab" && n.devSec >= 0 && std::abs(n.batteryPct - 87) < 0.5;
        CHECK(dev, "node battery with its time");
        tn.lat = -27.6; tn.lon = -48.62; tn.altM = 12;
        const MeshTelemetry t3 = run(rx, p, meshtasticPosition(tn, 0x9c, 3, 3, 1790000000u), lf.freqHz, lf.freqHz, 2e6, 300, 8);
        CHECK(t3.readings.size() == 2 && t3.readings.back().hasPosition && std::abs(t3.readings.back().lat + 27.6) < 1e-5 &&
              std::abs(t3.readings.back().lon + 48.62) < 1e-5 && t3.readings.back().batteryPct < 0, "position kept in the history");
    }

    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
