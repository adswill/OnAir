// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB subtitles (EN 300 743): a page / region / CLUT / object set built here with 2-, 4- and 8-bit run-length strings is decoded, and the
// rendered bitmap pixels, the region positions and the PTS timing are checked. No real broadcast recording is used (none is available).
#include "dect2/tv/dvbsub.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <algorithm>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

typedef std::vector<uint8_t> Bytes;

struct BW {
    Bytes v; int nb = 0;
    void put(unsigned val, int bits) { for (int i = bits - 1; i >= 0; i--) { if (nb % 8 == 0) v.push_back(0); if ((val >> i) & 1) v.back() |= (uint8_t)(0x80 >> (nb % 8)); nb++; } }
    void align() { nb = (int)v.size() * 8; }
};

static void put16(Bytes& b, int v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
static Bytes seg(int type, int page, const Bytes& payload) {
    Bytes s = {0x0F, (uint8_t)type};
    put16(s, page); put16(s, (int)payload.size());
    s.insert(s.end(), payload.begin(), payload.end());
    return s;
}
static void cat(Bytes& a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); }

// the PES carrying a display set, with a PTS (90 kHz ticks)
static Bytes pes(const Bytes& segs, uint64_t pts) {
    Bytes p = {0, 0, 1, 0xBD, 0, 0, 0x84, 0x80, 5};
    p.push_back((uint8_t)(0x21 | (((pts >> 30) & 7) << 1)));
    p.push_back((uint8_t)(pts >> 22));
    p.push_back((uint8_t)((((pts >> 15) & 0x7F) << 1) | 1));
    p.push_back((uint8_t)(pts >> 7));
    p.push_back((uint8_t)(((pts & 0x7F) << 1) | 1));
    p.push_back(0x20); p.push_back(0x00);
    cat(p, segs);
    p.push_back(0xFF);
    const size_t l = p.size() - 6;
    p[4] = (uint8_t)(l >> 8); p[5] = (uint8_t)l;
    return p;
}
static void feedTsPackets(DvbSubDecoder& d, const Bytes& pes, int pid, int& cc) {
    size_t pos = 0;
    while (pos < pes.size()) {
        uint8_t pkt[188];
        memset(pkt, 0xFF, sizeof pkt);
        pkt[0] = 0x47; pkt[1] = (uint8_t)((pos == 0 ? 0x40 : 0) | (pid >> 8)); pkt[2] = (uint8_t)pid;
        const size_t room = 184, n = std::min(room, pes.size() - pos);
        if (n < room) {
            pkt[3] = 0x30 | (cc & 15);
            const size_t stuff = room - n;
            pkt[4] = (uint8_t)(stuff - 1);
            if (stuff > 1) { pkt[5] = 0; memset(pkt + 6, 0xFF, stuff - 2); }
            memcpy(pkt + 4 + stuff, pes.data() + pos, n);
        } else { pkt[3] = 0x10 | (cc & 15); memcpy(pkt + 4, pes.data() + pos, n); }
        cc++; pos += n;
        d.feedTs(pkt);
    }
}

