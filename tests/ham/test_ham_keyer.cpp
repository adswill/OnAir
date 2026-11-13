// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham CW keyer and contest helpers: paddle scripts through every keyer mode (the element sequences of iambic A / B, Ultimatic, bug, straight),
// the edges back to text with the CW timing decoder, keyed audio without clicks; contest exchanges, cut numbers, the received exchange from
// decoded text, dupes and the Cabrillo line. Nothing is transmitted.
#include "dect2/ham/ham_keyer.h"
#include "dect2/ham/ham_contest.h"
#include "../common/test_util.h"
#include <cmath>
#include <string>
using namespace dect2::ham;

// "." / "-" of the key-down lengths (a mark under two dots is a dit)
static std::string elements(const std::vector<KeyEdge>& e, double dot) {
    std::string s;
    for (size_t i = 1; i < e.size(); i++) if (e[i - 1].down && !e[i].down) s += (e[i].ms - e[i - 1].ms) < 2 * dot ? '.' : '-';
    return s;
}
struct Step { double ms; bool dit, dah; };
static std::string play(KeyerMode m, const std::vector<Step>& steps, double endMs, std::vector<KeyEdge>* out = nullptr) {
    KeyerCfg c; c.mode = m; c.wpm = 20;
    CwKeyer k(c);
    for (const auto& s : steps) k.paddles(s.ms, s.dit, s.dah);
    for (double t = 0; t <= endMs; t += 1) k.run(t);
    auto e = k.takeEdges();
    if (out) *out = e;
    return elements(e, k.dotMs());
}

