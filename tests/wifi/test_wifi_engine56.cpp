// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi 5 and 6 GHz through the engine (setStandard(56), synthetic source modeOpt[0] = 1 / 2): the receiver learns the band from the radio's
// centre frequency, the OFDM chain decodes the scene, the mode accepts the frequencies of both bands and reports the band and channel.
#include "dect2/wifi/wifi_chan.h"
#include "dect2/wifi/wifi_rx.h"
#include "dect2/wifi/wifi_tel.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static void play(int band, int ch) {
    const ModeTuning mt = wifiTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = wifi::bandChannelMhz(band, ch) * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 56;
    t.synth.snrDb = 28;
    t.synth.modeOpt[0] = band;
    t.synth.modeOpt[1] = ch;
    FileOptions fo;
    e.setStandard(56);
    CHECK(e.start(dev, t, fo), "engine start");
    RxTelemetry rx;
    uint64_t seq = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 3.0) {
        if (e.latestRx(rx, seq)) seq = rx.seq;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const WifiTelemetry& d = rx.wifi;
    printf("  %s ch %d: %s, %llu OFDM frames\n", wifi::bandName(band), ch, wifiSummary(d).c_str(), (unsigned long long)d.ofdmFrames);
    CHECK(rx.standard == 55 && rx.rateOk, "standard %d", rx.standard);
    CHECK(d.band == band && d.channel == ch && d.aps.size() >= 3 && d.ofdmFrames > 100 && d.blocksBad == 0, "%s ch %d: band %d ch %d, %zu APs, %llu frames", wifi::bandName(band), ch, d.band, d.channel, d.aps.size(),
          (unsigned long long)d.ofdmFrames);
    if (band == wifi::Band6) {
        bool fils = false;
        for (const auto& a : d.aps) fils |= a.fils;
        CHECK(fils && d.filsFrames > 10, "FILS discovery frames %llu", (unsigned long long)d.filsFrames);
    }
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
}

int main() {
    const ModeTuning mt = wifiTuning();
    CHECK(mt.minMhz <= 2400 && mt.maxMhz >= 7125 && mt.minSampleRate == 20e6, "tuning range %.0f - %.0f MHz", mt.minMhz, mt.maxMhz);
    play(wifi::Band5, 149);
    play(wifi::Band6, 37);
    return testSummary("wifi engine 5/6 GHz");
}
