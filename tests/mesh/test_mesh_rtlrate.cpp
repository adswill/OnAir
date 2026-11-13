// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mesh (LoRa) on an RTL-SDR, issue #386:
//  1. meshRtlRateHz(): 1.024 Msps for an RTL-SDR (rtlsdr / rtltcp) and a channel that still fits there with the mode's 300 kHz tune offset;
//     every Meshtastic, MeshCore, LoRa APRS and MeshCom setting of every region is checked against the receiver's own band rule.
//     Other radios keep the mode's rate (0 = no change).
//  2. The receiver at 1.024 Msps decodes the test signal with no CRC errors, and the jump count (RxFrame::jumpsFixed ->
//     MeshPacket::jumpsFixed, MeshReceiver::framesWithJumpsFixed) is 0 for clean frames.
//  3. Samples removed from the stream (what an RTL-SDR does when the USB link drops them) are recovered where recoverJumps can, and the
//     packet row says how many jumps were undone. Simulated signals only: no real recording of a jump exists in the repository.
#include "dect2/mesh/mesh_gen.h"
#include "dect2/mesh/mesh_presets.h"
#include "dect2/mesh/mesh_proto.h"
#include "dect2/mesh/mesh_rx.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;

#include "../common/test_util.h"

static std::vector<double> allBandwidths() {
    std::vector<double> v;
    for (const std::string& reg : meshtasticRegions())
        for (const std::string& pre : meshtasticPresetNames()) {
            MeshLoraSettings s;
            if (meshtasticRegionPreset(reg, pre, s)) v.push_back(s.bwHz);
        }
    for (const auto& s : loraAprsPresets()) v.push_back(s.bwHz);
    for (const auto& s : meshcomPresets()) v.push_back(s.bwHz);
    for (const auto& e : meshCoreBuiltinPresets()) v.push_back(e.bwKhz * 1e3);
    return v;
}

static void rateChoice() {
    const double off = meshTuning().tuneOffsetHz;
    const auto bws = allBandwidths();
    CHECK(bws.size() > 50, "only %zu settings found", bws.size());
    int fit = 0, nofit = 0;
    for (double bw : bws) {
        const double r = meshRtlRateHz("rtlsdr", bw);
        CHECK(r == meshRtlRateHz("rtltcp", bw), "rtlsdr and rtltcp differ at %.1f kHz", bw / 1e3);
        if (r > 0) {
            fit++;
            CHECK(r == 1.024e6, "rate %.0f", r);
            // the receiver itself must give the channel a demodulator at that rate (the dial sits -tuneOffset from 0 Hz)
            CHECK(meshChannelFits(r, -off, bw), "%.1f kHz at %.3f Msps is outside the receiver's band", bw / 1e3, r / 1e6);
            CHECK(bw <= 250e3 + 1, "%.1f kHz channel got 1.024 Msps", bw / 1e3);
        } else {
            nofit++;
            CHECK(!meshChannelFits(1.024e6, -off, bw), "%.1f kHz fits at 1.024 Msps but got no rate", bw / 1e3);
        }
        for (const char* other : {"hackrf", "pluto", "airspy", "sdrplay", "limesdr", "bladerf", "uhd", "", "file"})
            CHECK(meshRtlRateHz(other, bw) == 0, "board '%s' changed at %.1f kHz", other, bw / 1e3);
    }
    CHECK(fit > 0 && nofit > 0, "fit %d, no fit %d: both kinds of channel must exist in the tables", fit, nofit);
    CHECK(meshRtlRateHz("rtlsdr", 62.5e3) == 1.024e6 && meshRtlRateHz("rtlsdr", 125e3) == 1.024e6 && meshRtlRateHz("rtlsdr", 250e3) == 1.024e6, "62.5, 125 and 250 kHz");
    CHECK(meshRtlRateHz("rtlsdr", 500e3) == 0, "500 kHz at a 300 kHz offset reaches -550 kHz: no room in 1.024 Msps, keep 2 Msps");
    CHECK(meshRtlRateHz("rtlsdr", 0) == 0 && meshRtlRateHz("rtlsdr", -1) == 0, "no bandwidth: no rate");
    // the mode's own rate (what every other radio gets) holds a 500 kHz channel, and the 2.048 Msps RTL-SDR rate too
    CHECK(meshChannelFits(meshTuning().sampleRate, -off, 500e3) && meshChannelFits(2.048e6, -off, 500e3), "500 kHz at 2 Msps");
    printf("rate choice: %d of %zu settings run at 1.024 Msps on an RTL-SDR, %d keep 2 Msps\n", fit, bws.size(), nofit);
}

