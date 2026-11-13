// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SAME receiver on synthetic signals, fed directly: back to back alerts with the attention tone and the end of message, carrier offsets, noise,
// a corrupted repeat, a missing repeat, partial county codes, many locations, and no false alerts on noise.
#include "dect2/same/same_gen.h"
#include "dect2/same/same_rx.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static const double kRate = 2000000;

static SameTelemetry run(SameGenConfig cfg, double extraSec = 3.0, int country = 0) {
    cfg.rate = kRate;
    cfg.tuneOffsetHz = sameTuning().tuneOffsetHz;
    cfg.loop = false;
    SameGenerator gen(cfg);
    SameReceiver rx;
    rx.configure(kRate);
    rx.setSignalOffset(-sameTuning().tuneOffsetHz);
    rx.setCountry(country);
    std::vector<cf32> buf(65536);
    const size_t total = (size_t)((gen.cycleSec() + extraSec) * kRate);
    for (size_t done = 0; done < total;) {
        const size_t n = std::min(buf.size(), total - done);
        gen.generate(buf.data(), n);
        rx.feed(buf.data(), n);
        done += n;
    }
    SameTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

static SameGenConfig twoAlerts(double snr, double cfo) {
    SameGenConfig c;
    c.snrDb = snr; c.cfoHz = cfo;
    SameGenMessage a; a.text = sameTestTornado(); a.toneSec = 2.0;
    SameGenMessage b; b.text = sameTestWeekly(); b.leadSec = 0.8;
    c.messages = {a, b};
    return c;
}

// alerts oldest first
static std::vector<SameAlert> inOrder(const SameTelemetry& t) { return std::vector<SameAlert>(t.alerts.rbegin(), t.alerts.rend()); }

static void checkTwo(const char* name, const SameTelemetry& t, int minAgree) {
    const auto v = inOrder(t);
    printf("%s: %zu messages, %llu bursts, snr %.1f dB, cfo %+.0f Hz, bad %llu\n", name, v.size(), (unsigned long long)t.bursts, t.snrDb, t.cfoHz, (unsigned long long)t.blocksBad);
    CHECK(v.size() == 4, "%s: %zu messages, want 4 (two alerts, two ends)", name, v.size());
    if (v.size() != 4) return;
    CHECK(v[0].valid && !v[0].eom && v[0].raw == sameTestTornado(), "%s: first alert '%s' (%s)", name, v[0].raw.c_str(), v[0].err.c_str());
    CHECK(v[0].msg.event == "TOR" && v[0].msg.locs.size() == 3 && v[0].msg.locs[1].pca == 3 && v[0].msg.callsign == "KHGX/NWS", "%s: first alert fields", name);
    CHECK(v[0].agreeing >= minAgree && v[0].bursts >= 2, "%s: %d of %d repeats agree", name, v[0].agreeing, v[0].bursts);
    CHECK(v[0].toneSec > 1.5 && v[0].toneSec < 2.6, "%s: tone %.2f s", name, v[0].toneSec);
    CHECK(v[1].valid && v[1].eom, "%s: end of the first", name);
    CHECK(v[2].valid && v[2].raw == sameTestWeekly() && v[2].msg.event == "RWT", "%s: second alert '%s'", name, v[2].raw.c_str());
    CHECK(v[3].valid && v[3].eom, "%s: end of the second", name);
    CHECK(t.blocksOk == 4 && t.blocksBad == 0, "%s: counters %llu / %llu", name, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    CHECK(t.tones.size() == 1, "%s: %zu tones", name, t.tones.size());
}

int main() {
    checkTwo("clean", run(twoAlerts(30, 0)), 3);
    checkTwo("carrier +4 kHz", run(twoAlerts(25, 4000)), 3);
    checkTwo("carrier -3 kHz, 14 dB", run(twoAlerts(14, -3000)), 2);
    checkTwo("noisy 10 dB", run(twoAlerts(10, 500)), 2);
    // 4.0 (docs/SENSITIVITY-4.0-A3.md): the tones through a 4.5 kHz channel of their own. Both alerts and both ends must come out exactly (3.9 decoded
    // about half of the messages at 3 dB and 12 to 22 percent at 2 dB)
    for (const auto& w : {std::pair<double, double>{3, 0}, {3, 3000}, {3, -5000}, {2, 0}}) {
        const auto t = run(twoAlerts(w.first, w.second));
        const auto v = inOrder(t);
        printf("weak %.0f dB, carrier %+.0f Hz: %zu messages, snr %.1f dB\n", w.first, w.second, v.size(), t.snrDb);
        CHECK(v.size() == 4 && v[0].valid && v[0].raw == sameTestTornado() && v[2].valid && v[2].raw == sameTestWeekly(), "weak %.0f dB %+.0f Hz: %zu messages", w.first, w.second, v.size());
    }

    {   // one repeat corrupted (bit errors in the text): the 3-way vote repairs it
        SameGenConfig c = twoAlerts(30, 0);
        c.messages[0].corruptBurst = 1; c.messages[0].corruptBits = 6;
        c.messages[1].corruptBurst = 0; c.messages[1].corruptBits = 3;
        const SameTelemetry t = run(c);
        checkTwo("corrupted repeat", t, 2);
        for (const auto& al : t.alerts) printf("  [%d/%d fixed %d] %s\n", al.agreeing, al.bursts, al.bitsFixed, al.raw.c_str());
        const auto v = inOrder(t);
        if (v.size() == 4) CHECK(v[0].agreeing == 2 && v[2].agreeing >= 2,   // (bitsFixed: 6 in 3.9; the damaged repeat now reaches the vote with a different tail, so the count is 0 and the vote still repairs it)
                     "voting fixed %d / %d bits, %d agree", v[0].bitsFixed, v[2].bitsFixed, v[0].agreeing);
    }
    {   // a repeat missing (2 of 3 heard) and a repeat damaged in the other
        SameGenConfig c = twoAlerts(30, 0);
        c.messages[0].dropBurst = 0;
        c.messages[1].dropBurst = 2;
        const SameTelemetry t = run(c);
        checkTwo("missing repeat", t, 2);
        const auto v = inOrder(t);
        if (v.size() == 4) CHECK(v[0].bursts == 2 && v[2].bursts == 2, "bursts %d %d", v[0].bursts, v[2].bursts);
    }
    {   // the same message heard twice only, with damage in one repeat: no majority, the whole repeat is used
        SameGenConfig c;
        c.snrDb = 30;
        SameGenMessage a; a.text = sameTestTornado(); a.dropBurst = 2; a.corruptBurst = 0; a.corruptBits = 2; a.sendEom = false;
        c.messages = {a};
        const SameTelemetry t = run(c);
        CHECK(t.alerts.size() == 1 && t.alerts[0].valid && t.alerts[0].raw == sameTestTornado(), "two repeats, one damaged: %zu alerts '%s'", t.alerts.size(), t.alerts.empty() ? "" : t.alerts[0].raw.c_str());
    }
    {   // 31 locations with all the PCA digits, and Canada's province names for the state part
        std::string text = "ZCZC-CIV-CEM";
        for (int i = 0; i < 31; i++) { char b[16]; snprintf(b, sizeof b, "-%d%02d%03d", i % 10, i < 20 ? 24 + i : 13, 100 + i); text += b; }
        text += "+0200-2051200-ABCDEFGH-";
        SameGenConfig c;
        c.snrDb = 25;
        SameGenMessage a; a.text = text; a.sendEom = false;
        c.messages = {a};
        const SameTelemetry t = run(c);
        for (const auto& al : t.alerts) printf("  [%d/%d fixed %d] %s | %s\n", al.agreeing, al.bursts, al.bitsFixed, al.raw.c_str(), al.err.c_str());
        CHECK(t.alerts.size() == 1 && t.alerts[0].valid && t.alerts[0].msg.locs.size() == 31, "31 locations: %zu alerts, %s", t.alerts.size(), t.alerts.empty() ? "" : t.alerts[0].err.c_str());
        if (t.alerts.size() == 1 && t.alerts[0].valid) {
            CHECK(t.alerts[0].msg.purgeMin == 120 && t.alerts[0].msg.issueDay == 205, "times");
            bool pcas = true;
            for (int i = 0; i < 31; i++) pcas = pcas && t.alerts[0].msg.locs[(size_t)i].pca == i % 10;
            CHECK(pcas, "pca digits");
        }
        const SameTelemetry tc = run(c, 3.0, 1);
        CHECK(tc.alerts.size() == 1 && tc.alerts[0].valid && tc.alerts[0].msg.locs[0].stateName == "Quebec", "Canada: '%s'", tc.alerts.empty() || tc.alerts[0].msg.locs.empty() ? "" : tc.alerts[0].msg.locs[0].stateName.c_str());
    }
    {   // noise alone: no message, no tone
        SameGenConfig c;
        c.noiseOnly = true; c.snrDb = 8;
        const SameTelemetry t = run(c, 6.0);
        CHECK(t.alerts.empty() && t.tones.empty(), "noise: %zu messages, %zu tones", t.alerts.size(), t.tones.size());
    }
    {   // the tone alone is found, and is not a message
        SameGenConfig c;
        c.snrDb = 25;
        SameGenMessage a; a.text = ""; a.repeats = 0; a.toneSec = 3.0; a.sendEom = false;
        c.messages = {a};
        const SameTelemetry t = run(c);
        CHECK(t.alerts.empty() && t.tones.size() == 1 && t.tones[0].durSec > 2.5, "tone alone: %zu tones", t.tones.size());
    }
    printf(fails ? "same rx: %d FAILED\n" : "same rx: all passed\n", fails);
    return fails ? 1 : 0;
}
