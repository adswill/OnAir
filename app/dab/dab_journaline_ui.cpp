// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "panels/journaline_view.h"
#include "imgui.h"
#include <cstdio>
#include <string>

using namespace dect2;

void journalineView(const char* id, JournalineDecoder& jl, JlViewState& st, int entry) {
    if (st.path.empty() || st.entry != (entry < 0 ? 0 : entry)) st.reset(entry);
    ImGui::PushID(id);
    const JlStats s = jl.stats();
    {   // progress: the objects received (of the number the TOC announces, when it came)
        char t[96];
        if (s.expected > 0) snprintf(t, sizeof t, "%d of %d objects received", s.expectedHave, s.expected);
        else snprintf(t, sizeof t, "%d objects received (no table of contents yet)", s.cached);
        const float f = s.expected > 0 ? (float)s.expectedHave / (float)s.expected : 0.f;
        ImGui::ProgressBar(f > 1.f ? 1.f : f, ImVec2(-1, 0), t);
        if (s.badCrc || s.badObjects) ImGui::TextDisabled("%llu data groups with a bad CRC, %llu objects not understood", (unsigned long long)s.badCrc, (unsigned long long)s.badObjects);
    }
    const bool canBack = st.path.size() > 1;
    if (!canBack) ImGui::BeginDisabled();
    if (ImGui::Button("Back")) st.path.pop_back();
    if (!canBack) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Home")) st.reset(entry);
    ImGui::SameLine();
    {   // breadcrumb: the titles of the pages on the path
        std::string bc;
        for (size_t i = 0; i < st.path.size(); i++) {
            JlObject o;
            std::string t = jl.get(st.path[i], o) && !o.title.empty() ? o.title : "#" + std::to_string(st.path[i]);
            if (t.size() > 28) t = t.substr(0, 26) + "..";
            bc += (i ? " > " : "") + t;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", bc.c_str());
    }
    ImGui::Separator();
    JlObject o;
    const uint16_t cur = st.path.back();
    if (!jl.get(cur, o)) {
        ImGui::TextDisabled("Waiting for page %u to arrive in the carousel...", (unsigned)cur);
        ImGui::PopID();
        return;
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(o.title.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    int jump = -1;
    if (o.type == 1) {
        for (size_t i = 0; i < o.links.size(); i++) {
            JlObject t;
            const bool have = jl.get(o.links[i].target, t);
            ImGui::PushID((int)i);
            if (!have) ImGui::BeginDisabled();
            const std::string label = o.links[i].text + (have ? "" : "  (waiting)");
            if (ImGui::Selectable(label.c_str())) jump = o.links[i].target;
            if (!have) ImGui::EndDisabled();
            ImGui::PopID();
        }
    } else if (o.type == 2) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(o.body.c_str());
        ImGui::PopTextWrapPos();
    } else if (o.type == 4) {
        int cols = 1;
        for (const auto& it : o.items) { int c = 1; for (char ch : it) if (ch == '\t') c++; if (c > cols) cols = c; }
        if (cols > 8) cols = 8;
        if (ImGui::BeginTable("jlist", cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            for (const auto& it : o.items) {
                ImGui::TableNextRow();
                size_t a = 0;
                for (int c = 0; c < cols; c++) {
                    const size_t b = c == cols - 1 ? std::string::npos : it.find('\t', a);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(it.substr(a, b == std::string::npos ? b : b - a).c_str());
                    if (b == std::string::npos) break;
                    a = b + 1;
                }
            }
            ImGui::EndTable();
        }
    }   // type 3: the title is the whole message
    for (const auto& g : o.general) {
        if (g.type == 0) {
            JlObject t;
            const bool have = jl.get(g.target, t);
            if (!have) ImGui::BeginDisabled();
            const std::string l = std::string("Go to: ") + (g.label.empty() ? "page " + std::to_string(g.target) : g.label) + "##g" + std::to_string(g.target);
            if (ImGui::SmallButton(l.c_str())) jump = g.target;
            if (!have) ImGui::EndDisabled();
        } else if (g.type >= 1 && g.type <= 4) {
            static const char* k[] = {"", "DAB / DRM link", "Web address", "Phone", "SMS"};
            ImGui::TextDisabled("%s: %s %s", k[g.type], g.address.c_str(), g.label.c_str());
        }
    }
    if (jump >= 0) st.path.push_back((uint16_t)jump);
    if (st.path.size() > 64) st.path.erase(st.path.begin() + 1);
    ImGui::PopID();
}
