// Voice insights (docs/SMART5-voice-3.9.md): talkgroup heatmap and busy-time prediction, trunk load with Erlang B, talkgroup change alerts.
// Simulated call streams only (Poisson arrivals, exponential holding times, a blocked-calls-lost trunk); no real trunked recording.
#include "dect2/insight/voice_activity.h"
#include <cstdio>
#include <random>
#include "../common/test_util.h"

using namespace dect2::insight;

static const double kMonday = 1767571200;   // 2026-01-05 00:00 UTC, a Monday

static void testBasics() {
    CHECK(talkgroupOf("TG 91/slot1") == "TG 91", "TG key");
    CHECK(talkgroupOf("TG 2602") == "TG 2602", "TG key without slot");
    CHECK(talkgroupOf("Private 1234567").empty() && talkgroupOf("unit 4711").empty() && talkgroupOf("radio 12").empty(), "private calls are skipped");
    CHECK(talkgroupOf("group 05-012") == "group 05-012", "EDACS group");
    CHECK(weekHour(kMonday, 0) == 0 && weekHour(kMonday + 86400 + 18 * 3600 + 5, 0) == 24 + 18, "Monday 0, Tuesday 18");
    CHECK(weekHour(kMonday - 1, 0) == 6 * 24 + 23, "Sunday 23");
    CHECK(weekHour(kMonday - 3600, 7200) == 1, "UTC+2: Monday 01 local");
    CHECK(std::fabs(erlangB(10, 5) - 0.018385) < 1e-5, "B(10,5) %.6f", erlangB(10, 5));
    CHECK(std::fabs(erlangB(1, 1) - 0.5) < 1e-12 && erlangB(5, 0) == 0, "B(1,1)");
    for (int n : {1, 4, 8, 20}) for (double a : {0.5, 2.0, 6.0, 15.0}) {
        const double c = a * (1 - erlangB(n, a));
        CHECK(std::fabs(offeredFromCarried(n, c) - a) < 1e-6 * a + 1e-9, "offered from carried n %d a %.1f", n, a);
    }
}

// six weeks: TG 1 busy on Tuesdays 18-20 and quiet otherwise, TG 9 (the system's chatter) a call every 20 min, round the clock
static void testHeatmap() {
    for (int tz : {0, 7200, -18000}) {
        std::mt19937 r(11 + tz);
        std::exponential_distribution<double> hold(1.0 / 15);
        TalkgroupHeatmap h(tz);
        std::vector<Insight> out;
        const double t0 = kMonday - tz;   // Monday 00:00 local
        for (int w = 0; w < 6; w++)
            for (int d = 0; d < 7; d++)
                for (int hr = 0; hr < 24; hr++) {
                    const double hs = t0 + ((w * 7 + d) * 24 + hr) * 3600.0;
                    for (int k = 0; k < 3; k++) h.addCall(hs + k * 1200 + 30, hold(r) + 2, "TG 9", &out);
                    const bool busy = d == 1 && (hr == 18 || hr == 19);
                    std::poisson_distribution<int> calls(busy ? 40 : 0.3);
                    const int nc = calls(r);
                    for (int k = 0; k < nc; k++) h.addCall(hs + std::uniform_real_distribution<double>(0, 3500)(r), hold(r) + 2, "TG 1", &out);
                }
        int falseBusy = 0;
        for (const auto& i : out) if (i.key == "TG 1") falseBusy++;
        CHECK(falseBusy == 0, "tz %d: no busy alerts in ordinary weeks (got %d)", tz, falseBusy);
        // from the Monday of week 7 at noon, the next busy hour is Tuesday 18:00 local
        const double mon7 = t0 + 6 * 7 * 86400.0 + 12 * 3600;
        const double nb = h.nextBusy("TG 1", mon7);
        CHECK(std::fabs(nb - (t0 + (6 * 7 + 1) * 86400.0 + 18 * 3600)) < 1, "tz %d: next busy %.0f h after Monday noon", tz, (nb - mon7) / 3600);
        auto top = h.busiest("TG 1", 2);
        CHECK(top.size() == 2 && (top[0].first == 24 + 18 || top[0].first == 24 + 19) && (top[1].first == 24 + 18 || top[1].first == 24 + 19),
              "tz %d: busiest hours Tue 18 and 19", tz);
        if (tz == 0) printf("heatmap: Tue 18 %.0f s/h, Tue 19 %.0f s/h, Wed 10 %.0f s/h\n", h.grid("TG 1")[24 + 18], h.grid("TG 1")[24 + 19], h.grid("TG 1")[48 + 10]);
        // week 7, Wednesday 10:00: an unusual 10 minutes of calls on TG 1
        out.clear();
        const double wed = t0 + (6 * 7 + 2) * 86400.0 + 10 * 3600;
        h.addCall(wed + 30, 5, "TG 9", &out);
        for (int k = 0; k < 20; k++) h.addCall(wed + 60 + k * 30, 20, "TG 1", &out);
        int busy = 0;
        for (const auto& i : out) if (i.kind == "voice_busy" && i.key == "TG 1") busy++;
        CHECK(busy == 1, "tz %d: one busy alert for an unusual Wednesday morning (got %d)", tz, busy);
        // save and load
        TalkgroupHeatmap h2(tz);
        h2.load(h.save());
        double md = 0;
        for (int s = 0; s < TalkgroupHeatmap::kSlots; s++) md = std::max(md, std::fabs(h2.grid("TG 1")[(size_t)s] - h.grid("TG 1")[(size_t)s]));
        CHECK(md < 1e-3, "tz %d: save/load differs by %.4f", tz, md);
        CHECK(h2.nextBusy("TG 1", mon7) == nb, "tz %d: loaded map predicts the same", tz);
    }
}

