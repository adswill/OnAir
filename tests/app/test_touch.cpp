// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The pure parts of the touch interface (app/ui/touch_logic.h): keypad entry and frequency parsing, the gesture recogniser (pinch,
// pan, long press, swipe, fling) and the kinetic scroll.
#include "../../app/ui/touch_logic.h"
#include <cstdio>
#include <cstdarg>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static bool nearBy(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

using namespace touch;

static void keypad() {
    Keypad k;
    CHECK(k.empty() && k.shown() == "-" && k.hz() == 0, "a new keypad is empty");
    for (char c : std::string("7.1")) k.key(c);
    CHECK(k.text == "7.1" && nearBy(k.value(), 7.1) && nearBy(k.hz(), 7.1e6), "7.1 MHz: %s %.1f", k.text.c_str(), k.hz());
    k.key('.');
    CHECK(k.text == "7.1", "a second point is ignored");
    k.key('x'); k.key('-');
    CHECK(k.text == "7.1", "other keys are ignored");
    k.backspace(); k.backspace();
    CHECK(k.text == "7", "backspace: %s", k.text.c_str());
    k.backspace(); k.backspace();
    CHECK(k.text.empty(), "backspace on empty stays empty");
    k.key('0'); k.key('0'); k.key('7');
    CHECK(k.text == "7", "leading zeros collapse: %s", k.text.c_str());
    k.clear(); k.key('.'); k.key('5');
    CHECK(k.text == "0.5" && nearBy(k.value(), 0.5), "a point first gives 0.: %s", k.text.c_str());
    k.clear(); k.key('0'); k.key('.'); k.key('0');
    CHECK(k.text == "0.0" && k.empty(), "0.0 is nothing to apply");
    k.clear();
    for (int i = 0; i < 20; i++) k.key('9');
    CHECK(k.text.size() == 12, "at most 12 characters: %zu", k.text.size());
    k.clear(); k.key('4'); k.key('5'); k.key('5'); k.unit = Unit::kHz;
    CHECK(nearBy(k.hz(), 455e3), "455 kHz = %.0f Hz", k.hz());
    k.unit = Unit::GHz; k.clear(); k.key('2'); k.key('.'); k.key('4');
    CHECK(nearBy(k.hz(), 2.4e9), "2.4 GHz");
    k.unit = Unit::Hz; k.clear(); k.key('1');
    CHECK(nearBy(k.hz(), 1), "1 Hz");
    k.setHz(7.074e6);
    CHECK(k.unit == Unit::MHz && k.text == "7.074", "setHz 7.074 MHz: %s %s", k.text.c_str(), unitName(k.unit));
    k.setHz(455e3);
    CHECK(k.unit == Unit::kHz && k.text == "455", "setHz 455 kHz: %s %s", k.text.c_str(), unitName(k.unit));
    k.setHz(1.2e9);
    CHECK(k.unit == Unit::GHz && k.text == "1.2", "setHz 1.2 GHz: %s", k.text.c_str());
    k.setHz(50);
    CHECK(k.unit == Unit::Hz && k.text == "50", "setHz 50 Hz: %s", k.text.c_str());
    k.setHz(100e6);
    CHECK(k.text == "100", "setHz 100 MHz: %s", k.text.c_str());
}

static void parse() {
    double hz = 0;
    CHECK(parseFrequency("145.5", Unit::MHz, hz) && nearBy(hz, 145.5e6), "bare number in the default unit");
    CHECK(parseFrequency("145.5", Unit::kHz, hz) && nearBy(hz, 145.5e3), "default kHz");
    CHECK(parseFrequency("7100k", Unit::MHz, hz) && nearBy(hz, 7.1e6), "7100k");
    CHECK(parseFrequency("433920 kHz", Unit::MHz, hz) && nearBy(hz, 433.92e6), "433920 kHz");
    CHECK(parseFrequency("  2.4G", Unit::MHz, hz) && nearBy(hz, 2.4e9), "2.4G");
    CHECK(parseFrequency("93.9 MHz", Unit::kHz, hz) && nearBy(hz, 93.9e6), "93.9 MHz overrides the default");
    CHECK(parseFrequency("1500 HZ", Unit::MHz, hz) && nearBy(hz, 1500), "HZ any case");
    CHECK(parseFrequency("0.455 m", Unit::kHz, hz) && nearBy(hz, 455e3), "m = MHz");
    CHECK(!parseFrequency("", Unit::MHz, hz), "empty");
    CHECK(!parseFrequency("abc", Unit::MHz, hz), "text");
    CHECK(!parseFrequency("12 parsecs", Unit::MHz, hz), "unknown unit");
    CHECK(!parseFrequency("-5", Unit::MHz, hz), "negative");
    CHECK(!parseFrequency(nullptr, Unit::MHz, hz), "null");
    CHECK(mhzText(522) == "522.000" && mhzText(7.07415) == "7.07415" && mhzText(93.9) == "93.900" && mhzText(0.455) == "0.455", "mhzText: %s %s", mhzText(522).c_str(), mhzText(7.07415).c_str());
    int n = 0; const double* s = tuneSteps(n);
    CHECK(n >= 10 && s[0] == 1 && s[n - 1] == 10e6, "steps run from 1 Hz to 10 MHz");
    char b[32];
    CHECK(std::string(stepLabel(100e3, b, sizeof b)) == "100 kHz" && std::string(stepLabel(1e6, b, sizeof b)) == "1 MHz" && std::string(stepLabel(6.25e3, b, sizeof b)) == "6.25 kHz" && std::string(stepLabel(10, b, sizeof b)) == "10 Hz", "step labels");
}

