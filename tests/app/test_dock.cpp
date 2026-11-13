// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The docking layout tree (app/ui/dock_tree.h): dock / tab / split / undock, layout and splitters, drop targets, text form, named layouts.
#include "ui/dock_tree.h"
#include <cmath>
#include <cstdio>
#include <set>
using namespace dock;

int main() {
    int fails = 0;
    auto chk = [&](bool ok, const char* m) { if (!ok) { std::printf("FAIL: %s\n", m); fails++; } };

    // building a tree
    Node t;
    chk(t.empty() && serialise(t).empty(), "empty tree");
    chk(dockAt(t, "", Zone::Left, "A"), "first window");
    chk(t.kind == Node::Leaf && t.tabs.size() == 1, "first window is the only leaf");
    chk(dockAt(t, "A", Zone::Right, "B", 0.3f), "dock right");
    chk(t.kind == Node::SplitH && t.kids[1].tabs[0] == "B" && std::fabs(t.ratio - 0.7f) < 1e-4f, "right split, share 0.3");
    chk(dockAt(t, "B", Zone::Bottom, "C"), "dock below B");
    chk(t.kids[1].kind == Node::SplitV && t.kids[1].kids[1].tabs[0] == "C", "C under B");
    chk(dockAt(t, "A", Zone::Centre, "D"), "tab with A");
    chk(t.kids[0].tabs.size() == 2 && t.kids[0].active == 1, "D is a tab of A, shown");
    chk(dockAt(t, "", Zone::Top, "E", 0.2f), "dock-space edge");
    chk(t.kind == Node::SplitV && t.kids[0].tabs[0] == "E", "E across the top");
    chk(panels(t).size() == 5, "five docked");
    chk(!dockAt(t, "A", Zone::Left, "A"), "onto itself: nothing");
    chk(!dockAt(t, "A", Zone::Centre, "D"), "into its own leaf: nothing");
    chk(!dockAt(t, "A", Zone::None, "Z"), "no zone: nothing");

    // moving and undocking
    chk(dockAt(t, "C", Zone::Centre, "D"), "move D to C's leaf");
    chk(leafOf(t, "D") == leafOf(t, "C") && leafOf(t, "A")->tabs.size() == 1, "moved");
    chk(remove(t, "E"), "undock E");
    chk(t.kind == Node::SplitH && !contains(t, "E"), "E's leaf collapsed into its sibling");
    chk(!remove(t, "E"), "undock twice");
    chk(remove(t, "B") && leafOf(t, "C") != nullptr, "undock B: C takes its place");
    chk(remove(t, "A") && remove(t, "C") && remove(t, "D") && t.empty(), "all undocked: empty");

    // tabs
    Node u;
    dockAt(u, "", Zone::Centre, "a"); dockAt(u, "a", Zone::Centre, "b"); dockAt(u, "a", Zone::Centre, "c");
    chk(u.active == 2, "last docked tab shown");
    chk(setActive(u, "a") && u.active == 0 && !setActive(u, "a"), "set active");
    chk(cycleTab(u, "a", 1) == "b" && cycleTab(u, "a", -1) == "c", "cycle tabs, wraps");
    chk(cycleTab(u, "a", 1, [](const std::string& s) { return s != "b"; }) == "c", "cycle skips closed windows");
    remove(u, "a");
    chk(u.tabs.size() == 2 && u.active >= 0 && u.active < 2, "active index valid after removing");

    // layout
    Node l;
    dockAt(l, "", Zone::Left, "A"); dockAt(l, "A", Zone::Right, "B"); dockAt(l, "B", Zone::Bottom, "C");
    std::vector<LeafBox> leaves; std::vector<SplitBar> bars;
    layout(l, Rect{0, 0, 1004, 504}, 4, {}, leaves, bars);
    chk(leaves.size() == 3 && bars.size() == 2, "three boxes, two bars");
    chk(leaves[0].r.w == 500 && leaves[1].r.x == 504 && leaves[1].r.w == 500, "split widths with the gap");
    chk(leaves[1].r.h == 250 && leaves[2].r.y == 254, "split heights with the gap");
    chk(bars[0].vertical && bars[0].r.x == 500 && bars[0].r.w == 4 && bars[0].path.empty(), "first bar");
    chk(!bars[1].vertical && bars[1].path.size() == 1 && bars[1].path[0] == 1, "second bar path");
    float area = 0; for (auto& b : leaves) area += b.r.w * b.r.h;
    chk(area <= 1004.f * 504.f, "boxes inside the space");
    layout(l, Rect{0, 0, 1004, 504}, 4, [](const std::string& s) { return s != "A"; }, leaves, bars);
    chk(leaves.size() == 2 && leaves[0].r.x == 0 && leaves[0].r.w == 1004 && bars.size() == 1, "a closed window gives its space away");
    layout(l, Rect{0, 0, 100, 100}, 4, [](const std::string&) { return false; }, leaves, bars);
    chk(leaves.empty() && bars.empty(), "nothing alive: nothing laid out");
    Node tabs2; dockAt(tabs2, "", Zone::Centre, "x"); dockAt(tabs2, "x", Zone::Centre, "y");
    layout(tabs2, Rect{0, 0, 10, 10}, 4, [](const std::string& s) { return s == "x"; }, leaves, bars);
    chk(leaves.size() == 1 && leaves[0].tabs.size() == 1 && leaves[0].active == 0, "active falls back to an alive tab");

    // splitters
    chk(std::fabs(ratioAt(250, 0, 1000, 100) - 0.25f) < 1e-5f, "ratio at a pixel");
    chk(std::fabs(ratioAt(10, 0, 1000, 100) - 0.1f) < 1e-5f, "kept to the least size");
    chk(std::fabs(ratioAt(990, 0, 1000, 100) - 0.9f) < 1e-5f, "kept to the least size (far side)");
    chk(ratioAt(50, 0, 100, 80) == 0.5f, "no room: halves");
    chk(setRatio(l, bars.empty() ? std::vector<int>{} : std::vector<int>{}, 0.3f) && std::fabs(l.ratio - 0.3f) < 1e-5f, "set ratio at root");
    chk(setRatio(l, {1}, 0.99f) && l.kids[1].ratio <= 0.95f, "ratio clamped");
    chk(!setRatio(l, {0}, 0.5f) && !setRatio(l, {1, 1}, 0.5f) && !setRatio(l, {5}, 0.5f), "no split there");

    // drop targets
    const Rect box{100, 100, 400, 300};
    chk(hitTest(box, 300, 250, 40, false) == Zone::Centre, "centre target");
    chk(hitTest(box, 300 - 48, 250, 40, false) == Zone::Left, "left target");
    chk(hitTest(box, 300 + 48, 250, 40, false) == Zone::Right, "right target");
    chk(hitTest(box, 300, 250 - 48, 40, false) == Zone::Top, "top target");
    chk(hitTest(box, 300, 250 + 48, 40, false) == Zone::Bottom, "bottom target");
    chk(hitTest(box, 120, 120, 40, false) == Zone::None, "corner: none");
    chk(hitTest(box, 125, 250, 40, true) == Zone::Left && hitTest(box, 300, 250, 40, true) == Zone::None, "outer edge targets, no centre");
    chk(hitTest(box, 300, 385, 40, true) == Zone::Bottom && hitTest(box, 475, 250, 40, true) == Zone::Right, "outer bottom, right");
    for (Zone z : {Zone::Left, Zone::Right, Zone::Top, Zone::Bottom}) {
        const Rect a = zoneRect(box, z, 40, false), b = zoneRect(box, z, 40, true);
        chk(a.x >= box.x && a.x + a.w <= box.x + box.w && b.y >= box.y && b.y + b.h <= box.y + box.h, "targets inside the box");
    }
    const Rect pr = previewRect(box, Zone::Right, 0.25f);
    chk(pr.x == 400 && pr.w == 100 && pr.h == 300, "preview right quarter");
    chk(previewRect(box, Zone::Centre, 0.5f).w == 400, "preview centre: whole box");

    // text form
    Node s;
    dockAt(s, "", Zone::Left, "Map"); dockAt(s, "Map", Zone::Right, "Odd|name(1),x\\y", 0.33f); dockAt(s, "Map", Zone::Centre, "Memory###Memory");
    const std::string txt = serialise(s);
    Node back;
    chk(parse(txt, back) && serialise(back) == txt, "round trip");
    chk(contains(back, "Odd|name(1),x\\y") && contains(back, "Memory###Memory"), "escaped names survive");
    chk(leafOf(back, "Map")->active == 1, "active tab kept");
    chk(std::fabs(back.ratio - s.ratio) < 1e-4f, "ratio kept");
    Node bad;
    for (const char* b : {"T(", "T(0)", "H(5000,T(0|a))", "X(1)", "T(0|a)junk", "H(5000,T(0|a),T(0|b)", "T(abc|a)", "H(99999999999,T(0|a),T(0|b))"})
        chk(!parse(b, bad) && bad.empty(), b);
    chk(parse("", bad) && bad.empty(), "empty text: empty tree");
    chk(parse("T(7|a|b)", bad) && bad.active == 0, "active out of range: first");
    std::string deep; for (int i = 0; i < 100; i++) deep += "H(5000,T(0|a),"; deep += "T(0|b)"; for (int i = 0; i < 100; i++) deep += ")";
    chk(!parse(deep, bad), "too deep refused");

    // named layouts
    std::vector<Named> nv;
    putNamed(nv, "3-dvbt2", "Wide", txt);
    putNamed(nv, "3-dvbt2", "Wide", "T(0|a)");
    putNamed(nv, "7", "Tabs\tonly", "T(0|b)");
    chk(nv.size() == 2 && nv[0].tree == "T(0|a)" && nv[1].name == "Tabs only", "add, replace, tabs cleaned");
    const std::vector<Named> nb = parseNamed(writeNamed(nv) + "garbage line\n3\tbroken\tT(\n");
    chk(nb.size() == 2 && nb[1].mode == "7" && nb[0].tree == "T(0|a)", "named round trip, bad lines dropped");
    chk(dropNamed(nv, "7", "Tabs only") && nv.size() == 1 && !dropNamed(nv, "7", "Tabs only"), "drop named");

    std::printf(fails ? "dock: %d failed\n" : "dock: ok\n", fails);
    return fails ? 1 : 0;
}
