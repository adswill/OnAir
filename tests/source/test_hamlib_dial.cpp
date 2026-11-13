// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A Hamlib rig is tuned to the dial frequency, whatever the mode (#358). The rig's own receiver gives the audio, so there is no DC spike to
// keep a narrow channel off; 3.7 still added the mode's tuneOffsetHz and FT8 on 7.074 MHz put an IC-7300 on 7.094. Runs the whole engine on
// Hamlib's dummy rig (model 1) with a silent sound card, for every HF mode with the 20 kHz offset (FT8, PSK31, MT63, JS8, ALE, marine ...)
// and the general receiver (start and retune), and checks the frequency the rig was given, the capture centre and the dial the app shows.
#include "dect2/engine/engine.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/source/hamlib_rig.h"
#include "dect2/source/source.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    // the device kinds: only a sound card and a Hamlib rig tune at the dial
    for (DeviceInfo::Kind k : {DeviceInfo::HackRF, DeviceInfo::Soapy, DeviceInfo::Native, DeviceInfo::File, DeviceInfo::Group}) {
        DeviceInfo d; d.kind = k;
        CHECK(!d.tunesAtDial(), "kind %d keeps the mode's tuning offset", (int)k);
    }
    { DeviceInfo d; d.kind = DeviceInfo::Audio; CHECK(d.tunesAtDial(), "a sound card tunes at the dial"); }
    { DeviceInfo d; d.kind = DeviceInfo::Hamlib; CHECK(d.tunesAtDial(), "a Hamlib rig tunes at the dial"); }
#ifdef DECT2_HAVE_HAMLIB
    HamlibRig r; r.model = 1; r.path = "dummy";
    const DeviceInfo dev = hamlibRigDevices({r})[0];   // no audio device named: the rig's audio is "silence"
    CHECK(dev.kind == DeviceInfo::Hamlib, "dummy rig entry");
    std::vector<int> modes;
    for (int m = 1; m < 1000; m++) if (const ModeTuning* mt = modeTuning(m)) if (mt->stdMode == m && (mt->tuneOffsetHz == 20000 || m == 57)) modes.push_back(m);   // the HF modes a rig runs (20 kHz offset) and the general receiver; scanning and Doppler modes retune on their own
    CHECK(modes.size() >= 8, "HF modes with a tuning offset: %zu", modes.size());
    int checked = 0;
    for (int m : modes) {
        const ModeTuning* mt = modeTuning(m);
        // the dial inside the mode's usual band, on a round frequency
        const double dial = std::round(std::max(mt->minMhz, std::min(mt->maxMhz, mt->defMhz)) * 1e3) * 1e3;
        Engine e;
        e.setStandard(m);
        TuneSettings t; t.centerHz = dial; t.sampleRate = 48000; t.bandwidthMhz = mt->bandwidthMhz; t.radio["hamlib_mode"] = "USB";
        if (!e.start(dev, t, FileOptions())) { std::printf("  note: %s (%d) did not start on the dummy rig, skipped\n", mt->id, m); continue; }
        checked++;
        double hz = 0;
        for (int i = 0; i < 100; i++) { hz = hamlibRigState().hz; if (std::fabs(hz - dial) < 1 || std::fabs(hz - dial - mt->tuneOffsetHz) < 1) break; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(std::fabs(hz - dial) < 1, "%s (%d): the rig was tuned to %.3f kHz for the dial %.3f kHz (offset %.0f Hz)", mt->id, m, hz / 1e3, dial / 1e3, mt->tuneOffsetHz);
        CHECK(std::fabs(e.captureCentreHz() - dial) < 1, "%s: capture centre %.0f, dial %.0f", mt->id, e.captureCentreHz(), dial);
        // a retune goes the same way
        TuneSettings t2 = t; t2.centerHz = dial + 3000;
        e.retune(t2);
        for (int i = 0; i < 100 && std::fabs(hamlibRigState().hz - t2.centerHz) >= 1; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(std::fabs(hamlibRigState().hz - t2.centerHz) < 1, "%s: after a retune the rig is at %.3f kHz, the dial %.3f kHz", mt->id, hamlibRigState().hz / 1e3, t2.centerHz / 1e3);
        e.stop();
    }
    CHECK(checked >= 5, "modes run on the dummy rig: %d", checked);
    std::printf("hamlib dial: %zu modes with a tuning offset, %d run on the dummy rig, %d failures\n", modes.size(), checked, fails);
#else
    std::printf("note: built without Hamlib, the rig part is skipped\n");
#endif
    return fails ? 1 : 0;
}
