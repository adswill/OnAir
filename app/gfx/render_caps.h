// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// What the renderer can afford: software OpenGL detection (Mesa llvmpipe / softpipe / swrast, Windows "GDI Generic"), the low-power draw
// mode it offers, the frame-rate floor of the frame loop on Linux, and frame-time statistics. docs/PERF-LINUX.md has the measurements.
// The policy parts are pure functions (tests/app/test_render_caps.cpp); render_caps.cpp holds the state, the hint window and the waits.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

struct App;

namespace rcaps {

// true for a renderer string of a CPU rasterizer: every pixel of every frame is drawn by the processor, beside the receiver's DSP
inline bool isSoftwareRenderer(const char* renderer) {
    if (!renderer || !*renderer) return false;
    static const char* const kSoft[] = {"llvmpipe", "softpipe", "swrast", "Software Rasterizer", "GDI Generic", "lavapipe", "SwiftShader"};
    for (const char* k : kSoft) {
        const size_t n = std::strlen(k);
        for (const char* p = renderer; *p; p++) {   // case-insensitive search
            size_t i = 0;
            while (i < n && p[i] && (p[i] | 0x20) == (k[i] | 0x20)) i++;
            if (i == n) return true;
        }
    }
    return false;
}

// The low-power draw setting: 0 not chosen yet (off; with a software renderer the hint asks once), 1 on, 2 off (the hint answered "no").
// The environment (ONAIR_LOW_POWER_DRAW=1 / 0) and --low-power-draw win over the setting.
enum Choice { Ask = 0, On = 1, Off = 2 };
inline bool lowPowerWanted(int choice, int envForce /* -1 none, 0 off, 1 on */) {
    if (envForce >= 0) return envForce == 1;
    return choice == On;
}
// the hint is shown once: software rendering, nothing chosen yet and nothing forced
inline bool showHint(bool software, int choice, int envForce) { return software && choice == Ask && envForce < 0; }

// The shortest gap between two frames. The frame loop wakes at once on input, so without a floor a stream of mouse-motion events (a
// 1000 Hz mouse) draws a frame per event; with a GPU the swap's vsync wait bounds that, with software OpenGL (no vsync) nothing does.
// floor: 1/60 s on Linux and with any software renderer (the rate the pacing already asks for while there is input), none elsewhere.
// Low-power draw: at most 30 fps while there is input, 10 fps while a receiver runs, 4 fps when nothing moves.
inline double minFrameGap(bool linuxOrSoftware, bool lowPower, bool recentInput, bool running) {
    if (lowPower) return recentInput ? 1.0 / 30 : running ? 0.1 : 0.25;
    return linuxOrSoftware ? 1.0 / 60 : 0.0;
}

// Frame-time statistics over a window of frames: percentiles of the frame-to-frame time and of the CPU work per frame.
struct FrameStats {
    std::vector<float> period, work;   // seconds
    void add(double periodS, double workS) { period.push_back((float)periodS); work.push_back((float)workS); }
    size_t size() const { return period.size(); }
    void clear() { period.clear(); work.clear(); }
    // q in [0, 1]: the value below which q of the samples lie (nearest rank); 0 when empty
    static double percentile(std::vector<float> v, double q) {
        if (v.empty()) return 0;
        q = std::max(0.0, std::min(1.0, q));
        const size_t k = std::min(v.size() - 1, (size_t)(q * (double)(v.size() - 1) + 0.5));
        std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)k, v.end());
        return v[k];
    }
};

// render_caps.cpp
void detect(const char* glRenderer);            // once, by the OpenGL back end after the context exists (never called with Metal)
bool software();                                // the renderer is a CPU rasterizer
const char* rendererName();
void forceLowPower(bool on);                    // --low-power-draw
void setUnpaced(bool on);                       // --shot: no floor (the shot runs count frames)
double frameStart();                            // glfwGetTime() when frameGate() let the current frame start
bool lowPowerDraw();                            // draw less: lower frame rate cap, simplified ADS-B map (no trails, no labels but the selected)
void setLowPowerChoice(int c);                  // View > Low-power drawing (saved)
int lowPowerChoice();
void frameGate(bool running);                   // in the frame loop before the frame is built: keeps minFrameGap() (events keep queueing)
void frameDone(double workS);                   // after the frame is presented: the statistics of DECT2_FRAMETIME=1
void hintWindow(App& app);                      // the one-time "software rendering" hint with its choice
void viewMenuItem();                            // View > Low-power drawing

}   // namespace rcaps