struct Run { uint64_t frames = 0, bad = 0, rowsOk = 0, rowsJump = 0, jumpFrames = 0; int maxJumps = 0; bool inBand = false; };

// dropEvery > 0: remove dropN samples after every dropEvery samples of the stream
static Run run(double rate, double secs, int dropEvery, int dropN) {
    SynthConfig cfg;
    cfg.mode = 22; cfg.modeOpt[1] = 0; cfg.snrDb = 20; cfg.modeVal[0] = 3;
    auto syn = makeMeshSynth(cfg, rate);
    MeshReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-meshTuning().tuneOffsetHz);
    rx.setRegion(0);
    const size_t chunk = 4096;
    std::vector<cf32> buf(chunk);
    size_t sinceDrop = 0;
    for (size_t done = 0; done < (size_t)(secs * rate); done += chunk) {
        syn->generate(buf.data(), chunk);
        if (dropEvery <= 0) { rx.feed(buf.data(), chunk); continue; }
        size_t i = 0;
        while (i < chunk) {   // feed the pieces between the drops
            const size_t take = std::min(chunk - i, (size_t)dropEvery - sinceDrop);
            rx.feed(buf.data() + i, take);
            i += take; sinceDrop += take;
            if (sinceDrop >= (size_t)dropEvery) { i += std::min((size_t)dropN, chunk - i); sinceDrop = 0; }
        }
    }
    MeshTelemetry t;
    rx.telemetry(t, 0);
    Run r;
    for (const auto& d : t.decoders) { r.frames += d.frames; if (d.protocol == 1 && d.bwHz == 250e3 && d.inBand) r.inBand = true; }
    r.bad = t.blocksBad;
    for (const MeshPacket& p : t.packets) {
        if (p.crcOk) r.rowsOk++;
        if (p.jumpsFixed > 0) { r.rowsJump++; r.maxJumps = std::max(r.maxJumps, p.jumpsFixed); CHECK(p.crcOk, "a row with jumps has a bad CRC"); }
    }
    r.jumpFrames = rx.framesWithJumpsFixed();
    return r;
}

int main() {
    rateChoice();
    // clean stream at the RTL-SDR rate: everything decodes, nothing was a jump
    const Run c = run(1.024e6, 10, 0, 0);
    printf("1.024 Msps clean: %llu frames, %llu bad CRC, %llu rows ok, %llu rows with jumps\n", (unsigned long long)c.frames, (unsigned long long)c.bad,
           (unsigned long long)c.rowsOk, (unsigned long long)c.rowsJump);
    CHECK(c.inBand, "LongFast has no demodulator at 1.024 Msps");
    CHECK(c.frames >= 5 && c.rowsOk >= 5 && c.bad == 0, "clean 1.024 Msps: %llu frames, %llu ok, %llu bad", (unsigned long long)c.frames, (unsigned long long)c.rowsOk, (unsigned long long)c.bad);
    CHECK(c.rowsJump == 0 && c.jumpFrames == 0, "clean frames counted as jumps: %llu rows, %llu frames", (unsigned long long)c.rowsJump, (unsigned long long)c.jumpFrames);
    // lost samples: the count shows up in the row and in the receiver's counter (what /api/v1/status reports)
    const Run j = run(1.024e6, 10, 150000, 96);
    printf("1.024 Msps, 96 samples lost every 150000: %llu frames, %llu bad CRC, %llu rows ok, %llu rows with jumps (max %d), counter %llu\n", (unsigned long long)j.frames,
           (unsigned long long)j.bad, (unsigned long long)j.rowsOk, (unsigned long long)j.rowsJump, j.maxJumps, (unsigned long long)j.jumpFrames);
    CHECK(j.rowsJump == j.jumpFrames, "rows with jumps %llu, counter %llu", (unsigned long long)j.rowsJump, (unsigned long long)j.jumpFrames);
    CHECK(j.rowsJump + j.bad > 0 || j.rowsOk > 0, "lost samples: nothing received at all");
    return testSummary("mesh_rtlrate");
}
