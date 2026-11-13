// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir Web frequency ranges (server/web/web_ranges.*): parsing with units, merging (overlap, adjacent, nested), the allow / block rules
// (block inside allow, mode lists, per listener class), the passband containment with its guard margin, the capture-edge rule, snapping,
// spectrum masking, the presets and the settings round trip. UNRUN when written: the host was on battery (see docs/PATHWAY.md).
#include "../../server/web/web_ranges.h"
#include "server_config.h"
#include <cmath>
#include <cstdio>
#include <limits>
using namespace onair_web;
#include "../common/test_util.h"

static bool closeTo(double a, double b, double tol = 1e-3) { return std::fabs(a - b) <= tol; }

static RangeView V(const std::vector<std::string>& allow, const std::vector<std::string>& block = {}, const std::vector<std::string>& presets = {}) {
    WebRanges r;
    std::string e;
    const bool ok = r.replace(allow, block, presets, "", e);
    CHECK(ok, "replace failed: %s", e.c_str());
    return r.view(RangeClass::Open);
}

static const double MHz = 1e6, kHz = 1e3;

static void testParseFreq() {
    double hz = 0; bool u = false; std::string e;
    CHECK(parseFreqHz("144MHz", 1, hz, u, e) && closeTo(hz, 144 * MHz) && u, "144MHz");
    CHECK(parseFreqHz("7.100 MHz", 1, hz, u, e) && closeTo(hz, 7.1 * MHz), "7.100 MHz with a space");
    CHECK(parseFreqHz("14kHz", 1, hz, u, e) && closeTo(hz, 14 * kHz), "14kHz");
    CHECK(parseFreqHz("2.4GHz", 1, hz, u, e) && closeTo(hz, 2.4e9), "2.4GHz");
    CHECK(parseFreqHz("7.2MHZ", 1, hz, u, e) && closeTo(hz, 7.2 * MHz), "units are case-insensitive (and 7.2*1e6 is exact after the 1 mHz rounding)");
    CHECK(parseFreqHz("145", 1, hz, u, e) && closeTo(hz, 145) && !u, "no unit: Hz");
    CHECK(parseFreqHz("145", 1e6, hz, u, e) && closeTo(hz, 145 * MHz) && !u, "no unit, default multiplier");
    CHECK(parseFreqHz("1e6", 1, hz, u, e) && closeTo(hz, 1e6), "exponent");
    CHECK(parseFreqHz("12.5k", 1, hz, u, e) && closeTo(hz, 12500), "short unit k");
    CHECK(parseFreqHz("1000000000000", 1, hz, u, e) && closeTo(hz, 1e12), "exactly the limit");
    CHECK(!parseFreqHz("1000000000001", 1, hz, u, e), "above the limit");
    CHECK(!parseFreqHz("1e13", 1, hz, u, e) && !e.empty(), "huge");
    CHECK(!parseFreqHz("1e999", 1, hz, u, e), "overflow to inf");
    CHECK(!parseFreqHz("inf", 1, hz, u, e), "inf");
    CHECK(!parseFreqHz("nan", 1, hz, u, e), "nan");
    CHECK(!parseFreqHz("-5", 1, hz, u, e), "negative");
    CHECK(!parseFreqHz("-5MHz", 1, hz, u, e), "negative with unit");
    CHECK(!parseFreqHz("", 1, hz, u, e), "empty");
    CHECK(!parseFreqHz("MHz", 1, hz, u, e), "no number");
    CHECK(!parseFreqHz("5 furlongs", 1, hz, u, e), "unknown unit");
    CHECK(parseFreqHz("0", 1, hz, u, e) && hz == 0, "zero is a frequency (a range cannot be empty, a frequency can be 0)");
}

