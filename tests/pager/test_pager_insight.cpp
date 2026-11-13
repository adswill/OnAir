// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pager insights (docs/SMART5-pager-3.9.md) and the insight hub that feeds every analyser from the automation events.
// Simulated page streams only: steady automatic pages with jitter, random dispatch pages, bursts, a channel going silent.
#include "dect2/insight/hub.h"
#include <cstdio>
#include <random>
#include "../common/test_util.h"

using namespace dect2::insight;

static const double kT0 = 1767571200;

static std::string randomText(std::mt19937& r) {
    static const char* w[] = {"FIRE", "ALARM", "MAIN", "ST", "UNIT", "RESPOND", "CODE", "BUILDING", "SMOKE", "CALL", "BACK", "WARD", "ROOM", "NORTH", "GATE", "TRUCK"};
    std::uniform_int_distribution<int> p(0, 15), n(3, 7), d(1, 999);
    std::string s;
    const int k = n(r);
    for (int i = 0; i < k; i++) s += std::string(w[p(r)]) + " ";
    return s + std::to_string(d(r));
}

static int count(const std::vector<Insight>& v, const char* kind) { int n = 0; for (const auto& i : v) n += i.kind == kind; return n; }

static void testAutomatic() {
    std::mt19937 r(5);
    int detected = 0, cases = 0;
    for (double every : {60.0, 300.0, 900.0, 3600.0})
        for (double jit : {0.0, 0.01, 0.04}) {
            PagerPatterns p;
            std::vector<Insight> out;
            std::normal_distribution<double> j(0, jit * every);
            for (int i = 0; i < 8; i++) {
                char txt[64];
                snprintf(txt, sizeof txt, "TEST PAGE %02d:%02d SITE 4", (i * (int)every / 3600) % 24, (i * (int)every / 60) % 60);
                p.add(kT0 + i * every + j(r), "POCSAG 1000", "152.0", txt, out);
            }
            cases++;
            const auto inf = p.info("POCSAG 1000");
            if (count(out, "pager_auto") == 1 && inf.automatic && std::fabs(inf.intervalSec - every) < 0.1 * every) detected++;
            else printf("  missed: every %.0f s jitter %.0f %%\n", every, jit * 100);
        }
    printf("automatic pages: %d/%d steady streams found\n", detected, cases);
    CHECK(detected == cases, "automatic %d/%d", detected, cases);
    // irregular gaps (30 %), random texts at a steady interval, random dispatch: never automatic
    int wrong = 0;
    for (unsigned s = 1; s <= 20; s++) {
        std::mt19937 q(s);
        std::normal_distribution<double> big(0, 0.3 * 600);
        std::exponential_distribution<double> poisson(1.0 / 600);
        PagerPatterns a, b, c;
        std::vector<Insight> out;
        double t = kT0;
        for (int i = 0; i < 12; i++) a.add(kT0 + i * 600 + big(q), "POCSAG 2", "c", "TEST PAGE", out);
        for (int i = 0; i < 12; i++) b.add(kT0 + i * 600, "POCSAG 3", "c", randomText(q), out);
        for (int i = 0; i < 12; i++) c.add(t += poisson(q), "POCSAG 4", "c", "AMBULANCE CALL WARD 3", out);
        wrong += a.automatic("POCSAG 2") + b.automatic("POCSAG 3") + c.automatic("POCSAG 4");
    }
    printf("automatic pages: %d/60 irregular or varied streams wrongly marked\n", wrong);
    CHECK(wrong <= 1, "false automatic %d", wrong);
}

static void testBurstAndSilent() {
    std::mt19937 r(9);
    std::exponential_distribution<double> gap(1.0 / 90);
    int bursts = 0, falseBursts = 0;
    for (int run = 0; run < 20; run++) {
        PagerPatterns p;
        std::vector<Insight> out;
        double t = kT0;
        // two hours of ordinary traffic from 30 capcodes, one page every 90 s on average
        std::uniform_int_distribution<int> cap(0, 29);
        while (t < kT0 + 7200) { p.add(t, "POCSAG " + std::to_string(100 + cap(r)), "152.0", randomText(r), out); t += gap(r); }
        falseBursts += count(out, "pager_burst");
        out.clear();
        // a capcode never heard pages 7 times in 4 minutes
        for (int i = 0; i < 7; i++) p.add(t + i * 35, "POCSAG 999", "152.0", randomText(r), out);
        bursts += count(out, "pager_burst");
    }
    printf("paging bursts: %d/20 found, %d false in 40 h of ordinary traffic\n", bursts, falseBursts);
    CHECK(bursts == 20 && falseBursts == 0, "bursts %d false %d", bursts, falseBursts);
    // silent channel
    PagerPatterns p;
    std::vector<Insight> out;
    double t = kT0;
    for (int i = 0; i < 40; i++) p.add(t += gap(r), "POCSAG " + std::to_string(i % 5), "152.0", randomText(r), out);
    out.clear();
    p.tick(t + 1000, out);
    CHECK(out.empty(), "not silent after 1000 s");
    p.tick(t + 2000, out);
    CHECK(count(out, "pager_silent") == 1 && out[0].key == "152.0", "silent after 2000 s");
    out.clear();
    p.tick(t + 4000, out);
    CHECK(out.empty(), "said once");
}

