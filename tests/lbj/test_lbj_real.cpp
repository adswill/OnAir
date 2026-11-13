// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Optional: the reporter's recordings from issue #153 (not in the repository). Looks in $LBJ_SAMPLE_DIR, then ~/Downloads, for
// onair_lbj_821.288MHz_0.96Msps_20261016-163822.cs8 (HackRF, int8 IQ, centre 821.288 MHz); skips (exit 0) when the file is absent.
// The file holds one train (19533, 67 km/h, near km 38) that moves 0.2 km between messages 11 s apart.
#include "dect2/lbj/lbj_modem.h"
#include "dect2/lbj/lbj_proto.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static FILE* openSample(const char* name) {
    std::vector<std::string> dirs;
    if (const char* d = std::getenv("LBJ_SAMPLE_DIR")) dirs.push_back(d);
    if (const char* h = std::getenv("HOME")) dirs.push_back(std::string(h) + "/Downloads");
    for (const auto& d : dirs) {
        FILE* f = std::fopen((d + "/" + name).c_str(), "rb");
        if (f) return f;
    }
    return nullptr;
}

int main() {
    FILE* f = openSample("onair_lbj_821.288MHz_0.96Msps_20261016-163822.cs8");
    if (!f) { printf("test_lbj_real: SKIPPED (sample recording not found)\n"); return 0; }
    lbj::Modem m;
    m.configure(960000);
    m.setOffset(-50500);              // 821.2375 MHz is 50.5 kHz below the recording's centre
    lbj::LbjSessionState st;
    std::vector<lbj::Burst> bursts;
    std::vector<cf32> x(32768);
    std::vector<uint8_t> raw(2 * x.size());
    std::vector<LbjMessage> msgs;
    for (size_t got; (got = std::fread(raw.data(), 2, x.size(), f)) > 0;) {
        for (size_t i = 0; i < got; i++) x[i] = cf32((int8_t)raw[2 * i] / 128.f, (int8_t)raw[2 * i + 1] / 128.f);
        bursts.clear();
        m.feed(x.data(), got, bursts);
        for (const auto& b : bursts)
            for (const auto& p : lbj::pagesFromBurst(b)) {
                LbjMessage o;
                if (lbj::parsePage(p, b.timeSec, st, o)) msgs.push_back(o);
            }
    }
    std::fclose(f);
    int full = 0;
    for (const auto& o : msgs) if (o.train == "19533" && o.speedKmh == "67" && o.direction == "down" && (o.kmMarker == "38.2" || o.kmMarker == "38.5" || o.kmMarker == "38.7")) full++;
    CHECK(full >= 2, "only %d of the three clean messages of train 19533 decoded", full);
    for (const auto& o : msgs) CHECK(o.train == "19533", "unexpected train %s", o.train.c_str());
    return testSummary("test_lbj_real");
}
