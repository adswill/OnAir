// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Golden-vector test of the Rust POCSAG / FLEX layer (rust/onair_pager, dect2::pager): code words (valid, damaged, random), text readers, the "looks like text"
// heuristic, POCSAG transmissions built by the generator and then damaged, the FLEX speed table, frame information words and FLEX phases (built by the generator,
// built word by word with random vector words, damaged, random), ONAIR_DIFF_ITERS (default 200000) per family. Every result goes into a digest
// (tests/common/golden.h) compared with the one recorded from the C++ layer the Rust code replaced.
#include "dect2/pager/pager_proto.h"
#include "../common/golden.h"
#include "../common/ordered.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
namespace A = dect2::pager;
static golden::Digest dig;

static long pocMsgs = 0, flexMsgs = 0, flexAlpha = 0, flexNum = 0, flexLong = 0, looksTrue = 0;

static std::mt19937 g(2024);
static std::string rnd(const char* alphabet, size_t n) { std::string s; for (size_t i = 0; i < n; i++) s += alphabet[g() % strlen(alphabet)]; return s; }

static void addPagerMsg(const dect2::PagerMessage& a) {
    dig.add(a.flex, a.address, a.function, a.type, a.text, a.fixed);
    dig.add(a.damaged, a.numText, a.alphaText, a.looksAlpha, a.readHow);
}
static void addMsgs(const std::vector<dect2::PagerMessage>& a) {
    dig.add((uint64_t)a.size());
    for (const auto& m : a) addPagerMsg(m);
}

static std::vector<uint8_t> randBits(size_t n, int mode) {
    std::vector<uint8_t> b(n);
    if (mode == 0) { for (auto& x : b) x = (uint8_t)(g() & 1); return b; }
    // text-like: 7-bit characters from a text alphabet, sometimes with noise
    static const char txt[] = "Hello World 0123456789 ALERT: server DOWN, call 555-1234 now! xY~[]";
    for (size_t i = 0; i + 7 <= n; i += 7) {
        int c = (g() % 20 == 0) ? (int)(g() & 0x7F) : (unsigned char)txt[g() % (sizeof txt - 1)];
        for (int k = 0; k < 7; k++) b[i + (size_t)k] = (uint8_t)((c >> k) & 1);
    }
    return b;
}

static void addWord(const A::PocsagWord& a) { dig.add(a.kind, a.a, a.b, a.bits, a.fixed); }

static void testWords(long iters) {
    for (int d = 0; d < 0x200000; d += 17) dig.add(A::encodeWord((uint32_t)d));
    for (long i = 0; i < iters; i++) {
        uint32_t w = (uint32_t)g();
        const int how = (int)(g() % 5);
        if (how) {
            w = A::encodeWord((uint32_t)g() & 0x1FFFFF);
            for (int k = (int)(g() % 5); k > 0; k--) w ^= 1u << (g() % 32);
            if (how == 4) w = (w & 0xFFFFFF00u) | (g() & 0xFF);
        }
        const A::Fixed a = A::correctWord(w);
        dig.add(a.ok, a.bits, a.data);
        dig.add(A::rev21(w));
        addWord(A::pocsagClassify(w));
    }
    // the sync and idle words with every one and two bit damage
    for (uint32_t base : {A::kPocsagIdle, A::kPocsagSync})
        for (int i = 0; i < 32; i++) for (int j = i; j < 32; j++) {
            const uint32_t w = base ^ (1u << i) ^ (j == i ? 0 : (1u << j));
            addWord(A::pocsagClassify(w));
        }
}

static void testText(long iters) {
    for (long i = 0; i < iters; i++) {
        const size_t n = g() % 400;
        const std::vector<uint8_t> b = randBits(n, (int)(g() % 2));
        std::vector<uint8_t> c = b;
        const std::vector<uint8_t>& cc = c;
        for (int k = (int)(g() % 3); k > 0 && !c.empty(); k--) c[g() % c.size()] ^= 1;
        for (const std::vector<uint8_t>* v : {&b, &cc}) {
            const bool la = A::pocsagLooksAlpha(*v);
            dig.add(la);
            looksTrue += la;
            dig.add(A::alphaFromBits(*v));
            for (bool fx : {false, true}) dig.add(A::numericFromBits(*v, fx));
        }
    }
}

