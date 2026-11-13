// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Several receivers in one process (docs/multi-sdr.md): RX 1 decodes ADS-B while RX 2 decodes ACARS, a radio opens once only, each
// receiver keeps its own results, stopping RX 2 leaves RX 1 running.
#include "dect2/adsb/adsb_gen.h"
#include "dect2/acars/acars_gen.h"
#include "dect2/engine/device_claims.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <unistd.h>
using namespace dect2;
namespace fs = std::filesystem;
#include "../common/test_util.h"

static bool waitRx(Engine& e, double secs, bool (*ok)(const RxTelemetry&)) {
    RxTelemetry rx;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
        if (e.latestRx(rx, 0) && ok(rx)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}
static size_t nFiles(const fs::path& d) { size_t n = 0; std::error_code ec; for (auto& f : fs::directory_iterator(d, ec)) { (void)f; n++; } return n; }

int main() {
    // one radio opens once
    DeviceInfo radio; radio.kind = DeviceInfo::Soapy; radio.board = "fake"; radio.serial = "0001"; radio.name = "fake radio";
    DeviceInfo radio2 = radio; radio2.serial = "0002";
    int holder = 0;
    CHECK(claimDevice(radio, 1, &holder), "first claim");
    CHECK(claimDevice(radio, 1, &holder), "same receiver again");
    CHECK(!claimDevice(radio, 2, &holder) && holder == 1, "second receiver got the radio (holder %d)", holder);
    CHECK(claimDevice(radio2, 2, &holder), "another radio");
    CHECK(deviceHolder(radio) == 1 && deviceHolder(radio2) == 2, "holders");
    {   // the engine refuses a radio another receiver holds, before it opens anything
        Engine e2; e2.setReceiverId(2);
        FileOptions fo; TuneSettings t;
        CHECK(!e2.start(radio, t, fo), "RX 2 started a radio RX 1 holds");
    }
    {   // two RTL-SDR dongles with the same serial (the factory 00000001) are two radios (#328)
        DeviceInfo d0; d0.kind = DeviceInfo::Native; d0.board = "rtlsdr"; d0.serial = "00000001"; d0.nativeArgs = "0"; d0.name = "RTL-SDR 00000001";
        DeviceInfo d1 = d0; d1.nativeArgs = "1";
        CHECK(deviceClaimKey(d0) != deviceClaimKey(d1), "two dongles with one serial share a claim key");
        CHECK(claimDevice(d0, 5, &holder) && claimDevice(d1, 6, &holder), "second dongle with the same serial refused (holder %d)", holder);
        CHECK(!claimDevice(d0, 6, &holder) && holder == 5, "the first dongle is held by RX 5");
        releaseDevice(5); releaseDevice(6);
    }
    releaseDevice(1);
    CHECK(deviceHolder(radio) == 0 && claimDevice(radio, 2, &holder), "released radio taken by RX 2");
    releaseDevice(2);
    DeviceInfo synth;   // the test signal and files: any number of receivers
    CHECK(claimDevice(synth, 1) && claimDevice(synth, 2), "synthetic source is shareable");

    const fs::path base = fs::temp_directory_path() / ("multi_rx_" + std::to_string(::getpid()));
    fs::remove_all(base);
    Engine rx1, rx2;
    rx1.setReceiverId(1); rx2.setReceiverId(2);
    rx1.setResultsDir((base / "results").string());
    rx2.setResultsDir((base / "results_rx2").string());
    FileOptions fo;
    TuneSettings a;
    a.centerHz = 1090e6; a.bandwidthMhz = 2; a.sampleRate = 2e6;
    a.synth.mode = 13; a.synth.pace = 8; a.synth.snrDb = 30; a.synth.modeOpt[0] = 12;
    TuneSettings c;
    c.centerHz = 131.725e6; c.bandwidthMhz = 2; c.sampleRate = 2e6;
    c.synth.mode = 18; c.synth.snrDb = 30; c.synth.modeVal[0] = 3;
    rx1.setStandard(13); rx2.setStandard(18);
    CHECK(rx1.start(DeviceInfo(), a, fo), "RX 1 start");
    CHECK(rx2.start(DeviceInfo(), c, fo), "RX 2 start");
    CHECK(waitRx(rx1, 30, [](const RxTelemetry& r) { return r.standard == 12 && r.adsb.aircraftCount >= 5; }), "RX 1 decodes ADS-B");
    CHECK(waitRx(rx2, 40, [](const RxTelemetry& r) { return r.standard == 17 && r.blocksOk >= 5; }), "RX 2 decodes ACARS");
    RxTelemetry r1, r2;
    rx1.latestRx(r1, 0); rx2.latestRx(r2, 0);
    CHECK(r1.standard == 12 && r2.standard == 17, "standards %d %d", r1.standard, r2.standard);
    CHECK(rx1.droppedSamples() == 0 && rx2.droppedSamples() == 0, "dropped samples");
    // stopping RX 2 leaves RX 1 running
    rx2.stop();
    const uint64_t s0 = r1.seq;
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    rx1.latestRx(r1, 0);
    CHECK(rx1.running() && !rx2.running() && r1.seq > s0, "RX 1 after RX 2 stopped: running %d seq %llu -> %llu", (int)rx1.running(), (unsigned long long)s0, (unsigned long long)r1.seq);
    rx1.stop();
    rx1.flushResults(); rx2.flushResults();
    CHECK(nFiles(base / "results") > 0 && nFiles(base / "results_rx2") > 0, "results per receiver: %zu / %zu files", nFiles(base / "results"), nFiles(base / "results_rx2"));
    std::error_code ec;
    fs::remove_all(base, ec);
    printf(fails ? "multi rx: %d FAILED\n" : "multi rx: all passed\n", fails);
    return fails ? 1 : 0;
}
