// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The pure parts of the touch interface: no ImGui, no App, so the unit tests (tests/app/test_touch.cpp) run them directly.
//   - Keypad: the number entry behind the on-screen numeric keypad (digits, '.', backspace, a unit) and parseFrequency().
//   - Gestures: a recogniser fed with up to two pointers per frame (pinch scale, two-finger pan, one-finger pan, long press, swipe).
//   - Kinetic: the inertia of a drag-scrolled list (a fling keeps scrolling and slows down).
// Why a recogniser of our own: GLFW and Dear ImGui hand us one pointer (the touch is emulated as a mouse); a backend with real
// multi-touch (or a test) feeds the second pointer, and everything else (long press, swipe, kinetic scroll) works from one.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace touch {

// ------------------------------------------------------------------------------------------------ the numeric keypad
enum class Unit { Hz = 0, kHz = 1, MHz = 2, GHz = 3 };
inline double unitHz(Unit u) { static const double m[] = {1.0, 1e3, 1e6, 1e9}; return m[(int)u]; }
inline const char* unitName(Unit u) { static const char* n[] = {"Hz", "kHz", "MHz", "GHz"}; return n[(int)u]; }

// A number being typed. key() takes '0'..'9' and '.', anything else is ignored; at most one point, 12 characters, and leading zeros
// collapse ("007" is "7", "0.5" stays). value() is the number as typed (0 for an empty entry).
struct Keypad {
    std::string text;
    Unit unit = Unit::MHz;
    void clear() { text.clear(); }
    void backspace() { if (!text.empty()) text.pop_back(); }
    void key(char c) {
        if (c == '.') {
            if (text.find('.') != std::string::npos || text.size() >= 12) return;
            if (text.empty()) text = "0";
            text += '.';
            return;
        }
        if (c < '0' || c > '9' || text.size() >= 12) return;
        if (text == "0") text.clear();   // no leading zeros: "0" then "7" is "7"
        text += c;
    }
    bool empty() const { return text.empty() || value() == 0.0; }   // nothing to apply
    double value() const { return text.empty() ? 0.0 : atof(text.c_str()); }
    double hz() const { return value() * unitHz(unit); }
    // the entry as shown: "7.1" or "-" when empty
    std::string shown() const { return text.empty() ? "-" : text; }
    // start from a frequency in Hz, in the unit that reads best ("7.1" MHz, "455" kHz); the entry is then replaced by the first key
    void setHz(double hz) {
        unit = hz >= 1e9 ? Unit::GHz : hz >= 1e6 ? Unit::MHz : hz >= 1e3 ? Unit::kHz : Unit::Hz;
        char b[40]; snprintf(b, sizeof b, "%.6f", hz / unitHz(unit));
        text = b;
        while (text.find('.') != std::string::npos && (text.back() == '0' || text.back() == '.')) { const bool dot = text.back() == '.'; text.pop_back(); if (dot) break; }
        if (text == "0") text.clear();
    }
};

// "145.5", "145.5 MHz", "7100k", "2.4G", "433920 kHz": a number with an optional unit (Hz, k, kHz, M, MHz, G, GHz, any case) in Hz.
// A bare number is in defaultUnit. False for text that is not a positive number.
inline bool parseFrequency(const char* s, Unit defaultUnit, double& hz) {
    if (!s) return false;
    while (*s == ' ') s++;
    char* end = nullptr;
    const double v = strtod(s, &end);
    if (end == s || !std::isfinite(v) || v < 0) return false;
    while (*end == ' ') end++;
    Unit u = defaultUnit;
    std::string t;
    for (const char* p = end; *p; p++) if (*p != ' ') t += (char)tolower((unsigned char)*p);
    if (!t.empty()) {
        if (t == "hz") u = Unit::Hz;
        else if (t == "k" || t == "khz") u = Unit::kHz;
        else if (t == "m" || t == "mhz") u = Unit::MHz;
        else if (t == "g" || t == "ghz") u = Unit::GHz;
        else return false;
    }
    hz = v * unitHz(u);
    return true;
}

