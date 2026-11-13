// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The KAAL receive path of the engine on a recording of the radio chain (8-bit IQ at 2.4 Msps with a carrier offset, written by the transmit generator at the
// HackRF's rate and the chain model of test_kaal_chain): file source -> engine -> KAAL standard 124. Reads KAAL_CHAIN_FILE (made by test_kaal_chain engine <path>);
// without it the test passes at once. Use: ./test_kaal_chain engine /tmp/chain.cs8 3000; KAAL_CHAIN_FILE=/tmp/chain.cs8 ./test_kaal_chain_engine
#include "dect2/engine/engine.h"
#include "dect2/kaal/kaal_tel.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const char* path = getenv("KAAL_CHAIN_FILE");
    if (!path) { printf("KAAL_CHAIN_FILE not set: skipped\n"); return 0; }
    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::File; dev.name = "kaal chain";
    TuneSettings tune; tune.sampleRate = 2.4e6; tune.centerHz = 434.92e6; tune.bandwidthMhz = 0.012;
    FileOptions fo; fo.path = path; fo.format = FileFormat::CS8; fo.sampleRate = 2.4e6; fo.loop = false;
    e.setStandard(124);
    CHECK(e.start(dev, tune, fo), "engine start");
    RxTelemetry rx;
    uint64_t seq = 0;
    double lockAt = -1, beaconAt = -1, bioAt = -1;
    const auto w0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - w0 < std::chrono::seconds(60)) {
        if (e.latestRx(rx, seq)) {
            seq = rx.seq;
            if (rx.kaal.state == 2 && lockAt < 0) lockAt = rx.kaal.timeSec;
            if (rx.kaal.beaconValid && beaconAt < 0) beaconAt = rx.kaal.timeSec;
            if (!rx.kaal.text.empty() && bioAt < 0) bioAt = rx.kaal.timeSec;
            if (rx.kaal.timeSec > 25) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    printf("engine on the chain recording: lock at %.1f s, beacon at %.1f s, bio at %.1f s, frames ok %llu bad %llu, cfo %.0f Hz\n", lockAt, beaconAt, bioAt,
           (unsigned long long)rx.kaal.blocksOk, (unsigned long long)rx.kaal.blocksBad, rx.kaal.cfoHz);
    CHECK(lockAt > 0 && lockAt < 10, "lock at %.1f s", lockAt);
    CHECK(beaconAt > 0 && beaconAt < 12, "beacon at %.1f s", beaconAt);
    CHECK(rx.kaal.blocksOk > 500 && rx.kaal.blocksBad * 20 < rx.kaal.blocksOk, "frames ok %llu bad %llu", (unsigned long long)rx.kaal.blocksOk, (unsigned long long)rx.kaal.blocksBad);
    e.stop();
    return testSummary("kaal_chain_engine");
}