static Bytes pageComp(int timeout, int version, int state, const std::vector<std::vector<int>>& regs) {
    Bytes b = {(uint8_t)timeout, (uint8_t)(version << 4 | state << 2 | 3)};
    for (auto& r : regs) { b.push_back((uint8_t)r[0]); b.push_back(0); put16(b, r[1]); put16(b, r[2]); }
    return b;
}
static Bytes regionComp(int id, int ver, bool fill, int w, int h, int depthCode, int clut, int objId, int ox, int oy) {
    Bytes b = {(uint8_t)id, (uint8_t)(ver << 4 | (fill ? 8 : 0) | 7)};
    put16(b, w); put16(b, h);
    b.push_back((uint8_t)(1 << 5 | depthCode << 2 | 3));   // compatibility level 1, depth, reserved
    b.push_back((uint8_t)clut); b.push_back(0); b.push_back(0);
    put16(b, objId);
    b.push_back((uint8_t)(0x00 | ((ox >> 8) & 0x0F))); b.push_back((uint8_t)ox);   // object_type 0 (bitmap)
    b.push_back((uint8_t)((oy >> 8) & 0x0F)); b.push_back((uint8_t)oy);
    return b;
}
// CLUT entries: {id, flags, y, cr, cb, t} full range, or flags with bit 0 clear: {id, flags, v16}
static void entryFull(Bytes& b, int id, int depthFlags, int y, int cr, int cb, int t) {
    b.push_back((uint8_t)id); b.push_back((uint8_t)(depthFlags | 1));
    b.push_back((uint8_t)y); b.push_back((uint8_t)cr); b.push_back((uint8_t)cb); b.push_back((uint8_t)t);
}
static Bytes objectData(int id, const Bytes& top, const Bytes& bot) {
    Bytes b;
    put16(b, id);
    b.push_back(0x00);   // version 0, coding method 0 (pixels)
    put16(b, (int)top.size()); put16(b, (int)bot.size());
    cat(b, top); cat(b, bot);
    return b;
}

static bool px(const DvbSubRegionImage& r, int x, int y, int R, int G, int B, int A, int tol = 3) {
    const uint8_t* p = &r.rgba[((size_t)y * r.w + x) * 4];
    if (A == 0) return p[3] == 0;
    return std::abs(p[0] - R) <= tol && std::abs(p[1] - G) <= tol && std::abs(p[2] - B) <= tol && std::abs(p[3] - A) <= 1;
}