// A frequency in MHz as text: at least 3 decimals ("522.000"), more only when it needs them (up to 6: "7.07415").
inline std::string mhzText(double mhz) {
    char b[48]; snprintf(b, sizeof b, "%.6f", mhz);
    std::string s = b;
    while (s.size() >= 4 && s.back() == '0' && s[s.size() - 4] != '.') s.pop_back();
    return s;
}

// The tuning step choices of the big up / down buttons, 1 Hz .. 10 MHz.
inline const double* tuneSteps(int& n) { static const double s[] = {1, 10, 100, 1e3, 5e3, 6.25e3, 9e3, 10e3, 12.5e3, 25e3, 100e3, 200e3, 1e6, 10e6}; n = (int)(sizeof s / sizeof *s); return s; }
inline const char* stepLabel(double hz, char* buf, size_t n) {
    if (hz >= 1e6) snprintf(buf, n, "%g MHz", hz / 1e6);
    else if (hz >= 1e3) snprintf(buf, n, "%g kHz", hz / 1e3);
    else snprintf(buf, n, "%g Hz", hz);
    return buf;
}

// ------------------------------------------------------------------------------------------------ gestures
struct Pointer { int id = 0; float x = 0, y = 0; bool down = false; };

// What one frame of input meant. pinch and pan are changes since the last frame (scale 1 and 0 when nothing happens).
struct GestureFrame {
    bool pinching = false, panning = false;
    float scale = 1.f;            // ratio of the finger distance now to the last frame (> 1: fingers moved apart: zoom in)
    float panDx = 0, panDy = 0;   // centre movement of the pinch, or the finger's movement in a one-finger pan
    float cx = 0, cy = 0;         // the centre of the gesture
    bool longPress = false;       // one frame: a finger rested long enough without moving
    int swipe = 0;                // one frame, at release: -1 swiped left, +1 swiped right (a quick, mostly horizontal flick), 0 none
    float flickVx = 0, flickVy = 0;   // at release: the finger's speed in px/s (starts a kinetic scroll)
};

struct GestureParams {
    float slop = 10.f;           // px a finger may wander and still be a "press"
    double longPressSec = 0.5;
    float swipeMinDist = 80.f;   // px
    double swipeMaxSec = 0.4;
    float swipeDominance = 2.f;  // |dx| must be this many times |dy|
};

