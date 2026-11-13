// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Drone Remote ID receiver end to end (rid_gen.h, rid_rx.h): Open Drone ID in BLE advertising and in Wi-Fi beacons through the BLE and
// Wi-Fi demodulators, each alone and both in one 20 MHz band at 2432 MHz, a carrier offset with noise, a file (no centre), and noise alone.
#include "dect2/rid/rid_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/rid/rid_proto.h"
#include "dect2/rid/rid_rx.h"
#include "dect2/rid/rid_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static RidTelemetry run(const RidGenConfig& c, double secs, double centerHz) {
    RidGenerator g(c);
    RidReceiver r;
    r.configure(c.rate);
    r.setCenterHz(centerHz);
    std::vector<cf32> buf(65536);
    const size_t total = (size_t)(secs * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    RidTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static const RidDrone* find(const RidTelemetry& t, const char* mac) {
    for (const auto& d : t.drones) if (d.mac == mac) return &d;
    return nullptr;
}

static void drone1(const RidTelemetry& t, const char* what) {
    const RidDrone* d = find(t, "C6:12:34:56:78:9A");
    CHECK(d != nullptr, "%s: BLE drone missing", what);
    if (!d) return;
    CHECK(d->viaBle && !d->viaWifi && d->serial == "1581F5FKD229400B" && d->sessionId == "SESSION-7F3A" && d->uaType == 2, "%s: BLE drone ID", what);
    CHECK(d->hasPos && std::fabs(d->lat - 54.6872) < 1e-6 && std::fabs(d->lon - 25.2797) < 1e-6 && std::fabs(d->height - 120) < 0.01 && std::fabs(d->altGeo - 245) < 0.01 &&
          std::fabs(d->speed - 8.5) < 0.01 && std::fabs(d->direction - 90) < 0.01 && d->status == 2, "%s: BLE drone position", what);
    CHECK(d->hasOperator && std::fabs(d->opLat - 54.6860) < 1e-6 && std::fabs(d->opLon - 25.2770) < 1e-6 && d->operatorId == "LTUabcd1234efgh" && d->selfId == "Survey flight" &&
          d->category == 1 && d->uaClass == 2 && d->authType == 1, "%s: BLE drone operator", what);
}

static void drone2(const RidTelemetry& t, const char* what) {
    const RidDrone* d = find(t, "60:60:1F:AA:BB:CC");
    CHECK(d != nullptr, "%s: Wi-Fi drone missing", what);
    if (!d) return;
    CHECK(d->viaWifi && !d->viaBle && d->caaId == "FIN87astrdge12k8" && d->uaType == 1 && d->operatorId == "FINop-998877", "%s: Wi-Fi drone ID", what);
    CHECK(d->hasPos && std::fabs(d->lat - 54.7001) < 1e-6 && std::fabs(d->lon - 25.3002) < 1e-6 && std::fabs(d->direction - 275) < 0.01 && d->heightType == 1 &&
          d->hasOperator && std::fabs(d->opLat - 54.6950) < 1e-6, "%s: Wi-Fi drone position", what);
}

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    {   // the plan: 2432 MHz hears BLE 38 and Wi-Fi 6; 2437 only Wi-Fi 6; 2402 BLE 37 and Wi-Fi 1
        const RidPlan a = ridPlan(2432e6), b = ridPlan(2437e6), c = ridPlan(2402e6), f = ridPlan(0);
        CHECK(a.bleChannel == 38 && std::fabs(a.bleOffsetHz + 6e6) < 1 && a.wifiChannel == 6 && std::fabs(a.wifiOffsetHz - 5e6) < 1, "plan 2432");
        CHECK(b.bleChannel == 0 && b.wifiChannel == 6 && std::fabs(b.wifiOffsetHz) < 1, "plan 2437");
        CHECK(c.bleChannel == 37 && c.wifiChannel == 1 && std::fabs(c.wifiOffsetHz - 10e6) < 1, "plan 2402 %d %.0f", c.wifiChannel, c.wifiOffsetHz);
        CHECK(f.bleChannel == 0 && f.wifiChannel == 0 && f.bleOffsetHz == 0 && f.wifiOffsetHz == 0, "plan file");
    }
    {   // both carriers in one band at 2432 MHz
        RidGenConfig c;
        const RidTelemetry t = run(c, 0.4, c.centerHz);
        printf("  both: %zu drones, %llu messages, BLE %llu/%llu, Wi-Fi %llu/%llu, bad %llu\n", t.drones.size(), (unsigned long long)t.blocksOk, (unsigned long long)t.bleRid,
               (unsigned long long)t.blePackets, (unsigned long long)t.wifiRid, (unsigned long long)t.wifiFrames, (unsigned long long)t.blocksBad);
        CHECK(t.drones.size() == 2 && t.bleChannel == 38 && t.wifiChannel == 6, "%zu drones", t.drones.size());
        drone1(t, "both");
        drone2(t, "both");
        CHECK(t.blePackets > t.bleRid && t.wifiFrames > t.wifiRid && t.bleRid >= 20 && t.wifiRid >= 20, "non-RID packets counted and skipped");
        bool bleLog = false, wifiLog = false;
        for (const auto& l : t.log) { if (l.transport == 0 && l.channel == 38) bleLog = true; if (l.transport == 1 && l.channel == 6) wifiLog = true; }
        CHECK(bleLog && wifiLog && t.state == 1, "log");
    }
    {   // only BLE, offset 20 kHz, 15 dB
        RidGenConfig c; c.wifi = false; c.cfoHz = 20e3; c.snrDb = 15;
        const RidTelemetry t = run(c, 0.4, c.centerHz);
        printf("  BLE: %zu drones, %llu messages\n", t.drones.size(), (unsigned long long)t.blocksOk);
        CHECK(t.drones.size() == 1, "BLE only: %zu drones", t.drones.size());
        drone1(t, "BLE");
    }
    {   // only Wi-Fi, offset -30 kHz, 12 dB
        RidGenConfig c; c.ble = false; c.cfoHz = -30e3; c.snrDb = 12;
        const RidTelemetry t = run(c, 0.4, c.centerHz);
        printf("  Wi-Fi: %zu drones, %llu messages\n", t.drones.size(), (unsigned long long)t.blocksOk);
        CHECK(t.drones.size() == 1, "Wi-Fi only: %zu drones", t.drones.size());
        drone2(t, "Wi-Fi");
    }
    {   // a file: no centre, BLE on its own at 0 Hz
        RidGenConfig c; c.centerHz = 0; c.wifi = false;
        const RidTelemetry t = run(c, 0.3, 0);
        CHECK(t.drones.size() == 1 && t.bleChannel == 0, "file BLE: %zu drones", t.drones.size());
        drone1(t, "file BLE");
    }
    {   // a file with Wi-Fi at 0 Hz
        RidGenConfig c; c.centerHz = 0; c.ble = false;
        const RidTelemetry t = run(c, 0.3, 0);
        CHECK(t.drones.size() == 1, "file Wi-Fi: %zu drones", t.drones.size());
        drone2(t, "file Wi-Fi");
    }
    {   // noise alone gives nothing
        RidReceiver r;
        r.configure(20e6);
        r.setCenterHz(2432e6);
        std::mt19937 rng(5);
        dect2::normal_distribution<float> nd(0.f, 0.05f);
        std::vector<cf32> buf(65536);
        for (int k = 0; k < 120; k++) { for (auto& v : buf) v = cf32{nd(rng), nd(rng)}; r.feed(buf.data(), buf.size()); }
        RidTelemetry t;
        r.telemetry(t, 0);
        CHECK(t.drones.empty() && t.blocksOk == 0 && t.state == 0 && t.log.empty(), "noise: %zu drones, %llu messages", t.drones.size(), (unsigned long long)t.blocksOk);
    }
    printf("  %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    printf(fails ? "rid rx: %d FAILED\n" : "rid rx: all passed\n", fails);
    return fails ? 1 : 0;
}
