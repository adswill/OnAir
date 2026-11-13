// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA SDS over the MAC: D-STATUS, the simple text and LIP payloads through parseTlSdu (MLE + CMCE) and the reassembly of fragments (FragReassembler).
#include "dect2/tetra/tetra_pdu.h"
#include "dect2/tetra/tetra_sds.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2::tetra;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void put(Bits& b, uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((v >> i) & 1); }
// the LLC BL-UDATA header and the CMCE discriminator around a CMCE PDU
static Bits tlSdu(const Bits& cmce) { Bits b; put(b, 2, 4); put(b, 2, 3); b.insert(b.end(), cmce.begin(), cmce.end()); return b; }

int main() {
    // D-STATUS
    Bits st; put(st, 8, 5); put(st, 1, 2); put(st, 4321, 24); put(st, 32769, 16);
    SdsMsg m;
    CHECK(parseStatus(st, m) && m.callingIssi == 4321 && m.kind == "status" && m.text.find("32769") != std::string::npos);
    Bits cut(st.begin(), st.begin() + 30);
    CHECK(!parseStatus(cut, m));
    Bits wrong = st; wrong[4] ^= 1;
    CHECK(!parseStatus(wrong, m));
    // simple text (table 29.21 PID 2) and immediate text (PID 9)
    CHECK(parseSdsData(buildSdsText(4321, "KEEP CLEAR"), m) && m.kind == "text" && m.text == "KEEP CLEAR" && m.callingIssi == 4321);
    Bits im = buildSdsText(4321, "ABC");
    size_t pidAt = 5 + 2 + 24 + 2 + 11;
    for (int i = 0; i < 8; i++) im[pidAt + (size_t)i] = (9 >> (7 - i)) & 1;
    CHECK(parseSdsData(im, m) && m.kind == "text" && m.text == "ABC");
    // OTAK (PID 1) is not text
    Bits ot = buildSdsText(4321, "ABC");
    for (int i = 0; i < 8; i++) ot[pidAt + (size_t)i] = (1 >> (7 - i)) & 1;
    CHECK(parseSdsData(ot, m) && m.kind != "text");

    // through the MLE, whole
    Bits text = buildSdsText(4321, "HELLO FROM TETRA");
    MacBlock b;
    parseTlSdu(tlSdu(text), 0x1234, -1, -1, b);
    CHECK(b.cmce.size() == 1 && b.cmce[0].addr == 0x1234 && b.cmce[0].bits.size() >= text.size());
    CHECK(parseSdsData(b.cmce[0].bits, m) && m.text == "HELLO FROM TETRA");

    // the same, in fragments: start, two MAC-FRAG, a MAC-END
    FragReassembler fr;
    Bits sdu = tlSdu(buildSdsText(4321, "A LONGER MESSAGE THAT NEEDS SEVERAL FRAGMENTS"));
    size_t third = sdu.size() / 4;
    Bits out; int addr, sl, car;
    MacBlock s; s.fragKind = 1; s.fragAddr = 77; s.fragSlots = 3; s.fragCarrier = 9;
    s.fragBits.assign(sdu.begin(), sdu.begin() + (long)third);
    CHECK(!fr.feed(s, out, addr, sl, car));
    for (size_t k = 1; k < 3; k++) {
        MacBlock f; f.fragKind = 2; f.fragBits.assign(sdu.begin() + (long)(k * third), sdu.begin() + (long)((k + 1) * third));
        CHECK(!fr.feed(f, out, addr, sl, car));
    }
    MacBlock e; e.fragKind = 3; e.fragBits.assign(sdu.begin() + (long)(3 * third), sdu.end());
    CHECK(fr.feed(e, out, addr, sl, car));
    CHECK(out == sdu && addr == 77 && sl == 3 && car == 9);
    MacBlock whole;
    parseTlSdu(out, addr, sl, car, whole);
    CHECK(whole.cmce.size() == 1 && parseSdsData(whole.cmce[0].bits, m) && m.text == "A LONGER MESSAGE THAT NEEDS SEVERAL FRAGMENTS");

    // a continuation without its start, a MAC-END alone, too many pieces: nothing
    FragReassembler fr2;
    CHECK(!fr2.feed(e, out, addr, sl, car));
    MacBlock st2; st2.fragKind = 1; st2.fragBits = Bits(10);
    CHECK(!fr2.feed(st2, out, addr, sl, car));
    MacBlock f2; f2.fragKind = 2; f2.fragBits = Bits(100);
    bool got = false;
    for (int k = 0; k < 50; k++) got = fr2.feed(f2, out, addr, sl, car) || got;
    CHECK(!got);
    printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