// blocked-calls-lost trunk with n channels: only the calls that got a channel are heard
static void simulateTrunk(int n, double offered, double hours, unsigned seed, TrunkLoad& tl, double& measuredBlocking) {
    std::mt19937 r(seed);
    const double meanHold = 20;
    std::exponential_distribution<double> gap(offered / meanHold), hold(1 / meanHold);
    std::vector<double> busyUntil((size_t)n, 0);
    int arrived = 0, blocked = 0;
    for (double t = kMonday + gap(r); t < kMonday + hours * 3600; t += gap(r)) {
        arrived++;
        int free = -1;
        for (int c = 0; c < n; c++) if (busyUntil[(size_t)c] <= t) { free = c; break; }
        if (free < 0) { blocked++; continue; }
        const double d = hold(r);
        busyUntil[(size_t)free] = t + d;
        tl.addCall(t, t + d, "ch" + std::to_string(free));
    }
    measuredBlocking = (double)blocked / std::max(1, arrived);
}

static void testTrunk() {
    for (double a : {1.0, 3.0, 5.0, 7.0, 9.0}) {
        TrunkLoad tl;
        double mb = 0;
        simulateTrunk(8, a, 24, (unsigned)(a * 100), tl, mb);
        CHECK(tl.channels() == 8, "8 channels seen, got %d", tl.channels());
        std::vector<Insight> out;
        tl.tick(kMonday + 25 * 3600 + 600, "TRUNK", out);
        double off = 0, blk = 0;
        int hrs = 0;
        for (const auto& h : tl.hours()) if (h.start >= kMonday && h.start < kMonday + 24 * 3600) { off += h.offeredErl; blk += h.blocking; hrs++; }
        off /= std::max(1, hrs); blk /= std::max(1, hrs);
        printf("trunk 8 ch, offered %.0f Erl: estimated %.2f Erl, blocking estimated %.3f measured %.3f, %zu alerts\n", a, off, blk, mb, out.size());
        CHECK(hrs == 24, "24 hours closed, got %d", hrs);
        CHECK(std::fabs(off - a) < 0.12 * a, "offered %.0f estimated %.2f", a, off);
        CHECK(std::fabs(blk - mb) < 0.03, "blocking estimated %.3f measured %.3f", blk, mb);
        if (a <= 3) CHECK(out.empty(), "no capacity alert at %.0f Erl", a);
        if (a >= 7) CHECK(out.size() >= 20, "capacity alerts at %.0f Erl: %zu", a, out.size());
        out.clear();
        tl.tick(kMonday + 25 * 3600 + 900, "TRUNK", out);
        CHECK(out.empty(), "each hour reported once");
    }
}

static void testChanges() {
    TalkgroupChanges c;
    std::vector<Insight> out;
    double t = kMonday;
    for (int i = 0; i < 5; i++) c.addCall(t += 600, "DMR TG 5", false, false, out);
    c.addCall(t += 600, "DMR TG 5", true, false, out);
    CHECK(out.size() == 1 && out[0].kind == "tg_encrypted", "clear to encrypted once");
    out.clear();
    for (int i = 0; i < 3; i++) c.addCall(t += 600, "DMR TG 5", true, false, out);
    c.addCall(t += 600, "DMR TG 5", false, false, out);
    CHECK(out.size() == 1 && out[0].kind == "tg_clear", "back to clear");
    out.clear();
    // flip-flopping: never 3 in a row, no alerts
    for (int i = 0; i < 10; i++) c.addCall(t += 600, "DMR TG 6", i % 2 == 0, false, out);
    CHECK(out.empty(), "flip-flop gives nothing, got %zu", out.size());
    // dormant
    c.addCall(t += 8 * 86400, "DMR TG 5", false, false, out);
    CHECK(out.size() == 1 && out[0].kind == "tg_awake", "awake after 8 days");
    out.clear();
    c.addCall(t += 60, "DMR TG 7", false, true, out);
    c.addCall(t += 1200, "DMR TG 7", false, true, out);
    CHECK(out.empty(), "two emergencies 20 min apart: no cluster");
    c.addCall(t += 300, "DMR TG 7", false, true, out);
    c.addCall(t += 60, "DMR TG 7", false, true, out);
    int em = 0;
    for (const auto& i : out) em += i.kind == "tg_emergency";
    CHECK(em == 1 && out[0].severity == 2, "one emergency cluster alarm, got %d", em);
}

int main() {
    testBasics();
    testHeatmap();
    testTrunk();
    testChanges();
    return testSummary("voice_insight");
}
