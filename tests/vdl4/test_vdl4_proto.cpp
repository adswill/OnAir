// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDL Mode 4 link layer without a radio: NRZI, HDLC, CRC, sync/DLS bursts.
#include "dect2/vdl4/vdl4_proto.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, msg, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " msg "\n", ##__VA_ARGS__); fails++; } } while (0)

int main() {
    Vdl4BurstFields f;
    f.src = (1u << 24) | 0x4ca2b1u;
    f.msgId = 3;
    f.info = vdl4DlsUserInfo("HELLO VDL4");
    auto frame = vdl4PackBurst(f);
    CHECK(vdl4FcsOk(frame.data(), frame.size()), "fcs");
    const auto line = vdl4BurstLineBits(vdl4HdlcBits({frame}));
    CHECK(line.size() > 80u, "line bits %zu", line.size());
    std::vector<uint8_t> data;
    CHECK(vdl4BurstDataBits(line, data), "burst data");
    const auto fr = vdl4HdlcFrames(data);
    CHECK(fr.size() == 1, "one frame");
    Vdl4BurstFields g;
    CHECK(vdl4ParseBurst(fr[0].data(), fr[0].size() - 2, g) && g.msgId == 3, "parse dls");
    Vdl4Message m;
    CHECK(vdl4DecodeMessage(g, 136.975e6, m) && m.userText.find("HELLO") != std::string::npos, "dls text");

    int latE = 0, lonE = 0;
    vdl4CprEncode(12.8557, -0.815, false, latE, lonE);
    CHECK(latE > 0 && lonE >= 0 && lonE < 16384, "cpr enc %d %d", latE, lonE);
    double lat = 0, lon = 0;
    CHECK(vdl4CprDecodeLocal(latE, lonE, false, 12.8, -0.82, lat, lon) && std::fabs(lat - 12.8557) < 0.02, "cpr dec %.4f %.4f", lat, lon);

    f.msgId = 1;
    f.info = vdl4SyncBurstInfo(51.47, -0.45, 35000, false, f.src, 1);
    frame = vdl4PackBurst(f);
    CHECK(vdl4ParseBurst(frame.data(), frame.size() - 2, g) && g.msgId == 1, "sync parse");
    CHECK(vdl4DecodeMessage(g, 136.9e6, m) && m.syncBurst, "sync decode");

    printf(fails ? "vdl4 proto: %d FAILED\n" : "vdl4 proto: all passed\n", fails);
    return fails ? 1 : 0;
}
