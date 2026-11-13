// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pagers with a deviation other than the standard 4.5 / 4.8 kHz (networks and transmitters differ), in both polarities: one full cycle must
// arrive for each. Part of the "pager decodes nothing although the bursts are there" investigation (issue 261).
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_rx.h"
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const double rate = 1.2e6;
    const auto want = pagerTestMessages();
    for (int inv = 0; inv < 2; inv++)
        for (int dev : {2500, 3500, 6000}) {
            SynthConfig c; c.snrDb = 25; c.modeOpt[1] = inv; c.modeOpt[4] = dev;
            auto syn = makePagerSynth(c, rate);
            std::vector<cf32> x((size_t)(rate * (pagerCycleSeconds() + 1)));
            for (size_t i = 0; i < x.size(); i += 65536) syn->generate(x.data() + i, std::min<size_t>(65536, x.size() - i));
            PagerReceiver rx;
            rx.configure(rate);
            rx.setSignalOffset(-pagerTuning().tuneOffsetHz);
            PagerTelemetry t; uint64_t seq = 0;
            auto take = [&] { PagerTelemetry n; if (rx.telemetry(n, seq)) { seq = n.seq; t = n; } };
            for (size_t i = 0; i < x.size(); i += 65536) { rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i)); take(); }
            std::vector<cf32> z(65536);
            for (int k = 0; k < 8; k++) { rx.feed(z.data(), z.size()); take(); }
            int got = 0;
            for (const PagerTestMessage& w : want) {
                bool f = false;
                if (t.messages) for (const PagerMessage& m : *t.messages) f |= m.address == w.address && m.speed == w.speed && m.text == w.text;
                got += f;
            }
            printf("%s deviation %d Hz: %d of %zu pages\n", inv ? "inverted" : "normal  ", dev, got, want.size());
            CHECK(got == (int)want.size(), "every page of the cycle arrives");
        }
    return testSummary("pager_dev");
}
