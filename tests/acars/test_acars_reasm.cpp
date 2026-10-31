// Multi-block ACARS message reassembly (core/include/dect2/acars/acars_reasm.h): ETB blocks are held, the ETX block completes one
// message; timeouts, resends, lost blocks, interleaved aircraft and labels, a new message number cutting the old one, and the same
// through the real block parser (acarsBuildFrame > acarsParseBlock) and through VDL2 AVLC frames (vdl2AcarsInfo > vdl2ParseFrame).
#include "dect2/acars/acars_proto.h"
#include "dect2/acars/acars_reasm.h"
#include "dect2/vdl2/vdl2_proto.h"
#include <cstdio>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

using AR = AcarsReassembler<AcarsMessage, &AcarsMessage::text>;
using VR = AcarsReassembler<Vdl2Message, &Vdl2Message::acarsText>;

static AcarsPiece piece(const AcarsMessage& m) {
    AcarsPiece p;
    p.reg = m.reg; p.label = m.label; p.msgNum = m.msgNum; p.downlink = m.downlink; p.blockId = m.blockId; p.msgSeq = m.msgSeq; p.finalBlock = m.finalBlock;
    return p;
}

// a real parsed block: downlink (digit block id) with message number, sequence letter and flight id in front of the text
static AcarsMessage dl(const char* reg, const char* label, char id, const char* num, char seq, const char* body, bool last) {
    AcarsBlockSpec s;
    s.reg = reg; s.label[0] = label[0]; s.label[1] = label[1]; s.blockId = id; s.lastBlock = last;
    s.text = std::string(num) + seq + "EK0201" + body;
    auto f = acarsBuildFrame(s, 0);
    // SYN SYN SOH | mode .. suffix | crc crc | DEL
    std::vector<uint8_t> t7;
    for (size_t i = 3; i + 3 < f.size(); i++) t7.push_back(f[i] & 0x7f);
    AcarsMessage m;
    bool ok = acarsParseBlock(t7.data(), (int)t7.size(), m);
    CHECK(ok, "parse");
    return m;
}
static AcarsMessage ul(const char* reg, const char* label, char id, const char* body, bool last) {
    AcarsBlockSpec s;
    s.reg = reg; s.label[0] = label[0]; s.label[1] = label[1]; s.blockId = id; s.lastBlock = last; s.ack = 0x15; s.text = body;
    auto f = acarsBuildFrame(s, 0);
    std::vector<uint8_t> t7;
    for (size_t i = 3; i + 3 < f.size(); i++) t7.push_back(f[i] & 0x7f);
    AcarsMessage m;
    CHECK(acarsParseBlock(t7.data(), (int)t7.size(), m), "parse");
    return m;
}