static Pointer P(int id, float x, float y, bool down = true) { Pointer p; p.id = id; p.x = x; p.y = y; p.down = down; return p; }

static void gestures() {
    {   // pinch: two fingers apart 100 -> 150 -> 75, centre moving
        Gestures g;
        Pointer p[2] = {P(0, 100, 200), P(1, 200, 200)};
        GestureFrame f = g.update(0.00, p, 2);
        CHECK(!f.pinching && f.scale == 1.f, "the first two-finger frame only starts");
        p[0] = P(0, 75, 200); p[1] = P(1, 225, 200);
        f = g.update(0.02, p, 2);
        CHECK(f.pinching && nearBy(f.scale, 1.5, 1e-4) && nearBy(f.panDx, 0) && nearBy(f.cx, 150), "pinch out: scale %.3f", f.scale);
        p[0] = P(0, 112.5f, 210); p[1] = P(1, 187.5f, 210);
        f = g.update(0.04, p, 2);
        CHECK(f.pinching && nearBy(f.scale, 0.5, 1e-4) && nearBy(f.panDy, 10, 1e-4), "pinch in: scale %.3f pan %.1f", f.scale, f.panDy);
        p[0] = P(0, 112.5f, 210); p[1] = P(1, 187.5f, 210);
        p[0].x += 30; p[1].x += 30;   // both fingers slide right, same distance: a pan, no zoom
        f = g.update(0.06, p, 2);
        CHECK(f.pinching && nearBy(f.scale, 1, 1e-4) && nearBy(f.panDx, 30, 1e-4), "two-finger pan: %.1f", f.panDx);
        p[0].down = p[1].down = false;
        f = g.update(0.08, p, 2);
        CHECK(!f.pinching && f.swipe == 0, "fingers up: nothing");
    }
    {   // one-finger pan: deltas after the slop
        Gestures g;
        Pointer p = P(0, 100, 100);
        g.update(0.00, &p, 1);
        p.y = 105;   // inside the slop: still a press
        GestureFrame f = g.update(0.02, &p, 1);
        CHECK(!f.panning, "inside the slop is not a pan");
        p.y = 130;
        f = g.update(0.04, &p, 1);
        CHECK(f.panning && nearBy(f.panDy, 25), "beyond the slop: pan delta %.1f", f.panDy);
        p.y = 140;
        f = g.update(0.06, &p, 1);
        CHECK(f.panning && nearBy(f.panDy, 10), "next delta %.1f", f.panDy);
    }
    {   // long press: once, after 0.5 s, only while still
        Gestures g;
        Pointer p = P(0, 50, 50);
        int n = 0;
        for (double t = 0; t < 1.2; t += 0.05) { GestureFrame f = g.update(t, &p, 1); n += f.longPress; if (f.longPress) CHECK(t >= 0.5 - 1e-9 && t < 0.56, "long press at %.2f s", t); }
        CHECK(n == 1, "one long press, got %d", n);
        Gestures h;
        p = P(0, 50, 50);
        n = 0;
        for (double t = 0; t < 1.2; t += 0.05) { if (t > 0.2) p.x += 4; n += h.update(t, &p, 1).longPress; }
        CHECK(n == 0, "a moving finger is no long press");
        Gestures k;
        p = P(0, 50, 50);
        k.update(0, &p, 1);
        p.down = false;
        k.update(0.3, &p, 1);
        p.down = true;
        n = 0;
        for (double t = 0.35; t < 0.8; t += 0.05) n += k.update(t, &p, 1).longPress;   // a new press restarts the clock
        CHECK(n == 0, "the clock restarts with a new press");
    }
    {   // swipes
        auto run = [](float dx, float dy, double dur, int steps) {
            Gestures g;
            Pointer p = P(0, 400, 400);
            g.update(0, &p, 1);
            for (int i = 1; i <= steps; i++) { p.x = 400 + dx * i / steps; p.y = 400 + dy * i / steps; g.update(dur * i / steps, &p, 1); }
            p.down = false;
            return g.update(dur + 0.016, &p, 1);
        };
        CHECK(run(-200, 10, 0.2, 10).swipe == -1, "quick left swipe");
        CHECK(run(200, -20, 0.2, 10).swipe == 1, "quick right swipe");
        CHECK(run(60, 0, 0.2, 10).swipe == 0, "too short");
        CHECK(run(-200, 0, 0.9, 20).swipe == 0, "too slow");
        CHECK(run(-200, 150, 0.2, 10).swipe == 0, "a diagonal drag is no swipe");
        CHECK(run(10, 300, 0.2, 10).swipe == 0, "a vertical flick is no swipe");
        const GestureFrame f = run(0, -300, 0.15, 15);   // a vertical flick starts a fling: finger moved up at 2000 px/s
        CHECK(f.flickVy < -1000 && f.swipe == 0, "fling speed %.0f", f.flickVy);
        Gestures g;   // a finger that rests before lifting does not fling
        Pointer p = P(0, 100, 100);
        g.update(0, &p, 1);
        p.y = 300; g.update(0.1, &p, 1);
        g.update(0.3, &p, 1);
        p.down = false;
        CHECK(g.update(0.4, &p, 1).flickVy == 0, "a rested finger does not fling");
    }
    CHECK(isDrag(11, 0) && !isDrag(5, 5), "drag slop");
}

