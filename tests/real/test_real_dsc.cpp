// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Marine DSC (MF/HF, 100 bd FSK) on a real recording: 22 s of single sideband audio with two ship-to-ship individual calls on 8291 kHz (from Dsc_examples.mp3 in
// the owner's downloads; origin of the recording unknown, see docs/REAL-SIGNALS.md). Stored at 12.5 kHz, 8 bit; the audio is played into the receiver as an
// IQ stream at 250 ksps (x20, linear interpolation, imaginary part 0) with the signal offset at the middle of the two tones (1700 Hz).
#include "dect2/marine/marine_rx.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const std::string path = std::string(DECT2_SOURCE_DIR) + "/tests/data/marine/dsc_mf_12500_s8.raw";
    std::ifstream f(path, std::ios::binary);
    CHECK(f.good(), "cannot open %s", path.c_str());
    if (!f.good()) return testSummary("real dsc");
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(raw.size() == 278865, "%zu samples", raw.size());
    const int up = 20;
    std::vector<cf32> x;
    x.reserve(raw.size() * up);
    for (size_t i = 0; i + 1 < raw.size(); i++) {
        const float a = (float)(int8_t)raw[i] / 128.f, b = (float)(int8_t)raw[i + 1] / 128.f;
        for (int k = 0; k < up; k++) x.push_back(cf32(a + (b - a) * (float)k / (float)up, 0.f));
    }
    MarineReceiver r;
    r.configure(250000);
    CHECK(r.ready(), "not ready");
    r.setSignalOffset(1700);
    r.setService(2);
    r.setLogCallback([](const std::string&) {});
    MarineTelemetry t, fin;
    uint64_t last = 0;
    for (size_t at = 0; at < x.size(); at += 65536) {
        r.feed(x.data() + at, std::min<size_t>(65536, x.size() - at));
        if (r.telemetry(t, last)) { last = t.seq; fin = t; }
    }
    int ok = 0, individual = 0, a = 0, b = 0;
    for (const auto& c : fin.dsc) {
        printf("  %s [ECC %s, EOS %d]\n", c.text.c_str(), c.eccOk ? "ok" : "bad", c.eos);
        if (!c.eccOk) continue;
        ok++;
        if (c.format == 120) individual++;
        if (c.fromMmsi == "005030001" && c.to.find("002320001") != std::string::npos) a++;
        if (c.fromMmsi == "002320001" && c.to.find("005030001") != std::string::npos) b++;
    }
    CHECK(fin.serviceActive == 2, "service %d", fin.serviceActive);
    CHECK(ok >= 2 && individual >= 2, "%d calls with a good ECC, %d individual calls (want 2)", ok, individual);
    CHECK(a >= 1 && b >= 1, "call 005030001 -> 002320001: %d, answer 002320001 -> 005030001: %d", a, b);
    return testSummary("real dsc");
}
