// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// D-STAR voice through a user-installed "ambe" codec plug-in (stub library, no codec): generator -> receiver -> plug-in -> PCM tap. Every received
// AMBE frame (72 bits) reaches the plug-in as 12 bytes and gives 160 samples. D-STAR carries no encryption OnAir reads, so there is no gate to test.
// NOT covered: a real codec or a real D-STAR recording.
#include "dect2/dstar/dstar_gen.h"
#include "dect2/dstar/dstar_rx.h"
#include "dect2/dstar/dstar_tel.h"
#include "../codec/plugin_test_util.h"
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    CHECK(plugtest::install("ambe", STUB_PLUGIN_PATH), "stub install");
    constexpr double kRate = 500000;
    DstarGenConfig c;
    c.rate = kRate; c.dialOffsetHz = -50000; c.snrDb = 30; c.loop = false; c.seed = 5;
    c.txs = dstarTestSchedule();
    double total = c.leadSec + 1.0;
    for (const auto& t : c.txs) total += t.durSec + 0.3 + t.gapSec;
    DstarGenerator g(c);
    std::vector<cf32> x((size_t)(total * kRate));
    g.generate(x.data(), x.size());
    DstarReceiver rx;
    rx.setSilent(true);
    rx.configure(kRate);
    rx.setSignalOffset(-50000);
    int frames = 0; size_t samples = 0;
    rx.setVoiceTap([&](const uint8_t*) { frames++; });
    rx.setAudioTap([&](const float*, size_t n) { samples += n; });
    for (size_t i = 0; i < x.size(); i += 25000) rx.feed(x.data() + i, std::min<size_t>(25000, x.size() - i));
    DstarTelemetry t;
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
