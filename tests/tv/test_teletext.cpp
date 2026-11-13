// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Teletext: Hamming 8/4 against the table from ETS 300 706, then a page encoded into TS packets and decoded again.
#include "dect2/tv/teletext.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <cstdint>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint8_t hamm(int n) {
    static const uint8_t t[16] = {0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F, 0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA};
    return t[n & 15];
}
static uint8_t oddParity(uint8_t c) { return __builtin_popcount(c & 0x7F) % 2 ? (c & 0x7F) : (uint8_t)((c & 0x7F) | 0x80); }

// one teletext data unit (44 bytes) for magazine/row with 40 payload bytes, bit-reversed as in a DVB PES
static std::vector<uint8_t> unit(int mag, int row, const uint8_t* payload40) {
    std::vector<uint8_t> u = {0x02, 44, 0xC0 | 7, 0xE4};
    const int addr = (mag & 7) | (row << 3);
    u.push_back(ttxReverse8(hamm(addr & 15)));
    u.push_back(ttxReverse8(hamm(addr >> 4)));
    for (int i = 0; i < 40; i++) u.push_back(ttxReverse8(payload40[i]));
    return u;
}


// a header payload (40 bytes): page number, subcode, erase (C4), subtitle (C6), national option (C12..C14), serial flag (C11)
static void headerPayload(uint8_t* h, int num, int sub, bool erase, bool subtitle, int national, bool serial = false, const char* text = "") {
    h[0] = hamm(num % 10); h[1] = hamm((num / 10) % 10);
    h[2] = hamm(sub & 15); h[3] = hamm(((sub >> 4) & 7) | (erase ? 8 : 0));
    h[4] = hamm((sub >> 8) & 15); h[5] = hamm(((sub >> 12) & 3) | (subtitle ? 8 : 0));
    h[6] = hamm(0); h[7] = hamm((national << 1) | (serial ? 1 : 0));
    for (int i = 0; i < 32; i++) h[8 + i] = oddParity(i < (int)strlen(text) ? text[i] : ' ');
}
static void rowPayload(uint8_t* r, const std::vector<int>& codes) {
    for (int i = 0; i < 40; i++) r[i] = oddParity(i < (int)codes.size() ? codes[i] : ' ');
}
static void feedUnits(TeletextDecoder& d, const std::vector<uint8_t>& du) {
    std::vector<uint8_t> pes = {0, 0, 1, 0xBD, 0, 0, 0x85, 0x80, 0x00, 0x10};
    pes.insert(pes.end(), du.begin(), du.end());
    const size_t plen = pes.size() - 6;
    pes[4] = (uint8_t)(plen >> 8); pes[5] = (uint8_t)plen;
    d.feedPes(pes.data(), pes.size());
}
static void add(std::vector<uint8_t>& du, int mag, int row, const uint8_t* payload40) {
    auto u = unit(mag, row, payload40);
    du.insert(du.end(), u.begin(), u.end());
}
// Hamming 24/18 encoder: data bits D1..D18 into the non-parity positions of a 24 bit word, P1..P5 so that the syndrome is zero, P6 for odd parity
static void ham2418(int data, uint8_t* out) {
    uint32_t w = 0;
    int k = 0;
    for (int n = 1; n <= 23; n++) { if ((n & (n - 1)) == 0) continue; if (data >> k & 1) w |= 1u << (n - 1); k++; }
    unsigned syn = 0;
    for (int n = 1; n <= 23; n++) if (w >> (n - 1) & 1) syn ^= (unsigned)n;
    for (int b = 0; b < 5; b++) if (syn >> b & 1) w |= 1u << ((1 << b) - 1);
    if (!(__builtin_popcount(w) & 1)) w |= 1u << 23;
    out[0] = w & 0xFF; out[1] = (w >> 8) & 0xFF; out[2] = (w >> 16) & 0xFF;
}