static void testParseSpec() {
    RangeRule r; std::string e;
    CHECK(parseRangeSpec("144-146MHz", r, e) && closeTo(r.r.lo, 144 * MHz) && closeTo(r.r.hi, 146 * MHz), "144-146MHz: %s", e.c_str());
    CHECK(r.r.modes == kAllModes && r.classes == kAllClasses, "defaults: all modes, all classes");
    CHECK(parseRangeSpec("7.000-7.200MHz,usb,lsb", r, e) && closeTo(r.r.lo, 7e6) && closeTo(r.r.hi, 7.2e6), "HF with modes");
    CHECK(r.r.modes == ((1u << (unsigned)Demod::Usb) | (1u << (unsigned)Demod::Lsb)), "mode mask usb+lsb");
    CHECK(parseRangeSpec("144-146MHz,nfm,member", r, e) && r.classes == classBit(RangeClass::Member), "class tag member");
    CHECK(parseRangeSpec("144-146MHz,open,invite", r, e) && r.classes == (classBit(RangeClass::Open) | classBit(RangeClass::Invite)), "two class tags");
    CHECK(parseRangeSpec("100kHz-200kHz", r, e) && closeTo(r.r.lo, 100e3) && closeTo(r.r.hi, 200e3), "unit on both");
    CHECK(parseRangeSpec("100-200kHz", r, e) && closeTo(r.r.lo, 100e3) && closeTo(r.r.hi, 200e3), "the start takes the end's unit");
    CHECK(parseRangeSpec("90MHz-100000kHz", r, e) && closeTo(r.r.lo, 90e6) && closeTo(r.r.hi, 100e6), "different units");
    CHECK(parseRangeSpec("100-200", r, e) && closeTo(r.r.lo, 100) && closeTo(r.r.hi, 200), "no unit at all: Hz");
    CHECK(parseRangeSpec("144 - 146 MHz", r, e) && closeTo(r.r.lo, 144 * MHz), "spaces around the dash");
    CHECK(parseRangeSpec("1e3-2e3", r, e) && closeTo(r.r.lo, 1000) && closeTo(r.r.hi, 2000), "exponents");
    CHECK(parseRangeSpec("1e-3-5", r, e) && closeTo(r.r.lo, 0.001) && closeTo(r.r.hi, 5), "negative exponent is not the separator");
    CHECK(parseRangeSpec("0-1MHz", r, e) && r.r.lo == 0, "from zero");
    CHECK(parseRangeSpec("1-1000000000000", r, e) && closeTo(r.r.hi, 1e12), "up to the limit");
    CHECK(parseRangeSpec("1-2MHz,note=20 m, Region 1 only", r, e) && r.note == "20 m, Region 1 only", "a note runs to the end of the line, commas included");
    CHECK(parseRangeSpec("1-2MHz,usb,note=x", r, e) && r.r.modes == (1u << (unsigned)Demod::Usb) && r.note == "x", "modes then note");
    // refused
    CHECK(!parseRangeSpec("146-144MHz", r, e) && e.find("above the end") != std::string::npos, "reversed: %s", e.c_str());
    CHECK(!parseRangeSpec("145-145MHz", r, e) && e.find("empty") != std::string::npos, "zero width: %s", e.c_str());
    CHECK(!parseRangeSpec("145.0000001-145MHz", r, e), "reversed by 0.1 Hz: refused");
    CHECK(!parseRangeSpec("-5-10MHz", r, e), "negative start");
    CHECK(!parseRangeSpec("5--3MHz", r, e), "negative end");
    CHECK(!parseRangeSpec("1-1e30", r, e), "huge");
    CHECK(!parseRangeSpec("1-1e999", r, e), "overflow");
    CHECK(!parseRangeSpec("144MHz", r, e), "no dash");
    CHECK(!parseRangeSpec("", r, e), "empty line");
    CHECK(!parseRangeSpec("   ", r, e), "blank line");
    CHECK(!parseRangeSpec("144-146MHz,foo", r, e), "unknown word");
    CHECK(!parseRangeSpec("abc-def", r, e), "not numbers");
    CHECK(!parseRangeSpec("144-146furlongs", r, e), "unknown unit");
    CHECK(!parseRangeSpec("144-146MHz,note=bad\x01text", r, e) || r.note.empty(), "control characters never reach a note");
    // the canonical form parses back to the same rule
    RangeRule a, b;
    CHECK(parseRangeSpec("7.000-7.200MHz,usb,lsb,member,note=40 m", a, e), "for the round trip");
    CHECK(parseRangeSpec(formatRangeSpec(a), b, e) && closeTo(a.r.lo, b.r.lo) && closeTo(a.r.hi, b.r.hi) && a.r.modes == b.r.modes && a.classes == b.classes && a.note == b.note,
          "round trip: %s", formatRangeSpec(a).c_str());
    CHECK(parseRangeSpec("0.001-0.002", a, e) && parseRangeSpec(formatRangeSpec(a), b, e) && closeTo(b.r.lo, 0.001) && closeTo(b.r.hi, 0.002), "millihertz round trip");
}

static void testMerge() {
    auto F = [](double a, double b, unsigned m = kAllModes) { FreqRange f; f.lo = a; f.hi = b; f.modes = m; return f; };
    auto m = mergeRanges({F(5, 10), F(1, 3), F(2, 4)});
    CHECK(m.size() == 2 && m[0].lo == 1 && m[0].hi == 4 && m[1].lo == 5 && m[1].hi == 10, "overlap merges, a gap keeps them apart (%zu)", m.size());
    m = mergeRanges({F(144e6, 145e6), F(145e6, 146e6)});
    CHECK(m.size() == 1 && m[0].lo == 144e6 && m[0].hi == 146e6, "adjacent ranges merge");
    m = mergeRanges({F(144e6, 145e6), F(145e6 + 1, 146e6)});
    CHECK(m.size() == 2, "a gap of 1 Hz is a gap");
    m = mergeRanges({F(144e6, 145e6), F(145e6 + 0.0005, 146e6)});
    CHECK(m.size() == 1, "a gap under 1 mHz is none");
    m = mergeRanges({F(1, 100), F(10, 20), F(30, 40)});
    CHECK(m.size() == 1 && m[0].hi == 100, "nested ranges vanish");
    m = mergeRanges({F(10, 5), F(1, 2)});
    CHECK(m.size() == 1 && m[0].lo == 1, "a reversed entry never gets in");
    m = mergeRanges({});
    CHECK(m.empty(), "empty");
    m = mergeRanges({F(0, 1e12), F(5, 6)});
    CHECK(m.size() == 1 && m[0].hi == 1e12, "huge values");
    auto usb = rangesForMode({F(1, 2, 1u << (unsigned)Demod::Usb), F(2, 3, 1u << (unsigned)Demod::Am), F(3, 4, 1u << (unsigned)Demod::Usb)}, Demod::Usb);
    CHECK(usb.size() == 2, "rangesForMode keeps only the mode's ranges (%zu)", usb.size());
    auto sub = subtractRanges({F(0, 10)}, {F(3, 4), F(6, 7)});
    CHECK(sub.size() == 3 && sub[0].hi == 3 && sub[1].lo == 4 && sub[1].hi == 6 && sub[2].lo == 7 && sub[2].hi == 10, "subtract two holes (%zu)", sub.size());
    sub = subtractRanges({F(0, 10)}, {F(-5, 20)});
    CHECK(sub.empty(), "subtract everything");
    sub = subtractRanges({F(0, 10)}, {F(20, 30)});
    CHECK(sub.size() == 1, "subtract nothing");
}

