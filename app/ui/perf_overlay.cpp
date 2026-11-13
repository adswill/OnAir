// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// View > Performance overlay: the frame rate and build time, the redraw pace and why, the process's CPU per thread and its wake-ups.
// DECT2_PERF=1 writes the same once every 5 s to standard error (no window needed). The numbers are read once a second.
#include "../app.h"
#include "../platform/power.h"
#include "perf_overlay.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {
struct Stats {
    double buildAcc = 0, buildMax = 0, t0 = 0;
    int frames = 0;
    // the last completed second
    float fps = 0, buildAvgMs = 0, buildMaxMs = 0;
    double interval = 0;
    bool battery = false, occluded = false;
    ::power::Usage usage;
    bool haveUsage = false;
    double logT = 0;
} P;
}   // namespace

void perfFrame(double now, double buildSec, double interval, bool battery, bool occluded, bool wanted) {
    P.interval = interval; P.battery = battery; P.occluded = occluded;
    static const bool log = getenv("DECT2_PERF") != nullptr;
    if (!wanted && !log) { P.t0 = 0; return; }   // nothing is read while nobody looks
    if (P.t0 == 0) { P.t0 = now; P.frames = 0; P.buildAcc = P.buildMax = 0; ::power::sampleUsage(P.usage); return; }
    P.frames++;
    P.buildAcc += buildSec;
    P.buildMax = std::max(P.buildMax, buildSec);
    if (now - P.t0 < 1.0) return;
    P.fps = (float)(P.frames / (now - P.t0));
    P.buildAvgMs = (float)(1e3 * P.buildAcc / std::max(1, P.frames));
    P.buildMaxMs = (float)(1e3 * P.buildMax);
    P.frames = 0; P.buildAcc = P.buildMax = 0; P.t0 = now;
    P.haveUsage = ::power::sampleUsage(P.usage);
    if (log && now - P.logT >= 5.0) {
        P.logT = now;
        fprintf(stderr, "perf: %.1f fps (pace %.0f fps%s%s), build %.2f ms avg / %.1f ms max, process %.1f %% CPU, %.0f %s/s; top:",
                P.fps, interval > 0 ? 1 / interval : 0.0, battery ? ", battery" : "", occluded ? ", covered" : "", P.buildAvgMs, P.buildMaxMs,
                P.usage.processPct, P.usage.wakeupsPerSec, P.usage.wakeupsName);
        for (size_t i = 0; i < P.usage.threads.size() && i < 6; i++) fprintf(stderr, " %s %.1f%%", P.usage.threads[i].name.c_str(), P.usage.threads[i].pct);
        fprintf(stderr, "\n");
    }
}

void perfOverlayDraw(App& a) {
    if (!a.perfOverlay) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 10 * gUi, vp->WorkPos.y + vp->WorkSize.y - 10 * gUi), ImGuiCond_Always, ImVec2(1, 1));
    ImGui::SetNextWindowBgAlpha(0.85f);
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##perfOverlay", nullptr, f)) {
        ImGui::TextUnformatted(tr("Performance"));
        ImGui::Separator();
        ImGui::Text("%.1f fps, %s %.0f fps%s%s", P.fps, tr("pace"), P.interval > 0 ? 1 / P.interval : 0.0, P.battery ? tr(", on battery") : "",
                    P.occluded ? tr(", window covered") : "");
        ImGui::Text("%s %.2f ms, %s %.1f ms", tr("frame build"), P.buildAvgMs, tr("max"), P.buildMaxMs);
        if (P.haveUsage) {
            if (P.usage.processPct >= 0) ImGui::Text("%s %.1f %%", tr("process CPU"), P.usage.processPct);
            if (P.usage.wakeupsPerSec >= 0) ImGui::Text("%.0f %s/s", P.usage.wakeupsPerSec, tr(P.usage.wakeupsName));
            if (!P.usage.threads.empty() && ImGui::BeginTable("##perfThr", 2, ImGuiTableFlags_SizingFixedFit)) {
                for (size_t i = 0; i < P.usage.threads.size() && i < 8; i++) {
                    if (P.usage.threads[i].pct < 0.05f) break;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(P.usage.threads[i].name.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%5.1f %%", P.usage.threads[i].pct);
                }
                ImGui::EndTable();
            }
        } else ImGui::TextDisabled("%s", tr("measuring..."));
    }
    ImGui::End();
}
