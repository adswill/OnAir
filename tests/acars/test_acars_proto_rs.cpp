// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Golden-vector test of the Rust ACARS block layer (rust/onair_acars): frames built by the transmitter, damaged frames (flipped bits, truncation,
// random bytes), every label, texts of the Q labels and the media advisory, and ONAIR_DIFF_ITERS (default 200000) seeded random inputs per function
// family. Every result goes into a digest (tests/common/golden.h) that is compared with the one recorded from the C++ block layer the Rust code replaced.
#include "dect2/acars/acars_proto.h"
#include "../common/golden.h"
#include "../common/ordered.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
namespace A = dect2;

static long repairedOk = 0, repairedFixed = 0, parsedOk = 0, decodedNonEmpty = 0;
static golden::Digest dig;

static std::mt19937 g(4242);
static std::string rnd(const char* alphabet, size_t n) { std::string s; for (size_t i = 0; i < n; i++) s += alphabet[g() % strlen(alphabet)]; return s; }

static void addMsg(const A::AcarsMessage& a) {
    dig.add(a.mode, a.reg, a.ack, a.label, a.labelText, a.blockId, a.downlink, a.finalBlock, a.msgNum, a.msgSeq, a.flightId);
    dig.add(a.sublabel, a.mfi, a.text, a.decoded);
}

// a block as transmitted: txt (after SOH, with parity, up to and including the suffix) and the two check bytes
struct Block { std::vector<uint8_t> txt; uint8_t crc[2]; };

static Block randomBlock() {
    A::AcarsBlockSpec s;
    s.mode = g() % 8 ? '2' : (char)(0x20 + g() % 90);
    s.reg = rnd("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.", g() % 9);
    const int a = (int)(g() % 4);
    s.ack = a == 0 ? 0x15 : a == 1 ? 0x06 : (uint8_t)('0' + g() % 10);
    static const char* labels[] = {"H1", "Q0", "QA", "Q1", "SA", "_d", "5Z", "10", "QL", "QT", "SQ", "44", "80", "B6"};
    const char* l = labels[g() % 14];
    s.label[0] = l[0]; s.label[1] = l[1];
    s.blockId = g() % 3 ? (char)('0' + g() % 10) : (char)('A' + g() % 26);
    s.hasText = g() % 8 != 0;
    switch (g() % 8) {
    case 0: s.text = "OMDB1030" "1045" "1100" "1115" "1130" "VHHH"; s.label[0] = 'Q'; s.label[1] = "1ABCDEFGHKLMNPQRST"[g() % 18]; break;
    case 1: s.text = ORD_CAT("0", "EL"[g() % 2], "VSHGC2XI"[g() % 8], rnd("0123456789", 6), rnd("VSHGC2XI", g() % 6), (g() % 2 ? "/text" : "")); s.label[0] = 'S'; s.label[1] = 'A'; break;
    case 2: s.text = "M01AEK0201" + rnd("ABC \r\n#- /xyz0123456789", g() % 60); break;
    case 3: s.text = ORD_CAT((g() % 2 ? "#M1B" : "- #M1"), (g() % 2 ? "/AT " : "/FN "), rnd("ABC \r\n", g() % 30)); s.label[0] = 'H'; s.label[1] = '1'; break;
    case 4: s.text = rnd("ABCDEFGHIJ\r\n\t\x01 0123456789", g() % 221); break;
    default: s.text = rnd("ABCDEF 0123456789\r\n/#-", g() % 100); break;
    }
    s.lastBlock = g() % 4 != 0;
    const std::vector<uint8_t> f = A::acarsBuildFrame(s, 0);
    Block b;
    b.txt.assign(f.begin() + 3, f.end() - 3);
    b.crc[0] = f[f.size() - 3]; b.crc[1] = f[f.size() - 2];
    return b;
}

static void damage(Block& b) {
    const int k = (int)(g() % 5);
    for (int i = 0; i < k; i++) {
        const size_t n = b.txt.size() + 2;
        const size_t p = g() % n;
        const uint8_t m = (uint8_t)(1u << (g() % 8));
        if (p < b.txt.size()) b.txt[p] ^= m; else b.crc[p - b.txt.size()] ^= m;
        if (g() % 6 == 0 && p < b.txt.size()) b.txt[p] ^= (uint8_t)(1u << (g() % 8));   // two bits in one byte
    }
    if (g() % 10 == 0) b.txt.resize(g() % (b.txt.size() + 1));
    if (g() % 12 == 0) ORD_CALL(b.txt.insert, b.txt.begin() + (long)(g() % (b.txt.size() + 1)), (uint8_t)g());
}

static void checkRepair(const Block& b, int maxPar) {
    std::vector<uint8_t> t1 = b.txt;
    int f1 = -7, p1 = -7;
    const bool r1 = A::acarsRepairBlock(t1.data(), (int)t1.size(), b.crc, f1, p1, maxPar);
    repairedOk += r1; repairedFixed += f1 > 0;
    dig.add(r1, f1, p1, t1);
}

static void stripParity(std::vector<uint8_t>& v) { for (auto& c : v) c &= 0x7f; }

