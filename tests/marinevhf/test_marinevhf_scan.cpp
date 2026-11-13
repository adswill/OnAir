// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Marine VHF scan logic (whole span and single tuner, dwell, hang, priority watch on 16) and the per-channel call log.
#include "dect2/marinevhf/marinevhf_scan.h"
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using namespace dect2::marinevhf;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Sched {   // key -> list of [from, to) intervals of activity
    std::map<int, std::vector<std::pair<double, double>>> on;
    bool at(int key, double t) const {
        auto it = on.find(key);
        if (it == on.end()) return false;
        for (auto& iv : it->second) if (t >= iv.first && t < iv.second) return true;
        return false;
    }
};

// Runs the scan from t0 to t1 in steps of dt; returns the answer at each step
static std::vector<int> run(ChannelScan& s, const Sched& sc, double t0, double t1, double dt = 0.05) {
    std::vector<int> out;
    for (long i = 0; t0 + (double)i * dt < t1 - 1e-9; i++) { const double t = t0 + (double)i * dt; out.push_back(s.update(t, [&](int k) { return sc.at(k, t); })); }
    return out;
}
static int at(const std::vector<int>& v, double t, double t0 = 0, double dt = 0.05) { return v[(size_t)std::llround((t - t0) / dt)]; }

int main() {
    const std::vector<int> list = {6, 8, 9, 12, 13, 22};

    // ---- whole span: idle, hold, hang
    {
        ChannelScan s; ScanConfig c; c.hangSec = 2.0; s.setConfig(c); s.setChannels(list);
        CHECK(s.update(0, [](int) { return true; }) == -1 && !s.running(), "not started: -1");
        s.start(0);
        CHECK(s.running() && s.state() == ChannelScan::Scanning, "started");
        Sched sc; sc.on[12] = {{1.0, 3.0}};
        auto v = run(s, sc, 0, 10);
        CHECK(at(v, 0.5) == -1, "nothing active: nothing plays");
        CHECK(at(v, 1.0) == 12 && at(v, 2.0) == 12, "plays 12 as it opens");
        CHECK(at(v, 3.0) == 12 && at(v, 4.9) == 12, "keeps 12 for the hang time after it closed");
        CHECK(at(v, 5.1) == -1 && s.state() == ChannelScan::Scanning, "releases after the hang");
    }
    // ---- whole span: two at once, list order after the last played, hold until quiet
    {
        ChannelScan s; ScanConfig c; c.hangSec = 1.0; s.setConfig(c); s.setChannels(list); s.start(0);
        Sched sc; sc.on[9] = {{1.0, 4.0}}; sc.on[13] = {{1.0, 8.0}}; sc.on[8] = {{0.5, 0.6}};
        auto v = run(s, sc, 0, 12);
        CHECK(at(v, 0.5) == 8, "8 opens first and is played");
        CHECK(at(v, 1.0) == 8 && at(v, 1.5) == 8, "8 held through its hang while 9 and 13 are active");
        CHECK(at(v, 1.65) == 9, "then the next active one after 8 in list order: 9 (%d)", at(v, 1.65));
        CHECK(at(v, 3.9) == 9, "9 held while active");
        CHECK(at(v, 5.1) == 13, "9 closed and 1 s of hang gone: 13 (%d)", at(v, 5.1));
        CHECK(at(v, 8.5) == 13 && at(v, 9.5) == -1, "13 until it is gone (%d, %d)", at(v, 8.5), at(v, 9.5));
    }
    // ---- whole span: hang 0, round robin continues after the last played
    {
        ChannelScan s; ScanConfig c; c.hangSec = 0; s.setConfig(c); s.setChannels(list); s.start(0);
        Sched sc; sc.on[6] = {{0, 100}}; sc.on[22] = {{0, 100}};
        auto v = run(s, sc, 0, 1);
        CHECK(at(v, 0.0) == 6 && at(v, 0.5) == 6, "both active: stays on 6");
        sc.on[6] = {{0, 0.2}};
        auto w = run(s, sc, 1.0, 2.0);
        CHECK(at(w, 1.0, 1.0) == 22 && at(w, 1.5, 1.0) == 22, "6 is gone: 22");
    }
    // ---- whole span: priority takeover and resume
    {
        ChannelScan s; ScanConfig c; c.hangSec = 2.0; c.priorityKey = 16; s.setConfig(c); s.setChannels(list); s.start(0);
        Sched sc; sc.on[9] = {{1.0, 30.0}}; sc.on[16] = {{10.0, 12.0}};
        auto v = run(s, sc, 0, 30);
        CHECK(at(v, 5.0) == 9, "holding 9");
        CHECK(at(v, 10.0) == 16 && s.visited() >= 2, "16 takes over the moment it opens");
        CHECK(at(v, 12.5) == 16 && at(v, 13.9) == 16, "16 held for its hang");
        CHECK(at(v, 14.1) == 9, "then 9 resumes because it is still active (%d)", at(v, 14.1));
    }
    {   // the interrupted channel is gone when 16 ends: no resume, scanning goes on
        ChannelScan s; ScanConfig c; c.hangSec = 1.0; c.priorityKey = 16; s.setConfig(c); s.setChannels(list); s.start(0);
        Sched sc; sc.on[9] = {{1.0, 4.5}}; sc.on[16] = {{3.0, 4.0}}; sc.on[22] = {{3.5, 20.0}};
        auto v = run(s, sc, 0, 12);
        CHECK(at(v, 2.0) == 9 && at(v, 3.0) == 16, "9 then 16");
        CHECK(at(v, 5.5) == 22, "9 ended, 16 ended: the active channel 22 (%d)", at(v, 5.5));
    }
    {   // the priority channel does not have to be in the list
        ChannelScan s; ScanConfig c; c.hangSec = 0.5; c.priorityKey = 16; s.setConfig(c); s.setChannels({6, 8}); s.start(0);
        Sched sc; sc.on[16] = {{1, 2}};
        auto v = run(s, sc, 0, 5);
        CHECK(at(v, 1.5) == 16 && at(v, 3.0) == -1, "16 plays though not listed");
    }
    {   // stop
        ChannelScan s; s.setChannels(list); s.start(0);
        s.stop();
        CHECK(!s.running() && s.update(1, [](int) { return true; }) == -1, "stopped");
    }

    // ---- single tuner: dwell sequence
    {
        ChannelScan s; ScanConfig c; c.singleTuner = true; c.dwellSec = 0.4; c.hangSec = 1.0; s.setConfig(c); s.setChannels({6, 8, 9}); s.start(0);
        Sched sc;
        auto v = run(s, sc, 0, 4.8);
        CHECK(at(v, 0.0) == 6 && at(v, 0.3) == 6, "starts on the first channel");
        CHECK(at(v, 0.5) == 8 && at(v, 0.9) == 9 && at(v, 1.3) == 6, "dwell 0.4 s each, in order, wrapping (%d %d %d)", at(v, 0.5), at(v, 0.9), at(v, 1.3));
        CHECK(at(v, 4.7) == 6 || at(v, 4.7) == 8 || at(v, 4.7) == 9, "still going");
        int six = 0, eight = 0, nine = 0;
        for (int k : v) { if (k == 6) six++; else if (k == 8) eight++; else if (k == 9) nine++; }
        CHECK(std::abs(six - eight) <= 9 && std::abs(eight - nine) <= 9, "even visits %d %d %d", six, eight, nine);
    }
    // ---- single tuner: stop on activity, hang, go on
    {
        ChannelScan s; ScanConfig c; c.singleTuner = true; c.dwellSec = 0.4; c.hangSec = 1.0; s.setConfig(c); s.setChannels({6, 8, 9, 12}); s.start(0);
        Sched sc; sc.on[9] = {{0.0, 5.0}};
        auto v = run(s, sc, 0, 10);
        CHECK(at(v, 0.2) == 6 && at(v, 0.5) == 8 && at(v, 0.9) == 9, "reaches 9");
        CHECK(at(v, 1.5) == 9 && at(v, 4.9) == 9, "holds 9 while active");
        CHECK(at(v, 5.5) == 9 && at(v, 6.1) == 12, "stays through the hang, then moves on to 12 (%d, %d)", at(v, 5.5), at(v, 6.1));
        CHECK(s.state() == ChannelScan::Scanning, "scanning again");
    }
    // ---- single tuner: priority watch on 16
    {
        ChannelScan s; ScanConfig c; c.singleTuner = true; c.dwellSec = 1.0; c.hangSec = 1.0; c.priorityKey = 16; c.priorityEverySec = 2.0; c.priorityDwellSec = 0.3;
        s.setConfig(c); s.setChannels({6, 8, 9}); s.start(0);
        Sched sc;
        auto v = run(s, sc, 0, 12);
        // every 2 s the radio visits 16 for 0.3 s and returns to the channel it left
        int visits16 = 0; bool prev16 = false;
        for (size_t i = 0; i < v.size(); i++) { const bool is16 = v[i] == 16; if (is16 && !prev16) visits16++; prev16 = is16; }
        CHECK(visits16 >= 4 && visits16 <= 6, "16 visited every ~2 s: %d times in 12 s", visits16);
        int on16 = 0; for (int k : v) if (k == 16) on16++;
        CHECK(on16 >= visits16 * 5 && on16 <= visits16 * 8, "each visit ~0.3 s: %d samples", on16);
        for (size_t i = 1; i + 1 < v.size(); i++) if (v[i] == 16 && v[i - 1] != 16) {   // the channel before and after the visit
            size_t j = i; while (j < v.size() && v[j] == 16) j++;
            if (j < v.size()) CHECK(v[j] == v[i - 1], "returns to the interrupted channel (%d vs %d)", v[j], v[i - 1]);
        }
    }
    {   // 16 active: the radio stays, then goes back
        ChannelScan s; ScanConfig c; c.singleTuner = true; c.dwellSec = 1.0; c.hangSec = 1.0; c.priorityKey = 16; c.priorityEverySec = 2.0; c.priorityDwellSec = 0.3;
        s.setConfig(c); s.setChannels({6, 8, 9}); s.start(0);
        Sched sc; sc.on[16] = {{1.0, 6.0}};
        auto v = run(s, sc, 0, 12);
        CHECK(at(v, 2.5) == 16 && at(v, 5.0) == 16 && at(v, 6.5) == 16, "stays on 16 while it is active (and through the hang)");
        CHECK(at(v, 7.5) != 16 && s.state() != ChannelScan::Priority, "goes back to the list afterwards (%d)", at(v, 7.5));
    }
    {   // holding a talking channel: the priority visit does not lose the hold
        ChannelScan s; ScanConfig c; c.singleTuner = true; c.dwellSec = 0.4; c.hangSec = 1.0; c.priorityKey = 16; c.priorityEverySec = 2.0; c.priorityDwellSec = 0.3;
        s.setConfig(c); s.setChannels({6, 8, 9}); s.start(0);
        Sched sc; sc.on[8] = {{0.0, 9.0}};
        auto v = run(s, sc, 0, 8);
        bool saw16 = false, back = false;
        for (size_t i = 0; i < v.size(); i++) { if (v[i] == 16) saw16 = true; if (saw16 && v[i] == 8) back = true; }
        CHECK(saw16 && back, "16 is checked during the hold and the radio returns to 8");
        CHECK(at(v, 7.9) == 8 && s.state() == ChannelScan::Holding, "still holding 8");
    }
    {   // only the priority channel
        ChannelScan s; ScanConfig c; c.singleTuner = true; c.priorityKey = 16; s.setConfig(c); s.setChannels({}); s.start(0);
        auto v = run(s, Sched(), 0, 3);
        CHECK(s.running() && at(v, 1.0) == 16, "no list: parked on 16");
    }

    // ---- call log
    {
        CallLog log(1.0);
        int64_t wall = 1000;
        log.setClock([&] { return wall++; });
        int closed = 0; log.setOnClose([&](const CallEntry&) { closed++; });
        for (int ti = 0; ti < 60; ti++) {
            const double t = ti * 0.1 + 1e-9;
            const bool a16 = t >= 1.0 && t < 2.0;
            const bool a9 = (t >= 1.5 && t < 2.0) || (t >= 2.5 && t < 3.0);   // 0.5 s gap < hang: one call
            log.update(t, 16, a16, -40 + (a16 ? (t - 1.0) * 10 : 0));
            log.update(t, 9, a9, -50);
        }
        const auto& e = log.entries();
        CHECK(e.size() == 2 && closed == 2, "two finished calls, got %zu (%d)", e.size(), closed);
        const auto e16 = log.entriesFor(16); const auto e9 = log.entriesFor(9);
        CHECK(e16.size() == 1 && e9.size() == 1, "one call on each channel");
        if (e16.size() == 1) {
            CHECK(std::fabs(e16[0].startSec - 1.0) < 0.06 && std::fabs(e16[0].endSec - 1.9) < 0.06, "16: %.2f to %.2f", e16[0].startSec, e16[0].endSec);
            CHECK(std::fabs(e16[0].durationSec() - 0.9) < 0.1, "16 duration %.2f", e16[0].durationSec());
            CHECK(e16[0].peakDb > e16[0].meanDb && e16[0].peakDb > -31.5 && e16[0].peakDb < -30.5, "16 peak %.1f mean %.1f", e16[0].peakDb, e16[0].meanDb);
            CHECK(e16[0].wallTime >= 1000 && !e16[0].open, "wall clock and closed");
        }
        if (e9.size() == 1) CHECK(std::fabs(e9[0].startSec - 1.5) < 0.06 && std::fabs(e9[0].endSec - 2.9) < 0.06, "9 merged across the 0.5 s gap: %.2f to %.2f", e9[0].startSec, e9[0].endSec);
        const auto st = log.stats(16);
        CHECK(st.calls == 1 && std::fabs(st.totalSec - e16[0].durationSec()) < 1e-9 && std::fabs(st.lastHeardSec - e16[0].endSec) < 1e-9, "stats");
        CHECK(log.stats(22).calls == 0, "unheard channel");
    }
    {   // a gap longer than the hang makes two calls; open calls are visible; closeAll ends them
        CallLog log(0.5);
        for (int ti = 0; ti < 50; ti++) { const double t = ti * 0.1 + 1e-9; log.update(t, 12, (t >= 0.5 && t < 1.0) || (t >= 2.0 && t < 2.5), -30); }
        CHECK(log.entries().size() == 2, "two calls across a 1 s gap: %zu", log.entries().size());
        log.update(6.0, 12, true, -20);
        CHECK(log.openCalls().size() == 1 && log.openCalls()[0].open && log.stats(12).calls == 3, "an open call counts in the stats");
        log.update(6.2, 12, true, -20);
        log.closeAll(6.3);
        CHECK(log.openCalls().empty() && log.entries().size() == 3 && std::fabs(log.entries().back().endSec - 6.2) < 1e-9, "closeAll ends at the last active moment");
        log.clear();
        CHECK(log.entries().empty() && log.stats(12).calls == 0, "cleared");
    }
    {   // the cap
        CallLog log(0.1, 5);
        for (int i = 0; i < 20; i++) { log.update(i * 1.0, 1, true, -30); log.update(i * 1.0 + 0.5, 1, false, -100); }
        CHECK(log.entries().size() == 5, "capped at 5: %zu", log.entries().size());
        CHECK(log.entries().front().startSec >= 14.0, "the oldest are dropped");
        CHECK(log.stats(1).calls >= 19, "the stats keep counting: %d", log.stats(1).calls);
    }

    if (fails) printf("%d failures\n", fails); else printf("marinevhf scan: all ok\n");
    return fails ? 1 : 0;
}