static void testGuard() {
    CHECK(guardMarginHz(0) == 200, "minimum 200 Hz");
    CHECK(closeTo(guardMarginHz(12500), 625), "5 percent of 12.5 kHz");
    CHECK(guardMarginHz(200000) == 5000, "at most 5 kHz");
    CHECK(guardMarginHz(-5) == 200 && guardMarginHz(std::numeric_limits<double>::quiet_NaN()) == 200, "nonsense bandwidth");
}

static void testCheckPassband() {
    const double nl = -6250, nh = 6250;   // NFM
    RangeView v = V({"144-146MHz,nfm", "7.000-7.200MHz,usb,lsb"});
    CHECK(v.restricted && v.active(), "restricted");
    CHECK(checkPassband(v, Demod::Nfm, 145 * MHz, nl, nh) == RangeVerdict::Ok, "NFM in the middle");
    CHECK(checkPassband(v, Demod::Nfm, 144 * MHz, nl, nh) == RangeVerdict::NotAllowed, "NFM exactly on the lower edge sticks out");
    CHECK(checkPassband(v, Demod::Nfm, 144.006 * MHz, nl, nh) == RangeVerdict::NotAllowed, "NFM: the guard margin still sticks out");
    CHECK(checkPassband(v, Demod::Nfm, 144.007 * MHz, nl, nh) == RangeVerdict::Ok, "NFM just inside the guard");
    CHECK(checkPassband(v, Demod::Nfm, 145.993 * MHz, nl, nh) == RangeVerdict::Ok, "NFM just inside the upper edge");
    CHECK(checkPassband(v, Demod::Nfm, 145.994 * MHz, nl, nh) == RangeVerdict::NotAllowed, "NFM too close to the upper edge");
    CHECK(checkPassband(v, Demod::Nfm, 100 * MHz, nl, nh) == RangeVerdict::NotAllowed, "outside everything");
    CHECK(checkPassband(v, Demod::Usb, 7.1 * MHz, 300, 2700) == RangeVerdict::Ok, "USB inside 40 m");
    CHECK(checkPassband(v, Demod::Usb, 7.2 * MHz, 300, 2700) == RangeVerdict::NotAllowed, "USB on the upper edge runs over");
    CHECK(checkPassband(v, Demod::Lsb, 7.2 * MHz, -2700, -300) == RangeVerdict::Ok, "LSB on the upper edge stays under it");
    CHECK(checkPassband(v, Demod::Lsb, 7.0 * MHz, -2700, -300) == RangeVerdict::NotAllowed, "LSB on the lower edge runs under it");
    CHECK(checkPassband(v, Demod::Am, 7.1 * MHz, -5000, 5000) == RangeVerdict::ModeNotAllowed, "AM in a USB/LSB range");
    CHECK(checkPassband(v, Demod::Usb, 145 * MHz, 300, 2700) == RangeVerdict::ModeNotAllowed, "USB in an NFM range");
    CHECK(checkPassband(v, Demod::Nfm, 145 * MHz, 0, 0) == RangeVerdict::Invalid, "zero-width passband");
    CHECK(checkPassband(v, Demod::Nfm, 145 * MHz, 5000, -5000) == RangeVerdict::Invalid, "reversed passband");
    CHECK(checkPassband(v, Demod::Nfm, std::numeric_limits<double>::quiet_NaN(), nl, nh) == RangeVerdict::Invalid, "NaN frequency");
    CHECK(checkPassband(v, Demod::Nfm, 145 * MHz, nl, std::numeric_limits<double>::infinity()) == RangeVerdict::Invalid, "infinite edge");
    CHECK(checkPassband(v, Demod::Nfm, -145 * MHz, nl, nh) == RangeVerdict::Invalid, "negative frequency");
    CHECK(checkPassband(v, Demod::Nfm, 1e15, nl, nh) == RangeVerdict::Invalid, "huge frequency");

    // adjacent allowed ranges act as one; a gap of 1 kHz does not
    v = V({"144-145MHz", "145-146MHz"});
    CHECK(checkPassband(v, Demod::Nfm, 145 * MHz, nl, nh) == RangeVerdict::Ok, "a passband across the join of adjacent ranges");
    v = V({"144-145MHz", "145.001-146MHz"});
    CHECK(checkPassband(v, Demod::Nfm, 145.0005 * MHz, nl, nh) == RangeVerdict::NotAllowed, "never spread over a gap");
    v = V({"144-146MHz", "145-147MHz"});
    CHECK(checkPassband(v, Demod::Nfm, 146 * MHz, nl, nh) == RangeVerdict::Ok, "overlapping ranges act as one");
    v = V({"144-148MHz", "145-146MHz"});
    CHECK(checkPassband(v, Demod::Nfm, 147 * MHz, nl, nh) == RangeVerdict::Ok, "a nested range changes nothing");

    // a block inside an allow always wins
    v = V({"144-148MHz"}, {"145-146MHz"});
    CHECK(checkPassband(v, Demod::Nfm, 144.5 * MHz, nl, nh) == RangeVerdict::Ok, "below the block");
    CHECK(checkPassband(v, Demod::Nfm, 145.5 * MHz, nl, nh) == RangeVerdict::Blocked, "inside the block");
    CHECK(checkPassband(v, Demod::Nfm, 144.9999 * MHz, nl, nh) == RangeVerdict::Blocked, "the passband reaches into the block");
    CHECK(checkPassband(v, Demod::Nfm, 144.99 * MHz, nl, nh) == RangeVerdict::Ok, "clear of the block");
    CHECK(checkPassband(v, Demod::Nfm, 146.0 * MHz, nl, nh) == RangeVerdict::Blocked, "on the upper edge of the block");
    CHECK(checkPassband(v, Demod::Nfm, 146.0075 * MHz, nl, nh) == RangeVerdict::Ok, "just above the block's guard");
    CHECK(checkPassband(v, Demod::Nfm, 147 * MHz, nl, nh) == RangeVerdict::Ok, "above the block");
    // block with no allow list (only blocks): everything else is open
    v = V({}, {"100-101MHz"});
    CHECK(!v.restricted && v.active(), "blocks only: not restricted, but active");
    CHECK(checkPassband(v, Demod::Nfm, 50 * MHz, nl, nh) == RangeVerdict::Ok, "anywhere else");
    CHECK(checkPassband(v, Demod::Nfm, 100.5 * MHz, nl, nh) == RangeVerdict::Blocked, "in the block");
    // block that names modes
    v = V({}, {"140-150MHz,wfm"});
    CHECK(checkPassband(v, Demod::Nfm, 145 * MHz, nl, nh) == RangeVerdict::Ok, "NFM in a WFM-only block");
    CHECK(checkPassband(v, Demod::Wfm, 145 * MHz, -100000, 100000) == RangeVerdict::Blocked, "WFM in it");
    // block and allow overlap in a different way: allow narrower than block
    v = V({"145.2-145.4MHz"}, {"145-146MHz"});
    CHECK(checkPassband(v, Demod::Nfm, 145.3 * MHz, nl, nh) == RangeVerdict::Blocked, "an allow range wholly inside a block");
    // no rules: everything passes, nothing is checked (not even the capture)
    v = RangeView();
    CHECK(!v.active(), "no rules");
    CHECK(checkPassband(v, Demod::Nfm, 5, nl, nh, 100 * MHz, 2.4 * MHz) == RangeVerdict::Ok, "no rules: untouched behaviour");
    // from zero
    v = V({"0-1MHz,am"});
    CHECK(checkPassband(v, Demod::Am, 0, -5000, 5000) == RangeVerdict::NotAllowed, "a passband below 0 Hz");
    CHECK(checkPassband(v, Demod::Am, 500e3, -5000, 5000) == RangeVerdict::Ok, "inside 0-1 MHz");
    // huge allow
    v = V({"0-1000000000000"});
    CHECK(checkPassband(v, Demod::Nfm, 145 * MHz, nl, nh) == RangeVerdict::Ok, "everything");
    CHECK(checkPassband(v, Demod::Nfm, 1e12, nl, nh) == RangeVerdict::NotAllowed, "the limit itself sticks out");
    for (auto x : {RangeVerdict::NotAllowed, RangeVerdict::ModeNotAllowed, RangeVerdict::Blocked, RangeVerdict::OutsideCapture, RangeVerdict::Invalid})
        CHECK(std::string(verdictText(x)).size() > 10, "a message for every refusal");
    CHECK(std::string(verdictText(RangeVerdict::Ok)).empty(), "no message for Ok");
}

