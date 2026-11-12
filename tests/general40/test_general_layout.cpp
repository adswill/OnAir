// The General receiver screen of 4.0 (docs/GENERAL-4.0.md): the logic under the layout and the VFO, without a window.
//  - the layout text in the preferences: round trip, old / foreign / damaged text gives the defaults, unknown keys skipped, values clamped
//  - the sidebar width for a window width: hides itself in a narrow window, the button shows it anyway; the toggle
//  - the VFO hit test (edges, body, centre line of USB / LSB, a band too narrow to grab its edges) and the drag maths, with snap and
//    with a step that does not divide the frequency, a grab off the centre, frequencies from LF to 6 GHz
//  - the frequency readout digits: place, typing a digit, the wheel (trackpad fractions add up), the limits
//  - the Auto demodulator from the band plan service and the built-in ranges
#include "general/general_layout.h"
#include "ui/freq_digits.h"
#include <cmath>
#include <cstdio>
#include <string>

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace genlayout;

static void testPrefs() {
    Layout L;
    L.sideW = 333; L.sideOpen = false; L.specFrac = 0.61f; L.drawerOpen = true; L.drawerH = 300; L.open = bit(SecRadio) | bit(SecLog); L.snap = false;
    Layout R;
    CHECK(decode(encode(L), R), "round trip decodes");
    CHECK(std::fabs(R.sideW - 333) < 0.5f && !R.sideOpen && std::fabs(R.specFrac - 0.61f) < 1e-3f && R.drawerOpen && std::fabs(R.drawerH - 300) < 0.5f &&
          R.open == (bit(SecRadio) | bit(SecLog)) && !R.snap, "round trip keeps every field: %s", encode(R).c_str());
    // nothing saved yet (an old profile) and damaged or foreign text: the defaults, never a crash
    for (const char* s : {"", "garbage", ";;;", "w=abc;v=x", "0.5,0.5,1", "v=;w=;", "=1;=2"}) {
        Layout D; D.sideW = 1;
        CHECK(!decode(s, D) && D == Layout{}, "'%s' gives the defaults", s);
    }
    // values out of range are clamped; unknown keys (a later version's) are skipped; missing keys keep their default
    Layout C;
    CHECK(decode("v=1;w=99999;f=5;dh=1;zz=7;future=abc;s=ffffffff", C), "clamped text decodes");
    CHECK(C.sideW == kSideMax && C.specFrac == kFracMax && C.drawerH == kDrawerMin && C.open == kAllSections && C.sideOpen && C.snap, "clamped: %s", encode(C).c_str());
    Layout M;
    CHECK(decode("v=1;w=250", M) && M.sideW == 250 && M.specFrac == Layout{}.specFrac && M.open == kDefaultOpen, "missing keys keep defaults");
    Layout N;
    CHECK(decode("v=1;w=-5;f=-1", N) && N.sideW == kSideMin && N.specFrac == kFracMin, "negative values clamped");
    // the default open sections: status, source, radio and audio; the rest folded
    CHECK((kDefaultOpen & bit(SecRadio)) && !(kDefaultOpen & bit(SecTools)), "default sections");
    // a second round trip is identical text (no drift from rounding)
    CHECK(encode(R) == encode(L), "stable text");
}

static void testSide() {
    Layout L;
    bool hid = false;
    CHECK(sideWidth(1600, L, false, &hid) == kSideDefault && !hid, "wide window: the saved width");
    L.sideW = 900;
    CHECK(sideWidth(1600, L, false, &hid) == kSideMax, "clamped to the maximum");
    L.sideW = 300;
    // 640 points: 280 left after the plot minimum: narrower sidebar
    CHECK(std::fabs(sideWidth(640, L, false, &hid) - 280) < 0.01f && !hid, "narrowed: %.1f", sideWidth(640, L, false));
    // 500 points: no room: hidden, the toggle shows it anyway (70 % of the width at most)
    CHECK(sideWidth(500, L, false, &hid) == 0 && hid, "narrow window hides the sidebar");
    bool forced = false;
    toggleSide(L, forced, hid);
    CHECK(forced && L.sideOpen && sideWidth(500, L, forced) == 300 * 1.0f, "button shows it: %.1f", sideWidth(500, L, forced));
    CHECK(sideWidth(400, L, true) <= 400 * 0.7f + 0.01f, "forced at most 70 %%");
    toggleSide(L, forced, hid);
    CHECK(!L.sideOpen && !forced && sideWidth(1600, L, false) == 0, "second press hides it");
    toggleSide(L, forced, false);
    CHECK(L.sideOpen && !forced && sideWidth(1600, L, false) == 300, "third press shows it again");
    CHECK(sideWidth(0, L, false) == 0 && sideWidth(-10, L, true) == 0, "no width");
}

