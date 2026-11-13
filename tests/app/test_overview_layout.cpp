// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The adaptive main area (app/ui/overview_layout.h, docs/LAYOUT-4.0.md): an empty lower row is hidden and comes back when content
// appears, a small plot moves into the right column, a short list becomes a strip, the user's dragged sizes are kept, content that
// flickers does not make the layout jump, and no box ever leaves the area or overlaps another, at every window size from 640 x 400 to
// 3840 x 2160. The panels' content is simulated by models of what the real panels draw (a fixed-size plot, a plot that fills its box, a
// square plot as tall as the row, a table that fills the column, a few text lines). Prints the share of the area left empty, before
// (the fixed 3.9 layout) and after, for each layout group of docs/LAYOUT-4.0.md.
#include "../../app/ui/overview_layout.h"
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace ovl;

// what a panel draws in a box of the given inner size
struct Model {
    enum Kind { Empty, Fixed, FillW, Fill, Square, Prop, Lines, Table } kind = Empty;
    float a = 0, b = 0;   // Fixed: w h; FillW: h; Square: caption; Prop: share of the height; Lines: count; Table: -
    Extent in(float iw, float ih, float lineH) const {
        switch (kind) {
        case Empty: return {0, 0};
        case Fixed: return {a, b};
        case FillW: return {iw, b};
        case Fill: return {iw, ih};
        case Square: { const float s = std::max(50.f, ih - a); return {s, s + a}; }
        case Prop: return {iw, std::floor(ih * a)};
        case Lines: return {std::min(iw, 220.f), a * lineH};
        case Table: return {iw, ih};
        }
        return {};
    }
};

struct Sim {
    In in;
    State st;
    Out o;
    Model right, bottom;
    void frame(double t) {
        in.now = t;
        o = layout(in, st);
        // measure what the panels drew in this frame's boxes: the input of the next frame
        const Box rb = o.probeRight ? o.probeRightBox : o.right;
        in.right = rb.shown() ? right.in(rb.w - in.padX, rb.h - in.padY, in.lineH) : Extent{};
        const Box bb = o.bottom.shown() ? o.bottom : o.probeBottom ? o.probeBottomBox : Box{};
        in.bottom = bb.shown() ? bottom.in(bb.w - in.padX, bb.h - in.padY, in.lineH) : Extent{};
    }
    void run(double from, double to, double dt = 1.0 / 30) { for (double t = from; t < to; t += dt) frame(t); }
};

static bool inside(const Box& b, const In& in) {
    return !b.shown() || (b.x >= -0.01f && b.y >= -0.01f && b.x + b.w <= in.W + 0.01f && b.y + b.h <= in.H + 0.01f);
}
static bool overlap(const Box& p, const Box& q) {
    if (!p.shown() || !q.shown()) return false;
    return p.x < q.x + q.w - 0.01f && q.x < p.x + p.w - 0.01f && p.y < q.y + q.h - 0.01f && q.y < p.y + p.h - 0.01f;
}
static void checkBoxes(const Out& o, const In& in, const char* what) {
    const Box* bx[4] = {&o.main, &o.right, &o.bottom, &o.strip};
    for (int i = 0; i < 4; i++) {
        CHECK(inside(*bx[i], in), "%s: box %d outside %gx%g (%g %g %g %g)", what, i, in.W, in.H, bx[i]->x, bx[i]->y, bx[i]->w, bx[i]->h);
        for (int j = i + 1; j < 4; j++) CHECK(!overlap(*bx[i], *bx[j]), "%s: boxes %d and %d overlap at %gx%g", what, i, j, in.W, in.H);
    }
    CHECK(o.main.w >= std::min(in.minMainW, in.W) - 0.5f, "%s: spectrum narrower than %g (%g) at %gx%g", what, in.minMainW, o.main.w, in.W, in.H);
}

static Sim make(float W, float H, Model r, Model b) {
    Sim s;
    s.in.W = W; s.in.H = H;
    s.in.rightAutoW = std::floor(std::max(236.f, std::min(330.f, W * 0.30f)));
    s.in.bottomAutoH = std::max(144.f, std::min(215.f, H - 4 - 240.f));
    s.right = r; s.bottom = b;
    return s;
}

static Model M(Model::Kind k, float a = 0, float b = 0) { Model m; m.kind = k; m.a = a; m.b = b; return m; }

