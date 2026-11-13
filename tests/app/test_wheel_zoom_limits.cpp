// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// #425, #442: the mouse wheel on a plot stops at limits and never hangs the program. #465: a mode without a slot (Mode Builder, Plugins) can be selected.
// Drives the real plot code headless with wheel events; a hang ends in the alarm.
#include "gfx/plot.h"
#include "gfx/plot_util.h"
#include "gfx/spec_look.h"
#include "state/mode_slot.h"
#include "ui/touch.h"
#include "imgui.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#ifndef _WIN32
#include <csignal>
#include <unistd.h>
#endif

namespace touch {
bool on() { return false; }
const GestureFrame& gesture() { static GestureFrame g; return g; }
bool longPressHeld() { return false; }   // drag to tune (gfx/drag_tune.h): no touch here
}

static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
#ifndef _WIN32
static void onAlarm(int) { printf("FAIL: the plot hangs (timed out)\n"); fflush(stdout); _exit(3); }
#endif
static void watchdog() {
#ifndef _WIN32
    signal(SIGALRM, onAlarm);
    alarm(60);
#endif
}

struct View { double xSpan = 0, ySpan = 0, xLo = 0, yLo = 0; };

// one frame of a plot set up like the spectrum (limited) or like the other plots (no limits); wheel: notches this frame
static View frame(bool limited, float wheel) {
    static double xl = 0, xh = 0;
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(420, 150);
    if (wheel != 0) io.AddMouseWheelEvent(0, wheel);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground);
    plt::Look look;
    look.on = true;
    plt::SetNextPlotLook(look);
    View v;
    if (plt::BeginPlot(limited ? "##limited" : "##plain", ImVec2(900, 500), plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_NoDoubleReset)) {
        plt::SetupAxes(nullptr, "dB", 0, plt::AxisFlags_Inside);
        plt::SetupAxisLimits(plt::X1, 95, 105, plt::Cond_Once);
        plt::SetupAxisLinks(plt::X1, &xl, &xh);
        plt::SetupAxisFormat(plt::X1, "%freq");
        plt::SetupAxisLimits(plt::Y1, -110, -30, plt::Cond_Once);
        if (limited) {
            plt::SetupAxisConstraints(plt::X1, 95, 105);
            plt::SetupAxisSpan(plt::X1, 0.001, 10);
            plt::SetupAxisConstraints(plt::Y1, -160, 20);
            plt::SetupAxisSpan(plt::Y1, 10, 180);
        }
        plt::GetPlotPos();   // ends the setup
        const plt::Rect r = plt::GetPlotLimits();
        v.xSpan = r.X.Max - r.X.Min; v.ySpan = r.Y.Max - r.Y.Min; v.xLo = r.X.Min; v.yLo = r.Y.Min;
        plt::EndPlot();
    }
    ImGui::End();
    ImGui::Render();
    return v;
}

static View spin(bool limited, float wheel, int notches) {
    View v;
    for (int i = 0; i < 4; i++) v = frame(limited, 0);   // the mouse arrives first
    for (int i = 0; i < notches; i++) v = frame(limited, wheel);
    return v;
}

static std::string slurp(const char* rel) {
    std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main() {
    watchdog();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1000, 600);
    io.DeltaTime = 1.f / 60;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();

    // the spectrum: stops at the limits on both axes, both ways
    View v = spin(true, 1.f, 800);
    check(std::fabs(v.xSpan - 0.001) < 1e-6, "wheel in stops at the narrowest frequency span");
    check(std::fabs(v.ySpan - 10) < 1e-6, "wheel in stops at the narrowest dB span");
    View w = spin(true, 1.f, 50);
    check(w.xLo == v.xLo && w.yLo == v.yLo, "at the limit more wheel leaves the view where it is");
    v = spin(true, -1.f, 800);
    check(std::fabs(v.xSpan - 10) < 1e-6, "wheel out stops at the band");
    check(std::fabs(v.ySpan - 180) < 1e-6, "wheel out stops at the widest dB span");
    check(v.yLo >= -160 && v.yLo + v.ySpan <= 20, "wheel out stays inside the dB range");

    // any other plot, with no limits of its own: no hang, and the view stays a usable range
    v = spin(false, 1.f, 1500);
    check(std::isfinite(v.xSpan) && v.xSpan > 0 && std::isfinite(v.ySpan) && v.ySpan > 0, "a plot without limits survives zooming in");
    check(v.xSpan >= plt::numericMinSpan(100, 100) * 0.99, "the view never gets below the floor the drawing needs");
    v = spin(false, -1.f, 3000);
    check(std::isfinite(v.xSpan) && v.xSpan <= 1e15 && std::isfinite(v.ySpan) && v.ySpan <= 1e15, "a plot without limits survives zooming out");
    ImGui::DestroyContext();

    // the tick loop that hung: a step lost in the rounding of a double
    check(slook::freqTicks(100.0, 100.0 + 6e-14, 900, 7, 28, 0, 0).empty(), "frequency ticks give up on a span too small for a double");
    check(!slook::freqTicks(100.0, 100.001, 900, 7, 28, 0, 0).empty(), "frequency ticks still work at the narrowest real span");

    check(plt::zoomFactorWithin(10, 0.9, 1, 100) == 0.9, "the factor is unchanged away from a limit");
    check(plt::zoomFactorWithin(1, 0.9, 1, 100) == 1, "at the narrowest span the factor is 1");
    check(plt::zoomFactorWithin(100, 1 / 0.9, 1, 100) == 1, "at the widest span the factor is 1");

    float lo = -40, hi = -30;
    check(plt::keepTraceInView(lo, hi, -95, -85, -160, 20) && hi >= -85 && lo <= -85 && hi - lo == 10, "a view above the trace comes down to it");
    lo = -140; hi = -130;
    check(plt::keepTraceInView(lo, hi, -95, -85, -160, 20) && lo <= -95 && hi >= -95 && hi - lo == 10, "a view below the trace goes up to it");
    lo = -100; hi = -90;
    check(!plt::keepTraceInView(lo, hi, -95, -85, -160, 20) && lo == -100 && hi == -90, "a view that shows some of the trace stays");
    lo = -40; hi = -30;
    check(!plt::keepTraceInView(lo, hi, 1e9f, -1e9f, -160, 20), "no trace: nothing moves");

    // #465: the family numbers beyond the end of the per-mode arrays
    double slots[300] = {};
    check(modeslot::at(slots, 0) == &slots[0] && modeslot::at(slots, 299) == &slots[299], "a real slot is found");
    for (int f : {300, 999, 1000, 19999, 20000, 29999, -1}) check(modeslot::at(slots, f) == nullptr, "a mode without a slot has no entry");
    const std::string st = slurp("app/state/app_state.cpp");
    check(st.find("a.famGain[to]") == std::string::npos && st.find("a.famFreq[to]") == std::string::npos, "setMode does not index the per-mode arrays unchecked");
    const std::string du = slurp("app/ui/draw_ui.cpp");
    const size_t d = du.find("void drawUI(App& a, ImVec2 disp, bool draw) {");
    check(d != std::string::npos && du.find("modeBuilderRegister(a);", d) < d + 300 && du.find("pluginRegister(a);", d) < d + 400, "the plugin and Mode Builder tunings exist before the frame draws");

    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