static dect2::automation::Event callEv(double end, double dur, const std::string& key, const char* verdict, bool emergency, double hz = 438.5e6) {
    dect2::automation::Event e("session.call");
    e.at(end, "DMR", hz);
    e.str("key", key).num("duration_s", dur).str("verdict", verdict).flag("emergency", emergency).str("kind", "call");
    return e;
}

static void testHub() {
    Hub h;
    h.setStation("LY1ABC", "KO24");
    // FT8
    h.onFtx(kT0, "20m", "CQ DL1XYZ JO62", -8);
    h.onFtx(kT0 + 15, "20m", "LY1ABC DL1XYZ -12", -9);
    h.onFtx(kT0 + 45, "20m", "LY1ABC DL1XYZ RR73", -9);
    auto v = h.take();
    CHECK(count(v, "heard_me") == 1, "heard_me once, got %d", count(v, "heard_me"));
    if (!v.empty()) {
        const auto e = toEvent(v[0]);
        CHECK(e.type == "insight.heard_me" && e.f.at("area").s == "ham" && e.f.count("lat"), "event %s", e.type.c_str());
        CHECK(v[0].text.find("-12 dB") != std::string::npos && v[0].text.find("km") != std::string::npos, "text: %s", v[0].text.c_str());
    }
    // voice: private calls ignored, a talkgroup going encrypted, an emergency cluster
    for (int i = 0; i < 4; i++) h.onEvent(callEv(kT0 + 100 + i * 60, 10, "TG 91/slot1", "clear", false));
    h.onEvent(callEv(kT0 + 400, 10, "TG 91/slot1", "encrypted", false));
    for (int i = 0; i < 6; i++) h.onEvent(callEv(kT0 + 500 + i, 5, "Private 2345", i % 2 ? "encrypted" : "clear", true));
    h.onEvent(callEv(kT0 + 600, 10, "TG 9/slot2", "clear", true));
    h.onEvent(callEv(kT0 + 700, 10, "TG 9/slot2", "clear", true));
    v = h.take();
    CHECK(count(v, "tg_encrypted") == 1 && count(v, "tg_emergency") == 1, "voice: encrypted %d emergency %d", count(v, "tg_encrypted"), count(v, "tg_emergency"));
    for (const auto& i : v) CHECK(i.key.find("2345") == std::string::npos, "a private call reached a finding: %s", i.text.c_str());
    CHECK(h.trunk("DMR").channels() == 3, "three channels (two slots and the private call's) seen, got %d", h.trunk("DMR").channels());
    // non-voice sessions are ignored
    dect2::automation::Event acars("session.call");
    acars.at(kT0 + 800, "ACARS", 131.725e6);
    acars.str("key", "TG 1").str("verdict", "encrypted");
    h.onEvent(acars);
    CHECK(h.take().empty(), "ACARS session ignored");
    // APRS
    for (int i = 0; i < 3; i++) {
        dect2::automation::Event a("aprs.packet");
        a.at(kT0 + 900 + i, "APRS", 144.8e6);
        a.str("from", "LY2OLD").str("path", "RELAY,WIDE3-3").str("info", "!x").num("lat", 54.7).num("lon", 25.3);
        h.onEvent(a);
    }
    v = h.take();
    CHECK(count(v, "aprs_path") == 1, "APRS path once, got %d", count(v, "aprs_path"));
    // pagers: an automatic page through the hub
    for (int i = 0; i < 8; i++) {
        dect2::automation::Event p("pager.message");
        p.at(kT0 + 1000 + i * 300, "POCSAG", 152.0e6);
        p.str("protocol", "POCSAG").num("address", 1234567).str("text", "TEST " + std::to_string(i));
        h.onEvent(p);
    }
    v = h.take();
    CHECK(count(v, "pager_auto") == 1 && h.pagerPatterns().automatic("POCSAG 1234567"), "pager auto through the hub");
    CHECK(h.recent().size() >= 5, "recent kept %zu", h.recent().size());
}

int main() {
    testAutomatic();
    testBurstAndSilent();
    testHub();
    return testSummary("pager_insight");
}
