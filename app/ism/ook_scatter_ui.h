#pragma once
#include "app.h"
#include "dect2/ism/ook_scatter.h"
#include <cstdio>
#include <string>
#include <vector>

struct OokScatterState {
    int sel = 1;
    int lastN = 10;
    bool logScale = true;
};

inline void ookScatterPanel(const std::vector<dect2::ookscatter::Burst>& bursts, int current, OokScatterState& S) {
    using namespace dect2::ookscatter;
    ImGui::TextUnformatted("On / off time scatter");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * gUi);
    const char* const kSel[] = {"Current burst", "Last N bursts", "All bursts"};
    ImGui::Combo("##oksel", &S.sel, kSel, 3);
    if (S.sel == 1) { ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ImGui::InputInt("##okn", &S.lastN, 0, 0); S.lastN = std::max(1, std::min(500, S.lastN)); }
    ImGui::SameLine();
    ImGui::Checkbox("Log axes", &S.logScale);
    const Sel sel = S.sel == 0 ? Sel::Current : S.sel == 1 ? Sel::LastN : Sel::All;
    const std::vector<Pair> pts = collect(bursts, sel, S.lastN, current);
    const Result res = analyse(pts);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy CSV")) ImGui::SetClipboardText(csv(pts).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy every (on, off) pair in microseconds as CSV.");
    if (pts.empty()) { ImGui::TextDisabled("no pulses yet"); return; }
    ImGui::Text("%d points, %zu cluster(s), encoding: %s", res.points, res.clusters.size(), res.guess.c_str());
    float lo, hi;
    range(pts, lo, hi);
    const float W = std::min(ImGui::GetContentRegionAvail().x, 420 * gUi), H = 220 * gUi, pad = 34 * gUi;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), IM_COL32(0, 0, 0, 40));
    const ImVec2 a(o.x + pad, o.y + 6 * gUi), b(o.x + W - 6 * gUi, o.y + H - pad);
    dl->AddRect(a, b, IM_COL32(128, 128, 128, 120));
    auto px = [&](float us) { return a.x + axis(us, lo, hi, S.logScale) * (b.x - a.x); };
    auto py = [&](float us) { return b.y - axis(us, lo, hi, S.logScale) * (b.y - a.y); };
    const ImU32 grid = IM_COL32(128, 128, 128, 60), txt = IM_COL32(170, 170, 170, 220);
    for (float v : {10.f, 20.f, 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f}) {
        if (v <= lo || v >= hi) continue;
        char t[16];
        std::snprintf(t, sizeof t, "%.0f", v);
        dl->AddLine(ImVec2(px(v), a.y), ImVec2(px(v), b.y), grid);
        dl->AddLine(ImVec2(a.x, py(v)), ImVec2(b.x, py(v)), grid);
        dl->AddText(ImVec2(px(v) - 8 * gUi, b.y + 2 * gUi), txt, t);
        dl->AddText(ImVec2(o.x + 2 * gUi, py(v) - 6 * gUi), txt, t);
    }
    dl->AddText(ImVec2(b.x - 60 * gUi, b.y + 16 * gUi), txt, "on (us)");
    dl->AddText(ImVec2(o.x + 2 * gUi, a.y - 2 * gUi), txt, "off");
    const ImU32 pal3[] = {IM_COL32(80, 170, 255, 200), IM_COL32(255, 170, 60, 200), IM_COL32(110, 220, 120, 200), IM_COL32(230, 100, 150, 200), IM_COL32(190, 140, 255, 200), IM_COL32(220, 220, 90, 200)};
    for (size_t i = 0; i < pts.size(); i++) {
        const int l = res.label[i];
        dl->AddCircleFilled(ImVec2(px(pts[i].on), py(pts[i].off)), 2.5f * gUi, l < 0 ? IM_COL32(150, 150, 150, 110) : pal3[l % 6]);
    }
    for (const Cluster& c : res.clusters) {
        char t[48];
        std::snprintf(t, sizeof t, "%.0f/%.0f", c.onUs, c.offUs);
        dl->AddText(ImVec2(px(c.onUs) + 5 * gUi, py(c.offUs) - 14 * gUi), IM_COL32(255, 255, 255, 230), t);
    }
    ImGui::Dummy(ImVec2(W, H));
    for (const Cluster& c : res.clusters) ImGui::TextDisabled("on / off %s", clusterText(c).c_str());
}