int main() {
    // all 16 codewords decode, and every single-bit error is corrected
    static const uint8_t t[16] = {0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F, 0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA};
    for (int n = 0; n < 16; n++) {
        CHECK(ttxHamming84(t[n]) == n, "hamming %d", n);
        for (int b = 0; b < 8; b++) CHECK(ttxHamming84(t[n] ^ (1 << b)) == n, "single-bit correction nibble %d bit %d", n, b);
    }
    CHECK(ttxHamming84(0x15 ^ 0x03) == -1 || ttxHamming84(0x15 ^ 0x03) != 0, "double error must not decode as the original");
    CHECK(ttxReverse8(0x01) == 0x80 && ttxReverse8(0xE4) == 0x27, "reverse8");

    // build page 123 in magazine 1: header + two rows, with a double-height and a mosaic row
    std::vector<uint8_t> du;
    uint8_t hdr[40];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = hamm(3); hdr[1] = hamm(2);                  // units 3, tens 2
    hdr[2] = hamm(0); hdr[3] = hamm(8); hdr[4] = hamm(0); hdr[5] = hamm(0); hdr[6] = hamm(0); hdr[7] = hamm(0); // S1, S2+C4(erase), ...
    const char* title = "DecT2 teletext test";
    for (int i = 0; i < 32; i++) hdr[8 + i] = oddParity(i < (int)strlen(title) ? title[i] : ' ');
    auto u0 = unit(1, 0, hdr); du.insert(du.end(), u0.begin(), u0.end());
    uint8_t r1[40], r2[40];
    for (int i = 0; i < 40; i++) r1[i] = oddParity(' ');
    r1[0] = oddParity(0x02);                             // green
    const char* msg = "Hello";
    for (int i = 0; i < 5; i++) r1[1 + i] = oddParity(msg[i]);
    auto u1 = unit(1, 1, r1); du.insert(du.end(), u1.begin(), u1.end());
    for (int i = 0; i < 40; i++) r2[i] = oddParity(' ');
    r2[0] = oddParity(0x0D);                             // double height
    r2[1] = oddParity(0x01);                             // red
    r2[2] = oddParity('W');
    auto u2 = unit(1, 2, r2); du.insert(du.end(), u2.begin(), u2.end());

    std::vector<uint8_t> pes = {0, 0, 1, 0xBD, 0, 0, 0x85, 0x80, 0x24};
    for (int i = 0; i < 36; i++) pes.push_back(0xFF);    // 36 bytes of PES header data (stuffing)
    pes[8] = 36;
    pes.push_back(0x10);                                 // data_identifier
    pes.insert(pes.end(), du.begin(), du.end());
    const size_t plen = pes.size() - 6;
    pes[4] = (uint8_t)(plen >> 8); pes[5] = (uint8_t)plen;

    // into 188-byte TS packets on PID 0x123
    TeletextDecoder d;
    d.setPid(0x123);
    size_t pos = 0; int cc = 0;
    while (pos < pes.size()) {
        uint8_t pkt[188];
        memset(pkt, 0xFF, sizeof pkt);
        pkt[0] = 0x47; pkt[1] = (pos == 0 ? 0x40 : 0) | 0x01; pkt[2] = 0x23;
        const size_t room = 184, n = std::min(room, pes.size() - pos);
        if (n < room) { // adaptation field stuffing
            pkt[3] = 0x30 | (cc & 15);
            const size_t stuff = room - n;
            pkt[4] = (uint8_t)(stuff - 1);
            if (stuff > 1) { pkt[5] = 0; memset(pkt + 6, 0xFF, stuff - 2); }
            memcpy(pkt + 4 + stuff, pes.data() + pos, n);
        } else { pkt[3] = 0x10 | (cc & 15); memcpy(pkt + 4, pes.data() + pos, n); }
        cc++; pos += n;
        d.feedTs(pkt);
    }
    TtxPage pg;
    CHECK(d.page(123, pg), "page 123 not found");
    if (d.page(123, pg)) {
        CHECK(pg.number == 123, "number %d", pg.number);
        std::string h; for (int i = 8; i < 28; i++) h += (char)pg.rows[0][i];
        CHECK(h.substr(0, 19) == title, "header text '%s'", h.c_str());
        CHECK((pg.rowMask & 6) == 6, "rows 1 and 2 missing (mask %x)", pg.rowMask);
        TtxGrid g;
        ttxRender(pg, g);
        CHECK(g[1][0].ch == ' ' && g[1][1].ch == 'H' && g[1][1].fg == 2, "row 1: green Hello (got '%c' fg %d)", (char)g[1][1].ch, g[1][1].fg);
        CHECK(g[2][2].ch == 'W' && g[2][2].dh == 1 && g[2][2].fg == 1, "row 2: red double-height W");
        CHECK(g[3][2].ch == 'W' && g[3][2].dh == 2, "row 3 continues the double-height character");
        printf("page 123 header: '%s'\n", h.c_str());
    }

    // Finnish / Swedish national option (C12..C14 = 010): 0x5B/0x5C/0x5D → Ä / Ö / Å
    TtxPage fin;
    fin.number = 100;
    fin.national = 2;
    fin.rowMask = 2;
    for (int i = 0; i < 40; i++) fin.rows[1][i] = ' ';
    fin.rows[1][0] = 0x02;
    fin.rows[1][1] = 'S';
    fin.rows[1][2] = 0x5B;
    fin.rows[1][3] = 'H';
    fin.rows[1][4] = 'K';
    fin.rows[1][5] = 0x5C;
    fin.rows[1][6] = 'N';
    TtxGrid finG;
    ttxRender(fin, finG);
    CHECK(finG[1][2].ch == 0x00C4 && finG[1][5].ch == 0x00D6, "Finnish subset Ä Ö (got %04x %04x)", finG[1][2].ch, finG[1][5].ch);
    // ---- Hamming 24/18: every single-bit error is corrected, every double error detected
    {
        uint32_t seed = 12345;
        int bad = 0, badSingle = 0, badDouble = 0;
        for (int t = 0; t < 600; t++) {
            seed = seed * 1664525u + 1013904223u;
            const int data = t == 0 ? 0 : t == 1 ? 0x3FFFF : (int)((seed >> 8) & 0x3FFFF);
            uint8_t w[3];
            ham2418(data, w);
            if (ttxHamming2418(w) != data) bad++;
            for (int b1 = 0; b1 < 24; b1++) {
                uint8_t e[3] = {w[0], w[1], w[2]};
                e[b1 / 8] ^= (uint8_t)(1 << (b1 % 8));
                if (ttxHamming2418(e) != data) badSingle++;
                if (t < 40) for (int b2 = b1 + 1; b2 < 24; b2++) {
                    uint8_t f[3] = {e[0], e[1], e[2]};
                    f[b2 / 8] ^= (uint8_t)(1 << (b2 % 8));
                    if (ttxHamming2418(f) != -1) badDouble++;
                }
            }
        }
        CHECK(bad == 0, "hamming 24/18 clean words wrong: %d", bad);
        CHECK(badSingle == 0, "hamming 24/18 single-bit errors not corrected: %d", badSingle);
        CHECK(badDouble == 0, "hamming 24/18 double-bit errors not detected: %d", badDouble);
        const uint8_t zero[3] = {0x00, 0x00, 0x80};   // data 0: all parity bits zero, P6 makes the word odd
        uint8_t z[3];
        ham2418(0, z);
        CHECK(z[0] == zero[0] && z[1] == zero[1] && z[2] == zero[2], "hamming 24/18 encoding of zero: %02x %02x %02x", z[0], z[1], z[2]);
        CHECK(ttxHamming2418(zero) == 0, "hamming 24/18 decode of the zero word");
        const uint8_t one[3] = {0x04, 0x00, 0x00 | 0x80};   // D1 (position 3) set: syndrome 3 -> P1, P2 set (positions 1, 2), total 3 ones -> odd
        uint8_t o[3];
        ham2418(1, o);
        CHECK(o[0] == 0x07 && o[1] == 0 && o[2] == 0, "hamming 24/18 encoding of D1: %02x %02x %02x", o[0], o[1], o[2]);
        (void)one;
    }

    // ---- national option subsets
    CHECK(ttxG0(0x23, 0) == 0x00A3 && ttxG0(0x23, 1) == '#' && ttxG0(0x5B, 1) == 0x00C4 && ttxG0(0x7E, 1) == 0x00DF, "German subset");
    CHECK(ttxG0(0x40, 4) == 0x00E0 && ttxG0(0x7B, 4) == 0x00E2 && ttxG0(0x7D, 3) == 0x00E8 && ttxG0(0x5B, 5) == 0x00E1 && ttxG0(0x7D, 6) == 0x00FA, "French/Italian/Spanish/Czech");
    CHECK(ttxG0('A', 3) == 'A' && ttxG0('z', 2) == 'z', "letters unchanged");

    // ---- a page with mixed attributes, national option 1 (German), Fastext links, subpages and the page cache
    {
        TeletextDecoder d2;
        std::vector<uint8_t> du2;
        uint8_t h[40], r[40];
        headerPayload(h, 25, 0x0001, true, false, 1, false, "German");
        add(du2, 2, 0, h);
        rowPayload(r, {0x04, 'A', 0x1D, 0x03, 'B', 0x08, 'C', 0x11, 0x23, 0x1A, 0x2C, 0x18, 'X', 0x07, 0x5B, 0x7E});
        add(du2, 2, 5, r);
        // Fastext (packet 27/0): red -> 234 (magazine 2 seen from magazine 2: relative 0 means "same magazine"), green -> 100 (mag 1), yellow -> none, cyan -> 899, index -> 200
        {
            uint8_t f[40];
            memset(f, 0, sizeof f);
            f[0] = hamm(0);
            auto link = [&](int slot, int cur, int page) {
                uint8_t* q = f + 1 + slot * 6;
                if (page < 0) { for (int i = 0; i < 6; i++) q[i] = hamm(15); return; }
                const int pm = (page / 100) & 7, rel = (cur & 7) ^ pm;
                q[0] = hamm(page % 10); q[1] = hamm((page / 10) % 10); q[2] = hamm(0);
                q[3] = hamm(rel & 1 ? 8 : 0); q[4] = hamm(0); q[5] = hamm(((rel >> 1) & 1) << 2 | ((rel >> 2) & 1) << 3);
            };
            link(0, 2, 234); link(1, 2, 100); link(2, 2, -1); link(3, 2, 899); link(4, 2, 200); link(5, 2, -1);
            for (int i = 37; i < 40; i++) f[i] = hamm(0);
            add(du2, 2, 27, f);
        }
        headerPayload(h, 25, 0x0002, true, false, 1, false, "second subpage");   // ends subpage 1
        add(du2, 2, 0, h);
        rowPayload(r, {'S', 'u', 'b', '2'});
        add(du2, 2, 3, r);
        headerPayload(h, 30, 0x0000, true, false, 0, false, "closes");
        add(du2, 2, 0, h);
        feedUnits(d2, du2);
        TtxPage p;
        CHECK(d2.page(225, p, 1), "page 225 subpage 1 not cached");
        // the packet-27 links were for the page being sent when it arrived (225/1)
        CHECK(p.national == 1, "national option %d", p.national);
        CHECK(p.fastext[0] == 234 && p.fastext[1] == 100 && p.fastext[2] == 0 && p.fastext[3] == 899 && p.fastext[4] == 200,
              "fastext links %d %d %d %d %d", p.fastext[0], p.fastext[1], p.fastext[2], p.fastext[3], p.fastext[4]);
        TtxGrid g;
        ttxRender(p, g);
        CHECK(g[5][0].fg == 4 && g[5][1].ch == 'A' && g[5][1].fg == 4 && g[5][1].bg == 0, "blue A");
        CHECK(g[5][2].bg == 4, "new background takes effect at the control cell");
        CHECK(g[5][4].ch == 'B' && g[5][4].fg == 3 && g[5][4].bg == 4 && !g[5][4].flash, "yellow B on blue");
        CHECK(g[5][6].ch == 'C' && g[5][6].flash && g[5][6].fg == 3, "flashing C");
        CHECK(g[5][8].mosaic && g[5][8].sextants == 3 && g[5][8].fg == 1 && !g[5][8].separated, "red contiguous mosaic with two sextants (%d)", g[5][8].sextants);
        CHECK(g[5][10].mosaic && g[5][10].sextants == 0x0C && g[5][10].separated, "separated mosaic");
        CHECK(g[5][12].conceal && ttxRowText(g, 5).find('X') == std::string::npos, "concealed X is hidden in the row text");
        CHECK(g[5][14].ch == 0x00C4 && g[5][15].ch == 0x00DF, "German national characters (%x %x)", g[5][14].ch, g[5][15].ch);
        const std::string row5 = ttxRowText(g, 5);
        CHECK(row5.find("AB") != std::string::npos || row5.substr(1, 1) == "A", "row text '%s'", row5.c_str());
        CHECK(d2.subpages(225) == std::vector<int>({1, 2}), "subpages of 225");
        TtxPage p2;
        CHECK(d2.page(225, p2) && p2.subcode == 2, "latest subpage is 2");
        CHECK(d2.page(225, p2, 2) && p2.rows[3][0] == 'S' && p2.complete, "subpage 2 content and completion");
        CHECK(d2.pages() == std::vector<int>({225, 230}), "page list");
    }

    // ---- a teletext subtitle page: header flag, boxed text only, erased by a new empty header
    {
        TeletextDecoder d3;
        std::vector<uint8_t> du3;
        uint8_t h[40], r[40];
        headerPayload(h, 88, 0, true, true, 0, false, "");   // page 888 (mag 8 is sent as 0)
        add(du3, 8, 0, h);
        rowPayload(r, {0x0A, 0x0A, 0x0B, 0x0B, 0x07, 'H', 'e', 'l', 'l', 'o', 0x0A, 0x0A, 'o', 'u', 't'});
        add(du3, 8, 20, r);
        headerPayload(h, 0, 0, true, false, 0, false, "x");   // next page ends 888
        add(du3, 8, 0, h);
        feedUnits(d3, du3);
        TtxPage p;
        CHECK(d3.page(888, p) && p.subtitle && p.complete, "page 888 is a completed subtitle page");
        TtxGrid g;
        ttxRender(p, g);
        CHECK(g[20][5].boxed && g[20][5].ch == 'H' && g[20][9].boxed && g[20][9].ch == 'o', "boxed Hello");
        CHECK(!g[20][12].boxed && g[20][12].ch == 'o', "text after the end box is not boxed");
        CHECK(!g[20][0].boxed, "before the start box");
        // an erase header with no rows clears it
        std::vector<uint8_t> du4;
        headerPayload(h, 88, 0, true, true, 0, false, "");
        add(du4, 8, 0, h);
        headerPayload(h, 0, 0, true, false, 0, false, "x");
        add(du4, 8, 0, h);
        feedUnits(d3, du4);
        CHECK(d3.page(888, p) && p.rowMask == 1, "erased subtitle page has only its header (mask %x)", p.rowMask);
    }
    printf(fails ? "teletext tests FAILED\n" : "teletext tests passed\n");
    return fails ? 1 : 0;
}