static void testHit() {
    // a 12.5 kHz band from x 100 to 200 with the centre at 150, grab 5 px
    CHECK(hitTest(101, 100, 200, 150, 5, true, true) == Hit::EdgeLo, "low edge");
    CHECK(hitTest(203, 100, 200, 150, 5, true, true) == Hit::EdgeHi, "high edge");
    CHECK(hitTest(150, 100, 200, 150, 5, true, true) == Hit::Body, "inside: body");
    CHECK(hitTest(120, 100, 200, 150, 5, true, true) == Hit::Body, "inside off centre: body");
    CHECK(hitTest(260, 100, 200, 150, 5, true, true) == Hit::None, "outside: nothing");
    CHECK(hitTest(150, 200, 100, 150, 5, true, true) == Hit::Body, "edges in either order");
    // USB: centre at 100, passband 110..200; only the outer (high) edge moves; the centre line can be grabbed
    CHECK(hitTest(111, 110, 200, 100, 5, false, true) == Hit::Body, "USB inner edge is not a handle");
    CHECK(hitTest(198, 110, 200, 100, 5, false, true) == Hit::EdgeHi, "USB outer edge");
    CHECK(hitTest(101, 110, 200, 100, 5, false, true) == Hit::Body, "USB centre line grabs the VFO");
    // LSB mirror
    CHECK(hitTest(12, 10, 90, 100, 5, true, false) == Hit::EdgeLo && hitTest(88, 10, 90, 100, 5, true, false) == Hit::Body, "LSB");
    // a band 8 px wide (CW at a wide zoom): only the body, a little wider than the band so it can be caught
    CHECK(hitTest(100, 100, 108, 104, 5, true, true) == Hit::Body && hitTest(97, 100, 108, 104, 5, true, true) == Hit::Body &&
          hitTest(120, 100, 108, 104, 5, true, true) == Hit::None, "tiny band: body only");
    // both edges within reach (a 14 px band): the nearer one
    CHECK(hitTest(104, 100, 112, 106, 5, true, true) == Hit::Body && hitTest(104, 100, 114, 107, 5, true, true) == Hit::EdgeLo, "12 px band is tiny (under 2.5 grabs), 14 px is not");
    CHECK(hitTest(101, 100, 120, 110, 5, true, true) == Hit::EdgeLo && hitTest(119, 100, 120, 110, 5, true, true) == Hit::EdgeHi, "nearer edge wins");
}

static void testDrag() {
    // grabbed 3 kHz above the centre of a VFO on 145.500 MHz; the mouse moves to 145.5234 MHz: the centre follows, snapped to 12.5 kHz
    const double c0 = 145.5e6, grab = 3000;
    CHECK(std::fabs(dragCentreHz(145.5234e6, grab, 12500, true) - 145.525e6) < 1e-3, "snap to 12.5 kHz: %.1f", dragCentreHz(145.5234e6, grab, 12500, true));
    CHECK(std::fabs(dragCentreHz(145.5234e6, grab, 12500, false) - 145.5204e6) < 1e-3, "no snap: exact");
    CHECK(std::fabs(dragCentreHz(c0 + grab, grab, 12500, true) - c0) < 1e-3, "no move: the same frequency");
    // 8.33 kHz (a step that does not divide the MHz): multiples of the step
    const double s833 = 25000.0 / 3;
    const double c = dragCentreHz(118.0101e6, 0, s833, true);
    CHECK(std::fabs(std::fmod(c, s833)) < 1e-3 || std::fabs(std::fmod(c, s833) - s833) < 1e-3, "8.33 kHz multiple: %.3f", c);
    // a broadcast station at 6 GHz and one at 153 kHz: no precision trouble
    CHECK(std::fabs(dragCentreHz(5.9999976e9, 0, 5000, true) - 6.0e9) < 1e-3, "6 GHz");
    CHECK(std::fabs(dragCentreHz(155.2e3, 2e3, 9000, true) - 153e3) < 1e-6, "LW 153 kHz on a 9 kHz raster");
    CHECK(dragCentreHz(1234.5, 0, 0, true) == 1234.5, "step 0: no snap");
    // edges: symmetric width is twice the distance; USB / LSB from the 300 Hz inner edge
    CHECK(edgeWidthHz(0, -6250, 300) == 12500 && edgeWidthHz(0, 6250, 300) == 12500, "symmetric");
    CHECK(edgeWidthHz(1, 3000, 300) == 2700 && edgeWidthHz(2, -3000, 300) == 2700, "USB / LSB");
    CHECK(edgeWidthHz(1, -500, 300) < 0, "USB dragged past the centre: negative (the caller clamps)");
    // the wheel over the VFO: from the nearest step
    CHECK(wheelTuneHz(145.5031e6, 1, 12500) == 145.5125e6 && wheelTuneHz(145.5031e6, -2, 12500) == 145.475e6, "wheel steps");
    CHECK(wheelTuneHz(7.1e6, 3, 0) == 7.1e6, "wheel with no step");
}

