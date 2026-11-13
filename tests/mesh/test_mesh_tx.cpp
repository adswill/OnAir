// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mesh transmit identity: PSK / node ID parsing, the packets built from a user's own node, decoded back with the receiver's decoder
// (names, node ID, channel, PSK, hop limit, callsign prefix), and warnings that never refuse anything.
#include "dect2/mesh/mesh_proto.h"
#include "dect2/mesh/mesh_tx.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static bool hasWarn(const std::vector<std::string>& w, const char* part) {
    for (const auto& s : w) if (s.find(part) != std::string::npos) return true;
    return false;
}

int main() {
    // PSK forms
    std::vector<uint8_t> k;
    CHECK(meshTxParsePsk("default", k) && k.size() == 16 && k[0] == 0xd4, "default");
    CHECK(meshTxParsePsk("AQ==", k) && k.size() == 16 && k[0] == 0xd4, "AQ==");
    CHECK(meshTxParsePsk("none", k) && k.empty(), "none");
    CHECK(meshTxParsePsk("000102030405060708090a0b0c0d0e0f", k) && k.size() == 16 && k[15] == 15, "hex16");
    CHECK(meshTxParsePsk("000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f", k) && k.size() == 32, "hex32");
    CHECK(meshTxParsePsk("AAECAwQFBgcICQoLDA0ODw==", k) && k.size() == 16 && k[15] == 15, "base64");
    CHECK(!meshTxParsePsk("zz!!", k) && k.empty(), "bad psk");
    CHECK(!meshTxParsePsk("AAEC", k), "3 byte psk");
    // node IDs
    uint32_t n = 0;
    CHECK(meshTxParseNodeId("!1a2b3c4d", n) && n == 0x1a2b3c4d, "!id");
    CHECK(meshTxParseNodeId("1A2B3C4D", n) && n == 0x1a2b3c4d, "id");
    CHECK(!meshTxParseNodeId("1a2b", n) && !meshTxParseNodeId("!1a2b3c4g", n), "bad ids");
    MeshTxIdentity a;
    a.callsign = "DL1ABC";
    const uint32_t d1 = meshTxNodeNum(a);
    CHECK(d1 >= 0x100 && d1 != 0xFFFFFFFFu && d1 == meshTxNodeNum(a), "derived id stable");
    a.callsign = "dl1abc ";
    CHECK(meshTxNodeNum(a) == d1, "derived id ignores case and spaces");
    a.nodeId = "!00000000";
    CHECK(meshTxNodeNum(a) == d1, "reserved id replaced");

    // a complete transmission decoded by the receiver's decoder
    MeshTxIdentity id;
    id.longName = "Kris Test Station"; id.shortName = "KRIS"; id.nodeId = "!deadbeef";
    id.channelName = "OnAirTest"; id.psk = "AAECAwQFBgcICQoLDA0ODw=="; id.hopLimit = 5;
    id.callsign = "LY1XYZ"; id.callsignInMessage = true;
    const auto pk = meshTxPackets(id, "hello mesh", 1000);
    CHECK(pk.size() == 2 && pk[0].what == "node info" && pk[1].what == "text", "two packets");
    MeshProto proto;
    CHECK(proto.addMeshtasticChannel("OnAirTest", "AAECAwQFBgcICQoLDA0ODw=="), "add channel");
    MeshRadioInfo radio;
    if (pk.size() == 2) {
        const MeshDecodeResult ni = proto.decodeMeshtastic(pk[0].bytes.data(), pk[0].bytes.size(), radio);
        CHECK(ni.ok && !ni.nodes.empty(), "nodeinfo decoded");
        if (!ni.nodes.empty()) {
            CHECK(ni.nodes[0].longName == "Kris Test Station", "long name '%s'", ni.nodes[0].longName.c_str());
            CHECK(ni.nodes[0].shortName == "KRIS", "short name '%s'", ni.nodes[0].shortName.c_str());
            CHECK(ni.nodes[0].nodeId == "!deadbeef", "node id '%s'", ni.nodes[0].nodeId.c_str());
        }
        const MeshDecodeResult tx = proto.decodeMeshtastic(pk[1].bytes.data(), pk[1].bytes.size(), radio);
        CHECK(tx.ok && !tx.messages.empty(), "text decoded");
        if (!tx.messages.empty()) {
            CHECK(tx.messages[0].text == "LY1XYZ: hello mesh", "text '%s'", tx.messages[0].text.c_str());
            CHECK(tx.messages[0].channel == "OnAirTest", "channel '%s'", tx.messages[0].channel.c_str());
            CHECK(tx.messages[0].from == "!deadbeef", "from '%s'", tx.messages[0].from.c_str());
            CHECK(tx.packet.hopLimit == 5 && tx.packet.hopStart == 5, "hops %d/%d", tx.packet.hopLimit, tx.packet.hopStart);
            CHECK(tx.messages[0].packetId == 1001, "packet id %u", tx.messages[0].packetId);
        }
        // a receiver without the channel's key cannot read it
        MeshProto other;
        const MeshDecodeResult nk = other.decodeMeshtastic(pk[1].bytes.data(), pk[1].bytes.size(), radio);
        CHECK(nk.messages.empty(), "decoded without the PSK");
    }
    // no callsign prefix unless asked; no node info when switched off; the short name is cut to 4 characters; hop limit is clamped
    id.callsignInMessage = false; id.sendNodeInfo = false; id.shortName = "TOOLONG"; id.hopLimit = 12;
    const auto p2 = meshTxPackets(id, "plain", 7);
    CHECK(p2.size() == 1 && p2[0].what == "text", "text only");
    if (!p2.empty()) {
        const MeshDecodeResult r = proto.decodeMeshtastic(p2[0].bytes.data(), p2[0].bytes.size(), radio);
        CHECK(!r.messages.empty() && r.messages[0].text == "plain", "plain text");
        CHECK(r.packet.hopLimit == 7, "hop clamp %d", r.packet.hopLimit);
    }
    id.sendNodeInfo = true;
    const auto p3 = meshTxPackets(id, "x", 7);
    if (!p3.empty()) {
        const MeshDecodeResult r = proto.decodeMeshtastic(p3[0].bytes.data(), p3[0].bytes.size(), radio);
        CHECK(!r.nodes.empty() && r.nodes[0].shortName == "TOOL", "short name cut '%s'", r.nodes.empty() ? "" : r.nodes[0].shortName.c_str());
    }
    // a bad PSK still produces packets (default key), with a warning
    MeshTxIdentity bad;
    bad.psk = "nonsense!!";
    CHECK(!meshTxPackets(bad, "t", 1).empty(), "bad psk still builds");
    // warnings are advice, never a refusal
    CHECK(hasWarn(meshTxWarnings(bad, "t"), "PSK"), "psk warning");
    MeshTxIdentity w;
    w.shortName = "ABCDEFG"; w.hopLimit = 6; w.psk = "none"; w.callsignInMessage = true; w.nodeId = "xyz";
    const auto ws = meshTxWarnings(w, "t");
    CHECK(hasWarn(ws, "short name") && hasWarn(ws, "hop limit") && hasWarn(ws, "unencrypted") && hasWarn(ws, "callsign") && hasWarn(ws, "node ID"), "%zu warnings", ws.size());
    CHECK(meshTxWarnings(MeshTxIdentity(), "t").empty(), "defaults warn about nothing");
    return testSummary("mesh_tx");
}