static void testCaptureEdge() {
    // capture 98.8 .. 101.2 MHz; usable part 96 % of the rate: 98.848 .. 101.152. A passband over the edge would wrap to the other end.
    const double c = 100 * MHz, r = 2.4 * MHz;
    RangeView v = V({}, {"101.1-101.2MHz"});
    CHECK(checkPassband(v, Demod::Nfm, 98.85 * MHz, -6250, 6250, c, r) == RangeVerdict::OutsideCapture, "NFM over the lower capture edge (it would wrap onto the blocked top)");
    CHECK(checkPassband(v, Demod::Nfm, 98.86 * MHz, -6250, 6250, c, r) == RangeVerdict::Ok, "just inside the usable part");
    CHECK(checkPassband(v, Demod::Nfm, 101.14 * MHz, -6250, 6250, c, r) == RangeVerdict::Blocked, "and the block itself, inside the usable part");
    CHECK(checkPassband(v, Demod::Nfm, 98.85 * MHz, -6250, 6250) == RangeVerdict::Ok, "no capture given: not checked");
    CHECK(checkPassband(v, Demod::Wfm, 98.9 * MHz, -100000, 100000, c, r) == RangeVerdict::OutsideCapture, "WFM at the edge");
    CHECK(checkPassband(v, Demod::Wfm, 99.0 * MHz, -100000, 100000, c, r) == RangeVerdict::Ok, "WFM further in");
    CHECK(checkPassband(v, Demod::Nfm, 90 * MHz, -6250, 6250, c, r) == RangeVerdict::OutsideCapture, "far outside the capture");
    // a window / the capture
    RangeView w = V({"144-146MHz"});
    CHECK(checkWindow(w, 144.5 * MHz, 145.5 * MHz) == RangeVerdict::Ok, "a window inside");
    CHECK(checkWindow(w, 143.9 * MHz, 145.5 * MHz) == RangeVerdict::NotAllowed, "a window sticking out");
    CHECK(checkWindow(w, 144 * MHz, 146 * MHz) == RangeVerdict::Ok, "the window is the range (no guard on windows)");
    CHECK(checkWindow(w, 145 * MHz, 145 * MHz) == RangeVerdict::Invalid, "zero-width window");
    CHECK(!captureVisible(w, 145 * MHz, 2.4 * MHz), "a capture wider than the allowed range is not visible");
    CHECK(captureVisible(w, 145 * MHz, 1.5 * MHz), "a capture inside is");
    CHECK(captureVisible(RangeView(), 145 * MHz, 2.4 * MHz), "no rules: visible");
    CHECK(!captureVisible(w, 145 * MHz, 0), "no capture running with rules: not visible");
    CHECK(!captureVisible(V({}, {"145.5-145.6MHz"}), 145 * MHz, 2.4 * MHz), "a block inside the capture hides it (the host decoder would show its activity)");
    CHECK(checkWindow(V({}, {"140-150MHz,wfm"}), 144 * MHz, 145 * MHz) == RangeVerdict::Blocked, "a decoder window ignores the mode lists of blocks (it shows everything)");
}