int main() {
    DvbSubDecoder d;
    d.setStream(0x200, 1, 2);

    // ---- display set 1: one 40x6 4-bit region at (100, 200); CLUT and object arrive on the ancillary page id 2
    Bytes clut = {1, 0};   // CLUT 1, version 0
    entryFull(clut, 1, 0xE0, 235, 128, 128, 0);       // white
    entryFull(clut, 2, 0xE0, 81, 240, 90, 0);         // red (BT.601)
    entryFull(clut, 3, 0xE0, 235, 128, 128, 255);     // white but T = 255: fully transparent
    clut.push_back(5); clut.push_back(0xE0);           // reduced precision: Y 6 bit, Cr 4, Cb 4, T 2  (white)
    { const int v = (63 << 10) | (8 << 6) | (8 << 2) | 0; clut.push_back((uint8_t)(v >> 8)); clut.push_back((uint8_t)v); }

    Bytes top, bot;
    {   // top line 0 (region row 1): 5 x code 2 (run 4..7), 1 x code 3, 1 x code 0 (switch_3 = 00), 2 x code 0 (switch_3 = 01)
        BW w;
        w.put(0, 4); w.put(1, 1); w.put(0, 1); w.put(1, 2); w.put(2, 4);          // run_length_4-7: 4 + 1 = 5 of code 2
        w.put(3, 4);
        w.put(0, 4); w.put(1, 1); w.put(1, 1); w.put(0, 2);                       // one pixel of colour 0
        w.put(0, 4); w.put(1, 1); w.put(1, 1); w.put(1, 2);                       // two pixels of colour 0
        w.put(0, 4); w.put(0, 1); w.put(0, 3);                                    // end of 4-bit string
        w.align();
        top.push_back(0x11); cat(top, w.v); top.push_back(0xF0);
    }
    {   // top line 1 (region row 3): 12 x code 1 (run 9..24: 9 + 3), then 5 and 5
        BW w;
        w.put(0, 4); w.put(1, 1); w.put(1, 1); w.put(2, 2); w.put(3, 4); w.put(1, 4);
        w.put(5, 4); w.put(5, 4);
        w.put(0, 4); w.put(0, 1); w.put(0, 3);
        w.align();
        top.push_back(0x11); cat(top, w.v); top.push_back(0xF0);
    }
    {   // bottom line 0 (region row 2): 30 x code 1 (run 25..280: 25 + 5)
        BW w;
        w.put(0, 4); w.put(1, 1); w.put(1, 1); w.put(3, 2); w.put(5, 8); w.put(1, 4);
        w.put(0, 4); w.put(0, 1); w.put(0, 3);
        w.align();
        bot.push_back(0x11); cat(bot, w.v); bot.push_back(0xF0);
    }
    {   // bottom line 1 (region row 4): 1 2 1 2 as plain 4-bit codes
        BW w;
        w.put(1, 4); w.put(2, 4); w.put(1, 4); w.put(2, 4);
        w.put(0, 4); w.put(0, 1); w.put(0, 3);
        w.align();
        bot.push_back(0x11); cat(bot, w.v); bot.push_back(0xF0);
    }
    Bytes set1;
    cat(set1, seg(0x10, 1, pageComp(5, 1, 2 /* mode change */, {{1, 100, 200}})));
    cat(set1, seg(0x14, 1, {0x00, 0x04, 0xFF, 0x02, 0xCF}));              // display definition 1280 x 720
    cat(set1, seg(0x11, 1, regionComp(1, 0, false, 40, 6, 2 /* 4 bit */, 1, 7, 2, 1)));
    cat(set1, seg(0x12, 2, clut));
    cat(set1, seg(0x13, 2, objectData(7, top, bot)));
    cat(set1, seg(0x10, 9, pageComp(5, 3, 2, {})));                      // another page's composition: must be ignored
    cat(set1, seg(0x80, 1, {}));
    int cc = 0;
    feedTsPackets(d, pes(set1, 900000), 0x200, cc);      // PTS 10.0 s
    CHECK(d.displays() == 1, "one display set decoded (got %llu)", (unsigned long long)d.displays());

    CHECK(d.displayAt(9.9) == nullptr, "not shown before its PTS");
    auto disp = d.displayAt(10.0);
    CHECK(disp != nullptr, "shown at its PTS");
    if (disp) {
        CHECK(disp->dispW == 1280 && disp->dispH == 720, "display definition %d x %d", disp->dispW, disp->dispH);
        CHECK(disp->regions.size() == 1, "one region");
        if (disp->regions.size() == 1) {
            const DvbSubRegionImage& r = disp->regions[0];
            CHECK(r.x == 100 && r.y == 200 && r.w == 40 && r.h == 6, "region geometry %d,%d %dx%d", r.x, r.y, r.w, r.h);
            bool ok = true;
            for (int x = 0; x < 40; x++) ok = ok && px(r, x, 0, 0, 0, 0, 0) && px(r, x, 5, 0, 0, 0, 0);
            CHECK(ok, "rows 0 and 5 are untouched (transparent)");
            // row 1 (object top field line 0): x 2..6 red, then transparent
            for (int x = 2; x <= 6; x++) CHECK(px(r, x, 1, 255, 0, 0, 255), "row1 x%d red", x);
            CHECK(px(r, 7, 1, 0, 0, 0, 0) && px(r, 8, 1, 0, 0, 0, 0) && px(r, 9, 1, 0, 0, 0, 0) && px(r, 10, 1, 0, 0, 0, 0), "row1 x7..10 transparent");
            CHECK(px(r, 0, 1, 0, 0, 0, 0) && px(r, 1, 1, 0, 0, 0, 0), "row1 left of the object transparent");
            // row 2 (bottom field line 0): 30 white pixels from x = 2
            bool w30 = true;
            for (int x = 2; x < 32; x++) w30 = w30 && px(r, x, 2, 255, 255, 255, 255);
            CHECK(w30 && px(r, 32, 2, 0, 0, 0, 0) && px(r, 1, 2, 0, 0, 0, 0), "row2: 30 white pixels from x2 (run 25-280)");
            // row 3 (top field line 1): 12 white, then two code-5 pixels (reduced precision CLUT entry, white)
            bool w12 = true;
            for (int x = 2; x < 14; x++) w12 = w12 && px(r, x, 3, 255, 255, 255, 255);
            CHECK(w12 && px(r, 14, 3, 255, 255, 255, 255) && px(r, 15, 3, 255, 255, 255, 255) && px(r, 16, 3, 0, 0, 0, 0), "row3: 12 white then two reduced-precision whites");
            // row 4 (bottom field line 1): white red white red
            CHECK(px(r, 2, 4, 255, 255, 255, 255) && px(r, 3, 4, 255, 0, 0, 255) && px(r, 4, 4, 255, 255, 255, 255) && px(r, 5, 4, 255, 0, 0, 255) && px(r, 6, 4, 0, 0, 0, 0), "row4: 1 2 1 2");
            // composed onto the 1280x720 canvas at the region position
            std::vector<uint8_t> canvas;
            dvbSubComposite(*disp, canvas);
            CHECK(canvas.size() == (size_t)1280 * 720 * 4, "canvas size");
            const uint8_t* c = &canvas[((size_t)201 * 1280 + 102) * 4];
            CHECK(c[0] > 250 && c[1] < 4 && c[2] < 4 && c[3] == 255, "canvas pixel (102,201) red");
            CHECK(canvas[((size_t)10 * 1280 + 10) * 4 + 3] == 0, "canvas elsewhere transparent");
        }
    }
    CHECK(d.displayAt(14.9) != nullptr, "still shown inside the timeout");
    CHECK(d.displayAt(15.2) == nullptr, "gone after the page time-out");
    {   // #112: the picture clock jumped (smoothed gaps, a stall) and the page was first seen only just before it expires: it stays for the minimum
        DvbSubDecoder j;
        j.setStream(0x200, 1, 2);
        int jcc = 0;
        feedTsPackets(j, pes(set1, 900000), 0x200, jcc);             // PTS 10.0 s, page_time_out 5 s
        CHECK(j.displayAt(14.9) != nullptr, "first seen near the end of its time-out");
        CHECK(j.displayAt(15.3) != nullptr, "kept for the minimum time on screen (%.1f s)", DvbSubDecoder::kMinShowSec);
        CHECK(j.displayAt(16.5) == nullptr, "and then gone");
        CHECK(j.displayAt(14.9) != nullptr, "the clock going back shows it again");
    }
    // the 33-bit PTS circle: a clock just after a wrap still sees an older display
    CHECK(ptsDelta(1.0, 95443.0) > 0 && ptsDelta(1.0, 95443.0) < 2.0, "pts wrap distance %.3f", ptsDelta(1.0, 95443.0));

    // ---- display set 2 (normal case): region 1 kept as is, a 2-bit region with a doubled top field and an 8-bit region
    Bytes clut2 = {2, 0};
    entryFull(clut2, 1, 0xE0, 235, 128, 128, 0);
    entryFull(clut2, 2, 0xE0, 81, 240, 90, 0);
    entryFull(clut2, 3, 0xE0, 41, 110, 240, 0);       // blue-ish
    Bytes clut3 = {3, 0};
    entryFull(clut3, 0x20, 0x20, 235, 128, 128, 0);
    entryFull(clut3, 0x21, 0x20, 81, 240, 90, 0);
    entryFull(clut3, 0x22, 0x20, 41, 110, 240, 0);
    Bytes top2, top3;
    {   // 2-bit: 1 2 3, a run of 5 x code 2 (run_length_3-10: 3 bits = 2), 2 x code 0 (switch_3 = 01), end of string
        BW w;
        w.put(1, 2); w.put(2, 2); w.put(3, 2);
        w.put(0, 2); w.put(1, 1); w.put(2, 3); w.put(2, 2);
        w.put(0, 2); w.put(0, 1); w.put(0, 1); w.put(1, 2);
        w.put(0, 2); w.put(0, 1); w.put(0, 1); w.put(0, 2);
        w.align();
        top2.push_back(0x10); cat(top2, w.v); top2.push_back(0xF0);
    }
    {   // 8-bit: 0x20, 4 x 0x21 (run_length_3-127), 0x22, end of string
        BW w;
        w.put(0x20, 8);
        w.put(0, 8); w.put(1, 1); w.put(4, 7); w.put(0x21, 8);
        w.put(0x22, 8);
        w.put(0, 8); w.put(0, 1); w.put(0, 7);
        w.align();
        top3.push_back(0x12); cat(top3, w.v); top3.push_back(0xF0);
    }
    Bytes set2;
    cat(set2, seg(0x10, 1, pageComp(4, 2, 0 /* normal */, {{1, 100, 200}, {2, 0, 300}, {3, 50, 300}})));
    cat(set2, seg(0x11, 1, regionComp(2, 0, false, 10, 2, 1 /* 2 bit */, 2, 8, 0, 0)));
    cat(set2, seg(0x11, 1, regionComp(3, 0, true, 6, 1, 3 /* 8 bit */, 3, 9, 0, 0)));
    cat(set2, seg(0x12, 2, clut2));
    cat(set2, seg(0x12, 2, clut3));
    cat(set2, seg(0x13, 2, objectData(8, top2, {})));
    cat(set2, seg(0x13, 2, objectData(9, top3, {})));
    cat(set2, seg(0x80, 1, {}));
    d.feedPes(pes(set2, 1800000).data(), pes(set2, 1800000).size());   // PTS 20.0 s
    auto d2 = d.displayAt(20.5);
    CHECK(d2 && d2->regions.size() == 3, "second display has three regions");
    if (d2 && d2->regions.size() == 3) {
        const DvbSubRegionImage& a = d2->regions[0];
        CHECK(px(a, 3, 3, 255, 255, 255, 255) && px(a, 5, 4, 255, 0, 0, 255), "region 1 kept its pixels across display sets");
        const DvbSubRegionImage& b = d2->regions[1];
        CHECK(b.x == 0 && b.y == 300 && b.w == 10 && b.h == 2, "region 2 geometry");
        for (int row = 0; row < 2; row++) {
            CHECK(px(b, 0, row, 255, 255, 255, 255) && px(b, 1, row, 255, 0, 0, 255) && px(b, 2, row, 0, 0, 255, 255, 12), "2-bit row %d pixels 1 2 3", row);
            bool run = true;
            for (int x = 3; x < 8; x++) run = run && px(b, x, row, 255, 0, 0, 255);
            CHECK(run && px(b, 8, row, 0, 0, 0, 0) && px(b, 9, row, 0, 0, 0, 0), "2-bit row %d: run of 5 red then two transparent", row);
        }
        const DvbSubRegionImage& c = d2->regions[2];
        CHECK(px(c, 0, 0, 255, 255, 255, 255) && px(c, 1, 0, 255, 0, 0, 255) && px(c, 4, 0, 255, 0, 0, 255) && px(c, 5, 0, 0, 0, 255, 255, 12), "8-bit pixels: white, 4 red, blue");
    }
    CHECK(d.displayAt(19.0) == nullptr, "between the first time-out and the second PTS nothing is shown");

    // ---- an empty page (no regions) clears the screen
    Bytes set3;
    cat(set3, seg(0x10, 1, pageComp(4, 3, 0, {})));
    cat(set3, seg(0x80, 1, {}));
    d.feedPes(pes(set3, 2700000).data(), pes(set3, 2700000).size());   // 30.0 s
    CHECK(d.displayAt(30.5) == nullptr, "an empty page shows nothing");
    CHECK(d.displayAt(20.5) != nullptr, "the earlier display is still due at its own time");

    // ---- another subtitle service (composition page 5) on the same PID is not mixed in
    DvbSubDecoder e;
    e.setStream(0x200, 5, 5);
    int cc2 = 0;
    feedTsPackets(e, pes(set1, 900000), 0x200, cc2);
    CHECK(e.displays() == 0, "segments of another composition page are ignored");
    e.setStream(0x201, 1, 2);
    feedTsPackets(e, pes(set1, 900000), 0x200, cc2);
    CHECK(e.displays() == 0, "other PIDs are ignored");

    printf(fails ? "dvbsub tests FAILED\n" : "dvbsub tests passed\n");
    return fails ? 1 : 0;
}