static std::vector<A::PocsagPage> randPages(int n) {
    std::vector<A::PocsagPage> p;
    for (int i = 0; i < n; i++) {
        A::PocsagPage pg;
        pg.ric = (uint32_t)(g() % 2097152);
        pg.fn = (int)(g() % 4);
        const int t = (int)(g() % 3);
        pg.type = t == 0 ? dect2::kPagerNumeric : t == 1 ? dect2::kPagerAlpha : dect2::kPagerTone;
        pg.text = t == 0 ? rnd("0123456789 -U*[]", g() % 40) : rnd("Hello World 0123456789 ALERT: call-me! xY", g() % 120);
        p.push_back(pg);
    }
    return p;
}

static void feedPocsag(const std::vector<uint8_t>& bits, int readMode, int damaged) {
    // skip the preamble (alternating bits) and walk the batches: sync word then 16 code words
    size_t pos = 0;
    while (pos + 32 <= bits.size()) {
        uint32_t w = 0;
        for (int i = 0; i < 32; i++) w = (w << 1) | bits[pos + (size_t)i];
        if (w == A::kPocsagSync) break;
        pos++;
    }
    A::PocsagParser pa;
    pa.readMode = readMode;
    std::vector<dect2::PagerMessage> oa;
    while (pos + 32 * 17 <= bits.size()) {
        pos += 32;
        for (int slot = 0; slot < 16; slot++, pos += 32) {
            uint32_t w = A::wordFromBits(&bits[pos]);
            if (damaged && g() % 4 == 0) for (int k = (int)(g() % 4); k > 0; k--) w ^= 1u << (g() % 32);
            if (damaged && g() % 40 == 0) w = (uint32_t)g();
            pa.word(w, slot, oa);
        }
    }
    pa.flush(oa);
    addMsgs(oa);
    dig.add(pa.ok, pa.fixed, pa.failed);
    pocMsgs += (long)oa.size();
}

static void testPocsag(long iters) {
    for (long i = 0; i < iters / 20; i++) {
        const std::vector<A::PocsagPage> pages = randPages(1 + (int)(g() % 6));
        const std::vector<uint8_t> bits = A::pocsagBits(pages, 64);
        dig.add(bits);
        ORD_CALL(feedPocsag, bits, (int)(g() % 4), (int)(g() % 2));
    }
}

// a phase built word by word: f[] are the 21-bit field values (first transmitted bit in bit 0)
static std::vector<uint8_t> phaseFromWords(const uint32_t* f) {
    std::vector<uint8_t> bits((size_t)A::kFlexPhaseWords * 32);
    for (int blk = 0; blk < 11; blk++)
        for (int w = 0; w < 8; w++) {
            const uint32_t W = A::encodeWord(A::rev21(f[blk * 8 + w] & 0x1FFFFF));
            for (int j = 0; j < 32; j++) bits[(size_t)(blk * 256 + j * 8 + w)] = (uint8_t)((W >> (31 - j)) & 1);
        }
    return bits;
}

static void checkPhase(const std::vector<uint8_t>& bits) {
    std::vector<dect2::PagerMessage> oa;
    A::FlexPhaseStat sa;
    A::flexDecodePhase(bits.data(), oa, sa);
    addMsgs(oa);
    dig.add(sa.ok, sa.fixed, sa.failed, sa.biwOk);
    flexMsgs += (long)oa.size();
    for (const auto& m : oa) { flexAlpha += m.type == dect2::kPagerAlpha; flexNum += m.type == dect2::kPagerNumeric; }
}

static void damage(std::vector<uint8_t>& bits, int how) {
    if (how == 1) for (int k = (int)(g() % 12); k > 0; k--) bits[g() % bits.size()] ^= 1;
    else if (how == 2) { const size_t s = g() % bits.size(); for (size_t i = s; i < bits.size() && i < s + 40 + g() % 200; i++) bits[i] = (uint8_t)(g() & 1); }
}

static void addMode(const A::FlexMode* a) {
    dig.add(a != nullptr);
    if (a) dig.add(a->code, a->baud, a->levels, a->speed);
}