static void testSnap() {
    const double nl = -6250, nh = 6250;
    RangeView v = V({"144-146MHz,nfm"});
    double o = 0;
    CHECK(snapFreq(v, Demod::Nfm, 145 * MHz, nl, nh, 0, 0, o) && o == 145 * MHz, "already inside: unchanged");
    CHECK(snapFreq(v, Demod::Nfm, 150 * MHz, nl, nh, 0, 0, o) && closeTo(o, 146 * MHz - 6250 - 625), "above: the highest allowed (%.3f)", o);
    CHECK(snapFreq(v, Demod::Nfm, 140 * MHz, nl, nh, 0, 0, o) && closeTo(o, 144 * MHz + 6250 + 625), "below: the lowest allowed");
    CHECK(checkPassband(v, Demod::Nfm, o, nl, nh) == RangeVerdict::Ok, "the snapped frequency passes the check");
    CHECK(!snapFreq(v, Demod::Am, 145 * MHz, -5000, 5000, 0, 0, o), "no range allows AM");
    RangeView tiny = V({"145.000-145.005MHz"});
    CHECK(!snapFreq(tiny, Demod::Nfm, 145 * MHz, nl, nh, 0, 0, o), "the allowed range is narrower than the passband");
    RangeView hf = V({"7.000-7.200MHz,usb,lsb"});
    CHECK(snapFreq(hf, Demod::Usb, 7.3 * MHz, 300, 2700, 0, 0, o) && closeTo(o, 7.2 * MHz - 2700 - 200), "USB snaps under the upper edge (%.3f)", o);
    CHECK(snapFreq(hf, Demod::Lsb, 6.9 * MHz, -2700, -300, 0, 0, o) && closeTo(o, 7.0 * MHz + 2700 + 200), "LSB snaps above the lower edge");
    RangeView b = V({"144-148MHz"}, {"145-146MHz"});
    CHECK(snapFreq(b, Demod::Nfm, 145.5 * MHz, nl, nh, 0, 0, o), "a block in the way: some frequency is found");
    CHECK(checkPassband(b, Demod::Nfm, o, nl, nh) == RangeVerdict::Ok && (o < 145 * MHz || o > 146 * MHz), "and it is outside the block (%.3f)", o);
    RangeView w = V({}, {"100-200MHz"});
    CHECK(snapFreq(w, Demod::Nfm, 150 * MHz, nl, nh, 0, 0, o) && checkPassband(w, Demod::Nfm, o, nl, nh) == RangeVerdict::Ok, "blocks only: moves out of the block");
    // keeps to the capture
    RangeView all = V({"0-1000000000000"});
    CHECK(snapFreq(all, Demod::Nfm, 99 * MHz, nl, nh, 100 * MHz, 2.4 * MHz, o) && closeTo(o, 99 * MHz), "inside the capture, unchanged");
    CHECK(snapFreq(all, Demod::Nfm, 90 * MHz, nl, nh, 100 * MHz, 2.4 * MHz, o) && o > 98.848 * MHz && checkPassband(all, Demod::Nfm, o, nl, nh, 100 * MHz, 2.4 * MHz) == RangeVerdict::Ok,
          "outside the capture: moved to its usable edge");
    CHECK(!snapFreq(v, Demod::Nfm, std::numeric_limits<double>::quiet_NaN(), nl, nh, 0, 0, o), "NaN");
    CHECK(!snapFreq(v, Demod::Nfm, 145 * MHz, 5, -5, 0, 0, o), "reversed passband");
}

