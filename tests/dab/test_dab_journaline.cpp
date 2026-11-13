// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Journaline (ETSI TS 102 979): data group, JML object, TOC and DAB packet mode parsing.
// Checked against the specification text: the CRC check value, the header bit layout, the object type / flag / revision fields, the JML codes,
// escape sequence lengths, general link targets, a deflate stream made by zlib (RFC 1951), the TOC layout, user application data.
// LOOPBACK ONLY (the generator below is written by the same author from the same text): the packet mode round trip, multi-packet groups,
// lost packets, repetition. Robustness: truncated and random input must never crash.
#include "dect2/dab/dab_journaline.h"
#include "dect2/dab/dab.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
typedef std::vector<uint8_t> Bytes;

#include "../common/test_util.h"

// ---- generator (independent bitwise CRC)
static uint16_t crcRef(const uint8_t* d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) { c ^= (uint16_t)(d[i] << 8); for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1); }
    return (uint16_t)~c;
}
static void put(Bytes& v, const std::string& s) { v.insert(v.end(), s.begin(), s.end()); }
static Bytes object(uint16_t id, int type, bool st, int rev, const Bytes& content, bool comp = false) {
    Bytes o = {(uint8_t)(id >> 8), (uint8_t)id, (uint8_t)((type << 5) | (st ? 16 : 0) | (comp ? 8 : 0) | rev)};
    if (comp) o.push_back(0x08);
    o.insert(o.end(), content.begin(), content.end());
    return o;
}
static Bytes group(int type, const Bytes& field) {
    Bytes g = {(uint8_t)(0x40 | type), 0x00};
    g.insert(g.end(), field.begin(), field.end());
    const uint16_t c = crcRef(g.data(), g.size());
    g.push_back((uint8_t)(c >> 8)); g.push_back((uint8_t)c);
    return g;
}
static Bytes menu(uint16_t id, const std::string& title, const std::vector<std::pair<int, std::string>>& l) {
    Bytes c = {0x01}; put(c, title);
    for (auto& e : l) { c.push_back(0x02); c.push_back((uint8_t)(e.first >> 8)); c.push_back((uint8_t)e.first); put(c, e.second); }
    c.push_back(0x00);
    return object(id, 1, true, 0, c);
}
static Bytes textObj(uint16_t id, const std::string& title, const std::string& body) {
    Bytes c = {0x01}; put(c, title); c.push_back(0x03); put(c, body); c.push_back(0x00);
    return object(id, 2, false, 3, c);
}
// packets of one address: size 24/48/72/96 (data 19/43/67/91)
static Bytes packets(const Bytes& g, int addr, int& ci, int sizeCode, bool pad = true) {
    Bytes out;
    const size_t cap = 24 * (sizeCode + 1) - 5;
    for (size_t off = 0; off < g.size() || (g.empty() && off == 0); off += cap) {
        const size_t n = std::min(cap, g.size() - off);
        const bool first = off == 0, last = off + n >= g.size();
        Bytes p(cap + 5, 0);
        p[0] = (uint8_t)((sizeCode << 6) | ((ci & 3) << 4) | (first ? 8 : 0) | (last ? 4 : 0) | (addr >> 8));
        p[1] = (uint8_t)addr; p[2] = (uint8_t)n;
        memcpy(&p[3], &g[off], n);
        const uint16_t c = crcRef(p.data(), p.size() - 2);
        p[p.size() - 2] = (uint8_t)(c >> 8); p[p.size() - 1] = (uint8_t)c;
        out.insert(out.end(), p.begin(), p.end());
        ci++;
        if (g.empty()) break;
    }
    (void)pad;
    return out;
}