int main() {
    // ---- the parser gives what the reassembler keys on
    {
        AcarsMessage a = dl("A6EDA", "H1", '1', "M01", 'A', "HELLO", false);
        CHECK(a.downlink && !a.finalBlock && a.msgNum == "M01" && a.msgSeq == 'A' && a.flightId == "EK0201" && a.text == "HELLO", "parsed ETB: num '%s' seq %c flight '%s' text '%s' final %d", a.msgNum.c_str(), a.msgSeq ? a.msgSeq : '-', a.flightId.c_str(), a.text.c_str(), a.finalBlock);
    }
    // ---- a three block downlink
    {
        AR r; AR::Done w; std::vector<AR::Done> cut;
        auto b1 = dl("A6EDA", "H1", '1', "M07", 'A', "WX OMDB 1200Z ", false);
        auto b2 = dl("A6EDA", "H1", '2', "M07", 'B', "WIND 270/12 ", false);
        auto b3 = dl("A6EDA", "H1", '3', "M07", 'C', "QNH 1008", true);
        CHECK(r.feed(piece(b1), AcarsMessage(b1), 10, w, cut) == AR::Result::Held, "first held");
        CHECK(r.feed(piece(b2), AcarsMessage(b2), 12, w, cut) == AR::Result::Held, "second held");
        CHECK(r.pending() == 1, "one pending");
        CHECK(r.feed(piece(b3), AcarsMessage(b3), 14, w, cut) == AR::Result::Complete, "third completes");
        CHECK(w.msg.text == "WX OMDB 1200Z WIND 270/12 QNH 1008" && w.blocks == 3 && !w.incomplete && cut.empty() && r.pending() == 0, "joined '%s' blocks %d incomplete %d", w.msg.text.c_str(), w.blocks, w.incomplete);
    }
    // ---- a single block message is left alone
    {
        AR r; AR::Done w; std::vector<AR::Done> cut;
        auto b = dl("A6EDA", "H1", '1', "M08", 'A', "SHORT", true);
        CHECK(r.feed(piece(b), AcarsMessage(b), 1, w, cut) == AR::Result::Single, "single");
    }
    // ---- a resend of an ETB block does not double the text; an out of order or skipped block flags the result
    {
        AR r; AR::Done w; std::vector<AR::Done> cut;
        auto b1 = dl("N123", "H1", '1', "M02", 'A', "AAA", false);
        auto b3 = dl("N123", "H1", '3', "M02", 'C', "CCC", true);
        r.feed(piece(b1), AcarsMessage(b1), 1, w, cut);
        CHECK(r.feed(piece(b1), AcarsMessage(b1), 3, w, cut) == AR::Result::Held, "resend held");
        CHECK(r.feed(piece(b3), AcarsMessage(b3), 5, w, cut) == AR::Result::Complete, "completes over the gap");
        CHECK(w.msg.text == "AAACCC" && w.blocks == 2 && w.incomplete, "resend not doubled, gap flagged: '%s' %d %d", w.msg.text.c_str(), w.blocks, w.incomplete);
    }
    // ---- timeout: the partial message comes out flagged, once
    {
        AR r; AR::Done w; std::vector<AR::Done> cut;
        auto b1 = dl("N123", "H1", '1', "M03", 'A', "PART ONE", false);
        r.feed(piece(b1), AcarsMessage(b1), 100, w, cut);
        r.flush(100 + kAcarsReasmTimeoutSec - 1, cut);
        CHECK(cut.empty() && r.pending() == 1, "not yet");
        r.flush(100 + kAcarsReasmTimeoutSec + 1, cut);
        CHECK(cut.size() == 1 && cut[0].incomplete && cut[0].blocks == 1 && cut[0].msg.text == "PART ONE" && r.pending() == 0, "timed out");
        cut.clear(); r.flush(1000, cut);
        CHECK(cut.empty(), "only once");
        // a clock that went back (the count restarted) also gives the partial out
        r.feed(piece(b1), AcarsMessage(b1), 500, w, cut);
        r.flush(2, cut);
        CHECK(cut.size() == 1, "clock restart");
    }
    // ---- two aircraft and two labels interleave without mixing
    {
        AR r; AR::Done w; std::vector<AR::Done> cut;
        auto a1 = dl("A6EDA", "H1", '1', "M01", 'A', "AIR-A1 ", false);
        auto b1 = dl("N777", "H1", '1', "M01", 'A', "AIR-B1 ", false);
        auto c1 = dl("A6EDA", "5Z", '2', "M02", 'A', "LBL-C1 ", false);
        auto a2 = dl("A6EDA", "H1", '2', "M01", 'B', "AIR-A2", true);
        auto b2 = dl("N777", "H1", '2', "M01", 'B', "AIR-B2", true);
        auto c2 = dl("A6EDA", "5Z", '3', "M02", 'B', "LBL-C2", true);
        r.feed(piece(a1), AcarsMessage(a1), 1, w, cut); r.feed(piece(b1), AcarsMessage(b1), 2, w, cut); r.feed(piece(c1), AcarsMessage(c1), 3, w, cut);
        CHECK(r.pending() == 3, "three pending");
        CHECK(r.feed(piece(b2), AcarsMessage(b2), 4, w, cut) == AR::Result::Complete && w.msg.text == "AIR-B1 AIR-B2", "B: '%s'", w.msg.text.c_str());
        CHECK(r.feed(piece(c2), AcarsMessage(c2), 5, w, cut) == AR::Result::Complete && w.msg.text == "LBL-C1 LBL-C2", "C: '%s'", w.msg.text.c_str());
        CHECK(r.feed(piece(a2), AcarsMessage(a2), 6, w, cut) == AR::Result::Complete && w.msg.text == "AIR-A1 AIR-A2", "A: '%s'", w.msg.text.c_str());
    }
    // ---- a new message number cuts the old one off
    {
        AR r; AR::Done w; std::vector<AR::Done> cut;
        auto o1 = dl("N5", "H1", '1', "M10", 'A', "OLD ", false);
        auto n1 = dl("N5", "H1", '2', "M11", 'A', "NEW ", false);
        auto n2 = dl("N5", "H1", '3', "M11", 'B', "END", true);
        r.feed(piece(o1), AcarsMessage(o1), 1, w, cut);
        r.feed(piece(n1), AcarsMessage(n1), 2, w, cut);
        CHECK(cut.size() == 1 && cut[0].incomplete && cut[0].msg.text == "OLD ", "old cut off");
        CHECK(r.feed(piece(n2), AcarsMessage(n2), 3, w, cut) == AR::Result::Complete && w.msg.text == "NEW END", "new joined '%s'", w.msg.text.c_str());
    }
    // ---- uplink: letters as block ids, no sequence letter; a resend (same block id) is not doubled
    {
        AR r; AR::Done w; std::vector<AR::Done> cut;
        auto u1 = ul("A6EDA", "A9", 'A', "FPL PART ONE ", false);
        auto u2 = ul("A6EDA", "A9", 'B', "PART TWO", true);
        CHECK(!u1.downlink, "uplink");
        r.feed(piece(u1), AcarsMessage(u1), 1, w, cut);
        CHECK(r.feed(piece(u1), AcarsMessage(u1), 2, w, cut) == AR::Result::Held, "resend held");
        CHECK(r.feed(piece(u2), AcarsMessage(u2), 3, w, cut) == AR::Result::Complete && w.msg.text == "FPL PART ONE PART TWO" && w.blocks == 2 && !w.incomplete, "uplink joined '%s'", w.msg.text.c_str());
    }
    // ---- bounds: many aircraft with open messages, and a very long message
    {
        AR r; AR::Done w; std::vector<AR::Done> cut;
        for (int i = 0; i < 100; i++) {
            char reg[16]; snprintf(reg, sizeof reg, "N%03d", i);
            auto b = dl(reg, "H1", '1', "M01", 'A', "X", false);
            r.feed(piece(b), AcarsMessage(b), i * 0.1, w, cut);
        }
        CHECK(r.pending() <= kAcarsReasmMaxPending && cut.size() >= 100 - kAcarsReasmMaxPending, "pending bounded: %zu, cut %zu", r.pending(), cut.size());
        AR r2; cut.clear();
        std::string big(200, 'Z');
        for (int i = 0; i < 60; i++) {
            auto b = dl("N1", "H1", (char)('0' + i % 10), "M01", (char)('A' + i % 26), big.c_str(), false);
            r2.feed(piece(b), AcarsMessage(b), i * 0.5, w, cut);
        }
        auto fin = dl("N1", "H1", '1', "M01", 'Z', "END", true);
        auto res = r2.feed(piece(fin), AcarsMessage(fin), 40, w, cut);
        // the cap is on the added text; the first block and the end come on top
        CHECK(res == AR::Result::Complete && w.msg.text.size() <= kAcarsReasmMaxText + 512, "long message bounded: %zu", w.msg.text.size());
    }
    // ---- VDL2: real AVLC frames, ETB then ETX, joined and re-read
    {
        VR r; VR::Done w; std::vector<VR::Done> cut;
        Vdl2Addr dst{0x123456, 4}, src{0xABCDEF, 1};
        auto mk = [&](char id, const std::string& body, bool last, double t) {
            auto info = vdl2AcarsInfo("A6-EDA", "H1", id, body, 0x15, '2', last);
            auto f = vdl2Frame(dst, src, vdl2ControlI(0, 0, false), info);
            Vdl2Message m;
            CHECK(vdl2ParseFrame(f.data(), f.size() - 2, m), "parse frame");
            m.timeSec = t;
            return m;
        };
        auto pv = [&](const Vdl2Message& m) {
            AcarsPiece p; p.reg = m.reg; p.label = m.label; p.msgNum = m.msgNum; p.downlink = m.fromAir; p.blockId = m.blockId; p.msgSeq = m.acarsSeq; p.finalBlock = m.acarsFinal;
            return p;
        };
        Vdl2Message v1 = mk('1', "M05AEK0201WEATHER ", false, 1), v2 = mk('2', "M05BEK0201REPORT TEXT", true, 3);
        CHECK(v1.acars && v1.acarsCrcOk && !v1.acarsFinal && v1.acarsSeq == 'A' && v1.fromAir, "vdl2 ETB parsed: acars %d crc %d final %d seq %c air %d", v1.acars, v1.acarsCrcOk, v1.acarsFinal, v1.acarsSeq ? v1.acarsSeq : '-', v1.fromAir);
        CHECK(r.feed(pv(v1), Vdl2Message(v1), 1, w, cut) == VR::Result::Held, "vdl2 held");
        CHECK(r.feed(pv(v2), Vdl2Message(v2), 3, w, cut) == VR::Result::Complete, "vdl2 complete");
        CHECK(w.msg.acarsText == "WEATHER REPORT TEXT" && w.blocks == 2, "vdl2 joined '%s'", w.msg.acarsText.c_str());
    }
    return testSummary("acars reassembly");
}