static void testMask() {
    std::vector<float> row(100);
    auto fresh = [&]() { for (int i = 0; i < 100; i++) row[(size_t)i] = -120.f + (float)i; };
    // 100 bins of 10 kHz over 100 .. 101 MHz; allowed 100.30 .. 100.60: bins 30..59 fit, the bins next to the hidden ones are hidden as well (31..58 stay)
    fresh();
    RangeView v = V({"100.30-100.60MHz"});
    size_t n = maskSpectrumRow(row, 100 * MHz, 101 * MHz, v);
    CHECK(n == 72, "72 bins hidden (%zu)", n);
    CHECK(row[31] == -89.f && row[58] == -62.f && row[45] == -75.f, "the visible bins are untouched");
    CHECK(row[0] == -84.f && row[30] == -84.f && row[59] == -84.f && row[99] == -84.f, "hidden bins carry the noise floor of the visible part (%.1f)", row[0]);
    // a block (all modes) in an unrestricted view
    fresh();
    v = V({}, {"100.40-100.50MHz"});
    n = maskSpectrumRow(row, 100 * MHz, 101 * MHz, v);
    CHECK(n == 12, "bins 39..50 hidden (%zu)", n);
    CHECK(row[38] == -82.f && row[51] == -69.f, "the rest is untouched");
    CHECK(row[39] == row[45] && row[45] == row[50], "the hidden ones are flat");
    CHECK(row[45] != -75.f, "the blocked activity is gone");
    // a mode-limited block is about listening: the spectrum stays
    fresh();
    v = V({}, {"100.40-100.50MHz,usb"});
    CHECK(maskSpectrumRow(row, 100 * MHz, 101 * MHz, v) == 0 && row[45] == -75.f, "a mode-limited block does not hide the spectrum");
    // a mode-limited allow still shows the spectrum
    fresh();
    v = V({"100.30-100.60MHz,usb"});
    CHECK(maskSpectrumRow(row, 100 * MHz, 101 * MHz, v) == 72, "an allow with modes shows the band");
    // nothing visible
    fresh();
    v = V({"200-201MHz"});
    CHECK(maskSpectrumRow(row, 100 * MHz, 101 * MHz, v) == 100 && row[0] == -150.f && row[99] == -150.f, "nothing allowed: all blank");
    // an allowed band narrower than three bins vanishes at this zoom
    fresh();
    v = V({"100.500-100.520MHz"});
    CHECK(maskSpectrumRow(row, 100 * MHz, 101 * MHz, v) == 100, "a band of two bins: nothing safely visible");
    // not restricted, no rules, bad row
    fresh();
    CHECK(maskSpectrumRow(row, 100 * MHz, 101 * MHz, RangeView()) == 0, "no rules");
    std::vector<float> none;
    CHECK(maskSpectrumRow(none, 100 * MHz, 101 * MHz, v) == 0, "empty row");
    CHECK(maskSpectrumRow(row, 101 * MHz, 100 * MHz, v) == 0, "reversed span: nothing done (the hub never sends one)");
    // the row's own edges: allowed range covers the whole row
    fresh();
    v = V({"99-102MHz"});
    CHECK(maskSpectrumRow(row, 100 * MHz, 101 * MHz, v) == 0, "everything allowed");
    // a block at the very edge of the row
    fresh();
    v = V({}, {"100.95-101MHz"});
    n = maskSpectrumRow(row, 100 * MHz, 101 * MHz, v);
    CHECK(n == 6 && row[94] == row[99], "an edge block: bins 94..99 (%zu)", n);
    // the row can start below the capture-relative origin: absolute frequencies near 0 Hz
    fresh();
    v = V({"0-50kHz"});
    CHECK(maskSpectrumRow(row, 0, 100e3, v) > 0 && row[0] == -120.f + 0, "row from 0 Hz: the first bins are visible (%.1f)", row[0]);
}