int main() {
    // ---- CRC: the check value of CRC-16 with all ones preset and the result inverted ("123456789" -> 0xD64E, published for CRC-16/GENIBUS)
    const uint8_t chk[] = {'1','2','3','4','5','6','7','8','9'};
    CHECK(dab::crc16(chk, 9) == 0xD64E, "crc check value %04X", dab::crc16(chk, 9));
    CHECK(crcRef(chk, 9) == 0xD64E, "reference crc");

    // ---- object header fields (clause 5.2): id 0x1234, type 3 title only, static, revision 5
    {
        Bytes c = {0x01, 'H', 'i', 0x00};
        Bytes o = object(0x1234, 3, true, 5, c);
        CHECK(o[2] == ((3 << 5) | 0x10 | 5), "desc byte");
        JlObject j;
        CHECK(jlParseObject(o.data(), o.size(), 0, j), "title only parses");
        CHECK(j.id == 0x1234 && j.type == 3 && j.isStatic && !j.compressed && j.revision == 5 && j.title == "Hi", "header fields");
        // an extended header of 2 bytes is skipped
        Bytes e = {o[0], o[1], o[2], 0xAA, 0xBB, 0x01, 'H', 'i', 0x00};
        CHECK(jlParseObject(e.data(), e.size(), 2, j) && j.title == "Hi", "extended header skipped");
        // reserved object types are ignored
        Bytes r = object(1, 5, false, 0, c);
        CHECK(!jlParseObject(r.data(), r.size(), 0, j), "reserved type ignored");
        Bytes z = object(1, 0, false, 0, c);
        CHECK(!jlParseObject(z.data(), z.size(), 0, j), "type 0 ignored");
    }
    // ---- menu: the coding example of figure 1: title, link items with 2 byte targets (a target may contain 0x00 / 0x02 bytes)
    {
        Bytes m = menu(0, "Sport", {{0xAABB, "Bundesliga"}, {0x0002, "Soccer"}, {0x0100, "Other \xC3\xA4"}});
        JlObject j;
        CHECK(jlParseObject(m.data(), m.size(), 0, j) && j.type == 1 && j.title == "Sport" && j.links.size() == 3, "menu");
        CHECK(j.links[0].target == 0xAABB && j.links[0].text == "Bundesliga", "link 0");
        CHECK(j.links[1].target == 0x0002 && j.links[1].text == "Soccer", "link with 0x02 in the target");
        CHECK(j.links[2].target == 0x0100 && j.links[2].text == "Other \xC3\xA4", "link utf-8");
    }
    // ---- plain text with escape sequences: line break, word break, highlight, intro end, data section ignored ("This is a [ANY]great[DATA] test!")
    {
        Bytes c = {0x01}; put(c, "T"); c.push_back(0x03);
        put(c, "This is a "); c.push_back(0x1A); c.push_back(4); put(c, "[ANY]");
        put(c, "great"); c.push_back(0x1A); c.push_back(5); put(c, "[DATA]"); put(c, " test!");
        c.push_back(0x10); put(c, "next"); c.push_back(0x11); put(c, "word"); c.push_back(0x12); put(c, "hl"); c.push_back(0x13);
        c.push_back(0x14); put(c, "detail"); c.push_back(0x1C); c.push_back(0x77); put(c, "!");
        c.push_back(0x00);
        Bytes o = object(7, 2, false, 0, c);
        JlObject j;
        CHECK(jlParseObject(o.data(), o.size(), 0, j), "text parses");
        CHECK(j.body == "This is a great test!\nnextwordhl\n\ndetail!", "body '%s'", j.body.c_str());
    }
    // ---- list with columns (JML code 0x05); a block ends at the next JML code
    {
        Bytes c = {0x01}; put(c, "Table"); c.push_back(0x04); put(c, "TSV"); c.push_back(0x05); put(c, "3:0"); c.push_back(0x04); put(c, "BVB"); c.push_back(0x05); c.push_back(0x05); put(c, "4:1");
        Bytes o = object(9, 4, false, 0, c);   // no end code: the content section ends the block
        JlObject j;
        CHECK(jlParseObject(o.data(), o.size(), 0, j) && j.items.size() == 2 && j.items[0] == "TSV\t3:0" && j.items[1] == "BVB\t\t4:1" && j.title == "Table", "list");
    }
    // ---- unknown JML codes are ignored, the block's text stays
    {
        Bytes c = {0x01}; put(c, "A"); c.push_back(0x0B); put(c, "B"); c.push_back(0x00);
        Bytes o = object(3, 3, false, 0, c);
        JlObject j;
        CHECK(jlParseObject(o.data(), o.size(), 0, j) && j.title == "AB", "unknown JML code");
    }
    // ---- deflate (compress flag, method 0x08): a real zlib raw stream (level 9) of "\x01Daily news\x03" + 6 x "Germany won the match 2:1 after extra time. " + 0x00
    {
        static const uint8_t z[] = {0x63,0x74,0x49,0xcc,0xcc,0xa9,0x54,0xc8,0x4b,0x2d,0x2f,0x66,0x76,0x4f,0x2d,0xca,0x4d,0xcc,0xab,0x54,0x28,0xcf,0xcf,0x53,0x28,0xc9,0x48,0x55,0xc8,0x4d,0x2c,0x49,0xce,0x50,0x30,0xb2,0x32,0x54,0x48,0x4c,0x2b,0x49,0x2d,0x52,0x48,0xad,0x28,0x29,0x4a,0x54,0x28,0xc9,0xcc,0x4d,0xd5,0x53,0x18,0xce,0x6a,0x19,0x00};
        Bytes o = object(0x0010, 2, true, 1, Bytes(z, z + sizeof z), true);
        JlObject j;
        CHECK(jlParseObject(o.data(), o.size(), 0, j), "compressed parses");
        std::string want; for (int i = 0; i < 6; i++) want += "Germany won the match 2:1 after extra time. ";
        CHECK(j.compressed && j.title == "Daily news" && j.body == want, "inflated text");
        // an unknown compression method and a corrupt stream are rejected
        Bytes bad = o; bad[3] = 0x09;
        CHECK(!jlParseObject(bad.data(), bad.size(), 0, j), "unknown method");
        Bytes cor = o; for (size_t i = 4; i < cor.size(); i++) cor[i] ^= 0xFF;
        CHECK(!jlParseObject(cor.data(), cor.size(), 0, j) || j.title != "Daily news", "corrupt deflate");
    }
    // ---- general link target (5.3.2.3): object link with a label and a URL link
    {
        Bytes c = {0x01}; put(c, "L"); c.push_back(0x03); put(c, "x");
        c.push_back(0x1A); c.push_back(8 - 1 + 0);   // DS: type 0x03, link type 0, id 0x00 0x42, end marker 0, label "ab"  = 1+1+2+1+2 = 7 bytes
        // (length byte = 7 - 1 = 6)
        c.back() = 6;
        c.push_back(0x03); c.push_back(0x00); c.push_back(0x00); c.push_back(0x42); c.push_back(0x00); put(c, "ab");
        c.push_back(0x1A); c.push_back((uint8_t)(1 + 1 + 14 - 1)); c.push_back(0x03); c.push_back(0x02); put(c, "http://a.b/c"); c.push_back(0x00); put(c, "w");
        c.push_back(0x00);
        Bytes o = object(5, 2, false, 0, c);
        JlObject j;
        CHECK(jlParseObject(o.data(), o.size(), 0, j), "links parse");
        CHECK(j.body == "x", "data sections not in the text: '%s'", j.body.c_str());
        CHECK(j.general.size() == 2, "two general links");
        if (j.general.size() == 2) {
            CHECK(j.general[0].type == 0 && j.general[0].target == 0x0042 && j.general[0].label == "ab", "object link");
            CHECK(j.general[1].type == 2 && j.general[1].address == "http://a.b/c" && j.general[1].label == "w", "url link '%s'", j.general[1].address.c_str());
        }
    }
    // ---- data group (clause 8.1.1): header 0x40 / 0x46, CRC, restrictions
    {
        Bytes o = textObj(0, "T", "Body");
        Bytes g = group(0, o);
        int t; const uint8_t* f; size_t fl; bool bad;
        CHECK(jlDataGroup(g.data(), g.size(), t, f, fl, &bad) && t == 0 && fl == o.size() && !bad, "group ok");
        Bytes g6 = group(6, o);
        CHECK(jlDataGroup(g6.data(), g6.size(), t, f, fl, &bad) && t == 6, "management group");
        Bytes b = g; b[5] ^= 1;
        CHECK(!jlDataGroup(b.data(), b.size(), t, f, fl, &bad) && bad, "bad crc");
        Bytes seg = g; seg[0] |= 0x20; { const uint16_t c = crcRef(seg.data(), seg.size() - 2); seg[seg.size() - 2] = (uint8_t)(c >> 8); seg.back() = (uint8_t)c; }
        CHECK(!jlDataGroup(seg.data(), seg.size(), t, f, fl, &bad) && !bad, "segment flag set: discarded");
        Bytes ty = g; ty[0] = 0x43; { const uint16_t c = crcRef(ty.data(), ty.size() - 2); ty[ty.size() - 2] = (uint8_t)(c >> 8); ty.back() = (uint8_t)c; }
        CHECK(!jlDataGroup(ty.data(), ty.size(), t, f, fl, &bad), "unknown type discarded");
        Bytes big(4093, 'x');
        Bytes gb = group(0, big);
        CHECK(!jlDataGroup(gb.data(), gb.size(), t, f, fl, &bad), "field over 4092 discarded");
    }
    // ---- TOC (6.2.2): 3 entries, revision 4, block 0 of 2, 5 objects in the service
    {
        Bytes c = {0x54, 4, 2, 0, 0x00, 0x00, 0x00, 0x05, 0x00, 0x3C, 3, 0x00, 0x00};
        const int ids[3] = {0x0000, 0x0001, 0x0002};
        for (int i : ids) { c.push_back((uint8_t)(i >> 8)); c.push_back((uint8_t)i); c.push_back(0x20); }
        int rev, cnt, idx, nobj, to; std::vector<JlTocEntry> e;
        CHECK(jlParseToc(c.data(), c.size(), rev, cnt, idx, nobj, to, e), "toc parses");
        CHECK(rev == 4 && cnt == 2 && idx == 0 && nobj == 5 && to == 60 && e.size() == 3 && e[2].id == 2 && e[2].desc == 0x20, "toc fields");
        JournalineDecoder d;
        d.feedDataGroup(group(6, c).data(), group(6, c).size());
        Bytes g0 = group(0, menu(0, "Root", {{1, "One"}, {2, "Two"}}));
        d.feedDataGroup(g0.data(), g0.size());
        JlStats s = d.stats();
        CHECK(s.expected == 5 && s.expectedHave == 1 && s.cached == 1 && s.tocBlocks == 1, "progress %d/%d", s.expectedHave, s.expected);
        // the carousel repeats an unchanged object: no update; a changed revision is an update
        const uint64_t q = d.seq();
        d.feedDataGroup(g0.data(), g0.size());
        CHECK(d.seq() == q, "repeat is no update");
        Bytes m2 = menu(0, "Root 2", {{1, "One"}, {2, "Two"}});
        d.feedDataGroup(group(0, m2).data(), group(0, m2).size());
        JlObject j;
        CHECK(d.seq() != q && d.get(0, j) && j.title == "Root 2" && d.stats().updates == 2, "update");
        Bytes sh = c; sh.resize(12);
        CHECK(!jlParseToc(sh.data(), sh.size(), rev, cnt, idx, nobj, to, e), "short toc");
    }
    // ---- user application data (8.1.2)
    {
        int v, x, en;
        const uint8_t a[] = {0x00, 0x00}, b[] = {0x00, 0x02, 0x12, 0x34};
        CHECK(jlUserAppData(a, 2, v, x, en) && v == 0 && x == 0 && en == -1, "ua default entry");
        CHECK(jlUserAppData(b, 4, v, x, en) && x == 2 && en == 0x1234, "ua entry");
        CHECK(!jlUserAppData(a, 1, v, x, en), "ua short");
    }
    // ---- LOOPBACK: packet mode, all four packet sizes, several groups, other addresses, FEC packets, lost packets, repetition, odd frame cuts
    {
        std::vector<Bytes> gs;
        gs.push_back(group(0, menu(0, "Root", {{1, "News"}, {2, "Sport"}})));
        std::string longText; for (int i = 0; i < 60; i++) longText += "Line " + std::to_string(i) + " of a long text. ";
        gs.push_back(group(0, textObj(1, "News", longText)));
        gs.push_back(group(0, textObj(2, "Sport", "Short")));
        for (int sc = 0; sc < 4; sc++) {
            DabPacketDemux dm(300);
            JournalineDecoder dec;
            int ci = 0, cio = 0;
            Bytes stream;
            for (auto& g : gs) {
                Bytes other = packets(Bytes(30, 0x55), 77, cio, 0);        // another address: skipped
                stream.insert(stream.end(), other.begin(), other.end());
                Bytes p = packets(g, 300, ci, sc);
                stream.insert(stream.end(), p.begin(), p.end());
            }
            // feed in odd pieces, split anywhere
            for (size_t off = 0; off < stream.size();) { const size_t n = std::min<size_t>(stream.size() - off, 37 + (size_t)(off % 11)); dm.feed(&stream[off], n, [&](const uint8_t* d, size_t l) { dec.feedDataGroup(d, l); }); off += n; }
            JlObject j;
            CHECK(dec.stats().cached == 3 && dec.get(1, j) && j.body.find("Line 59 of a long text.") != std::string::npos, "packet mode size %d: %d objects", sc, dec.stats().cached);
        }
        // a lost packet in the middle of the long group: the group is dropped, the next one is fine
        DabPacketDemux dm(300); JournalineDecoder dec;
        int ci = 0;
        Bytes p1 = packets(gs[1], 300, ci, 0);
        const size_t cnt = p1.size() / 24;
        Bytes stream;
        for (size_t k = 0; k < cnt; k++) if (k != 3) stream.insert(stream.end(), p1.begin() + (long)(k * 24), p1.begin() + (long)(k * 24 + 24));
        Bytes p2 = packets(gs[2], 300, ci, 0);
        stream.insert(stream.end(), p2.begin(), p2.end());
        dm.feed(stream.data(), stream.size(), [&](const uint8_t* d, size_t l) { dec.feedDataGroup(d, l); });
        JlObject j;
        CHECK(dec.stats().cached == 1 && dec.get(2, j), "lost packet drops one group only (%d cached)", dec.stats().cached);
        // a damaged packet: skipped by its CRC
        DabPacketDemux dm2(300); JournalineDecoder dec2; ci = 0;
        Bytes p3 = packets(gs[2], 300, ci, 0); p3[5] ^= 0x40;
        dm2.feed(p3.data(), p3.size(), [&](const uint8_t* d, size_t l) { dec2.feedDataGroup(d, l); });
        CHECK(dec2.stats().cached == 0 && dm2.badPackets >= 1, "damaged packet");
    }
    // ---- LOOPBACK: DRM packet mode (ES 201 980 clause 6.6.1): 8 bit header, PPI padding, CRC-16, data units over several packets, a lost packet
    {
        const int len = 20, pid = 2;
        Bytes g1 = group(0, menu(0, "Root", {{1, "A"}}));
        Bytes g2 = group(0, textObj(1, "A", std::string(150, 'x')));
        Bytes stream;
        int ci = 0;
        auto pk = [&](const Bytes& g, bool skipMiddle) {
            size_t off = 0, k = 0;
            while (off < g.size()) {
                const size_t n = std::min<size_t>((size_t)len - 1, g.size() - off);   // PPI = 1 costs one byte of the data field
                const bool first = off == 0, last = off + n >= g.size();
                Bytes p(len + 3, 0);
                p[0] = (uint8_t)((first ? 0x80 : 0) | (last ? 0x40 : 0) | (pid << 4) | 8 | (ci & 7));
                p[1] = (uint8_t)n;
                memcpy(&p[2], &g[off], n);
                const uint16_t c = crcRef(p.data(), p.size() - 2);   // G16, ones preset, inverted: the same CRC as DAB
                p[p.size() - 2] = (uint8_t)(c >> 8); p[p.size() - 1] = (uint8_t)c;
                ci++; k++;
                if (!(skipMiddle && k == 2)) stream.insert(stream.end(), p.begin(), p.end());
                off += n;
            }
        };
        pk(g1, false); pk(g2, true); pk(g1, false);
        DrmPacketDemux dm; dm.configure(len, pid);
        JournalineDecoder dec;
        int units = 0;
        for (size_t off = 0; off < stream.size(); off += (size_t)(len + 3) * 3) {
            const size_t n = std::min<size_t>(stream.size() - off, (size_t)(len + 3) * 3);
            dm.feed(&stream[off], n - n % (size_t)(len + 3), [&](const uint8_t* d, size_t l) { units++; dec.feedDataGroup(d, l); });
        }
        JlObject j;
        CHECK(units == 2 && dec.get(0, j) && !dec.get(1, j), "drm: %d data units, the damaged one dropped", units);
    }
    // ---- robustness: random bytes, truncations of every valid group, every single-byte corruption of an object
    {
        srand(7);
        Bytes valid = group(0, menu(0, "Root", {{1, "News"}, {2, "Sport"}}));
        JournalineDecoder d;
        for (size_t n = 0; n <= valid.size(); n++) d.feedDataGroup(valid.data(), n);
        Bytes o = textObj(4, "Title", "Text \x1A\x05 text");
        JlObject j;
        for (size_t n = 0; n <= o.size(); n++) jlParseObject(o.data(), n, 0, j);
        for (size_t i = 0; i < o.size(); i++) for (int v : {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x10, 0x14, 0x1A, 0x1B, 0x1C, 0x1D, 0x7F, 0xFF}) { Bytes c = o; c[i] = (uint8_t)v; jlParseObject(c.data(), c.size(), 0, j); jlParseObject(c.data(), c.size(), 3, j); }
        for (int it = 0; it < 20000; it++) {
            Bytes r((size_t)(rand() % 200)); for (auto& b : r) b = (uint8_t)rand();
            if (it & 1 && r.size() > 3) { r[2] = (uint8_t)((rand() % 5) << 5 | (it & 8) | (rand() & 0x17)); if (it & 2) r[3] = 0x08; }
            jlParseObject(r.data(), r.size(), it % 3, j);
            int a, b, c, e, f; std::vector<JlTocEntry> te;
            if (it & 4 && !r.empty()) r[0] = 0x54;
            jlParseToc(r.data(), r.size(), a, b, c, e, f, te);
            d.feedDataGroup(r.data(), r.size());
            Bytes g = group(it % 2 ? 0 : 6, r);   // a valid CRC around garbage
            d.feedDataGroup(g.data(), g.size());
        }
        DabPacketDemux dm(5);
        for (int it = 0; it < 2000; it++) { Bytes r((size_t)(rand() % 300)); for (auto& b : r) b = (uint8_t)rand(); dm.feed(r.data(), r.size(), [&](const uint8_t* x, size_t l) { d.feedDataGroup(x, l); }); }
        // a deflate bomb: the output is capped, never crashes
        Bytes bomb = {0x00, 0x01, 0x03, 0x08, 0x05, 0x00, 0x00, 0x00};
        jlParseObject(bomb.data(), bomb.size(), 0, j);
        d.stats(); d.all();
    }
    printf(fails ? "journaline: %d FAILED\n" : "journaline: all passed\n", fails);
    return fails ? 1 : 0;
}
