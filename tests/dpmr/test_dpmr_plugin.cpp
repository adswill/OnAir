// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// dPMR voice through a user-installed "ambe2" codec plug-in (stub library, no codec): generator -> receiver -> plug-in -> PCM tap. Every received voice
// frame (72 bits) reaches the plug-in as 12 bytes and gives 160 samples. dPMR has no encryption in the signal OnAir reads, so there is no gate to
// test. NOT covered: a real codec or a real dPMR recording.
#include "dect2/dpmr/dpmr_gen.h"
#include "dect2/dpmr/dpmr_rx.h"
#include "dect2/dpmr/dpmr_tel.h"
#include "dect2/dpmr/dpmr_proto.h"
#include "../codec/plugin_test_util.h"
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    CHECK(plugtest::install("ambe2", STUB_PLUGIN_PATH), "stub install");
    DpmrGenConfig c;
    c.rate = 500000; c.snrDb = 40; c.loop = false; c.seed = 3;
    DpmrGenTx tx; tx.kind = DpmrGenTx::Voice; tx.durSec = 2;
    c.txs.push_back(tx);
    DpmrGenerator g(c);
    DpmrReceiver rx;
    rx.setSilent(true);
    rx.configure(c.rate);
    rx.setChannel(dpmr::pmr446Mhz(1));
    int frames = 0; size_t samples = 0;
    rx.setVoiceTap([&](const uint8_t*) { frames++; });
    rx.setAudioTap([&](const float*, size_t n) { samples += n; });
    std::vector<cf32> buf(25000);
    for (double s = 0; s < 0.5 + tx.durSec + 0.4 + 1.5; s += buf.size() / c.rate) { g.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    DpmrTelemetry t;
    rx.telemetry(t, 0);
    const int plug = plugtest::gFrames;
    printf("%d voice frames, %d to the plug-in, %zu samples, decoded %llu\n", frames, plug, samples, (unsigned long long)t.voiceDecoded);
    CHECK(frames > 20, "too few voice frames (%d)", frames);
    CHECK(plug > 0 && plug >= frames * 9 / 10 && plug <= frames, "plug-in got %d of %d frames", plug, frames);
    CHECK(plugtest::gBadSize == 0, "%d frames of the wrong size", plugtest::gBadSize.load());
    CHECK(samples == (size_t)plug * 160, "audio %zu samples for %d frames", samples, plug);
    printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
