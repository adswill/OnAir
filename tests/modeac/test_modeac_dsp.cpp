// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode A / C reply detector against the generator's own list: replies among Mode S traffic at 2, 4 and 10 Msps, replies only, noise only
// (no replies), a carrier error, the SPI pulse, an emergency squawk, and a Mode S-only stream (no phantom replies out of message bits).
#include "dect2/modeac/modeac_gen.h"
#include "dect2/modeac/modeac_live.h"
#include "dect2/modeac/modeac_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Result { std::set<uint16_t> trueBits; size_t sent = 0, found = 0, bitsOk = 0, phantom = 0, spiOk = 0, spiSent = 0; double perSec = 0; ModeAcTelemetry tel; };

static Result run(ModeAcGenConfig c, double seconds, int confirm = 1) {
    ModeAcGenerator g(c);
    ModeAcReceiver rx;
    rx.configure(c.rate);
    rx.setConfirm(confirm);
    std::vector<ModeAcDemod::Raw> got;
    rx.setReplyCallback([&](const ModeAcDemod::Raw& r) { got.push_back(r); });
    std::vector<cf32> buf(1 << 16);
    const size_t total = (size_t)(seconds * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) {
        g.generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
    }
    Result r;
    const double tEnd = (double)total / c.rate - 0.01;
    std::vector<bool> used(got.size(), false);
    for (const auto& t : g.sent()) {
        r.trueBits.insert(t.bits);
        if (t.t > tEnd || t.t < 0.05) continue;
        r.sent++;
        if (t.spi) r.spiSent++;
        for (size_t i = 0; i < got.size(); i++) {
            if (used[i] || std::fabs(got[i].timeSec - t.t) > 2e-6) continue;
            used[i] = true;
            r.found++;
            if (got[i].bits == t.bits) r.bitsOk++;
            if (t.spi && got[i].spi) r.spiOk++;
            break;
        }
    }
    for (size_t i = 0; i < got.size(); i++) if (!used[i] && got[i].timeSec > 0.05) r.phantom++;
    r.perSec = (double)got.size() / seconds;
    ModeAcTelemetry t;
    rx.telemetry(t, 0);
    r.tel = t;
    return r;
}

int main() {
    const double rates[] = {2e6, 4e6, 10e6};
    for (double rate : rates) {
        ModeAcGenConfig c; c.rate = rate; c.aircraft = 12; c.seed = 3;
        const Result r = run(c, 12.0);
        printf("  %.0f Msps: sent %zu found %zu (right bits %zu) phantom %zu, Mode S good %llu, noise %.1f dBFS\n", rate / 1e6, r.sent, r.found, r.bitsOk, r.phantom, (unsigned long long)r.tel.modeSGood, (double)r.tel.noiseDbfs);
        const double need = rate < 3e6 ? 0.55 : 0.80;
        const Result q = run(c, 12.0, 2);   // the table as a user sees it (two replies to show a code): only codes that were sent
        size_t foreign = 0;
        for (const auto& k : q.tel.codes) if (!q.trueBits.count(k.bits)) foreign++;
        CHECK(foreign <= (rate < 3e6 ? 2u : 0u), "%.0f Msps: %zu of %u table codes were never sent", rate / 1e6, foreign, q.tel.codeCount);
        CHECK(r.sent > 100, "only %zu replies sent", r.sent);
        CHECK(r.found >= need * r.sent, "%.0f Msps: found %zu of %zu", rate / 1e6, r.found, r.sent);
        const size_t tol = rate < 3e6 ? 5 : 1;   // percent
        CHECK(r.bitsOk * 100 >= r.found * (100 - tol), "%.0f Msps: %zu of %zu found replies have the wrong bits", rate / 1e6, r.found - r.bitsOk, r.found);
        CHECK(r.phantom * 100 <= r.sent * tol, "%.0f Msps: %zu phantom replies against %zu sent", rate / 1e6, r.phantom, r.sent);
    }
    {   // replies only, a carrier error of the whole capture
        ModeAcGenConfig c; c.rate = 4e6; c.modeS = false; c.cfoHz = 150e3; c.seed = 5;
        const Result r = run(c, 8.0);
        printf("  replies only, +150 kHz: sent %zu found %zu bits %zu phantom %zu\n", r.sent, r.found, r.bitsOk, r.phantom);
        CHECK(r.found >= r.sent * 85 / 100 && r.bitsOk >= r.found * 99 / 100 && r.phantom <= 2, "replies only: %zu/%zu, bits %zu, phantom %zu", r.found, r.sent, r.bitsOk, r.phantom);
    }
    {   // Mode S traffic only: nothing is made of the message bits
        ModeAcGenConfig c; c.rate = 4e6; c.replyRate = 0.05; c.aircraft = 40; c.seed = 7;
        ModeAcGenerator g(c);
        ModeAcReceiver rx; rx.configure(c.rate); rx.setConfirm(1);
        size_t got = 0;
        rx.setReplyCallback([&](const ModeAcDemod::Raw&) { got++; });
        std::vector<cf32> buf(1 << 16);
        for (size_t done = 0; done < (size_t)(8 * c.rate); done += buf.size()) { g.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
        printf("  40 aircraft, 1/20 of the replies: %zu sent, %zu found\n", g.sent().size(), got);
        CHECK(got <= g.sent().size() + 1, "%zu replies found, %zu sent", got, g.sent().size());
    }
    {   // noise only
        ModeAcGenConfig c; c.rate = 4e6; c.aircraft = 0; c.seed = 9;
        const Result r = run(c, 4.0);
        CHECK(r.found == 0 && r.phantom == 0, "noise: %zu phantom replies", r.phantom);
    }
    {   // the SPI pulse and an emergency squawk
        ModeAcGenConfig c; c.rate = 4e6; c.modeS = false; c.special = 2; c.seed = 11; c.replyRate = 3;
        const Result r = run(c, 10.0);
        printf("  SPI: %zu sent, %zu seen\n", r.spiSent, r.spiOk);
        CHECK(r.spiSent > 5 && r.spiOk >= r.spiSent * 8 / 10, "SPI %zu of %zu", r.spiOk, r.spiSent);
        ModeAcGenConfig e; e.rate = 4e6; e.special = 1; e.seed = 11; e.replyRate = 3;
        const Result q = run(e, 10.0, 2);
        bool found = false;
        for (const auto& k : q.tel.codes) if (k.squawk == 7700 && !k.altValid) found = true;
        CHECK(found, "7700 not in the table (%u codes)", q.tel.codeCount);
    }
    return testSummary("modeac dsp");
}