static void kinetic() {
    Kinetic k;
    k.fling(30);
    CHECK(!k.moving(), "a slow lift does not fling");
    k.fling(-2000);
    CHECK(k.moving() && k.vel == -2000, "fling");
    float total = 0, last = 1e9f;
    int frames = 0;
    bool mono = true;
    while (k.moving() && frames < 2000) {
        const float d = k.step(1.f / 60);
        if (std::fabs(d) > std::fabs(last) + 1e-4f) mono = false;
        last = d; total += d; frames++;
    }
    CHECK(mono, "the steps only shrink");
    CHECK(!k.moving() && frames > 30 && frames < 600, "stops after %d frames", frames);
    CHECK(total < -400 && total > -520, "distance about v/friction = 500 px: %.1f", total);   // 2000 / 4
    Kinetic a, b;   // frame-rate independent: 30 and 120 fps travel the same distance
    a.fling(1500); b.fling(1500);
    float da = 0, db = 0;
    for (int i = 0; i < 400; i++) da += a.step(1.f / 30);
    for (int i = 0; i < 1600; i++) db += b.step(1.f / 120);
    CHECK(std::fabs(da - db) / db < 0.12f, "frame rate independent: %.1f vs %.1f", da, db);
    Kinetic s;
    s.fling(2000); s.stop();
    CHECK(s.step(0.016f) == 0 && !s.moving(), "stop");
}

static void autoStart() {
    CHECK(autoTouch(800, 480, false) && autoTouch(1024, 600, false) && autoTouch(720, 720, false), "handhelds start in touch");
    CHECK(!autoTouch(1920, 1080, false) && !autoTouch(1366, 768, false) && !autoTouch(1280, 680, false), "laptops do not");
    CHECK(!autoTouch(800, 480, true), "a saved choice is kept");
    CHECK(!autoTouch(0, 0, false), "unknown screen size");
    // #139: an install from before 3.7 has "touchUi" = false saved by every save; that is not a choice, so the small screen still starts in touch once
    CHECK(autoTouch(640, 480, touchChosen(false, false)), "an upgrade on a 640 x 480 screen starts in touch once");
    CHECK(!autoTouch(640, 480, touchChosen(false, true)), "after the automatic start, turning it off is kept");
    CHECK(touchChosen(true, false), "touch already on");
}