static void checkParse(const std::vector<uint8_t>& t) {
    for (int pre = 0; pre < 2; pre++) {
        A::AcarsMessage a;
        if (pre) { a.sublabel = "ZZ"; a.mfi = "YY"; a.decoded = "old"; a.text = "old text"; a.msgNum = "OLD"; }
        const bool r1 = A::acarsParseBlock(t.data(), (int)t.size(), a);
        parsedOk += r1; decodedNonEmpty += !a.decoded.empty();
        dig.add(r1);
        addMsg(a);
    }
}

int main() {
    const char* it = getenv("ONAIR_DIFF_ITERS");
    const long iters = it ? atol(it) : 200000;

    // labels: every pair of printable characters
    for (int a = 0; a < 256; a++) for (int b = 0; b < 256; b++)
        dig.add(A::acarsLabelText((char)a, (char)b));
    // syndromes and the check sequence
    for (int e = -2; e < 300; e++) for (int bit = -1; bit < 10; bit++) dig.add(A::acarsSyndrome(e, bit));
    for (int c = 0; c < 65536; c += 7) for (int b = 0; b < 256; b += 5) dig.add(A::acarsCrcUpdate((uint16_t)c, (uint8_t)b));
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> v(g() % 300);
        for (auto& c : v) c = (uint8_t)g();
        const uint16_t start = (uint16_t)g();
        dig.add(A::acarsCrc(v.data(), v.size(), start));
    }

    // repair: transmitted blocks, damaged ones, random bytes
    for (long i = 0; i < iters; i++) {
        Block b = randomBlock();
        checkRepair(b, 3);
        Block d = b;
        damage(d);
        checkRepair(d, (int)(g() % 6));
        checkRepair(d, 3);
    }
    for (long i = 0; i < iters / 20; i++) {
        Block r;
        r.txt.resize(g() % 275);
        for (auto& c : r.txt) c = (uint8_t)g();
        r.crc[0] = (uint8_t)g(); r.crc[1] = (uint8_t)g();
        checkRepair(r, 3);
    }

    // parsing: transmitted blocks (parity removed), damaged ones, random bytes
    for (long i = 0; i < iters; i++) {
        Block b = randomBlock();
        std::vector<uint8_t> t = b.txt;
        stripParity(t);
        checkParse(t);
        Block d = b;
        damage(d);
        t = d.txt;
        stripParity(t);
        checkParse(t);
    }
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> t(g() % 60);
        for (auto& c : t) c = (uint8_t)(g() % 3 ? 0x20 + g() % 95 : g() & 0x7f);
        if (!t.empty() && g() % 2) t.back() = g() % 2 ? 0x03 : 0x17;
        if (t.size() > 13 && g() % 2) t[12] = 0x02;
        checkParse(t);
    }
    for (long i = 0; i < iters / 20; i++) {   // the longest blocks
        Block b = randomBlock();
        std::vector<uint8_t> t = b.txt;
        stripParity(t);
        ORD_CALL(t.insert, t.end() - 1, (size_t)(g() % 300), (uint8_t)('A' + g() % 26));
        checkParse(t);
    }

    // decoded text: every Q label with good and damaged texts, the media advisory, other labels
    static const char* q = "012ABCDEFGHKLMNPQRSTXZ";
    for (long i = 0; i < iters; i++) {
        std::string label, text;
        if (g() % 5 == 0) label = "SA"; else label = std::string("Q") + q[g() % 22];
        if (g() % 12 == 0) label = rnd("QSAH1", 2);
        if (g() % 12 == 0) label = rnd("QS", g() % 4);
        if (label == "SA" || g() % 8 == 0) {
            text = ORD_CAT("0", "ELX"[g() % 3], "VSHGC2XIZ"[g() % 9], rnd("0123456789", g() % 3 ? 6 : 5), rnd("VSHGC2XIZ", g() % 8), (g() % 2 ? "/" + rnd("abc", 3) : ""));
            if (g() % 3 == 0) text[g() % text.size()] = (char)(g() & 0x7f);
        } else {
            for (int k = 0; k < 6; k++) text += g() % 2 ? rnd("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 4) : ORD_CAT(rnd("012", 1), rnd("0123456789", 1), rnd("0123456", 1), rnd("0123456789", 1));
            if (g() % 3 == 0) text.resize(g() % (text.size() + 1));
            if (g() % 3 == 0 && !text.empty()) text[g() % text.size()] = (char)(g() & 0x7f);
        }
        dig.add(A::acarsDecodeText(label, text));
    }
    for (int k = 0; k < 400; k++) {   // long media advisories (many links)
        std::string text = "0LV123456" + rnd("VSHGC2XI", (size_t)k) + "/x";
        dig.add(A::acarsDecodeText("SA", text));
    }

    printf("acars_proto_rs: coverage: %ld repairs ok, %ld with flipped bits, %ld parses ok, %ld with a decoded reading\n", repairedOk, repairedFixed, parsedOk, decodedNonEmpty);
    if (!repairedFixed || !parsedOk || !decodedNonEmpty) { printf("the inputs no longer reach the interesting paths\n"); return 1; }
    return golden::check("acars_proto_rs", dig, 0xebcd9c2b4eed071cull, !it);
}
