// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The tracker book on synthetic time: a tracker that stays near for long enough raises one alert, one that is far, patchy or marked as the
// user's own does not, rotating addresses are chained, and identifiers are forgotten after the memory window.
#include "dect2/bletrk/bletrk_book.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

static bletrk::Detection det(bletrk::Kind k = bletrk::FindMy) { bletrk::Detection d; d.kind = k; d.separated = k == bletrk::FindMy; d.status = 0x10; return d; }

// plays one advertiser from t0 to t1 s: a packet every `step` s at the level; ticks every 0.25 s; returns the alerts
static int play(BletrkBook& b, const std::string& addr, double t0, double t1, float lvl, double step = 0.5, bletrk::Kind k = bletrk::FindMy, double skipFrom = -1, double skipTo = -1) {
    int alerts = 0;
    for (double t = t0; t <= t1; t += 0.25) {
        const bool skip = skipFrom >= 0 && t >= skipFrom && t < skipTo;
        if (!skip && std::fmod(t - t0, step) < 0.25) b.hear(addr, det(k), lvl, t, 1700000000 + (int64_t)t);
        alerts += (int)b.tick(t, 1700000000 + (int64_t)t).size();
    }
    return alerts;
}

int main() {
    {   // near for 12 minutes: one alert, after the 10 minutes
        BletrkBook b;
        const int n = play(b, "AA:BB:CC:DD:EE:01", 0, 720, -45);
        BletrkTelemetry t; b.fill(t, 720);
        CHECK(n == 1, "alerts %d", n);
        CHECK(t.trackers.size() == 1 && t.trackers[0].following && t.trackers[0].alerted && t.followingNow == 1, "following");
        CHECK(t.alerts.size() == 1 && t.alerts[0].text.find("Unwanted tracking") != std::string::npos, "alert text");
        CHECK(t.trackers[0].label.find("..EE:01") != std::string::npos && t.trackers[0].label.find("AA:BB") == std::string::npos, "label '%s'", t.trackers[0].label.c_str());
        CHECK(t.nowByKind[bletrk::FindMy] == 1 && t.windowByKind[bletrk::FindMy] == 1, "counts");
    }
    {   // not yet at 9 minutes
        BletrkBook b;
        CHECK(play(b, "AA:BB:CC:DD:EE:02", 0, 540, -45) == 0, "alert too early");
    }
    {   // far away: seen for an hour, never near
        BletrkBook b;
        CHECK(play(b, "AA:BB:CC:DD:EE:03", 0, 3600, -90) == 0, "far tracker alerted");
        BletrkTelemetry t; b.fill(t, 3600);
        CHECK(t.trackers.size() == 1 && t.trackers[0].proximity == 0 && !t.trackers[0].following, "far");
    }
    {   // patchy: heard in 1 minute of 3 only
        BletrkBook b;
        int n = 0;
        n += play(b, "AA:BB:CC:DD:EE:04", 0, 60, -45);
        n += play(b, "AA:BB:CC:DD:EE:04", 780, 840, -45);
        BletrkTelemetry t; b.fill(t, 840);
        CHECK(n == 0, "patchy tracker alerted (%d)", n);
    }
    {   // marked as the user's own
        BletrkBook b;
        play(b, "AA:BB:CC:DD:EE:05", 0, 30, -45);
        BletrkTelemetry t; b.fill(t, 30);
        b.ignore(t.trackers[0].id, true);
        CHECK(play(b, "AA:BB:CC:DD:EE:05", 30, 900, -45) == 0, "own tracker alerted");
        b.fill(t, 900);
        CHECK(t.trackers[0].ignored && !t.trackers[0].following, "own tracker flagged");
    }
    {   // rotation: a new address 5 s after the old one goes silent continues the tracker
        BletrkBook b;
        int n = play(b, "AA:BB:CC:DD:EE:10", 0, 400, -45);
        n += play(b, "AA:BB:CC:DD:EE:11", 405, 800, -46);
        BletrkTelemetry t; b.fill(t, 800);
        CHECK(n == 1, "rotation: alerts %d", n);
        CHECK(t.trackers.size() == 1 && t.trackers[0].addresses == 2, "rotation: %zu trackers, %d addresses", t.trackers.size(), t.trackers.empty() ? 0 : t.trackers[0].addresses);
    }
    {   // two tags at once are two trackers, not a rotation
        BletrkBook b;
        for (double t = 0; t < 60; t += 0.5) {
            b.hear("AA:BB:CC:DD:EE:20", det(), -45, t, 0);
            b.hear("AA:BB:CC:DD:EE:21", det(), -46, t + 0.1, 0);
            b.tick(t, 0);
        }
        BletrkTelemetry t; b.fill(t, 60);
        CHECK(t.trackers.size() == 2, "two tags: %zu", t.trackers.size());
    }
    {   // a different kind is never chained
        BletrkBook b;
        play(b, "AA:BB:CC:DD:EE:30", 0, 100, -45, 0.5, bletrk::FindMy);
        play(b, "AA:BB:CC:DD:EE:31", 105, 200, -45, 0.5, bletrk::Tile);
        BletrkTelemetry t; b.fill(t, 200);
        CHECK(t.trackers.size() == 2, "kinds chained");
    }
    {   // the level trend
        BletrkBook b;
        for (double t = 0; t <= 100; t += 0.25) { if (std::fmod(t, 0.5) < 0.25) b.hear("AA:BB:CC:DD:EE:40", det(), (float)(-80 + 0.4 * t), t, 0); b.tick(t, 0); }
        BletrkTelemetry t; b.fill(t, 100);
        CHECK(t.trackers[0].trend == 1, "approaching: trend %d", t.trackers[0].trend);
    }
    {   // forgotten after the window, clear empties
        BletrkBook b;
        play(b, "AA:BB:CC:DD:EE:50", 0, 20, -45);
        b.tick(20 + kBletrkForgetSec - 1, 0);
        BletrkTelemetry t; b.fill(t, 0);
        CHECK(t.trackers.size() == 1, "forgotten too early");
        b.tick(20 + kBletrkForgetSec + 1, 0);
        b.fill(t, 0);
        CHECK(t.trackers.empty(), "not forgotten: %zu", t.trackers.size());
        play(b, "AA:BB:CC:DD:EE:51", 3000, 3010, -45);
        b.clear();
        b.fill(t, 0);
        CHECK(t.trackers.empty() && t.alerts.empty(), "clear");
    }
    return testSummary("bletrk book");
}