static void testFlex(long iters) {
    for (int code = 0; code < 65536; code++)
        for (int md = 0; md <= 5; md++) addMode(A::flexModeForCode((uint16_t)code, md));
    for (int sp = -2; sp < 10; sp++) addMode(A::flexModeForSpeed(sp));
    for (int cy = -3; cy < 20; cy++) for (int fr = -3; fr < 135; fr++) dig.add(A::flexFiwData(cy, fr));
    for (uint32_t f = 0; f < 0x200000; f += 3) {
        int c1 = -1, f1 = -1;
        const bool a = A::flexFiwCheck(f, c1, f1);
        dig.add(a); if (a) dig.add(c1, f1);
    }
    for (long i = 0; i < iters / 4; i++) {
        // phases from the generator
        std::vector<A::FlexPage> pg;
        const int n = (int)(g() % 14);
        for (int k = 0; k < n; k++) {
            A::FlexPage p;
            p.cap = 1 + g() % 1933000;
            const int t = (int)(g() % 3);
            p.type = t == 0 ? dect2::kPagerTone : t == 1 ? dect2::kPagerNumeric : dect2::kPagerAlpha;
            p.text = t == 1 ? rnd("0123456789 -U[]", g() % 40) : rnd("Hello World 0123456789 ALERT: call-me!", g() % 100);
            pg.push_back(p);
        }
        std::vector<uint8_t> bits = A::flexPhaseBits(pg);
        dig.add(bits);
        damage(bits, (int)(g() % 3));
        checkPhase(bits);
    }
    static const uint32_t types[8] = {2, 3, 5, 5, 7, 4, 0, 6};
    for (long i = 0; i < iters / 2; i++) {
        // phases built word by word: the BIW, random address words (short and long) and random vector / message words
        uint32_t f[88];
        for (auto& x : f) x = (uint32_t)g() & 0x1FFFFF;
        const int aoff = 1 + (int)(g() % 4), voff = aoff + 1 + (int)(g() % 12);
        f[0] = ((uint32_t)voff << 10) | ((uint32_t)(aoff - 1) << 8) | (g() & 0xFF);
        for (int k = aoff; k < voff && k < 88; k++) {
            const int t = (int)(g() % 6);
            if (t < 4) f[k] = 0x8000 + 1 + g() % 0x100000;           // short address
            else if (t == 4) f[k] = (g() % 2) ? 1 + g() % 0x8000 : 0x1E0001 + g() % 0x1FFF;   // long address (first word)
            if (g() % 12 == 0) f[k] = (g() % 2) ? 0 : 0x1FFFFF;
        }
        for (int k = voff; k < 88; k++) {
            if (g() % 3) {
                const uint32_t type = (g() % 5 == 0) ? (g() % 8) : types[g() % 8];
                f[k] = (f[k] & ~0x70u) | (type << 4);
                f[k] = (f[k] & ~(0x7Fu << 7)) | ((uint32_t)(g() % 3 ? 1 + g() % 80 : g() % 100) << 7);
            }
        }
        std::vector<uint8_t> bits = phaseFromWords(f);
        damage(bits, (int)(g() % 3));
        checkPhase(bits);
        for (int k = aoff; k < voff && k < 88; k++) flexLong += (f[k] < 0x8001 || f[k] > 0x1E0000);
    }
    for (long i = 0; i < iters / 20; i++) {
        std::vector<uint8_t> bits(2816);
        for (auto& x : bits) x = (uint8_t)(g() & 1);
        checkPhase(bits);
    }
}

int main() {
    const char* it = getenv("ONAIR_DIFF_ITERS");
    const long iters = it ? atol(it) : 200000;
    testWords(iters);
    testText(iters);
    testPocsag(iters);
    testFlex(iters);
    printf("test_pager_proto_rs: pocsag messages %ld, flex messages %ld, alpha %ld, numeric %ld, long addr %ld, looks-alpha %ld\n", pocMsgs, flexMsgs, flexAlpha, flexNum, flexLong, looksTrue);
    // recorded again for 3.0: FLEX BIW / vector-word check nibble, long-address header in the second vector word, data polarity (flexnext-030)
    return golden::check("pager_proto_rs", dig, 0x3c47c499bb95d830ull, !it);
}
