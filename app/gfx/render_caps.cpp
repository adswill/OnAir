// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Software-renderer detection, low-power drawing, the frame-rate floor and frame-time statistics (render_caps.h, docs/PERF-LINUX.md).
#include "render_caps.h"
#include "app.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#if defined(__linux__)
#include <dirent.h>
#include <sys/resource.h>
#endif

namespace rcaps {
namespace {
bool gSoftware = false;
std::string gRenderer;
int gChoice = -1;          // the saved setting, read on first use
int gForce = -2;           // -2 not read yet, -1 none, 0 / 1 forced by the environment or the flag
bool gUnpaced = false;     // --shot: the unpaced loop its frame counts rely on
double gLastStart = 0, gLastActive = -10;
FrameStats gStats;
double gStatsT0 = 0, gPrevStart = 0;

int force() {
    if (gForce == -2) {
        gForce = -1;
        if (const char* e = getenv("ONAIR_LOW_POWER_DRAW")) if (*e) gForce = (*e == '0' || *e == 'n' || *e == 'N' || *e == 'f' || *e == 'F') ? 0 : 1;
    }
    return gForce;
}
int choice() {
    if (gChoice < 0) gChoice = (int)plat::basePrefs().getI("lowPowerDraw", Ask);
    return gChoice;
}
}   // namespace

#if defined(__linux__)
// Mesa's llvmpipe draws on its own threads (one per processor, made with the context), at the same priority as the sample path: Linux
// gives an unprivileged program no way to raise the receiver threads above 0 (setThreadPriority's nice -5 fails), but it may lower
// others. The rasterizer threads get nice 5, as the background decoders have, so a busy frame waits rather than the radio's samples.
static int lowerRasterizerThreads() {
    int n = 0;
    DIR* d = opendir("/proc/self/task");
    if (!d) return 0;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        char path[64], comm[32] = "";
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", e->d_name);
        if (FILE* f = fopen(path, "r")) { if (!fgets(comm, sizeof comm, f)) comm[0] = 0; fclose(f); }
        if (strncmp(comm, "llvmpipe", 8) == 0 || strncmp(comm, "lp_", 3) == 0 || strncmp(comm, "softpipe", 8) == 0)
            if (setpriority(PRIO_PROCESS, (id_t)atoi(e->d_name), 5) == 0) n++;
    }
    closedir(d);
    return n;
}
#endif

void detect(const char* glRenderer) {
    gRenderer = glRenderer ? glRenderer : "";
    gSoftware = isSoftwareRenderer(glRenderer);
#if defined(__linux__)
    if (gSoftware && !getenv("ONAIR_KEEP_RASTER_PRIORITY")) {
        const int n = lowerRasterizerThreads();
        if (n) fprintf(stderr, "renderer: %d rasterizer threads at nice 5 (ONAIR_KEEP_RASTER_PRIORITY=1 keeps them at 0)\n", n);
    }
#endif
    fprintf(stderr, "renderer: %s%s\n", gRenderer.empty() ? "(unknown)" : gRenderer.c_str(),
            gSoftware ? " (software: every frame is drawn by the processor; View > Low-power drawing, or ONAIR_LOW_POWER_DRAW=1)" : "");
}
bool software() { return gSoftware; }
const char* rendererName() { return gRenderer.c_str(); }
void forceLowPower(bool on) { force(); gForce = on ? 1 : 0; }
void setUnpaced(bool on) { gUnpaced = on; }
bool lowPowerDraw() { return lowPowerWanted(choice(), force()); }
int lowPowerChoice() { return choice(); }
void setLowPowerChoice(int c) {
    gChoice = c;
    plat::basePrefs().setI("lowPowerDraw", c);
    plat::basePrefs().flush();
}

void frameGate(bool running) {
    double now = glfwGetTime();
    if (ImGui::GetCurrentContext()) {   // the input of the last frame: moving, clicking, scrolling or typing
        const ImGuiIO& io = ImGui::GetIO();
        bool active = io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || io.MouseWheel != 0 || io.MouseWheelH != 0 || io.WantTextInput;
        for (bool d : io.MouseDown) active = active || d;
        if (active) gLastActive = now;
    }
#if defined(__linux__)
    const bool floorOn = true;
#else
    const bool floorOn = gSoftware;
#endif
    const double gap = gUnpaced ? 0.0 : minFrameGap(floorOn, lowPowerDraw(), now - gLastActive < 1.0, running);
    const double target = gLastStart + gap;
    while (now < target) {   // events that come meanwhile are queued for this frame
        glfwWaitEventsTimeout(target - now);
        now = glfwGetTime();
    }
    gLastStart = now;
}

void frameDone(double workS) {
    static const bool on = getenv("DECT2_FRAMETIME") != nullptr;
    if (!on) return;
    const double now = glfwGetTime();
    if (gPrevStart > 0) gStats.add(gLastStart - gPrevStart, workS);
    gPrevStart = gLastStart;
    if (gStatsT0 == 0) gStatsT0 = now;
    if (now - gStatsT0 >= 5.0 && gStats.size() > 0) {
        fprintf(stderr, "frame: %.1f fps, period p50 %.1f / p99 %.1f ms, work p50 %.1f / p99 %.1f ms%s\n", gStats.size() / (now - gStatsT0),
                1e3 * FrameStats::percentile(gStats.period, 0.5), 1e3 * FrameStats::percentile(gStats.period, 0.99),
                1e3 * FrameStats::percentile(gStats.work, 0.5), 1e3 * FrameStats::percentile(gStats.work, 0.99), lowPowerDraw() ? " (low-power drawing)" : "");
        gStats.clear();
        gStatsT0 = now;
    }
}
double frameStart() { return gLastStart; }

void hintWindow(App&) {
    if (!showHint(gSoftware, choice(), force())) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = std::min(vp->WorkSize.x - 16 * gUi, 460 * gUi);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 8 * gUi, vp->WorkPos.y + vp->WorkSize.y - 40 * gUi), ImGuiCond_Always, ImVec2(1, 1));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    if (!ImGui::Begin("##swrenderhint", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) { ImGui::End(); return; }
    ImGui::TextUnformatted(tr("Performance"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled(tr("OpenGL runs in software here (%s): the processor draws every frame, beside the receiver."), gRenderer.c_str());
    ImGui::TextUnformatted(tr("Low-power drawing lowers the frame rate and simplifies the aircraft map (no trails, labels only for the selected aircraft). "
                              "Installing the graphics driver for your GPU is the real fix."));
    ImGui::PopTextWrapPos();
    if (ImGui::Button(tr("Use low-power drawing"))) setLowPowerChoice(On);
    ImGui::SameLine();
    if (ImGui::Button(tr("Keep full drawing"))) setLowPowerChoice(Off);
    ImGui::TextDisabled("%s", tr("View > Low-power drawing changes it later."));
    ImGui::End();
}

void viewMenuItem() {
    bool on = lowPowerDraw();
    ImGui::BeginDisabled(force() >= 0);
    if (ImGui::MenuItem(tr("Low-power drawing"), nullptr, &on)) setLowPowerChoice(on ? On : Off);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", force() >= 0 ? tr("Set by ONAIR_LOW_POWER_DRAW or --low-power-draw for this run.")
                                             : tr("A lower frame rate cap and a simpler aircraft map (no trails). For computers without a graphics driver (software OpenGL)."));
}

}   // namespace rcaps
