// Teletext: Hamming 8/4 against the table from ETS 300 706, then a page encoded into TS packets and decoded again.
#include "dect2/teletext.h"
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
    printf(fails ? "teletext tests FAILED\n" : "teletext tests passed\n");
    return fails ? 1 : 0;
}
