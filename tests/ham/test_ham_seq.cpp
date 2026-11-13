// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The QSO sequencer with WSJT-X style message flows (4.0): a CQ answered with a report straight away, RR73 sent once (no 73 needed) and sent
// again when the R-report is repeated (logged once), a station that repeats its grid or report keeps the QSO alive past the retry limit,
// caller priority among several callers of one slot, CQ DX / CQ POTA, JT65 RRR waiting for the 73, calling a station that works someone else,
// and the Q65 / FST4 / MSK144 mode tables. Simulated decode streams only (the radio path is tests/ham/test_ham_modes.cpp).
#include "dect2/ham/ham.h"
#include <cstdio>
#include <string>
using namespace dect2::ham;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static QsoEngine mk(int mode = 0) {
    QsoEngine::Config c;
    c.myCall = "LY1ABC"; c.myGrid = "KO24pq"; c.mode = mode; c.watchdogSec = 0;
    return QsoEngine(c);
}

int main() {
    // ---- CQ answered with a report at once (the caller skipped Tx1) ----
    {
        QsoEngine e = mk(); int logged = 0; QsoRecord lq;
        e.onLog = [&](const QsoRecord& r) { logged++; lq = r; };
        e.startCq(0);
        CHECK(e.messageForSlot(0) == "CQ LY1ABC KO24"); e.txSent(0);
        e.onDecode(16, "LY1ABC K1ABC -12", -15, 15, 1200);
        CHECK(e.state() == QsoEngine::Tx3RReport && e.dxCall() == "K1ABC");
        CHECK(e.messageForSlot(30) == "K1ABC LY1ABC R-15"); e.txSent(30);
        e.onDecode(46, "LY1ABC K1ABC RR73", -14, 45, 1200);
        CHECK(e.state() == QsoEngine::Tx5Bye && logged == 1 && lq.rstRcvd == "-12" && lq.rstSent == "-15" && lq.call == "K1ABC");
        CHECK(e.messageForSlot(60) == "K1ABC LY1ABC 73"); e.txSent(60);
        CHECK(e.state() == QsoEngine::Cq && e.messageForSlot(90) == "CQ LY1ABC KO24" && logged == 1);
    }
    // ---- RR73 once; the R-report repeated gets RR73 again, logged once ----
    {
        QsoEngine e = mk(); int logged = 0;
        e.onLog = [&](const QsoRecord&) { logged++; };
        e.startCq(0); e.messageForSlot(0); e.txSent(0);
        e.onDecode(16, "LY1ABC DL1XYZ JO62", -3, 15, 1000);
        CHECK(e.messageForSlot(30) == "DL1XYZ LY1ABC -03"); e.txSent(30);
        e.onDecode(46, "LY1ABC DL1XYZ R-09", -4, 45, 1000);
        CHECK(e.state() == QsoEngine::Tx4RR73 && logged == 1);
        CHECK(e.messageForSlot(60) == "DL1XYZ LY1ABC RR73"); e.txSent(60);
        CHECK(e.state() == QsoEngine::Cq && e.lastDx() == "DL1XYZ");            // RR73 needs no answer: CQ again
        e.onDecode(76, "LY1ABC DL1XYZ R-09", -4, 75, 1000);                    // they did not copy it
        CHECK(e.state() == QsoEngine::Tx4RR73 && e.dxCall() == "DL1XYZ");
        CHECK(e.messageForSlot(90) == "DL1XYZ LY1ABC RR73"); e.txSent(90);
        CHECK(e.state() == QsoEngine::Cq && logged == 1);
        e.onDecode(166, "LY1ABC DL1XYZ R-09", -4, 165, 1000);                  // long after: a new call would be needed, not answered
        CHECK(e.state() == QsoEngine::Cq);
        e.onDecode(181, "LY1ABC DL1XYZ 73", -4, 180, 1000);
        CHECK(e.state() == QsoEngine::Cq && logged == 1);
    }
    // ---- the station repeats its grid: our report was not copied; the retry limit counts only unanswered sends ----
    {
        QsoEngine e = mk();
        e.startCq(0); e.messageForSlot(0); e.txSent(0);
        e.onDecode(16, "LY1ABC DL1XYZ JO62", -3, 15, 1000);
        for (int i = 0; i < 6; i++) {
            CHECK(e.messageForSlot(30 + 30 * i) == "DL1XYZ LY1ABC -03");
            e.txSent(30 + 30 * i);
            e.onDecode(46 + 30 * i, "LY1ABC DL1XYZ JO62", -3, 45 + 30 * i, 1000);
        }
        CHECK(e.state() == QsoEngine::Tx2Report);
        // then silence: four more sends and the QSO is given up, CQ resumes
        for (int i = 0; i < 4; i++) { CHECK(e.messageForSlot(210 + 30 * i) == "DL1XYZ LY1ABC -03"); e.txSent(210 + 30 * i); }
        CHECK(e.messageForSlot(330) == "CQ LY1ABC KO24" && e.state() == QsoEngine::Cq);
    }
    // ---- they repeat the report: we keep sending the R-report, with their new report ----
    {
        QsoEngine e = mk();
        e.callStation(0, "K1ABC", "FN42", -10, 15, 900);
        e.messageForSlot(30); e.txSent(30);
        e.onDecode(46, "LY1ABC K1ABC -10", -10, 45, 900);
        for (int i = 0; i < 5; i++) { CHECK(e.messageForSlot(60 + 30 * i) == "K1ABC LY1ABC R-10"); e.txSent(60 + 30 * i); e.onDecode(76 + 30 * i, "LY1ABC K1ABC -08", -10, 75 + 30 * i, 900); }
        CHECK(e.state() == QsoEngine::Tx3RReport && e.messageForSlot(210) == "K1ABC LY1ABC R-10");
    }
    // ---- several callers in one slot: the priority hook picks; without it the first one stays ----
    {
        QsoEngine e = mk();
        e.priority = [](const std::string& c) { return c == "3Y0J" ? 10 : 0; };
        e.startCq(0); e.messageForSlot(0); e.txSent(0);
        e.onDecode(16, "LY1ABC DL1XYZ JO62", -3, 15, 1000);
        e.onDecode(16, "LY1ABC 3Y0J JD15", -19, 15, 1400);
        CHECK(e.dxCall() == "3Y0J" && e.state() == QsoEngine::Tx2Report && e.messageForSlot(30) == "3Y0J LY1ABC -19");
        e.txSent(30);
        e.onDecode(46, "LY1ABC DL1XYZ JO62", -3, 45, 1000);                    // answered already: no switch now
        CHECK(e.dxCall() == "3Y0J");
        QsoEngine f = mk();
        f.startCq(0); f.messageForSlot(0); f.txSent(0);
        f.onDecode(16, "LY1ABC DL1XYZ JO62", -3, 15, 1000);
        f.onDecode(16, "LY1ABC 3Y0J JD15", -19, 15, 1400);
        CHECK(f.dxCall() == "DL1XYZ");
        QsoEngine g = mk();                                                      // a lower priority does not replace
        g.priority = [](const std::string& c) { return c == "DL1XYZ" ? 5 : 1; };
        g.startCq(0); g.messageForSlot(0); g.txSent(0);
        g.onDecode(16, "LY1ABC DL1XYZ JO62", -3, 15, 1000);
        g.onDecode(16, "LY1ABC K1ABC FN42", -3, 15, 1100);
        CHECK(g.dxCall() == "DL1XYZ");
    }
    // ---- CQ with a modifier ----
    {
        QsoEngine e = mk();
        e.config().cqMod = "dx";
        e.startCq(0);
        CHECK(e.messageForSlot(0) == "CQ DX LY1ABC KO24");
        const ParsedMsg p = parseMessage("CQ DX LY1ABC KO24");
        CHECK(p.kind == ParsedMsg::Cq && p.from == "LY1ABC" && p.grid == "KO24");
        CHECK(msgCqMod("POTA", "K1ABC", "FN42") == "CQ POTA K1ABC FN42" && msgCqMod("", "K1ABC", "FN42") == "CQ K1ABC FN42");
        CHECK(msgCqMod("a1-b", "K1ABC", "") == "CQ AB K1ABC" && msgCqMod("TOOLONG", "K1ABC", "FN42") == "CQ TOOL K1ABC FN42");
        CHECK(parseMessage("CQ POTA K1ABC FN42").from == "K1ABC");
    }
    // ---- JT65: RRR waits for the 73 ----
    {
        QsoEngine e = mk(4); int logged = 0;
        e.onLog = [&](const QsoRecord&) { logged++; };
        e.startCq(0);
        CHECK(e.messageForSlot(0) == "CQ LY1ABC KO24"); e.txSent(0);
        e.onDecode(62, "LY1ABC K1ABC FN42", -21, 60, 1270);
        CHECK(e.messageForSlot(120) == "K1ABC LY1ABC -21"); e.txSent(120);
        e.onDecode(182, "LY1ABC K1ABC R-23", -21, 180, 1270);
        CHECK(e.state() == QsoEngine::Tx4RR73 && logged == 1);
        CHECK(e.messageForSlot(240) == "K1ABC LY1ABC RRR"); e.txSent(240);
        CHECK(e.state() == QsoEngine::Tx4RR73);                                 // RRR: wait for their 73
        CHECK(e.messageForSlot(360) == "K1ABC LY1ABC RRR"); e.txSent(360);
        e.onDecode(302, "LY1ABC K1ABC 73", -21, 300, 1270);
        CHECK(e.state() == QsoEngine::Cq && logged == 1);
    }
    // ---- calling a station that answers someone else: keep calling (tail-ending), give up after the retries ----
    {
        QsoEngine e = mk(); std::string ev;
        e.onEvent = [&](const std::string& s) { ev = s; };
        e.callStation(0, "K1ABC", "FN42", -10, 15, 900);
        CHECK(e.messageForSlot(30) == "K1ABC LY1ABC KO24"); e.txSent(30);
        e.onDecode(46, "OH2XX K1ABC -05", -5, 45, 900);
        CHECK(e.state() == QsoEngine::Tx1Grid);
        for (int i = 1; i < 4; i++) { CHECK(!e.messageForSlot(30 + 30 * i).empty()); e.txSent(30 + 30 * i); }
        CHECK(e.messageForSlot(150).empty() && ev == "retries" && e.state() == QsoEngine::Idle);
    }
    // ---- the newer modes in the tables ----
    CHECK(slotPeriod(6) == 60 && slotPeriod(7) == 60 && slotPeriod(8) == 15);
    CHECK(std::string(hamModeName(6)) == "Q65" && std::string(hamModeName(7)) == "FST4" && std::string(hamModeName(8)) == "MSK144");
    CHECK(hamModeHasQso(6) && hamModeHasQso(7) && hamModeHasQso(8) && !hamModeUsesRRR(6) && !hamModeUsesRRR(8));
    CHECK(hamModeBwHz(6, 0) == 110 && hamModeBwHz(6, 2) == 440 && hamModeBwHz(8, 0) == 2400 && txStartDelay(6) == 1.0);
    {   // MSK144: 15 s, RR73 once
        QsoEngine e = mk(8);
        e.callStation(0, "W1AW", "FN31", 5, 15, 1500);
        CHECK(e.messageForSlot(30) == "W1AW LY1ABC KO24"); e.txSent(30);
        e.onDecode(46, "LY1ABC W1AW R+05", 5, 45, 1500);
        CHECK(e.state() == QsoEngine::Tx4RR73 && e.messageForSlot(60) == "W1AW LY1ABC RR73");
        e.txSent(60);
        CHECK(e.state() == QsoEngine::Idle);
    }

    printf(fails ? "ham_seq: %d FAILED\n" : "ham_seq: all passed\n", fails);
    return fails ? 1 : 0;
}