static void testClasses() {
    CHECK(rangeClassOf(UserClass::Public) == RangeClass::Open && rangeClassOf(UserClass::Member) == RangeClass::Member && rangeClassOf(UserClass::Invited) == RangeClass::Invite, "class mapping");
    RangeClass c;
    CHECK(rangeClassFromName("public", c) && c == RangeClass::Open && rangeClassFromName("password", c) && c == RangeClass::Member && rangeClassFromName("invited", c) && c == RangeClass::Invite, "class names");
    CHECK(!rangeClassFromName("admin", c), "unknown class");

    onair_server::ConfigFile cfg;
    cfg.parse("web_allow_range=144-146MHz,nfm\n"
              "web_allow_range=7.000-7.200MHz,usb,lsb,member\n"
              "web_block_range=145.8-146MHz\n"
              "web_range_preset=ham_hf@invite\n"
              "web_range_action=clamp\n");
    WebRanges r;
    std::vector<std::string> warn;
    r.load(cfg, &warn);
    CHECK(warn.empty(), "no warnings for good lines (%zu: %s)", warn.size(), warn.empty() ? "" : warn[0].c_str());
    CHECK(r.clampMode(), "web_range_action=clamp");
    RangeView o = r.view(RangeClass::Open), m = r.view(RangeClass::Member), i = r.view(RangeClass::Invite);
    CHECK(o.restricted && o.allow.size() == 1 && o.block.size() == 1, "open: one allow, the block (%zu, %zu)", o.allow.size(), o.block.size());
    CHECK(m.allow.size() == 2, "member: both rules (%zu)", m.allow.size());
    CHECK(i.allow.size() == 1 + 10 && i.restricted, "invite: the 144-146 rule and the ten ham HF bands (%zu)", i.allow.size());
    CHECK(checkPassband(o, Demod::Usb, 7.1 * MHz, 300, 2700) == RangeVerdict::NotAllowed, "open listeners have no HF");
    CHECK(checkPassband(m, Demod::Usb, 7.1 * MHz, 300, 2700) == RangeVerdict::Ok, "members have 40 m");
    CHECK(checkPassband(i, Demod::Usb, 14.1 * MHz, 300, 2700) == RangeVerdict::Ok, "invited have 20 m from the preset");
    CHECK(checkPassband(i, Demod::Usb, 7.1 * MHz, 300, 2700) == RangeVerdict::Ok, "and 40 m from the preset too (preset has all modes)");
    CHECK(checkPassband(o, Demod::Nfm, 145.9 * MHz, -6250, 6250) == RangeVerdict::Blocked, "the block applies to every class");

    // only blocks for one class: the other classes are not restricted
    onair_server::ConfigFile c2;
    c2.parse("web_allow_range=144-146MHz,open\n");
    WebRanges r2;
    r2.load(c2, nullptr);
    CHECK(r2.view(RangeClass::Open).restricted && !r2.view(RangeClass::Member).restricted && !r2.view(RangeClass::Member).active(), "a class with no rule is unrestricted");

    // an allow-list of bad lines must not look like "no restriction" silently
    onair_server::ConfigFile c3;
    c3.parse("web_allow_range=146-144MHz\nweb_allow_range=nonsense\nweb_range_preset=nosuchpreset\n");
    WebRanges r3;
    std::vector<std::string> w3;
    r3.load(c3, &w3);
    bool loud = false;
    for (const auto& x : w3) if (x.find("NOT restricted") != std::string::npos) loud = true;
    CHECK(w3.size() >= 4 && loud, "bad lines are named and the open result is announced (%zu)", w3.size());
    CHECK(!r3.view(RangeClass::Open).active(), "nothing was loaded");

    // "all" lifts the limit for the class it is given to
    onair_server::ConfigFile c4;
    c4.parse("web_allow_range=144-146MHz\nweb_range_preset=all@member\n");
    WebRanges r4;
    r4.load(c4, nullptr);
    CHECK(checkPassband(r4.view(RangeClass::Member), Demod::Nfm, 50 * MHz, -6250, 6250) == RangeVerdict::Ok, "all@member: members tune anywhere");
    CHECK(checkPassband(r4.view(RangeClass::Open), Demod::Nfm, 50 * MHz, -6250, 6250) == RangeVerdict::NotAllowed, "open listeners stay limited");

    // a block preset
    onair_server::ConfigFile c5;
    c5.parse("web_range_preset=no_cellular_no_public_safety\n");
    WebRanges r5;
    r5.load(c5, nullptr);
    RangeView p = r5.view(RangeClass::Open);
    CHECK(!p.restricted && !p.block.empty() && !p.notes.empty() && p.notes[0].find("typical") != std::string::npos, "a block preset blocks and carries its note");
    CHECK(checkPassband(p, Demod::Nfm, 935 * MHz, -6250, 6250) == RangeVerdict::Blocked, "GSM downlink is blocked");
    CHECK(checkPassband(p, Demod::Nfm, 145 * MHz, -6250, 6250) == RangeVerdict::Ok, "2 m is not");

    // save and load again
    onair_server::ConfigFile out;
    r.save(out);
    WebRanges back;
    back.load(out, nullptr);
    CHECK(back.allowSpecs() == r.allowSpecs() && back.blockSpecs() == r.blockSpecs() && back.presetSpecs() == r.presetSpecs() && back.clampMode(), "settings round trip");
    CHECK(out.getAll("web_allow_range").size() == 2 && out.get("web_range_action") == "clamp", "one line per rule in the file");
    CHECK(out.getAll("web_range_preset").size() == 1 && out.getAll("web_range_preset")[0] == "ham_hf@invite", "preset line with its class");
    // clearing writes empty values
    r.clear();
    onair_server::ConfigFile out2;
    out2.parse(out.text());
    r.save(out2);
    WebRanges empty;
    empty.load(out2, nullptr);
    CHECK(empty.empty(), "cleared and saved: nothing left after loading");
}

