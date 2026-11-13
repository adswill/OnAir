// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Credits window (Help, Credits and licences; also the "by: ..." button in the menu bar): every project whose code, data or
// specification OnAir uses, with its author, licence and a link that opens in the browser. The table is generated from
// tools/credits_data.tsv into credits_data.h; the full notices are in THIRD_PARTY.md. The Contributors tab (people, and a view by mode)
// is generated from tools/contributors.tsv into contributors_data.h; modeCreditsLine() adds the names to a mode's tooltip.
#include "../app.h"
#include "../ui/typography.h"
#include "credits_data.h"
#include "contributors_data.h"
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "dect2/engine/updater.h"
#include "dect2/codec/codec_arch.h"
#include <cstring>

static void licencesTab() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(tr("OnAir is free software under the GNU General Public License v3 or later. It is built on the work of the people and projects below; thank you."));
    ImGui::TextDisabled("%s", tr("Click a link to open it in your browser. The full notices and open licence points are in THIRD_PARTY.md."));
    ImGui::PopTextWrapPos();
    static char filter[64] = "";
    ImGui::SetNextItemWidth(220 * gUi);
    ImGui::InputTextWithHint("##creditfilter", tr("Filter"), filter, sizeof filter);
    ImGui::Separator();
    if (ImGui::BeginTable("##credits", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Project"), ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn(tr("Author"), ImGuiTableColumnFlags_WidthStretch, 2.5f);
        ImGui::TableSetupColumn(tr("Licence"), ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn(tr("Used for"), ImGuiTableColumnFlags_WidthStretch, 3.f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < kCreditCount; i++) {
            const CreditEntry& c = kCredits[i];
            if (filter[0]) {
                std::string hay = std::string(c.name) + " " + c.author + " " + c.licence + " " + c.where;
                std::string f = filter;
                for (auto& ch : hay) ch = (char)tolower((unsigned char)ch);
                for (auto& ch : f) ch = (char)tolower((unsigned char)ch);
                if (hay.find(f) == std::string::npos) continue;
            }
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableNextColumn();
            if (ImGui::Selectable(c.name, false, ImGuiSelectableFlags_None)) dect2::openUrl(c.url);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s\n\n%s", c.url, c.how, c.note);
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", c.author);
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", c.licence);
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", c.where);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

// "area:Foo" or a mode name -> the modes of one row
static std::vector<std::string> splitModes(const char* m) {
    std::vector<std::string> v; std::string cur;
    for (const char* p = m;; p++) {
        if (*p == ';' || !*p) { size_t b = cur.find_first_not_of(' '); if (b != std::string::npos) v.push_back(cur.substr(b)); cur.clear(); if (!*p) break; }
        else cur += *p;
    }
    return v;
}

std::string modeCreditsLine(const char* modeName) {
    static std::map<std::string, std::string> cache;
    static bool built = false;
    if (!built) {
        built = true;
        std::map<std::string, std::set<std::string>> by;
        for (int i = 0; i < kContribCount; i++)
            for (const auto& m : splitModes(kContribs[i].modes)) if (m.compare(0, 5, "area:")) by[m].insert(kContribs[i].who);
        for (auto& [m, names] : by) {
            std::string t; for (const auto& n : names) { if (!t.empty()) t += ", "; t += n; }
            cache[m] = std::string("\n\n") + tr("Credits") + ": " + t;
        }
    }
    auto it = cache.find(modeName);
    return it == cache.end() ? std::string() : it->second;
}

static ImVec4 kindColour(const std::string& k) {
    if (k == "pull request") return ImVec4(0.45f, 0.78f, 0.55f, 1);
    if (k == "optimisation") return ImVec4(0.95f, 0.72f, 0.35f, 1);
    if (k == "fix") return ImVec4(0.55f, 0.75f, 0.95f, 1);
    if (k == "bug report") return ImVec4(0.92f, 0.55f, 0.50f, 1);
    if (k == "samples") return ImVec4(0.75f, 0.62f, 0.95f, 1);
    if (k == "translation") return ImVec4(0.55f, 0.85f, 0.85f, 1);
    if (k == "testing") return ImVec4(0.85f, 0.85f, 0.50f, 1);
    return ImVec4(0.72f, 0.76f, 0.82f, 1);   // idea
}

static const char* kindLabel(const std::string& k) {
    if (k == "pull request") return tr("pull request");
    if (k == "optimisation") return tr("optimisation");
    if (k == "fix") return tr("fix");
    if (k == "bug report") return tr("bug report");
    if (k == "samples") return tr("samples");
    if (k == "translation") return tr("translation");
    if (k == "testing") return tr("testing");
    return tr("idea");
}

static void personLine(const ContribEntry& c) {
    ImGui::TextColored(kindColour(c.kind), "[%s]", kindLabel(c.kind));
    ImGui::SameLine(); ImGui::TextWrapped("%s  (%s)", c.what, c.refs);
}

static void contributorsTab() {
    static char filter[64] = "";
    static int view = 0;
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(tr("Thank you to everyone who sent fixes, reported bugs, shared recordings, translated or suggested ideas. The author is Kristijonas Adomaitis."));
    ImGui::TextDisabled("%s", tr("Want your name changed or removed? Tell us on Discord or open an issue."));
    ImGui::PopTextWrapPos();
    ImGui::RadioButton(tr("People"), &view, 0); ImGui::SameLine();
    ImGui::RadioButton(tr("By mode"), &view, 1); ImGui::SameLine(0, 24 * gUi);
    ImGui::SetNextItemWidth(220 * gUi);
    ImGui::InputTextWithHint("##contribfilter", tr("Filter"), filter, sizeof filter);
    ImGui::Separator();
    const std::string f = lower(filter);
    auto rowMatches = [&](const ContribEntry& c) {
        if (f.empty()) return true;
        return lower(std::string(c.who) + " " + c.kind + " " + c.what + " " + c.modes + " " + c.refs).find(f) != std::string::npos;
    };
    ImGui::BeginChild("##contribscroll", ImVec2(0, 0));
    if (view == 0) {
        std::map<std::string, std::vector<int>, bool (*)(const std::string&, const std::string&)> people(
            [](const std::string& x, const std::string& y) { return lower(x) < lower(y); });
        for (int i = 0; i < kContribCount; i++) if (rowMatches(kContribs[i])) people[kContribs[i].who].push_back(i);
        for (auto& [who, idx] : people) {
            ImGui::PushID(who.c_str());
            const ContribEntry& first = kContribs[idx[0]];
            if (first.link[0] != '-') { if (ImGui::Selectable(who.c_str(), false, 0, ImVec2(ImGui::CalcTextSize(who.c_str()).x, 0))) dect2::openUrl(first.link); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", first.link); }
            else ImGui::TextUnformatted(who.c_str());
            for (int i : idx) {
                ImGui::Indent(16 * gUi);
                personLine(kContribs[i]);
                ImGui::Unindent(16 * gUi);
            }
            ImGui::PopID();
        }
    } else {
        for (int g = 0; g < kNumGroups; g++) {
            bool head = false;
            for (const auto& m : allModes()) if (m.group == g) {
                std::set<std::string> names; std::vector<int> rows;
                for (int i = 0; i < kContribCount; i++) {
                    bool hit = false;
                    for (const auto& x : splitModes(kContribs[i].modes)) if (x == m.name) hit = true;
                    if (hit && rowMatches(kContribs[i]) ) { names.insert(kContribs[i].who); rows.push_back(i); }
                }
                if (rows.empty()) continue;
                if (!head) { head = true; ImGui::Spacing(); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kGroupCols[g]), "%s", tr(kGroupNames[g])); }
                std::string t; for (const auto& n : names) { if (!t.empty()) t += ", "; t += n; }
                ImGui::PushID(m.name);
                ImGui::Bullet(); ImGui::TextUnformatted(m.name); ImGui::SameLine(); ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", t.c_str()); ImGui::PopTextWrapPos();
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    for (int i : rows) ImGui::Text("%s: %s", kContribs[i].who, kContribs[i].what);
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
        }
        // areas that are not one mode (radios, network radios, interface ...)
        std::map<std::string, std::set<std::string>> areas;
        for (int i = 0; i < kContribCount; i++) if (rowMatches(kContribs[i]))
            for (const auto& x : splitModes(kContribs[i].modes)) if (!x.compare(0, 5, "area:")) areas[x.substr(5)].insert(kContribs[i].who);
        if (!areas.empty()) {
            ImGui::Spacing(); ImGui::TextColored(ImVec4(0.72f, 0.76f, 0.82f, 1), "%s", tr("GENERAL & TOOLS"));
            for (auto& [ar, names] : areas) {
                std::string t; for (const auto& n : names) { if (!t.empty()) t += ", "; t += n; }
                ImGui::Bullet(); ImGui::TextUnformatted(ar.c_str()); ImGui::SameLine(); ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", t.c_str()); ImGui::PopTextWrapPos();
            }
        }
    }
    ImGui::EndChild();
}

void creditsWindow(App& a) {
    if (!a.creditsOpen) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(900 * gUi, vp->WorkSize.x * 0.95f), std::min(620 * gUi, vp->WorkSize.y * 0.9f)), ImGuiCond_Appearing);
    if (!ImGui::Begin(trw("Credits"), &a.creditsOpen)) { ImGui::End(); return; }
    if (dect2::codec::runningUnderRosetta()) {   // the Intel build on an Apple Silicon Mac: said where people look for the version
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "%s", dect2::codec::rosettaNotice().c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton(tr("Open download page"))) dect2::openUrl("https://github.com/adswill/OnAir/releases/latest");
        ImGui::Separator();
    }
    if (ImGui::BeginTabBar("##credtabs")) {
        const int want = a.creditsTab; a.creditsTab = -1;
        if (typo::beginTabItem(tr("Contributors"), nullptr, want == 0 ? ImGuiTabItemFlags_SetSelected : 0)) { contributorsTab(); ImGui::EndTabItem(); }
        if (typo::beginTabItem(tr("Projects and licences"), nullptr, want == 1 ? ImGuiTabItemFlags_SetSelected : 0)) { licencesTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
