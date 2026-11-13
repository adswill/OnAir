// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mesh session report (mesh_report.h): the summary lines, the nodes most heard first with their SNR range, hops, battery, position,
// and the packets that did not decrypt.
#include "dect2/mesh/mesh_report.h"
#include <cstdio>
#include <string>
using namespace dect2;

#include "test_util.h"

static bool has(const std::string& s, const std::string& x) { return s.find(x) != std::string::npos; }

int main() {
    MeshTelemetry t;
    t.timeSec = 4000;
    t.blocksOk = 98; t.blocksBad = 2;
    t.counts = {{1, "TEXT_MESSAGE_APP", 10}, {1, "NODEINFO_APP", 30}};
    MeshPacket enc; enc.crcOk = true; enc.protocol = 1; enc.decrypted = false; enc.timeSec = 100;
    MeshPacket ok = enc; ok.decrypted = true; ok.timeSec = 3700;
    t.packets = {enc, ok};
    MeshNode a; a.id = "!e326743b"; a.longName = "Sao Jose Barreiros"; a.shortName = "SJB1"; a.hwModel = "HELTEC_MESH_NODE_T114"; a.role = "CLIENT";
    a.packets = 4; a.snrMin = 27.5f; a.snrMax = 29.0f; a.snrSum = 4 * 28.25; a.hopsAway = 0; a.batteryPct = 101; a.firstHeard = 100; a.lastHeard = 3700;
    a.hasPosition = true; a.lat = -27.6; a.lon = -48.62; a.altM = 12;
    MeshNode b; b.id = "!62c27efc"; b.longName = "luanlmd"; b.packets = 1; b.snrMin = b.snrMax = 5.5f; b.snrSum = 5.5; b.hopsAway = 4; b.batteryPct = 63;
    b.encrypted = 1; b.firstHeard = b.lastHeard = b.devSec = 2000; b.hasEnv = true; b.tempC = 22.4; b.humidity = 79; b.pressureHpa = 1015; b.envSec = 2000;
    t.nodes = {b, a};
    MeshMessage m; m.timeSec = 3700; m.from = "!e326743b"; m.fromName = "Sao Jose Barreiros"; m.to = "^all"; m.channel = "LongFast"; m.text = "Teste 4";
    t.messages = {m};
    // now = 2026-10-14 15:00:00 UTC at t.timeSec: the first packet at 13:55:00, the last at 14:55:00
    const std::string r = meshReport(t, "ANZ 915  919.875 MHz", 1791990000.0, true);
    CHECK(has(r, "Period: 2026-10-14 13:55 - 14:55 (1 h 0 min)"), "period\n%s", r.c_str());
    CHECK(has(r, "Listening to: ANZ 915  919.875 MHz"), "plan");
    CHECK(has(r, "Frames: 98 good, 2 with a CRC error (2.0 %)"), "frames\n%s", r.c_str());
    CHECK(has(r, "Not decrypted (unknown channel key) among the last 2 packets: 1"), "encrypted packets");
    CHECK(has(r, "Packet types: NODEINFO_APP 30, TEXT_MESSAGE_APP 10"), "types, most first");
    CHECK(has(r, "Nodes: 2"), "node count");
    CHECK(r.find("Sao Jose Barreiros (SJB1)") < r.find("luanlmd"), "the most heard node first");
    CHECK(has(r, "4 packets, SNR 27.5 / 28.2 / 29.0 dB (min / mean / max), direct"), "SNR range\n%s", r.c_str());
    CHECK(has(r, "HELTEC_MESH_NODE_T114, CLIENT") && has(r, "  powered\n") && has(r, "position -27.60000, -48.62000, 12 m"), "hardware, power, position");
    CHECK(has(r, "1 packet (1 not decrypted), SNR 5.5 / 5.5 / 5.5 dB (min / mean / max), 4 hops") && has(r, "battery 63 % at 14:26"), "second node\n%s", r.c_str());
    CHECK(has(r, "heard 13:55 - 14:55") && has(r, "heard 14:26 - 14:26"), "heard times");
    CHECK(has(r, "Messages: 1\n14:55  Sao Jose Barreiros > LongFast: Teste 4"), "messages\n%s", r.c_str());
    CHECK(has(r, "  sensors 22.4 C, 79 %, 1015 hPa at 14:26 (last reading)"), "sensor reading with its time\n%s", r.c_str());
    printf("mesh report: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
