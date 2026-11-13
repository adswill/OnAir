// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Picture saving rules (app/state/picsave.h): names, paths, collisions, error text, delete with undo.
#include "../../app/state/picsave.h"
#include <cstdio>
#include <set>
using namespace picsave;
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
int main() {
    check(defaultFolder("/home/u") == "/home/u/Pictures/OnAir" && defaultFolder("") == "./Pictures/OnAir", "default folder");
    check(safe("Meteor-M2 3") == "Meteor-M2-3" && safe("a/b\\c:d") == "a-b-c-d" && safe("") == "picture" && safe("///") == "picture" && safe("ham_slow-scan ") == "ham_slow-scan", "safe names");
    check(safe("\xC3\xA4x") == "x", "non-ASCII dropped");
    check(trimSlash("/a/b//") == "/a/b" && trimSlash("/") == "/", "trim");
    const time_t t = 1760000000;
    const std::string st = stamp(t);
    check(st.size() == 15 && st[8] == '-', "stamp shape");
    check(fileName("SSTV", t) == st + "-SSTV.png", "file name has the time and the mode");
    check(buildPath("/p/OnAir/", "weather fax", t) == "/p/OnAir/weather-fax/" + st + "-weather-fax.png", "path: folder, mode folder, name");
    std::set<std::string> have = {"/x/a.png", "/x/a-2.png"};
    auto ex = [&](const std::string& p) { return have.count(p) > 0; };
    check(uniquePath("/x/b.png", ex) == "/x/b.png", "free name kept");
    check(uniquePath("/x/a.png", ex) == "/x/a-3.png", "taken name counts on");
    have.insert("/x.d/n"); check(uniquePath("/x.d/n", ex) == "/x.d/n-2", "no extension, dot in the folder");
    check(parentOf("/a/b/c.png") == "/a/b" && parentOf("c.png") == ".", "parent");
    check(savedText("/a/b.png") == "Saved to /a/b.png", "saved text");
    check(tooltipText("/p/", "SSTV") == "Save this picture to /p/SSTV", "tooltip names the folder");
    check(failText("/ro", false).find("cannot write to /ro") != std::string::npos && failText("/ro", true).find("create the folder /ro") != std::string::npos, "plain error");

    PendingDelete d;
    check(!d.canUndo() && d.undo().empty() && !d.hidden("a"), "empty");
    d.mark("a", 10); d.mark("b", 12); d.mark("a", 13);
    check(d.count() == 2 && d.hidden("a") && d.hidden("b") && !d.hidden("c"), "marked once each");
    check(d.undo() == "b" && !d.hidden("b") && d.hidden("a"), "undo brings the latest back");
    check(d.expire(15, 8).empty() && d.hidden("a"), "not yet");
    const auto gone = d.expire(18.5, 8);
    check(gone.size() == 1 && gone[0] == "a" && !d.canUndo(), "time up: really deleted");
    d.mark("x", 0); d.mark("y", 1);
    check(d.flush().size() == 2 && d.count() == 0, "flush on close");
    if (!fails) printf("ok\n");
    return fails;
}