int main() {
    const double dot = 60;   // 20 WPM
    // ---- iambic: one paddle repeats, squeeze alternates ----
    CHECK(play(KeyerMode::IambicB, {{0, true, false}, {290, false, false}}, 1000) == "...", "dit held 290 ms: three dits \"%s\"", play(KeyerMode::IambicB, {{0, true, false}, {290, false, false}}, 1000).c_str());
    CHECK(play(KeyerMode::IambicA, {{0, false, true}, {450, false, false}}, 1500) == "--", "dah held 450 ms: two dahs");
    // squeeze, dit first, released during the second element (the dah, 120 .. 300 ms): A stops ("A"), B adds the bonus dit ("R")
    const std::vector<Step> sq = {{0, true, false}, {20, true, true}, {200, false, false}};
    CHECK(play(KeyerMode::IambicA, sq, 1500) == ".-", "iambic A squeeze: \"%s\"", play(KeyerMode::IambicA, sq, 1500).c_str());
    CHECK(play(KeyerMode::IambicB, sq, 1500) == ".-.", "iambic B squeeze bonus: \"%s\"", play(KeyerMode::IambicB, sq, 1500).c_str());
    // the letter C (-.-.) squeezed dah first, held 4 elements (480 ms of dah dit dah dit = 4 + 2 + 4 + 2 ... released in the last dit)
    const std::vector<Step> c4 = {{0, false, true}, {10, true, true}, {650, false, false}};
    CHECK(play(KeyerMode::IambicA, c4, 2000) == "-.-.", "iambic A squeeze dah first: C \"%s\"", play(KeyerMode::IambicA, c4, 2000).c_str());
    // dah memory (B): a tap of the dah paddle during a dit is sent after it
    const std::vector<Step> mem = {{0, true, false}, {20, true, true}, {40, true, false}, {50, false, false}};
    CHECK(play(KeyerMode::IambicB, mem, 1000) == ".-", "B dah memory \"%s\"", play(KeyerMode::IambicB, mem, 1000).c_str());
    CHECK(play(KeyerMode::IambicA, mem, 1000) == ".", "A has no memory \"%s\"", play(KeyerMode::IambicA, mem, 1000).c_str());
    // Ultimatic: dit held, then dah pressed too: dahs repeat while both are held
    CHECK(play(KeyerMode::Ultimatic, {{0, true, false}, {100, true, true}, {900, false, false}}, 2000).substr(0, 4) == ".---", "Ultimatic: last pressed repeats \"%s\"",
          play(KeyerMode::Ultimatic, {{0, true, false}, {100, true, true}, {900, false, false}}, 2000).c_str());
    // bug: dits automatic, dah paddle straight (a 300 ms key-down)
    {
        std::vector<KeyEdge> e;
        const std::string s = play(KeyerMode::Bug, {{0, true, false}, {170, false, false}, {400, false, true}, {700, false, false}}, 1500, &e);
        CHECK(s == "..-", "bug \"%s\"", s.c_str());
        CHECK(e.size() == 6 && std::fabs(e[5].ms - e[4].ms - 300) < 1, "bug dah is as long as the contact (%zu edges)", e.size());
    }
    // straight key: the edges follow the key
    {
        KeyerCfg c; c.mode = KeyerMode::Straight; CwKeyer k(c);
        k.straight(100, true); k.straight(150, false); k.straight(300, true); k.straight(480, false);
        auto e = k.takeEdges();
        CHECK(e.size() == 4 && e[0].ms == 100 && e[3].ms == 480, "straight key edges");
    }
    // timing: dits are a dot long, gaps a dot, weight
    {
        std::vector<KeyEdge> e;
        play(KeyerMode::IambicB, {{0, true, false}, {170, false, false}}, 1000, &e);
        CHECK(e.size() == 4 && std::fabs(e[1].ms - e[0].ms - dot) < 1.01 && std::fabs(e[2].ms - e[1].ms - dot) < 1.01, "dot and gap 60 ms at 20 WPM");
        KeyerCfg c; c.weight = 60;
        auto t = keyerText("E E", c);
        CHECK(t.size() == 4 && std::fabs(t[1].ms - t[0].ms - 72) < 1e-6, "weight 60: dit 72 ms (%.1f)", t[1].ms - t[0].ms);
    }
    // ---- text through the keyer timing and back through the CW decoder, at several speeds and weights ----
    for (double wpm : {12.0, 20.0, 28.0, 40.0}) {
        KeyerCfg c; c.wpm = wpm;
        const std::string msg = "CQ TEST LY1ABC 5NN 15 TU";
        double got = 0;
        const std::string d = keyerDecode(keyerText(msg, c), &got);
        CHECK(d.find("LY1ABC 5NN 15") != std::string::npos, "%.0f WPM: decoded \"%s\"", wpm, d.c_str());
        CHECK(std::fabs(got - wpm) < wpm * 0.1, "%.0f WPM read as %.1f", wpm, got);
    }
    // the paddles played by a script that types the letters (each element pressed for its own length) give the same text
    {
        KeyerCfg c; c.wpm = 25; c.mode = KeyerMode::IambicB;
        CwKeyer k(c);
        const double d = k.dotMs();
        double t = 0;
        auto press = [&](bool dah) {   // hold the paddle for most of one element, release during it
            k.paddles(t, !dah, dah);
            const double len = dah ? 3 * d : d;
            for (double u = t; u < t + len * 0.6; u += 1) k.run(u);
            k.paddles(t + len * 0.6, false, false);
            t += len + d;
            for (double u = t - d - len * 0.4; u < t; u += 1) k.run(u);
        };
        for (const char* code : {"-.-.", "--.-"}) {   // CQ
            for (const char* p = code; *p; p++) press(*p == '-');
            t += 2 * d;
            for (double u = t - 2 * d; u < t; u += 1) k.run(u);
        }
        for (double u = t; u < t + 1000; u += 1) k.run(u);
        const std::string got = keyerDecode(k.takeEdges());
        CHECK(got.find("CQ") != std::string::npos, "paddles typed CQ: \"%s\"", got.c_str());
    }
    // audio: keyed tone with soft edges (no click: the envelope never jumps)
    {
        KeyerCfg c;
        auto a = keyerAudio(keyerText("TEST", c), 700, 48000);
        float pk = 0, jump = 0;
        for (size_t i = 0; i < a.size(); i++) { pk = std::max(pk, std::fabs(a[i])); if (i) jump = std::max(jump, std::fabs(a[i] - a[i - 1])); }
        CHECK(pk > 0.95f && pk <= 1.0f, "audio peak %.3f", pk);
        CHECK(jump < 0.1f, "no clicks: largest step %.3f", jump);
    }

    // ---- contests ----
    {
        CHECK(cutNumbers("599") == "5NN" && cutNumbers("001") == "TT1" && cutNumbers("150", true) == "AET", "cut numbers");
        CHECK(uncutNumbers("5NN") == "599" && uncutNumbers("TT1") == "001" && uncutNumbers("ONE") == "ONE" && uncutNumbers("NAME") == "NAME", "uncut");
        ContestStation s; s.myCall = "LY1ABC"; s.cqZone = 15; s.ituZone = 29; s.myName = "Kris"; s.myState = ""; s.myClass = "1D"; s.mySection = "DX";
        const KeyerContestDef& ww = *contestById("cqww-rtty");
        CHECK(contestExchangeText(ww, s, 1, "", false) == "599 15", "CQ WW RTTY from DX \"%s\"", contestExchangeText(ww, s, 1, "", false).c_str());
        CHECK(contestExchangeText(*contestById("cqwpx-rtty"), s, 7, "", true) == "599 007 599 007", "WPX twice");
        CHECK(contestExchangeText(*contestById("cqww-cw"), s, 1, "", false) == "5NN 15", "CQ WW CW cut");
        CHECK(contestExchangeText(*contestById("cqwpx-cw"), s, 102, "", false) == "5NN 1T2", "WPX CW cut serial \"%s\"", contestExchangeText(*contestById("cqwpx-cw"), s, 102, "", false).c_str());
        CHECK(contestExchangeText(*contestById("arrl-rtty"), s, 3, "", false) == "599 003", "Roundup from DX: serial");
        CHECK(contestExchangeText(*contestById("bartg-rtty"), s, 3, "1234", false) == "599 003 1234", "BARTG with time");

        ContestExchange x; std::string why;
        CHECK(contestParseExchange(ww, "LY1ABC DE W1AW 599 05 MA 599 05 MA", x, &why) && x.get(ContestField::CqZone) == "5" && x.get(ContestField::State) == "MA", "W1AW in CQ WW RTTY: zone %s state %s (%s)", x.get(ContestField::CqZone).c_str(), x.get(ContestField::State).c_str(), why.c_str());
        CHECK(contestParseExchange(ww, "LY1ABC DL1XYZ 599 14 599 14", x) && x.get(ContestField::CqZone) == "14" && x.get(ContestField::State).empty(), "DX: no state needed");
        CHECK(!contestParseExchange(ww, "LY1ABC DL1XYZ 599", x, &why) && why.find("zone") != std::string::npos, "missing zone: %s", why.c_str());
        CHECK(contestParseExchange(*contestById("cqww-cw"), "TU 5NN 14", x) && x.get(ContestField::Rst) == "599" && x.get(ContestField::CqZone) == "14", "CW cut exchange");
        CHECK(contestParseExchange(*contestById("cqwpx-cw"), "5NN 1T2", x) && x.get(ContestField::Serial) == "102", "WPX CW serial 1T2 -> %s", x.get(ContestField::Serial).c_str());
        CHECK(contestParseExchange(*contestById("arrl-rtty"), "K1ABC 599 ON ON", x) && x.get(ContestField::StateOrSerial) == "ON", "Roundup province");
        CHECK(contestParseExchange(*contestById("naqp-rtty"), "W9XYZ DE K1ABC BOB MA BOB MA", x) && x.get(ContestField::Name) == "BOB" && x.get(ContestField::State) == "MA", "NAQP name %s", x.get(ContestField::Name).c_str());
        CHECK(contestParseExchange(*contestById("arrl-fd"), "K1ABC 2A EMA", x) && x.get(ContestField::Class) == "2A" && x.get(ContestField::Section) == "EMA", "Field Day");
        CHECK(contestParseExchange(*contestById("iaru-hf"), "599 28", x) && x.get(ContestField::ItuZone) == "28", "IARU ITU zone");
        CHECK(!contestParseExchange(*contestById("iaru-hf"), "599 95", x), "ITU zone 95 refused");

        ContestLog log(ww);
        ContestQso q; q.call = "W1AW"; q.band = "20m"; q.mode = "RY"; q.freqHz = 14080000; q.utc = 1792670400; q.sent = "599 15"; q.rcvd = "599 05 MA";
        CHECK(!log.dupe("W1AW", "20m", "RY"), "not a dupe yet");
        log.add(q);
        CHECK(log.dupe("w1aw", "20m", "RY") && !log.dupe("W1AW", "40m", "RY") && log.nextSerial() == 2, "dupe on the same band and mode only");
        const std::string line = log.cabrilloLine(q, "LY1ABC");
        CHECK(line == "QSO: 14080 RY 2026-10-22 1200 LY1ABC        599 15     W1AW          599 05 MA", "Cabrillo \"%s\"", line.c_str());
        const std::string cab = log.cabrillo(s);
        CHECK(cab.find("CONTEST: CQ-WW-RTTY") != std::string::npos && cab.find("END-OF-LOG:") != std::string::npos, "Cabrillo file");
        auto m = contestMacros(ww, s, "W1AW", contestExchangeText(ww, s, 2, "", true));
        CHECK(m.size() == 6 && m[1].text == "W1AW 599 15 599 15 W1AW\n", "exchange macro \"%s\"", m[1].text.c_str());
    }
    return testSummary("ham_keyer");
}