static void testDigits() {
    using namespace fdigits;
    CHECK(placeOf(kDigits - 1) == 1 && placeOf(kDigits - 4) == 1000 && placeOf(0) == 1000000000000LL, "places");
    const long long hz = 145500000;   // 0000145.500.000
    CHECK(digitAt(hz, kDigits - 7) == 5 && digitAt(hz, kDigits - 8) == 4 && digitAt(hz, kDigits - 1) == 0, "digits of 145.5 MHz");
    // typing 7 over the 1 MHz digit: 145 -> 147; over a leading zero: 1 GHz added
    CHECK(hz + typeDelta(hz, kDigits - 7, 7) == 147500000, "type 7 over the MHz digit");
    CHECK(hz + typeDelta(hz, kDigits - 10, 1) == 1145500000, "type over a leading zero");
    CHECK(typeDelta(hz, kDigits - 7, 5) == 0, "typing the same digit changes nothing");
    // the wheel: a notch is a step; trackpad tenths add up to one step and keep the rest
    float acc = 0;
    CHECK(wheelSteps(acc, 1.f) == 1 && acc == 0, "one notch");
    int got = 0;
    for (int i = 0; i < 25; i++) got += wheelSteps(acc, 0.1f);
    CHECK(got == 2 && acc > 0.4f && acc < 0.6f, "trackpad: 25 x 0.1 gives 2 steps, %.2f kept", acc);
    acc = 0;
    CHECK(wheelSteps(acc, -2.f) == -2, "two notches down");
    // carry: 145.999 MHz + 1 kHz -> 146.000 MHz; down past zero stops at the low limit; past the top at the high limit
    CHECK(std::fabs(applyMhz(145999000, 1000, 0.001, 6000) - 146.0) < 1e-12, "carry over");
    CHECK(applyMhz(5000, -10000, 0.001, 6000) == 0.001, "low limit");
    CHECK(applyMhz(5999000000LL, 10000000LL, 0.001, 6000) == 6000, "high limit");
}

static void testAuto() {
    const AutoPick fm = autoPick(builtInService(98.4e6), 98.4e6, 0);
    CHECK(fm.ok && fm.demod == 1 && fm.widthHz == 180e3, "FM broadcast: WFM 180 kHz");
    CHECK(autoPick("fm", 98.4e6, 200e3).widthHz == 200e3 && autoPick("fm", 98.4e6, 1e6).widthHz == 180e3, "the plan's width when it is a WFM width");
    const AutoPick mw = autoPick(builtInService(999e3), 999e3, 0);
    CHECK(mw.ok && mw.demod == 2 && mw.widthHz == 9e3, "MW: AM");
    CHECK(autoPick(builtInService(125.0e6), 125.0e6, 0).demod == 2, "airband: AM");
    CHECK(autoPick(builtInService(156.8e6), 156.8e6, 0).demod == 0, "marine 16: NFM");
    CHECK(autoPick(builtInService(446.00625e6), 446.00625e6, 0).demod == 0, "PMR446: NFM");
    CHECK(autoPick(builtInService(7.1e6), 7.1e6, 0).demod == 4, "40 m voice: LSB");
    CHECK(autoPick(builtInService(14.2e6), 14.2e6, 0).demod == 3, "20 m voice: USB");
    CHECK(autoPick(builtInService(14.03e6), 14.03e6, 0).demod == 5, "20 m CW segment: CW");
    CHECK(autoPick(builtInService(5.36e6), 5.36e6, 0).demod == 3, "60 m: USB");
    CHECK(autoPick(builtInService(145.5e6), 145.5e6, 0).demod == 0 && autoPick(builtInService(144.3e6), 144.3e6, 0).demod == 3, "2 m: FM above 144.5, SSB below");
    CHECK(autoPick(builtInService(439.0e6), 439.0e6, 0).demod == 0, "70 cm repeaters: NFM");
    CHECK(!autoPick(builtInService(600e6), 600e6, 0).ok && !autoPick("tv", 600e6, 8e6).ok && !autoPick("", 3e9, 0).ok, "no rule: no change");
    CHECK(std::string(builtInService(2.4e9)).empty(), "no built-in range at 2.4 GHz");
}

int main() {
    testPrefs();
    testSide();
    testHit();
    testDrag();
    testDigits();
    testAuto();
    std::printf("general_layout: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