// #139: the control size and the shell's frame (touch_logic.h shellLayout)
static void controlSize() {
    CHECK(touchSizeFactor(kTouchSizeDefault) == 1.f, "Default is the size the touch interface always had");
    for (int i = 1; i < kTouchSizes; i++) CHECK(touchSizeFactor(i) > touchSizeFactor(i - 1), "sizes grow from Small to Extra large (%d)", i);
    CHECK(clampTouchSize(-3) == 0 && clampTouchSize(99) == kTouchSizes - 1 && touchSizeFactor(99) == touchSizeFactor(kTouchSizes - 1), "out of range is clamped");
    CHECK(touchTarget(1, touchSizeFactor(0)) >= 32.f && nearBy(touchTarget(1, 1), 48) && nearBy(touchTarget(2, 1), 96), "targets: Small >= 32 px, Default 48 px per display scale");
    for (int i = 0; i < kTouchSizes; i++) CHECK(touchSizeNames()[i] && *touchSizeNames()[i], "a name for size %d", i);

    const float gaps = 7 * 10.f + 2 * 12.f;   // applyMetrics' item spacing and window padding at display scale 1
    // a desktop-sized display at the default size: exactly the layout from before (top 1.1 targets, rows 1.15, one row when wide)
    ShellLayout big = shellLayout(1920, 1080, 1, 1, gaps);
    CHECK(nearBy(big.topH, 48 * 1.1f, 1e-3) && nearBy(big.rowH, 48 * 1.15f, 1e-3) && big.dockRows == 1 && !big.stepInMore && !big.shortScreen && !big.statusOnTop, "1920 x 1080 unchanged");
    ShellLayout tab = shellLayout(800, 1280, 1, 1, gaps);
    CHECK(tab.dockRows == 2 && tab.statusOnTop && !tab.shortScreen && nearBy(tab.rowH, 48 * 1.15f, 1e-3), "a portrait tablet: two rows, status on top");
    ShellLayout tabHiDpi = shellLayout(1600, 2560, 2, 1, 2 * gaps);
    CHECK(tabHiDpi.dockRows == 2 && nearBy(tabHiDpi.target, 96), "the same tablet at display scale 2");

    // 640 x 480 (#139): before, two 55 px dock rows and a 53 px top bar; now one row without Step and tighter bars
    const float sp = 8, pad = 10;
    ShellLayout old; old.target = 48; old.topH = 48 * 1.1f; old.rowH = 48 * 1.15f; old.dockRows = 2;
    const float before = shellBodyHeight(old, 480, sp, pad);
    ShellLayout s640 = shellLayout(640, 480, 1, 1, gaps);
    CHECK(s640.shortScreen && s640.dockRows == 1 && s640.stepInMore && !s640.statusOnTop, "640 x 480: a short screen, one dock row, Step in More");
    const float now = shellBodyHeight(s640, 480, sp, pad);
    CHECK(now >= before + 70, "640 x 480 default size: the panels get %.0f px instead of %.0f", now, before);
    std::printf("640 x 480 body height: before %.0f px, Default %.0f px", before, now);
    float prev = 1e9;
    for (int i = 0; i < kTouchSizes; i++) {
        ShellLayout L = shellLayout(640, 480, 1, touchSizeFactor(i), gaps);
        const float h = shellBodyHeight(L, 480, sp, pad);
        std::printf(", %s %.0f", touchSizeNames()[i], h);
        CHECK(h <= prev + 1e-3, "a larger control size never gives the panels more room (%d)", i);
        CHECK(h > 0, "the panels keep some room at %s", touchSizeNames()[i]);
        prev = h;
        // the smallest one-row buttons keep 0.8 of a target
        if (L.dockRows == 1 && L.stepInMore) CHECK((640 - gaps) * 0.8f / kDockOneRowWeightNoStep >= 0.8f * L.target - 1e-3, "one-row buttons wide enough at %d", i);
    }
    std::printf("\n");
    CHECK(shellLayout(640, 480, 1, touchSizeFactor(4), gaps).dockRows == 2, "Extra large on 640 x 480: the buttons no longer fit one row");
    CHECK(shellLayout(800, 480, 1, 1, gaps).dockRows == 1 && shellLayout(320, 240, 1, 1, gaps).dockRows == 2, "800 x 480 one row; 320 x 240 two");
    CHECK(shellLayout(640, 480, 0, 1, gaps).target == 48, "a zero display scale counts as 1");
    CHECK(shellBodyHeight(shellLayout(200, 150, 1, 1.4f, gaps), 150, sp, pad) == 0, "a tiny display never gives a negative body");
}

int main() {
    controlSize();
    autoStart();
    keypad();
    parse();
    gestures();
    kinetic();
    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
