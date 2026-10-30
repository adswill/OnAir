// The layout of the names on the spectrum (app/gfx/autolabel_logic.h): the store, merging of the same station, stacking in rows without overlap,
// thinning when the view is wide, the detail when there is room, the edges of the plot, and the hit test.
#include "../../app/gfx/autolabel_logic.h"
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace autolabel;

static Label mk(double mhz, const char* text, int src, float prio = 0, const char* detail = "") {
    Label l; l.mhz = mhz; l.text = text; l.src = src; l.prio = prio; l.detail = detail; return l;
}
// 6 px a character, 8 px of padding; detail adds its length
static float width(const Label& l, bool d) { return 6.f * (float)(l.text.size() + (d && !l.detail.empty() ? 2 + l.detail.size() : 0)) + 8.f; }

static View view(double lo, double hi, int rows = 3) {
    View v; v.loMhz = lo; v.hiMhz = hi; v.px0 = 0; v.px1 = 1000; v.maxRows = rows; v.gap = 4; return v;
}
// Fixed-width names make the space available to bandwidth lines explicit: 0..100 MHz maps to 0..1000 pixels.
static Result bandLayout(const Store& s, const View& v) {
    Result r = layout(s, v, [](const Label&, bool) { return 100.f; });
    layoutBandwidth(s, v, r);
    return r;
}
static bool overlap(const Result& r) {
    for (size_t i = 0; i < r.placed.size(); i++)
        for (size_t j = i + 1; j < r.placed.size(); j++)
            if (r.placed[i].row == r.placed[j].row && r.placed[i].x0 < r.placed[j].x1 && r.placed[j].x0 < r.placed[i].x1) return true;
    return false;
}

