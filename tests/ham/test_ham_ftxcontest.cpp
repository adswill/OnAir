// FT8 / FT4 contest exchanges (4.0): the 77-bit ARRL Field Day (i3 0, n3 3 / 4) and RTTY Roundup (i3 3) messages pack and unpack to the same
// text (the decoder could read them before; now OnAir can send them), what is not such a message still packs as before, the Ham parser
// reads the exchange, and the sequencer runs the WSJT-X contest sequence both ways and logs the exchange for Cabrillo.
#include "../../core/src/hfdig/hfdig_ftx_int.h"
#include "dect2/ham/ham.h"
#include <cstdio>
#include <cstring>
#include <string>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    using namespace dect2;
    // ---- pack / unpack ----
    {
        const char* msgs[] = {"WA9XYZ KA1ABC R 16A EMA", "K1ABC W9XYZ 6A WI", "K1ABC W9XYZ 17B NTX", "K1ABC W9XYZ R 32F ONE",
                              "W9XYZ K1ABC 579 WI", "W9XYZ K1ABC R 589 MA", "TU; W9XYZ K1ABC R 579 MA", "K1ABC G4XYZ 559 0013", "K1ABC G4XYZ R 529 7999",
                              "K1ABC VE3XYZ 599 ON", "K1ABC W9XYZ 579 X42"};
        ftx::CallHash hash;
        for (const char* m : msgs) {
            uint8_t b[77];
            const bool ok = ftx::pack77(m, b);
            CHECK(ok);
            const std::string u = ok ? ftx::unpack77(b, &hash) : "";
            if (u != m) printf("  \"%s\" came back as \"%s\"\n", m, u.c_str());
            CHECK(u == m);
        }
        // not contest messages: unchanged behaviour (standard, free text)
        uint8_t b[77];
        CHECK(ftx::pack77("K1ABC W9XYZ R EN37", b) && ftx::unpack77(b, &hash) == "K1ABC W9XYZ R EN37");
        CHECK(!ftx::pack77("K1ABC W9XYZ 33A WI", b) || ftx::unpack77(b, &hash) != "K1ABC W9XYZ 33A WI");   // 33 transmitters: not Field Day (free text)
        CHECK(!ftx::pack77("K1ABC W9XYZ 6G WI", b) || ftx::unpack77(b, &hash) != "K1ABC W9XYZ 6G WI");     // class G does not exist
        CHECK(!ftx::pack77("K1ABC W9XYZ 6A XYZ", b) || ftx::unpack77(b, &hash) != "K1ABC W9XYZ 6A XYZ");   // not a section
        CHECK(!ftx::pack77("K1ABC W9XYZ 519 WI", b) || ftx::unpack77(b, &hash) != "K1ABC W9XYZ 519 WI");   // RST 51x is not in the message type
        CHECK(!ftx::pack77("K1ABC W9XYZ 579 8000", b) || ftx::unpack77(b, &hash) != "K1ABC W9XYZ 579 8000");   // serials go to 7999
    }
    // ---- the Ham parser ----
    {
        using ham::ParsedMsg;
        ParsedMsg p = ham::parseMessage("K1ABC W9XYZ 6A WI");
        CHECK(p.kind == ParsedMsg::Exch && p.to == "K1ABC" && p.from == "W9XYZ" && p.exch == "6A WI" && p.contest == 1);
        p = ham::parseMessage("W9XYZ K1ABC R 2B EMA");
        CHECK(p.kind == ParsedMsg::RExch && p.exch == "2B EMA");
        p = ham::parseMessage("TU; W9XYZ K1ABC R 579 MA");
        CHECK(p.kind == ParsedMsg::RExch && p.to == "W9XYZ" && p.exch == "579 MA" && p.contest == 2);
        CHECK(ham::parseMessage("K1ABC W9XYZ R EN37").kind == ParsedMsg::Other || ham::parseMessage("K1ABC W9XYZ R EN37").kind != ParsedMsg::Exch);
        CHECK(ham::parseMessage("CQ FD K1ABC FN42").kind == ParsedMsg::Cq && ham::parseMessage("CQ FD K1ABC FN42").from == "K1ABC");
        CHECK(ham::parseMessage("TNX BOB 73 GL").kind == ParsedMsg::Other);
        CHECK(ham::msgExch("w9xyz", "k1abc", "2b  ema", true) == "W9XYZ K1ABC R 2B EMA" && ham::msgExch("K1ABC", "W9XYZ", "6A WI", false) == "K1ABC W9XYZ 6A WI");
    }
    // ---- the sequence: we call CQ FD, W9XYZ answers ----
    {
        ham::QsoConfig c; c.myCall = "K1ABC"; c.myGrid = "FN42"; c.watchdogSec = 0; c.contest = 1; c.myExch = "2B EMA"; c.cqMod = "FD";
        ham::QsoEngine e(c);
        int logged = 0; ham::QsoRecord lq;
        e.onLog = [&](const ham::QsoRecord& r) { logged++; lq = r; };
        e.startCq(0);
        CHECK(e.messageForSlot(0) == "CQ FD K1ABC FN42"); e.txSent(0);
        e.onDecode(16, "K1ABC W9XYZ 6A WI", -5, 15, 1200);
        CHECK(e.state() == ham::QsoEngine::Tx3RReport && e.dxCall() == "W9XYZ");
        CHECK(e.messageForSlot(30) == "W9XYZ K1ABC R 2B EMA"); e.txSent(30);
        e.onDecode(46, "K1ABC W9XYZ RR73", -5, 45, 1200);
        CHECK(logged == 1 && lq.call == "W9XYZ" && lq.srx == "6A WI" && lq.stx == "2B EMA" && lq.rstSent.empty() && lq.contestId == "ARRL-FD");
        CHECK(e.state() == ham::QsoEngine::Cq && e.messageForSlot(60) == "CQ FD K1ABC FN42");
        e.txSent(60);
        // they did not copy our exchange and send theirs again: we repeat ours, the retry count starts again
        e.onDecode(76, "K1ABC N0XX 1D MN", -9, 75, 900);
        CHECK(e.messageForSlot(90) == "N0XX K1ABC R 2B EMA"); e.txSent(90);
        e.onDecode(106, "K1ABC N0XX 1D MN", -9, 105, 900);
        CHECK(e.messageForSlot(120) == "N0XX K1ABC R 2B EMA");
    }
    // ---- the sequence: we answer a CQ RU (RTTY Roundup) ----
    {
        ham::QsoConfig c; c.myCall = "W9XYZ"; c.myGrid = "EN37"; c.watchdogSec = 0; c.contest = 2; c.myExch = "579 WI";
        ham::QsoEngine e(c);
        int logged = 0; ham::QsoRecord lq;
        e.onLog = [&](const ham::QsoRecord& r) { logged++; lq = r; };
        e.callStation(0, "K1ABC", "FN42", -7, 15, 1500);
        CHECK(e.messageForSlot(30) == "K1ABC W9XYZ 579 WI"); e.txSent(30);
        e.onDecode(46, "W9XYZ K1ABC R 589 MA", -7, 45, 1500);
        CHECK(logged == 1 && lq.rstRcvd == "589" && lq.srx == "MA" && lq.rstSent == "579" && lq.stx == "WI" && lq.contestId == "ARRL-RTTY");
        CHECK(e.state() == ham::QsoEngine::Tx4RR73 && e.messageForSlot(60) == "K1ABC W9XYZ RR73");
        e.txSent(60);
        CHECK(e.state() == ham::QsoEngine::Idle);
        e.onDecode(76, "TU; W9XYZ K1ABC R 589 MA", -7, 75, 1500);   // our RR73 was lost: it goes again, logged once
        CHECK(e.state() == ham::QsoEngine::Tx4RR73 && e.messageForSlot(90) == "K1ABC W9XYZ RR73" && logged == 1);
    }
    printf(fails ? "ham_ftxcontest: %d FAILED\n" : "ham_ftxcontest: all passed\n", fails);
    return fails ? 1 : 0;
}