static void testReplaceAndJson() {
    WebRanges r;
    std::string e;
    const unsigned g0 = r.generation();
    CHECK(r.replace({"144-146MHz,nfm", "430-440MHz"}, {"145.8-146MHz"}, {"airband@open+member"}, "clamp", e), "replace: %s", e.c_str());
    CHECK(r.generation() != g0 && r.clampMode(), "generation bumped");
    CHECK(r.view(RangeClass::Open).allow.size() == 2 + 1 && r.view(RangeClass::Invite).allow.size() == 2, "preset limited to two classes");
    const auto before = r.allowSpecs();
    CHECK(!r.replace({"144-146MHz", "bad"}, {}, {}, "", e) && !e.empty(), "a bad line is refused");
    CHECK(r.allowSpecs() == before, "and nothing changed");
    CHECK(!r.replace({}, {}, {"nosuch"}, "", e), "unknown preset refused");
    CHECK(!r.replace({}, {}, {}, "sometimes", e), "unknown action refused");
    WebRanges rn;
    CHECK(!rn.replace({"144-146MHz,note=a\nb"}, {}, {}, "", e) || rn.allowSpecs()[0].find('\n') == std::string::npos, "a newline never reaches the file");
    std::vector<std::string> many;
    for (int i = 0; i < 200; i++) many.push_back(std::to_string(i + 1) + "-" + std::to_string(i + 2) + "MHz");
    CHECK(!r.replace(many, {}, {}, "", e), "more than 128 ranges refused");
    const std::string j = r.json();
    CHECK(j.find("\"available_presets\"") != std::string::npos && j.find("ham_hf") != std::string::npos && j.find("\"action\":\"clamp\"") != std::string::npos, "admin JSON");
    const std::string rj = rangesJson(r.view(RangeClass::Open), true);
    CHECK(rj.find("\"restricted\":true") != std::string::npos && rj.find("\"allow\":[{\"lo\":") != std::string::npos && rj.find("\"action\":\"clamp\"") != std::string::npos, "welcome JSON: %s", rj.substr(0, 120).c_str());
    CHECK(rj.find("\"modes\":[\"nfm\"]") != std::string::npos, "mode lists are sent");
    const std::string none = rangesJson(RangeView(), false);
    CHECK(none.find("\"restricted\":false") != std::string::npos, "unrestricted JSON");
}

static void testPresets() {
    const auto& ps = rangePresets();
    CHECK(ps.size() >= 8, "the eight presets");
    for (const auto& p : ps) {
        std::vector<RangeRule> rules;
        bool blk = false;
        CHECK(presetRules(p.name, kAllClasses, rules, blk) && !rules.empty() && blk == p.block, "preset %s resolves", p.name);
        for (const auto& r : rules) CHECK(r.r.lo >= 0 && r.r.hi > r.r.lo && r.r.hi <= kMaxRangeHz && r.r.modes != 0, "preset %s has a sane range %g-%g", p.name, r.r.lo, r.r.hi);
        std::string d = p.description;
        CHECK(!d.empty(), "preset %s has a description", p.name);
        // every preset says it is typical, not authoritative
        if (std::string(p.name) != "all") CHECK(d.find("typical") != std::string::npos || d.find("Typical") != std::string::npos, "preset %s is marked as typical", p.name);
    }
    std::vector<RangeRule> x;
    bool b;
    CHECK(!presetRules("nope", kAllClasses, x, b), "unknown preset");
    CHECK(presetRules("HAM_HF", kAllClasses, x, b), "preset names are case-insensitive");
    // the ranges themselves: a few the host would notice if they were wrong
    RangeView ham = V({}, {}, {"ham_hf"});
    CHECK(checkPassband(ham, Demod::Usb, 14.2 * MHz, 300, 2700) == RangeVerdict::Ok, "20 m allowed");
    CHECK(checkPassband(ham, Demod::Am, 15.5 * MHz, -5000, 5000) == RangeVerdict::NotAllowed, "a shortwave broadcast band is not ham");
    RangeView bc = V({}, {}, {"broadcast_am_fm"});
    CHECK(checkPassband(bc, Demod::Wfm, 98.0 * MHz, -100000, 100000) == RangeVerdict::Ok, "FM broadcast, WFM");
    CHECK(checkPassband(bc, Demod::Am, 98.0 * MHz, -5000, 5000) == RangeVerdict::ModeNotAllowed, "AM on the FM band");
    CHECK(checkPassband(bc, Demod::Am, 1000e3, -5000, 5000) == RangeVerdict::Ok, "medium wave");
    RangeView air = V({}, {}, {"airband"});
    CHECK(checkPassband(air, Demod::Am, 121.5 * MHz, -5000, 5000) == RangeVerdict::Ok, "the guard frequency 121.5 MHz");
    RangeView both = V({}, {}, {"airband", "marine"});
    CHECK(checkPassband(both, Demod::Nfm, 156.8 * MHz, -6250, 6250) == RangeVerdict::Ok, "two presets together (marine channel 16)");
    RangeView all = V({}, {}, {"all"});
    CHECK(all.restricted && checkPassband(all, Demod::Nfm, 12.3 * MHz, -6250, 6250) == RangeVerdict::Ok, "all");
}

static void testErrConstant() {
    CHECK(std::string(err::ForbiddenRange) == "forbidden_range", "the error code in the protocol");
}

int main() {
    testParseFreq();
    testParseSpec();
    testMerge();
    testGuard();
    testCheckPassband();
    testCaptureEdge();
    testSnap();
    testMask();
    testClasses();
    testReplaceAndJson();
    testPresets();
    testErrConstant();
    return testSummary("web_ranges");
}