int main() {
    // the store: sorted, range query with the source mask
    {
        Store s;
        s.add(mk(101.0, "B", Fm)); s.add(mk(99.0, "A", Dab)); s.add(mk(100.0, "C", FreqDb)); s.add(mk(120.0, "D", Fm));
        s.finish();
        CHECK(s.size() == 4 && s.at(0).text == "A" && s.at(3).text == "D", "sorted by frequency");
        CHECK(s.inRange(99.5, 110, kAllSources).size() == 2, "range");
        CHECK(s.inRange(0, 200, 1u << Fm).size() == 2, "mask keeps only FM");
        CHECK(s.inRange(0, 200, 0).empty(), "no source on, no label");
        CHECK(s.inRange(150, 160, kAllSources).empty(), "nothing in an empty range");
        CHECK(s.at(0).prio == srcPriority(Dab), "the source's priority is added");
    }
    // plain stacking: three names on the same spot go to three rows, the fourth is dropped
    {
        Store s;
        for (int i = 0; i < 4; i++) s.add(mk(100.0, "STATION", Fm, (float)i));
        s.finish();
        View v = view(90, 110);
        v.mask = kAllSources;
        Result r = layout(s, v, width);
        CHECK(r.candidates == 1, "four labels of one station are one (same frequency): %zu", r.candidates);
        CHECK(r.placed.size() == 1 && r.placed[0].row == 0, "one placed on row 0");
        Store t;
        t.add(mk(100.000, "ALPHA", Band)); t.add(mk(100.002, "BRAVO", Band)); t.add(mk(100.004, "CHARLIE", Band)); t.add(mk(100.006, "DELTA", Band));
        t.finish();
        r = layout(t, v, width);   // band-plan names never merge
        CHECK(r.placed.size() == 3 && r.dropped == 1, "three rows, one dropped: %zu placed %zu dropped", r.placed.size(), r.dropped);
        CHECK(!overlap(r), "no overlap");
        int rows[3] = {0, 0, 0};
        for (const Placed& p : r.placed) rows[p.row]++;
        CHECK(rows[0] == 1 && rows[1] == 1 && rows[2] == 1, "one name in each row");
    }
    // the strongest source wins the first row and the last place
    {
        Store s;
        s.add(mk(100.0, "plain", Band)); s.add(mk(100.0, "decoded", Fm)); s.add(mk(100.0, "memory", Memories));
        s.finish();
        Result r = layout(s, view(90, 110, 1), width);
        CHECK(r.placed.size() == 1 && s.at(r.placed[0].idx).text == "decoded", "the decoded name stays when only one fits");
    }
    // the same station from two sources: the higher priority remains; with the stronger source off the other one shows
    {
        Store s;
        s.add(mk(93.900, "Noor FM", FreqDb)); s.add(mk(93.9004, "NOOR", Fm));
        s.finish();
        Result r = layout(s, view(90, 100), width);
        CHECK(r.placed.size() == 1 && s.at(r.placed[0].idx).text == "NOOR", "merged to the RDS name");
        View v = view(90, 100); v.mask = 1u << FreqDb;
        r = layout(s, v, width);
        CHECK(r.placed.size() == 1 && s.at(r.placed[0].idx).text == "Noor FM", "the database name when FM is off");
        Store t;
        t.add(mk(93.9, "Noor", Fm)); t.add(mk(94.1, "Other", Fm));
        t.finish();
        CHECK(layout(t, view(90, 100), width).placed.size() == 2, "stations 200 kHz apart stay two");
    }
    // different protocols sharing a frequency remain separate labels on separate rows, even when a database entry or memory has higher priority
    for (int src : {FreqDb, Memories}) {
        Store s;
        Label a = mk(869.525, "Aidon e-meters", src, 0.5f); a.mode = "ism"; a.bwMhz = 0.025; s.add(a);
        Label b = mk(869.525, "LoRaWAN EU868 RX2", FreqDb); b.mode = "lorawan"; b.bwMhz = 0.125; s.add(b);
        Label c = mk(869.525, "Meshtastic EU868 default (LongFast)", FreqDb); c.mode = "meshtastic"; c.bwMhz = 0.250; s.add(c); s.finish();
        for (View v : {view(869, 870), view(869.5, 869.55)}) {
            Result r = layout(s, v, width);
            CHECK(r.candidates == 3 && r.placed.size() == 3 && !overlap(r), "all three protocols retain their labels at both zoom levels");
            int rows[3] = {}; for (const Placed& p : r.placed) rows[p.row]++;
            CHECK(rows[0] == 1 && rows[1] == 1 && rows[2] == 1, "one co-channel label per row");
        }
    }
    // distinct database and memory names in one mode remain separate; duplicate entries still merge to the higher priority
    for (int aSrc : {FreqDb, Memories}) for (int bSrc : {FreqDb, Memories}) {
        Store s;
        Label a = mk(100, "CHANNEL A", aSrc); a.mode = "ism"; a.bwMhz = 0.125; s.add(a);
        Label b = a; b.text = "CHANNEL B"; b.src = bSrc; b.bwMhz = 0.025; s.add(b);
        Label c = a; c.prio = 0.5f; c.bwMhz = 0.250; s.add(c); s.finish();
        Result r = layout(s, view(99, 101), width);
        CHECK(r.candidates == 2 && r.placed.size() == 2 && !overlap(r), "keep distinct names, merge duplicate identities (%d, %d)", aSrc, bSrc);
        for (const Placed& p : r.placed) if (s.at(p.idx).text == "CHANNEL A") CHECK(s.at(p.idx).bwMhz == 0.250, "the higher priority bandwidth wins for a duplicate entry");
        if (aSrc != bSrc) {
            r = layout(s, view(99, 101, 1), width);
            CHECK(r.candidates == 2 && r.placed.size() == 1 && r.dropped == 1 && s.at(r.placed[0].idx).src == Memories, "the memory takes priority when only one name fits");
        }
    }
    // a memory with the same name overrides the database, including its bandwidth, in either insertion order
    for (bool memoryFirst : {false, true}) {
        Store s;
        Label a = mk(100, "CHANNEL A", FreqDb); a.mode = "ism"; a.bwMhz = 0.125;
        Label b = a; b.src = Memories; b.bwMhz = 0.250;
        s.add(memoryFirst ? b : a); s.add(memoryFirst ? a : b); s.finish();
        Result r = layout(s, view(99, 101), width);
        CHECK(r.candidates == 1 && r.placed.size() == 1 && s.at(r.placed[0].idx).src == Memories && s.at(r.placed[0].idx).bwMhz == 0.250, "the memory overrides the matching database entry");
    }
    // zoom-aware: a wide view thins the names out, a narrow one shows them all
    {
        Store s;
        for (int i = 0; i < 100; i++) { char n[16]; std::snprintf(n, sizeof n, "ST%03d", i); s.add(mk(88.0 + i * 0.2, n, Fm)); }
        s.finish();
        Result wide = layout(s, view(88, 108), width);
        CHECK(!overlap(wide), "wide view: no overlap");
        CHECK(wide.dropped > 0 && wide.placed.size() < 100, "wide view: some names held back (%zu placed)", wide.placed.size());
        Result narrow = layout(s, view(93, 95), width);
        CHECK(narrow.dropped == 0 && narrow.placed.size() >= 9, "narrow view: all %zu names in view are shown", narrow.placed.size());
        CHECK(!overlap(narrow), "narrow view: no overlap");
        // zooming in never loses a name that was shown: the 3 rows of the wide view are a subset of what the narrow view shows when it covers them
        Result mid = layout(s, view(96, 100), width);
        bool all = true;
        for (const Placed& p : wide.placed) {
            const double f = s.at(p.idx).mhz;
            if (f < 96 || f > 100) continue;
            bool found = false;
            for (const Placed& q : mid.placed) found = found || q.idx == p.idx;
            all = all && found;
        }
        CHECK(all, "a name shown wide is still shown when zoomed in around it");
    }
    // the detail is added when there is room for all, and not otherwise
    {
        Store s;
        s.add(mk(100.0, "Mux A", Dvb, 0, "One, Two, Three")); s.add(mk(106.0, "Mux B", Dvb, 0, "Four"));
        s.finish();
        Result r = layout(s, view(90, 110), width);
        CHECK(r.detailed && r.dropped == 0, "room for the detail");
        Store t;
        for (int i = 0; i < 40; i++) { char n[16]; std::snprintf(n, sizeof n, "M%02d", i); t.add(mk(90.0 + i * 0.5, n, Dvb, 0, "a long list of services")); }
        t.finish();
        r = layout(t, view(90, 110), width);
        CHECK(!r.detailed, "no detail when crowded");
        CHECK(!overlap(r), "no overlap");
    }
    // the edges: a name with its frequency in view is clipped by the plot (the box centred on the frequency), never moved
    {
        Store s;
        s.add(mk(90.0, "LEFTEDGE", Fm)); s.add(mk(110.0, "RIGHTEDGE", Fm)); s.add(mk(111.0, "OUTSIDE", Fm));
        s.finish();
        Result r = layout(s, view(90, 110), width);
        CHECK(r.placed.size() == 2, "the one outside the view is not placed");
        for (const Placed& p : r.placed) CHECK(std::fabs((p.x0 + p.x1) * 0.5f - p.cx) < 0.01f, "centred on its frequency: %.0f..%.0f", p.x0, p.x1);
    }
    // the edges, issue #230: a name whose frequency is in view is clipped at the edge, not dragged along
    {
        Store s;
        s.add(mk(90.0, "LEFTEDGE", Fm));
        s.finish();
        Result r = layout(s, view(90, 110), width);
        CHECK(r.placed.size() == 1 && r.placed[0].centreIn && std::fabs(r.placed[0].cx - 0.f) < 0.01f, "pointer at the true frequency");
        CHECK(r.placed[0].x0 < 0, "box centred on the frequency, clipped by the plot: %.1f", r.placed[0].x0);
        r = layout(s, view(90.5, 110), width);
        CHECK(r.placed.empty(), "frequency left the view, no band: gone");
    }
    // the same x in two panes: the mapping uses the whole axis extent, the label area (px) only limits the boxes
    {
        Store s; s.add(mk(100.0, "MID", Fm)); s.add(mk(95.0, "Q", Fm)); s.finish();
        View a = view(90, 110), b = view(90, 110);
        a.ax0 = 0; a.ax1 = 1000;
        b.ax0 = 0; b.ax1 = 1000; b.px1 = 904;   // the waterfall keeps room for its button on the right
        Result ra = layout(s, a, width), rb = layout(s, b, width);
        CHECK(ra.placed.size() == 2 && rb.placed.size() == 2, "both placed");
        for (size_t i = 0; i < 2; i++) CHECK(std::fabs(ra.placed[i].cx - rb.placed[i].cx) < 0.001f, "same x in both panes: %.2f %.2f", ra.placed[i].cx, rb.placed[i].cx);
        CHECK(std::fabs(ra.placed[1].cx - 500.f) < 0.01f && std::fabs(ra.placed[0].cx - 250.f) < 0.01f, "linear in the span: %.2f", ra.placed[0].cx);
        // an offset axis (left margin 70 px, retina scale 2)
        View c = view(90, 110); c.ax0 = 140; c.ax1 = 2140; c.px0 = 140; c.px1 = 2140;
        Result rc = layout(s, c, width);
        CHECK(std::fabs(rc.placed[1].cx - 1140.f) < 0.01f && std::fabs(xOf(c, 90) - 140.f) < 0.001f && std::fabs(xOf(c, 110) - 2140.f) < 0.001f, "offset and scale");
        // a span shifted by a pan: the same frequency moves by the shift, linearly
        View d = view(91, 111);
        CHECK(std::fabs(xOf(d, 100.0) - 450.f) < 0.01f, "pan");
    }
    // a name with a nominal band stays while the band overlaps the view; its pointer stays at the centre (not drawn when out of view)
    {
        Store s;
        Label l = mk(100.0, "DVB-T2 CH 26", Band); l.bwMhz = 8; s.add(l);
        Label n = mk(100.0, "NOBAND", Fm); s.add(n);
        s.finish();
        Result r = layout(s, view(103, 110), width);   // band 96..104 overlaps 103..110, the centre is left of the view
        CHECK(r.placed.size() == 1 && !r.placed[0].centreIn && r.placed[0].side == -1 && r.placed[0].x0 >= 0, "stub on the left edge, box inside");
        CHECK(r.placed[0].bar0 >= 0 && r.placed[0].bar1 > r.placed[0].bar0 && r.placed[0].bar1 < 200.f, "bar clipped to the view: %.0f..%.0f", r.placed[0].bar0, r.placed[0].bar1);
        r = layout(s, view(104.5, 110), width);
        CHECK(r.placed.empty(), "band left the view: gone");
        r = layout(s, view(90, 110), width);
        CHECK(r.placed.size() == 2 && r.placed[0].centreIn && r.placed[1].centreIn, "both in view");
        int bars = 0; for (const Placed& p : r.placed) bars += p.bar1 > p.bar0;
        CHECK(bars == 1, "only the one with a known band has a bar");
        r = layout(s, view(110, 120), width);
        CHECK(r.placed.empty(), "wholly right of the view");
        Store t; Label u = mk(130.0, "RIGHT", Band); u.bwMhz = 8; t.add(u); t.finish();
        r = layout(t, view(100, 127), width);   // 126..134 overlaps, centre right of the view
        CHECK(r.placed.size() == 1 && r.placed[0].side == 1 && r.placed[0].x1 <= 1000, "stub on the right, box inside");
    }
    // overlapping wide bands never lose a bar or consume space beyond their own name
    {
        Store s, withoutBands;
        for (int i = 0; i < 3; i++) {
            Label l = mk(100.0 + i * 0.5, ("B" + std::to_string(i)).c_str(), Band);
            withoutBands.add(l); l.bwMhz = 8; s.add(l);
        }
        s.finish(); withoutBands.finish();
        Result r = layout(s, view(90, 110), width), plain = layout(withoutBands, view(90, 110), width);
        CHECK(r.placed.size() == 3 && plain.placed.size() == 3, "all three wide bands fit");
        for (size_t i = 0; i < r.placed.size(); i++) {
            const Placed& p = r.placed[i];
            CHECK(p.bar1 > p.bar0 && p.bar0 >= p.x0 && p.bar1 <= p.x1, "every bandwidth line stays under its own name");
            CHECK(p.fadeLeft && p.fadeRight, "wide bands fade at both edges of the name");
            CHECK(i < plain.placed.size() && p.row == plain.placed[i].row && p.x0 == plain.placed[i].x0 && p.x1 == plain.placed[i].x1, "bandwidth does not change name placement");
        }
    }
    // a narrow band has true endpoints, even below the old three-pixel cutoff; both panes use the same frequency scale
    {
        Store s; Label l = mk(109.0, "NARROW", FreqDb); l.bwMhz = 0.04; s.add(l); s.finish();
        View a = view(90, 110), b = a;
        a.ax0 = b.ax0 = 0; a.ax1 = b.ax1 = 1000; b.px1 = 904;
        Result ra = layout(s, a, width), rb = layout(s, b, width);
        CHECK(ra.placed.size() == 1 && rb.placed.size() == 1, "both panes keep the narrow band near the right edge");
        layoutBandwidth(s, a, ra); layoutBandwidth(s, b, rb);
        const Placed& p = ra.placed[0]; const Placed& q = rb.placed[0];
        CHECK(std::fabs(p.bar0 - 949.f) < 0.01f && std::fabs(p.bar1 - 951.f) < 0.01f, "two-pixel bandwidth at its true frequency");
        CHECK(!p.fadeLeft && !p.fadeRight, "true endpoints do not fade");
        CHECK(p.bar0 == q.bar0 && p.bar1 == q.bar1, "same bandwidth endpoints despite the waterfall button margin");
    }
    // broad allocations keep their range for visibility, but do not duplicate the allocation strip with a bandwidth line
    {
        Store s; Label l = mk(100.0, "ALLOCATION", Band); l.bwMhz = 8; l.allocation = true; s.add(l); s.finish();
        for (View v : {view(90, 110), view(103, 110)}) {
            Result r = layout(s, v, width);
            CHECK(r.placed.size() == 1, "allocation name remains while its range overlaps the view");
            CHECK(r.placed[0].bar1 <= r.placed[0].bar0, "no bandwidth line for an allocation");
        }
    }
    // a sparse channel can use the empty space beyond its name, including more than half of an unneeded gap
    {
        Store s;
        Label a = mk(30, "WIDE", Band); a.bwMhz = 48; s.add(a);
        Label b = mk(60, "NARROW", Band); b.bwMhz = 5; s.add(b); s.finish();
        Result r = bandLayout(s, view(0, 100));
        const Placed& p = r.placed[0];
        CHECK(p.x0 == 250 && p.x1 == 350 && p.bar0 == 60 && p.bar1 == 540, "wide band gets the unused gap, with its name unchanged");
        CHECK(!p.fadeLeft && !p.fadeRight, "a full band beyond the text has true endpoints");
        CHECK(r.placed[1].bar0 == 575 && r.placed[1].bar1 == 625, "the neighbour's narrow band stays unchanged");
    }
    // both wider bands are shown in full when their requests fit in the gap
    {
        Store s;
        Label a = mk(30, "LEFT", Band); a.bwMhz = 20; s.add(a);
        Label b = mk(60, "RIGHT", Band); b.bwMhz = 20; s.add(b); s.finish();
        Result r = bandLayout(s, view(0, 100));
        CHECK(r.placed[0].bar1 == 400 && r.placed[1].bar0 == 500, "both full bands fit in their shared gap");
        for (const Placed& p : r.placed) CHECK(!p.fadeLeft && !p.fadeRight, "no fading when both fit");
    }
    // adjacent marine channels meet at true endpoints despite different name widths
    {
        Store s;
        Label a = mk(156.475, "Marine VHF channel 69", FreqDb); a.bwMhz = 0.025; s.add(a);
        Label b = mk(156.500, "Marine VHF channel 10", FreqDb); b.bwMhz = 0.025; s.add(b);
        Label c = mk(156.525, "DSC distress calling (VHF channel 70)", FreqDb); c.bwMhz = 0.025; s.add(c); s.finish();
        View v = view(156.45, 156.55);
        Result r = layout(s, v, width); layoutBandwidth(s, v, r);
        CHECK(r.placed.size() == 3, "all three marine channels fit");
        for (const Placed& p : r.placed) CHECK(p.row == 0 && !p.fadeLeft && !p.fadeRight, "touching bands have ticks rather than fades");
        CHECK(std::fabs(r.placed[0].bar1 - r.placed[1].bar0) < 0.01f && std::fabs(r.placed[1].bar1 - r.placed[2].bar0) < 0.01f, "adjacent channels share their real borders");
    }
    // a label wider than its own channel must not hide the neighbouring channel's true endpoint
    {
        Store s;
        Label a = mk(156.825, "Long-range AIS 2 (channel 76)", FreqDb); a.bwMhz = 0.025; s.add(a);
        Label b = mk(156.850, "Marine VHF channel 17", FreqDb); b.bwMhz = 0.025; s.add(b); s.finish();
        View v = view(156.8055, 156.9255); v.px1 = 824;
        Result r = layout(s, v, width); layoutBandwidth(s, v, r);
        CHECK(r.placed.size() == 2 && r.placed[0].row == r.placed[1].row, "both channel names fit on one row");
        CHECK(r.placed[0].x1 > r.placed[0].bar1, "AIS text extends past its own bandwidth");
        CHECK(!r.placed[0].fadeRight && !r.placed[1].fadeLeft && std::fabs(r.placed[0].bar1 - r.placed[1].bar0) < 0.01f, "text overhang does not force a fade at the shared border");
    }
    // narrow and wide adjacent channels retain their real widths in either order, including a gap under the name overhang
    {
        for (bool narrowFirst : {false, true}) for (double gap : {0.0, 0.1}) {
            Store s;
            Label a = mk(narrowFirst ? 30 : 24, "LEFT", Band); a.bwMhz = narrowFirst ? 1 : 13; s.add(a);
            Label b = mk((narrowFirst ? 37 : 31) + gap, "RIGHT", Band); b.bwMhz = narrowFirst ? 13 : 1; s.add(b); s.finish();
            View v = view(0, 100);
            Result r = layout(s, v, [](const Label&, bool) { return 60.f; }); layoutBandwidth(s, v, r);
            for (const Placed& p : r.placed) CHECK(p.row == 0 && !p.fadeLeft && !p.fadeRight && std::fabs((p.bar1 - p.bar0) - s.at(p.idx).bwMhz * 10) < 0.01f, "preserve narrow/wide bandwidths without moving names");
            CHECK(std::fabs((r.placed[1].bar0 - r.placed[0].bar1) - gap * 10) < 0.01f, "true endpoints meet or retain their real gap");
        }
    }
    // a real gap smaller than the name spacing must not be widened by clipping bandwidths
    {
        Store s;
        Label a = mk(30, "LEFT", Band); a.bwMhz = 29.8; s.add(a);
        Label b = mk(60, "RIGHT", Band); b.bwMhz = 29.8; s.add(b); s.finish();
        Result r = bandLayout(s, view(0, 100));
        CHECK(r.placed[0].bar1 == 449 && r.placed[1].bar0 == 451, "preserve the true two-pixel gap");
        CHECK(!r.placed[0].fadeRight && !r.placed[1].fadeLeft, "separate bands keep their endpoint ticks");
    }
    // competing wide bands split the gap equally, retaining the gap between them and independent endpoints
    {
        Store s;
        Label a = mk(30, "LEFT", Band); a.bwMhz = 60; s.add(a);
        Label b = mk(60, "RIGHT", Band); b.bwMhz = 60; s.add(b); s.finish();
        View v = view(0, 100);
        Result r = bandLayout(s, v);
        CHECK(r.placed[0].bar1 == 448 && r.placed[1].bar0 == 452, "equal shares with four pixels between the bands");
        CHECK(!r.placed[0].fadeLeft && r.placed[0].fadeRight && r.placed[1].fadeLeft && !r.placed[1].fadeRight, "only the contested ends fade");
        const auto before = r.placed;
        std::reverse(r.placed.begin(), r.placed.end());
        layoutBandwidth(s, v, r); layoutBandwidth(s, v, r);
        for (const Placed& p : r.placed) {
            const Placed& q = before[p.idx];
            CHECK(p.bar0 == q.bar0 && p.bar1 == q.bar1 && p.fadeLeft == q.fadeLeft && p.fadeRight == q.fadeRight, "independent of processing order and repeat calls");
            CHECK(p.x0 == q.x0 && p.x1 == q.x1 && p.row == q.row, "bandwidth sharing never moves a name");
        }
    }
    // a small request returns its unused share to a larger request
    {
        Store s;
        Label a = mk(30, "SMALL", Band); a.bwMhz = 16; s.add(a);
        Label b = mk(60, "LARGE", Band); b.bwMhz = 70; s.add(b); s.finish();
        Result r = bandLayout(s, view(0, 100));
        CHECK(r.placed[0].bar1 == 380 && !r.placed[0].fadeRight, "small extension is shown completely");
        CHECK(r.placed[1].bar0 == 384 && r.placed[1].fadeLeft, "larger request gets the remainder of the gap");
    }
    // names without a bandwidth, including allocations, still reserve their space
    {
        for (bool allocation : {false, true}) {
            Store s;
            Label a = mk(30, "WIDE", Band); a.bwMhz = 80; s.add(a);
            Label b = mk(60, "NAME", Band); b.allocation = allocation; b.bwMhz = allocation ? 80 : 0; s.add(b); s.finish();
            Result r = bandLayout(s, view(0, 100));
            CHECK(r.placed[0].bar1 == 546 && r.placed[0].fadeRight, "wide band stops before its neighbour's name");
            CHECK(r.placed[1].bar1 <= r.placed[1].bar0, "no bandwidth line for an unknown bandwidth or allocation");
        }
    }
    // labels on different rows do not constrain each other's bandwidth
    {
        Store s;
        Label a = mk(50, "FIRST", Band); a.bwMhz = 30; s.add(a);
        Label b = mk(51, "SECOND", Band); b.bwMhz = 30; s.add(b); s.finish();
        Result r = bandLayout(s, view(0, 100));
        CHECK(r.placed[0].row != r.placed[1].row, "names are on different rows");
        for (const Placed& p : r.placed) CHECK(!p.fadeLeft && !p.fadeRight && p.bar1 - p.bar0 == 300, "both rows show their full bandwidth");
    }
    // the plot clips outer bands without fades or false endpoint ticks at the viewport boundary
    {
        Store s;
        Label a = mk(5, "LEFT", Band); a.bwMhz = 20; s.add(a);
        Label b = mk(95, "RIGHT", Band); b.bwMhz = 20; s.add(b); s.finish();
        Result r = bandLayout(s, view(0, 100));
        CHECK(r.placed[0].bar0 == -50 && r.placed[0].bar1 == 150 && !r.placed[0].fadeLeft && !r.placed[0].fadeRight, "left endpoint stays outside the plot without a fade");
        CHECK(r.placed[1].bar0 == 850 && r.placed[1].bar1 == 1050 && !r.placed[1].fadeLeft && !r.placed[1].fadeRight, "right endpoint stays outside the plot without a fade");
    }
    // an offscreen centre keeps its band: the viewport clips one end, while a neighbour can still fade the other
    {
        Store s;
        Label a = mk(866, "DTMB channel DS-57", FreqDb); a.bwMhz = 8; s.add(a);
        Label b = mk(868.5, "NEIGHBOUR", FreqDb); b.bwMhz = 0.125; s.add(b); s.finish();
        Result r = bandLayout(s, view(867, 869));
        CHECK(r.placed.size() == 2 && !r.placed[0].centreIn, "offscreen DTMB centre stays as an edge label");
        CHECK(r.placed[0].bar0 == -2500 && !r.placed[0].fadeLeft, "offscreen left endpoint does not fade");
        CHECK(r.placed[0].bar1 == 696 && r.placed[0].fadeRight, "right endpoint fades before its neighbour");
    }
    // many candidates: capped, still fast and without overlap
    {
        Store s;
        for (int i = 0; i < 20000; i++) s.add(mk(30.0 + i * 0.001, "CH", Band));
        s.finish();
        Result r = layout(s, view(30, 50), width);
        CHECK(r.candidates <= 800 && !overlap(r) && !r.placed.empty(), "capped: %zu candidates", r.candidates);
    }
    // degenerate views
    {
        Store s; s.add(mk(100, "X", Fm)); s.finish();
        CHECK(layout(s, view(100, 100), width).placed.empty(), "empty frequency span");
        View v = view(90, 110); v.px1 = v.px0;
        CHECK(layout(s, v, width).placed.empty(), "empty pixel span");
        CHECK(layout(s, view(90, 110, 0), width).placed.empty(), "no rows");
    }
    // the hit test
    {
        Store s;
        s.add(mk(95, "LEFT", Fm)); s.add(mk(105, "RIGHT", Fm));
        s.finish();
        Result r = layout(s, view(90, 110), width);
        CHECK(r.placed.size() == 2, "two placed");
        const Placed& p = r.placed[0];
        const float rowH = 14, gap = 2, y0 = 10;
        CHECK(hit(r, (p.x0 + p.x1) / 2, y0 + 5, y0, rowH, gap) == 0, "inside the first name");
        CHECK(hit(r, (p.x0 + p.x1) / 2, y0 + 15, y0, rowH, gap) == -1, "in the gap under it");
        CHECK(hit(r, (p.x0 + p.x1) / 2, y0 - 1, y0, rowH, gap) == -1, "above the rows");
        CHECK(hit(r, p.x1 + 40, y0 + 5, y0, rowH, gap) == -1, "beside it");
        CHECK(hit(r, (r.placed[1].x0 + r.placed[1].x1) / 2, y0 + 1, y0, rowH, gap) == 1, "the second name");
    }
    // helpers
    CHECK(trimmed("  NOOR FM  ") == "NOOR FM" && trimmed("   ").empty() && trimmed("").empty(), "trimmed");
    CHECK(mix(1, std::string("a")) != mix(1, std::string("b")) && mix(1, std::string("a")) == mix(1, std::string("a")), "fingerprint");
    if (failures) { std::printf("%d failed\n", failures); return 1; }
    std::printf("autolabel: all passed\n");
    return 0;
}