int main() {
    // ---- an empty lower row (CODAR, most HF and data modes): hidden after the mode opens, the spectrum takes the height
    {
        Sim s = make(1300, 820, M(Model::Lines, 1), M(Model::Empty));
        s.run(0, 2);
        CHECK(s.o.bottomAt == Bottom::Hidden && !s.o.bottom.shown(), "empty lower row hidden");
        CHECK(s.o.probeBottom, "an empty lower row is still watched");
        CHECK(s.o.rightAt == Right::Strip && s.o.strip.shown() && !s.o.right.shown(), "a one-line list becomes a strip");
        CHECK(s.o.main.w == s.in.W && s.o.main.y + s.o.main.h == s.in.H, "the spectrum takes the full width and the rest of the height");
        CHECK(s.o.strip.h <= s.in.lineH * 4.4f + s.in.padY, "the strip is short (%g)", s.o.strip.h);
        // content arrives (decoding started): the row comes back, not at once, then glides; no switching back and forth
        s.bottom = M(Model::Fill);
        s.frame(2.0);
        CHECK(s.o.bottomAt == Bottom::Hidden, "not on the first frame with content");
        s.run(2.0 + 1.0 / 30, 2.2);
        CHECK(s.o.bottomAt == Bottom::Hidden, "still waiting before the hold time");
        s.run(2.2, 2.4);
        CHECK(s.o.bottomAt == Bottom::Row, "back after the hold time");
        s.run(2.4, 3.0);
        CHECK(!s.o.animating && std::fabs(s.o.bottom.h - fullRowH(s.in)) < 0.5f, "row glided to its full height (%g)", s.o.bottom.h);
        // the list turns into a table that fills its column (stations found): back to a column
        s.right = M(Model::Table);
        s.run(3.0, 5.0);
        CHECK(s.o.rightAt == Right::Column && s.o.right.shown() && !s.o.strip.shown(), "a growing list gets its column back");
        checkBoxes(s.o, s.in, "codar");
    }
    // ---- flickering content: on and off every frame for 10 s; the layout changes at most once a second
    {
        Sim s = make(1600, 900, M(Model::Table), M(Model::Empty));
        s.run(0, 1);
        int switches = 0;
        Bottom prev = s.o.bottomAt;
        for (int f = 0; f < 300; f++) {
            s.bottom = (f & 1) ? M(Model::Fill) : M(Model::Empty);
            s.frame(1 + f / 30.0);
            if (s.o.bottomAt != prev) { switches++; prev = s.o.bottomAt; }
        }
        CHECK(switches == 0, "content flickering every frame never moves the row (%d switches)", switches);
        // on for 0.5 s, off for 0.5 s: at most one change per second
        switches = 0;
        for (int f = 0; f < 600; f++) {
            s.bottom = ((f / 15) & 1) ? M(Model::Fill) : M(Model::Empty);
            s.frame(11 + f / 30.0);
            if (s.o.bottomAt != prev) { switches++; prev = s.o.bottomAt; }
        }
        CHECK(switches <= 20, "slow flicker: %d switches in 20 s", switches);
    }
    // ---- NOAA DSB: a square 'Symbols' plot as tall as the row and a two-line list: the plot goes into a narrow column under the list,
    //      the spectrum gets the full height and all but the plot's width
    {
        Sim s = make(1500, 860, M(Model::Lines, 2), M(Model::Square, 17));
        s.run(0, 3);
        CHECK(s.o.bottomAt == Bottom::Column, "small plot moves into the column");
        CHECK(s.o.rightAt == Right::Column && s.o.right.shown(), "the list stays above it");
        CHECK(s.o.main.h == s.in.H && s.o.main.y == 0, "spectrum full height (%g of %g)", s.o.main.h, s.in.H);
        CHECK(s.o.bottom.x >= s.o.right.x && s.o.bottom.y >= s.o.right.y + s.o.right.h, "plot under the list");
        CHECK(s.o.right.w < fullColW(s.in), "the column only as wide as the plot (%g < %g)", s.o.right.w, fullColW(s.in));
        const Extent e = s.bottom.in(s.o.bottom.w - s.in.padX, s.o.bottom.h - s.in.padY, s.in.lineH);
        CHECK(e.w <= s.o.bottom.w - s.in.padX + 1 && e.h <= s.o.bottom.h - s.in.padY + 1, "the plot fits its box (%g x %g in %g x %g)", e.w, e.h, s.o.bottom.w, s.o.bottom.h);
        checkBoxes(s.o, s.in, "noaa dsb");
        // stays there (no ratchet, no flip) for a long run
        Out before = s.o;
        s.run(3, 20);
        CHECK(s.o.bottomAt == Bottom::Column && std::fabs(s.o.bottom.h - before.bottom.h) < 0.5f && std::fabs(s.o.right.w - before.right.w) < 0.5f, "stable over 17 s");
        // the list becomes a table (frames listed): the column widens to its full width, the plot stays under it
        s.right = M(Model::Table);
        s.run(20, 23);
        CHECK(s.o.bottomAt == Bottom::Column && std::fabs(s.o.right.w - fullColW(s.in)) < 0.5f, "a full list widens the column (%g)", s.o.right.w);
        checkBoxes(s.o, s.in, "noaa dsb table");
    }
    // ---- a fixed small box (140 x 160) and a full list
    {
        Sim s = make(1400, 800, M(Model::Table), M(Model::Fixed, 140, 160));
        s.run(0, 3);
        CHECK(s.o.bottomAt == Bottom::Column && s.o.bottom.h <= 160 + s.in.padY + 0.5f, "fixed plot in the column (%g)", s.o.bottom.h);
        CHECK(s.o.right.h >= s.in.lineH * 4, "the list keeps room");
        // the plot grows wider than the column (a second plot): back to a row
        s.bottom = M(Model::Fixed, 600, 160);
        s.run(3, 6);
        CHECK(s.o.bottomAt == Bottom::Row, "a wide plot goes back to the row");
        CHECK(s.o.bottom.h <= 160 + s.in.padY + 0.5f, "and the row is only as tall as the plot (%g)", s.o.bottom.h);
        checkBoxes(s.o, s.in, "fixed");
    }
    // ---- DVB-T: four plots across the row, as tall as the row: the 3.9 layout
    {
        Sim s = make(1600, 900, M(Model::Table), M(Model::Fill));
        s.run(0, 3);
        const Out f = fixedLayout(s.in);
        CHECK(s.o.bottomAt == Bottom::Row && std::fabs(s.o.bottom.h - f.bottom.h) < 0.5f && std::fabs(s.o.right.w - f.right.w) < 0.5f, "full content: the fixed layout");
        CHECK(std::fabs(s.o.main.w - f.main.w) < 0.5f && std::fabs(s.o.main.h - f.main.h) < 0.5f, "same spectrum box");
    }
    // ---- a short full-width row (a 80 px history line): the row shrinks to it
    {
        Sim s = make(1600, 900, M(Model::Table), M(Model::FillW, 0, 80));
        s.run(0, 3);
        CHECK(s.o.bottomAt == Bottom::Row && std::fabs(s.o.bottom.h - std::max(s.in.minBottomH, 80 + s.in.padY)) < 0.5f, "row shrinks to its content (%g)", s.o.bottom.h);
    }
    // ---- content that sizes itself to 60 % of its box: shrinks once, does not ratchet down
    {
        Sim s = make(1600, 900, M(Model::Table), M(Model::Prop, 0.6f));
        s.run(0, 2);
        const float h1 = s.o.bottom.h;
        s.run(2, 20);
        CHECK(s.o.bottomAt == Bottom::Row && std::fabs(s.o.bottom.h - h1) < 0.5f && h1 > s.in.minBottomH, "no ratchet (%g then %g)", h1, s.o.bottom.h);
    }
    // ---- the user's sizes: a dragged row stays a row at that height; empty content still hides it; a dragged column stays a column
    {
        Sim s = make(1500, 860, M(Model::Lines, 2), M(Model::Square, 17));
        s.in.userBottomH = 260; s.in.userRightW = 340;
        s.run(0, 3);
        CHECK(s.o.bottomAt == Bottom::Row && std::fabs(s.o.bottom.h - 260) < 0.5f, "user row height kept (%g)", s.o.bottom.h);
        CHECK(s.o.rightAt == Right::Column && std::fabs(s.o.right.w - 340) < 0.5f, "user column width kept (%g)", s.o.right.w);
        s.bottom = M(Model::Empty);
        s.run(3, 6);
        CHECK(s.o.bottomAt == Bottom::Hidden, "an empty row hides even at a dragged height");
        s.bottom = M(Model::Square, 17);
        s.run(6, 9);
        CHECK(s.o.bottomAt == Bottom::Row && std::fabs(s.o.bottom.h - 260) < 0.5f, "and comes back at the dragged height");
    }
    // ---- adaptive off: exactly the fixed layout, at once
    {
        Sim s = make(1500, 860, M(Model::Lines, 1), M(Model::Empty));
        s.run(0, 2);
        s.in.adaptive = false;
        s.frame(2);
        const Out f = fixedLayout(s.in);
        CHECK(s.o.bottom.h == f.bottom.h && s.o.right.w == f.right.w && s.o.main.w == f.main.w && s.o.main.h == f.main.h, "adaptive off = fixed");
    }
    // ---- no lower row in this view (ATSC 3.0, TX): hidden at once, never probed
    {
        Sim s = make(1500, 860, M(Model::Table), M(Model::Fill));
        s.run(0, 2);
        s.in.bottomAllowed = false;
        s.frame(2);
        CHECK(!s.o.probeBottom && s.o.bottomAt == Bottom::Hidden, "not allowed: hidden");
        s.run(2 + 1.0 / 30, 3);
        CHECK(!s.o.bottom.shown() && s.o.main.h == s.in.H, "and the spectrum takes the height");
    }
    // ---- every window size and every content pair: boxes inside the area, apart, the spectrum at least its minimum, no change
    //      once settled (the layout is a fixed point of its own measurements)
    {
        const Model rights[] = {M(Model::Empty), M(Model::Lines, 1), M(Model::Lines, 3), M(Model::Lines, 8), M(Model::Table)};
        const Model bottoms[] = {M(Model::Empty), M(Model::Fixed, 140, 140), M(Model::Fixed, 900, 120), M(Model::FillW, 0, 60), M(Model::Fill),
                                 M(Model::Square, 17), M(Model::Prop, 0.5f), M(Model::Lines, 1)};
        int cases = 0, unstable = 0;
        for (float W = 640; W <= 3840; W += 160)
            for (float H = 400; H <= 2160; H += 160)
                for (const Model& r : rights)
                    for (const Model& b : bottoms) {
                        Sim s = make(W, H, r, b);
                        s.in.minMainW = std::min(260.f, W * 0.4f);
                        s.run(0, 4);
                        checkBoxes(s.o, s.in, "sweep");
                        const Out o1 = s.o;
                        s.run(4, 6);
                        if (o1.bottomAt != s.o.bottomAt || o1.rightAt != s.o.rightAt || std::fabs(o1.main.w - s.o.main.w) > 0.5f || std::fabs(o1.main.h - s.o.main.h) > 0.5f) unstable++;
                        cases++;
                    }
        CHECK(unstable == 0, "%d of %d cases still moving after 4 s", unstable, cases);
        std::printf("sweep: %d window sizes x content cases, %d unstable\n", cases, unstable);
    }
    // ---- the share of the area left empty, per layout group (docs/LAYOUT-4.0.md), in a 1600 x 900 main area
    {
        struct G { const char* name; Model r, b; };
        const G groups[] = {
            {"1 no lower content, own list (CODAR, ACARS, AIS, FT8 ...)", M(Model::Table), M(Model::Empty)},
            {"2 no lower content, no list (CIS HF, cell, KAAL ...)", M(Model::Empty), M(Model::Empty)},
            {"3 square plot, own list (NOAA DSB, wxsat, PSK sats)", M(Model::Table), M(Model::Square, 17)},
            {"4 square plot, short list (the same, stopped)", M(Model::Lines, 2), M(Model::Square, 17)},
            {"5 a few text lines below, own list (GOES GRB ...)", M(Model::Table), M(Model::Lines, 4)},
            {"6 full-width plots, own list (FT8 audio, DMR ...)", M(Model::Table), M(Model::Fill)},
            {"7 short full-width row (a level history)", M(Model::Table), M(Model::FillW, 0, 90)},
            {"8 no lower content, one status line (stopped)", M(Model::Lines, 1), M(Model::Empty)},
            {"9 the TV modes (DVB-T, DAB, FM, ATSC)", M(Model::Table), M(Model::Fill)},
        };
        std::printf("%-58s %10s %10s %10s %10s\n", "group (1600 x 900)", "empty 3.9", "empty 4.0", "spec 3.9", "spec 4.0");
        for (const G& g : groups) {
            Sim s = make(1600, 900, g.r, g.b);
            s.run(0, 4);
            const Out f = fixedLayout(s.in);
            auto nat = [&](const Model& m, const Box& bx) { return bx.shown() ? m.in(bx.w - s.in.padX, bx.h - s.in.padY, s.in.lineH) : Extent{0, 0}; };
            const Extent fr = nat(g.r, f.right), fb = nat(g.b, f.bottom);
            const Box rb = s.o.right.shown() ? s.o.right : s.o.strip;
            const Extent ar = nat(g.r, s.o.probeRight ? s.o.probeRightBox : rb), ab = nat(g.b, s.o.bottom);
            const float u0 = unusedShare(s.in, f, fr.h, fb.w, fb.h), u1 = unusedShare(s.in, s.o, std::min(ar.h, rb.h), ab.w, ab.h);
            std::printf("%-58s %9.0f%% %9.0f%% %9.0f%% %9.0f%%\n", g.name, u0 * 100, u1 * 100, mainShare(s.in, f) * 100, mainShare(s.in, s.o) * 100);
            CHECK(u1 <= u0 + 0.005f, "%s: more empty space than before", g.name);
            CHECK(mainShare(s.in, s.o) >= mainShare(s.in, f) - 0.005f, "%s: a smaller spectrum than before", g.name);
        }
    }
    std::printf(failures ? "overview layout: %d failures\n" : "overview layout: all passed\n", failures);
    return failures ? 1 : 0;
}