class Gestures {
public:
    GestureParams P;
    // t in seconds; p[0..n-1] the pointers that exist this frame (down or up). One call per frame.
    GestureFrame update(double t, const Pointer* p, int n) {
        GestureFrame f;
        const Pointer* d[2] = {nullptr, nullptr};
        int nd = 0;
        for (int i = 0; i < n && nd < 2; i++) if (p[i].down) d[nd++] = &p[i];
        if (nd >= 2) {   // two fingers: pinch and pan about their centre
            const float dist = std::hypot(d[0]->x - d[1]->x, d[0]->y - d[1]->y);
            const float cx = (d[0]->x + d[1]->x) * 0.5f, cy = (d[0]->y + d[1]->y) * 0.5f;
            if (twoLast_ && dist > 1.f && lastDist_ > 1.f) {
                f.pinching = true; f.scale = dist / lastDist_;
                f.panDx = cx - lastCx_; f.panDy = cy - lastCy_;
            }
            twoLast_ = true; lastDist_ = dist; lastCx_ = cx; lastCy_ = cy;
            f.cx = cx; f.cy = cy;
            one_ = false; moved_ = true; longDone_ = true;   // a second finger ends the one-finger story
            return f;
        }
        twoLast_ = false;
        if (nd == 1) {
            const Pointer& q = *d[0];
            if (!one_) { one_ = true; moved_ = false; longDone_ = false; sx_ = lx_ = q.x; sy_ = ly_ = q.y; st_ = lt_ = t; vx_ = vy_ = 0; id_ = q.id; }
            else {
                const double dt = std::max(1e-3, t - lt_);
                if (std::hypot(q.x - sx_, q.y - sy_) > P.slop) moved_ = true;
                if (moved_) {
                    f.panning = true; f.panDx = q.x - lx_; f.panDy = q.y - ly_;
                    const float a = 0.5f;   // speed: a short running average, so one jittery frame does not decide the fling
                    vx_ = vx_ * (1 - a) + (float)((q.x - lx_) / dt) * a; vy_ = vy_ * (1 - a) + (float)((q.y - ly_) / dt) * a;
                }
                lx_ = q.x; ly_ = q.y; lt_ = t;
            }
            f.cx = q.x; f.cy = q.y;
            if (!moved_ && !longDone_ && t - st_ >= P.longPressSec) { f.longPress = true; longDone_ = true; }
            return f;
        }
        if (one_) {   // the finger went up: a swipe, or the start of a fling
            one_ = false;
            const float dx = lx_ - sx_, dy = ly_ - sy_;
            f.cx = lx_; f.cy = ly_;
            if (moved_ && t - st_ <= P.swipeMaxSec && std::fabs(dx) >= P.swipeMinDist && std::fabs(dx) >= P.swipeDominance * std::fabs(dy)) f.swipe = dx < 0 ? -1 : 1;
            if (moved_ && t - lt_ < 0.1) { f.flickVx = vx_; f.flickVy = vy_; }   // a finger that stopped before lifting does not fling
        }
        return f;
    }
    void reset() { one_ = twoLast_ = false; }
private:
    bool one_ = false, moved_ = false, longDone_ = false, twoLast_ = false;
    float sx_ = 0, sy_ = 0, lx_ = 0, ly_ = 0, vx_ = 0, vy_ = 0, lastDist_ = 0, lastCx_ = 0, lastCy_ = 0;
    double st_ = 0, lt_ = 0;
    int id_ = 0;
};

// ------------------------------------------------------------------------------------------------ kinetic scrolling
// A fling keeps a list moving: velocity decays by friction (per second, exponential), and stops under a floor. step(dt) returns
// the distance to scroll this frame.
struct Kinetic {
    float vel = 0;            // px/s, the sign is the direction
    float friction = 4.0f;    // 1/s: the speed falls to 1/e in 0.25 s
    float stopBelow = 8.f;    // px/s
    void fling(float v) { vel = std::fabs(v) > 60.f ? v : 0.f; }   // a slow lift is not a fling
    void stop() { vel = 0; }
    bool moving() const { return vel != 0; }
    float step(float dt) {
        if (vel == 0) return 0;
        const float d = vel * dt;
        vel *= std::exp(-friction * dt);
        if (std::fabs(vel) < stopBelow) vel = 0;
        return d;
    }
};

// Whether a drag (px moved so far) is a scroll gesture and not a click on a row: beyond the slop.
inline bool isDrag(float dx, float dy, float slop = 10.f) { return std::hypot(dx, dy) > slop; }

// First start on a small screen (a handheld, a 7 inch Raspberry Pi panel, a cyberdeck, #139): the touch interface is the better start than a
// desktop layout squeezed into 800 x 480. Only when the user never chose (no saved touch setting) and the usable screen is small.
// Whether the user already chose: the automatic start was offered once (touchAutoSeen), or the touch interface is on. The saved "touchUi" key alone
// is not a choice: every save writes it (false by default), so an install from before 3.7 never got the automatic start (#139).
inline bool touchChosen(bool savedTouchOn, bool autoSeen) { return savedTouchOn || autoSeen; }
inline bool autoTouch(int workW, int workH, bool chosenBefore) {
    if (chosenBefore || workW <= 0 || workH <= 0) return false;
    return workW <= 1100 || workH <= 560;   // 800 x 480, 1024 x 600, 720 x 720: yes; a 1280 x 720 laptop: no
}

}   // namespace touch
