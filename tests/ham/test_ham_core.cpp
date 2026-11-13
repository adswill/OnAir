// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham operation core: band table and TX gate, ADIF round trip and worked-before, slot timing, message parsing, the QSO state machine.
#include "dect2/ham/ham.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace dect2::ham;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static QsoEngine mk(int mode = 0) {
    QsoEngine::Config c;
    c.myCall = "LY1ABC"; c.myGrid = "KO24"; c.mode = mode;
    return QsoEngine(c);
}

int main() {
    // bands and the gate
    CHECK(bandNameFor(14.074e6) == "20m");
    CHECK(bandNameFor(14.4e6).empty());
    CHECK(hamSignalInBand(14.074e6, 1500, 50));
    CHECK(!hamSignalInBand(14.349e6, 1500, 50));   // runs past the band edge
    CHECK(!hamSignalInBand(100e6, 1000, 50));     // FM broadcast
    CHECK(hamCallValid("LY1ABC") && hamCallValid("K1A") && hamCallValid("DL/LY1ABC/P") && !hamCallValid("HELLO") && !hamCallValid("12345"));
    CHECK(hamGridValid("KO24") && hamGridValid("ko24ab") && !hamGridValid("ZZ99") && !hamGridValid("KO2"));
    HamTxCheck g; g.call = "LY1ABC"; g.grid = "KO24"; g.dialHz = 14.074e6; g.audioHz = 1500;
    std::string why;
    // owner rule 2026-10-26: only Arm stops a burst; everything else is a warning (the user decides)
    CHECK(!hamTxAllowed(g, why) && why.find("armed") != std::string::npos);
    g.armed = true;
    CHECK(hamTxAllowed(g, why) && why.find("No amateur licence is set") != std::string::npos && why.find("responsible") != std::string::npos);   // no licence: warned
    g.licenceConfirmed = true;
    CHECK(hamTxAllowed(g, why) && why.empty() && hamTxWarnings(g).empty());
    g.dialHz = 100e6;
    CHECK(hamTxAllowed(g, why) && why.find("is not an amateur band") != std::string::npos);
    g.dialHz = 27.265e6;   // 11 m (CB): not an amateur band, warned, then it transmits
    CHECK(hamTxAllowed(g, why) && why.find("27.265 MHz is not an amateur band (11 m is CB). You are responsible for being allowed to transmit here.") != std::string::npos);
    g.dialHz = 27.185e6; g.audioHz = 1000;
    CHECK(hamTxAllowed(g, why) && hamTxWarnings(g).size() == 1);
    g.dialHz = 14.074e6; g.call = "X";
    CHECK(hamTxAllowed(g, why) && why.find("callsign") != std::string::npos);
    g.call = "LY1ABC"; g.dialHz = std::nan("");
    CHECK(!hamTxAllowed(g, why) && why.find("not valid") != std::string::npos);   // not a number: cannot be keyed
    g.dialHz = 14.074e6; g.armed = false;
    CHECK(!hamTxAllowed(g, why));

    // timing
    CHECK(slotPeriod(0) == 15.0 && slotPeriod(1) == 7.5 && slotPeriod(2) == 3.75);
    CHECK(slotEven(30.0, 15) && !slotEven(15.0, 15) && slotEven(44.9, 15) && !slotEven(45.0, 15));
    CHECK(slotEven(0, 7.5) && !slotEven(7.5, 7.5) && slotEven(15, 7.5));
    CHECK(slotStart(slotIndex(37.2, 15), 15) == 30.0);
    CHECK(slotIndex(3.74, 3.75) == 0 && slotIndex(3.75, 3.75) == 1);

    // messages
    CHECK(reportText(-5) == "-05" && reportText(12) == "+12" && reportText(-99) == "-30" && reportText(99) == "+32");
    CHECK(msgCq("ly1abc", "ko24ab") == "CQ LY1ABC KO24");
    CHECK(msgReport("K1ABC", "LY1ABC", -7, true) == "K1ABC LY1ABC R-07");
    CHECK(msgFree("hello world, this is long") == "HELLO WORLD T");
    ParsedMsg p = parseMessage("LY1ABC K1ABC -12");
    CHECK(p.kind == ParsedMsg::Report && p.to == "LY1ABC" && p.from == "K1ABC" && p.report == -12);
    CHECK(parseMessage("LY1ABC K1ABC R+03").kind == ParsedMsg::RReport);
    CHECK(parseMessage("LY1ABC K1ABC RR73").kind == ParsedMsg::RR73);
    CHECK(parseMessage("LY1ABC K1ABC 73").kind == ParsedMsg::Bye73);
    CHECK(parseMessage("LY1ABC K1ABC FN42").kind == ParsedMsg::Grid);
    p = parseMessage("CQ DX K1ABC FN42");
    CHECK(p.kind == ParsedMsg::Cq && p.from == "K1ABC" && p.grid == "FN42");
    CHECK(parseMessage("CQ K1ABC FN42").from == "K1ABC");

    // ADIF
    QsoRecord q; q.call = "K1ABC"; q.grid = "FN42"; q.mode = "FT8"; q.freqHz = 14.074e6; q.rstSent = "-05"; q.rstRcvd = "-10";
    q.myCall = "LY1ABC"; q.myGrid = "KO24"; q.startUtc = 1760918400.0 + 3661; q.endUtc = q.startUtc + 30;   // 2025-10-20 01:01:01
    const std::string rec = adifRecord(q);
    CHECK(rec.find("<CALL:5>K1ABC") != std::string::npos && rec.find("<QSO_DATE:8>20251020") != std::string::npos && rec.find("<TIME_ON:6>010101") != std::string::npos);
    CHECK(rec.find("<BAND:3>20m") != std::string::npos && rec.find("<EOR>") != std::string::npos);
    const auto back = adifParse(adifHeader() + rec);
    CHECK(back.size() == 1 && back[0].call == "K1ABC" && back[0].grid == "FN42" && back[0].mode == "FT8" && back[0].rstRcvd == "-10" && back[0].startUtc == q.startUtc);
    QsoRecord q4 = q; q4.mode = "FT4"; q4.call = "DL1XYZ";
    const std::string r4 = adifRecord(q4);
    CHECK(r4.find("<MODE:4>MFSK") != std::string::npos && r4.find("<SUBMODE:3>FT4") != std::string::npos);
    CHECK(adifParse(r4).size() == 1 && adifParse(r4)[0].mode == "FT4");
    const std::string path = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/ham_test_" + std::to_string(rand()) + ".adi";
    std::remove(path.c_str());
    std::string err;
    CHECK(adifAppend(path, q, err) && adifAppend(path, q4, err));
    WorkedLog w;
    CHECK(w.load(path) && w.size() == 2);
    CHECK(w.worked("k1abc") && !w.worked("W9ZZZ"));
    CHECK(w.workedOn("K1ABC", "20m", "FT8") && !w.workedOn("K1ABC", "20m", "FT4") && w.workedOn("DL1XYZ", "20m", "FT4") && !w.workedOn("K1ABC", "40m", "FT8"));
    std::remove(path.c_str());

    // engine: we call a station (Tx1 .. Tx5)
    {
        QsoEngine e = mk(); int logged = 0; QsoRecord lq;
        e.onLog = [&](const QsoRecord& r) { logged++; lq = r; };
        CHECK(e.messageForSlot(0).empty());   // idle sends nothing
        e.callStation(100, "K1ABC", "FN42", -8, 105.0 /*slot index 7: odd*/, 1234);
        CHECK(e.state() == QsoEngine::Tx1Grid && e.config().txEven && e.config().txHz == 1234);
        CHECK(e.messageForSlot(105.0).empty());   // odd slot: not ours
        CHECK(e.messageForSlot(120.0) == "K1ABC LY1ABC KO24");
        e.txSent(120);
        e.onDecode(136, "LY1ABC K1ABC -11", -11, 135.0, 1234);
        CHECK(e.state() == QsoEngine::Tx3RReport);
        CHECK(e.messageForSlot(150.0) == "K1ABC LY1ABC R-11");
        e.txSent(150);
        e.onDecode(166, "LY1ABC K1ABC RR73", -10, 165.0, 1234);
        CHECK(e.state() == QsoEngine::Tx5Bye && logged == 1 && lq.call == "K1ABC" && lq.rstRcvd == "-11" && lq.rstSent == "-11");
        CHECK(e.messageForSlot(180.0) == "K1ABC LY1ABC 73");
        e.txSent(180);
        CHECK(e.state() == QsoEngine::Idle && logged == 1);
    }
    // engine: CQ answered (Cq .. Tx4)
    {
        QsoEngine e = mk(); int logged = 0; QsoRecord lq;
        e.onLog = [&](const QsoRecord& r) { logged++; lq = r; };
        e.startCq(0);
        CHECK(e.messageForSlot(0) == "CQ LY1ABC KO24");
        e.txSent(0);
        e.onDecode(16, "LY1ABC DL1XYZ JO62", -3, 15.0, 1000);
        CHECK(e.state() == QsoEngine::Tx2Report && e.dxCall() == "DL1XYZ" && e.config().txEven);
        CHECK(e.messageForSlot(30.0) == "DL1XYZ LY1ABC -03");
        e.txSent(30);
        e.onDecode(46, "LY1ABC DL1XYZ R-09", -3, 45.0, 1000);
        CHECK(e.state() == QsoEngine::Tx4RR73 && logged == 1 && lq.rstRcvd == "-09" && lq.rstSent == "-03");
        CHECK(e.messageForSlot(60.0) == "DL1XYZ LY1ABC RR73");
        e.txSent(60);
        e.onDecode(76, "LY1ABC DL1XYZ 73", -3, 75.0, 1000);
        CHECK(e.state() == QsoEngine::Cq && logged == 1);   // back to CQ
        CHECK(e.messageForSlot(90.0) == "CQ LY1ABC KO24");
    }
    // other stations are ignored; hold keeps the TX frequency; autoSeq off
    {
        QsoEngine e = mk(); e.config().hold = true; e.config().txHz = 1700;
        e.callStation(0, "K1ABC", "", 0, 15.0, 900);
        CHECK(e.config().txHz == 1700);
        e.onDecode(16, "LY1ABC W9ZZZ -05", -5, 15.0, 900);
        CHECK(e.state() == QsoEngine::Tx1Grid);
        e.onDecode(16, "OTHER K1ABC -05", -5, 15.0, 900);
        CHECK(e.state() == QsoEngine::Tx1Grid);
        e.config().autoSeq = false;
        e.onDecode(16, "LY1ABC K1ABC -05", -5, 15.0, 900);
        CHECK(e.state() == QsoEngine::Tx1Grid);
    }
    // retries, halt and the watchdog
    {
        QsoEngine e = mk(); std::string ev; e.onEvent = [&](const std::string& s) { ev = s; };
        e.callStation(0, "K1ABC", "", 0, 15.0, 900);
        for (int i = 0; i < 4; i++) { CHECK(!e.messageForSlot(30.0 + 30 * i).empty()); e.txSent(30 + 30 * i); }
        CHECK(e.messageForSlot(150.0).empty() && e.state() == QsoEngine::Idle && ev == "retries");
        e.callStation(200, "K1ABC", "", 0, 195.0, 900);
        e.halt();
        CHECK(e.state() == QsoEngine::Idle && e.messageForSlot(210.0).empty());
        e.startCq(1000);
        CHECK(!e.messageForSlot(1000.0).empty());
        CHECK(e.messageForSlot(1000.0 + 400).empty() && e.watchdogHit() && ev == "watchdog" && !e.active());
        e.startCq(2000); e.userAction(2300);
        CHECK(!e.messageForSlot(2400.0).empty());   // 100 s since the last user action
    }
    // FT4: 7.5 s slots
    {
        QsoEngine e = mk(1);
        e.callStation(0, "K1ABC", "", 0, 7.5 /*odd*/, 800);
        CHECK(e.config().txEven && e.messageForSlot(15.0) == "K1ABC LY1ABC KO24" && e.messageForSlot(22.5).empty());
    }
    printf(fails ? "ham: %d FAILED\n" : "ham: all passed\n", fails);
    return fails ? 1 : 0;
}
