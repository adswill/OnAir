// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// How often the main window is redrawn: the policy of paceFrame() (main.cpp) as a pure function, so it can be tested and benchmarked
// without a window (tests/app/test_frame_pace.cpp, tests/bench/bench_ui_power.cpp). The loop sleeps in glfwWaitEventsTimeout() for the
// interval; input wakes it at once. 0 = no pacing (the --shot runs count frames).
#pragma once

namespace pace {

struct State {
    bool shot = false;              // --shot: the unpaced loop its frame counts rely on
    bool shown = true;              // visible and not minimised
    bool occluded = false;          // shown, but no part of it can be seen (covered by other windows, another Space, screen off)
    bool popOut = false;            // the pop-out video window is open: it keeps playing whatever the main window does
    bool noOsWindows = false;       // the touch shell draws without OS windows
    bool reduceCpuHidden = true;    // View > Reduce CPU when hidden
    bool focused = true;
    bool recentInput = false;       // typing, dragging, hovering in the last second
    bool video = false;             // a TV service is playing
    bool running = false;           // a receiver runs: spectrum and waterfall move
    bool liveWaterfall = false;     // running and the waterfall is not paused: a shown window keeps 30 fps, focused or not (#246)
    bool fastWaterfall = false;     // the waterfall makes fast rows (the CW preset): a focused window draws 60 a second so the rows scroll in small steps
    bool onBattery = false;         // the computer runs on its battery
    bool batterySaver = true;       // View > Power > Low power mode is not off (onBattery then means: it applies now)
};

inline double shownInterval(const State& s) {
    const bool saver = s.onBattery && s.batterySaver;
    if (s.video) return 1.0 / 60;                               // the picture keeps its frame rate, on battery too
    if (s.recentInput) return saver ? 1.0 / 30 : 1.0 / 60;     // typing, dragging
    if (s.running && s.focused && s.liveWaterfall && s.fastWaterfall && !saver) return 1.0 / 60;   // (not on battery power-saving)
    if (s.running) return s.focused || (s.liveWaterfall && !saver) ? 1.0 / 30 : saver ? 0.1 : 1.0 / 15;   // spectrum and waterfall (#246)
    return s.focused ? 0.1 : 0.25;                              // nothing is moving
}

inline double interval(const State& s) {
    if (s.shot) return 0;
    const double v = shownInterval(s);
    if (!s.popOut && !s.noOsWindows) {
        if (!s.shown) return s.reduceCpuHidden ? 0.1 : 0.25;   // nothing to see (the hidden frame is skipped with the CPU saver)
        if (s.occluded) return v > 0.1 ? v : 0.1;               // nothing can be seen: at most 10 fps (the spectrum and the player go on)
    }
    return v;
}

}   // namespace pace
