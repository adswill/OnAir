// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// WSPR through the engine as the interface and dect2cli use it: the WSPR entry on the synthetic source. The test signal starts at an
// arbitrary point of the system clock's 2-minute slots, so the decoder must find the slots from the signals (slot search) and decode the
// test stations within two slots (240 s) of signal. The source runs faster than real time; the run is counted in seconds of signal.
#include "dect2/engine/engine.h"
#include "dect2/engine/engine_testkit.h"
#include "dect2/hfdig/hfdig_ftx.h"
#include "dect2/hfdig/hfdig_rx.h"
#include "dect2/engine/mode_tuning.h"
#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
using namespace dect2;
using namespace dect2::enginetest;

#include "../common/test_util.h"

int main() {
    const SubMode* sm = subModeById("wspr");
    CHECK(sm != nullptr, "no wspr entry");
    if (!sm) return 1;
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    const ModeTuning ht = hfdigTuning();
    t.centerHz = sm->defMhz * 1e6 + ht.tuneOffsetHz;
    t.bandwidthMhz = ht.bandwidthMhz;
    t.sampleRate = ht.sampleRate;
    t.synth.mode = sm->stdMode;
    t.synth.pace = 20.0;
    t.synth.snrDb = 20;
    applySubModeSynth(t.synth, *sm);
    FileOptions fo;
    e.setStandard(sm->stdMode);
    applySubMode(e, sm);
    e.hfdig().setSilent(true);
    CHECK(e.start(dev, t, fo), "engine start");
    std::set<std::string> want, got;
    for (const auto& s : ftxTestStations(3)) want.insert(s.msg);
    RxTelemetry rx;
    uint64_t seq = 0;
    const double s0 = signalSecs(e);
    double at = -1;
    while (signalSecs(e) - s0 < 240.0 + 10.0) {
        if (e.latestRx(rx, seq)) {
            seq = rx.seq;
            for (const auto& d : rx.hfdig.ftx.decodes) if (d.mode == 3 && d.slotUtc < 130.0) got.insert(d.msg);
            if (at < 0 && !got.empty()) at = signalSecs(e) - s0;
            if (got.size() >= 5) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // the decoder works on its own thread, behind the faster-than-real-time source: wait for the slots already cut. Only the slots
    // whose window ended within the first 2 slots of signal count (a window ends about 120 s after its slot time)
    auto collect = [&] {
        e.latestRx(rx, 0);
        for (const auto& d : rx.hfdig.ftx.decodes) if (d.mode == 3 && d.slotUtc < 130.0) got.insert(d.msg);
        return got.size() >= 5;
    };
    waitFor(90.0, collect);
    const HfdigFtxTelemetry& f = rx.hfdig.ftx;
    printf("slots %llu, dropped %llu, audio %.0f s, last slot %d decodes\n", (unsigned long long)f.slots[3], (unsigned long long)f.slotsDropped, f.audioSamples / kHfdigAudioRate, f.lastSlotCount[3]);
    int found = 0;
    for (const auto& m : got) { printf("  %s\n", m.c_str()); if (want.count(m)) found++; }
    printf("WSPR engine: %d of %zu stations, first after %.0f s of signal, clock mode %d\n", found, want.size(), at, rx.hfdig.ftx.timeMode);
    CHECK(found >= 4, "only %d of the test stations decoded within 2 slots", found);
    CHECK(got.size() == (size_t)found, "false decodes");
    e.stop();
    printf(fails ? "wspr engine: %d FAILED\n" : "wspr engine: all passed\n", fails);
    return fails ? 1 : 0;
}
