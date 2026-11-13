// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB: the FIC tab. The tree of everything the fast information channel carries, FIG by FIG, in the manner of etisnoop
// (https://github.com/Opendigitalradio/etisnoop): a node per FIG type and extension, a node per distinct FIG under it with how often and how
// regularly it comes, and the decoded fields of that FIG under that (dab_fig.h).
#include "app.h"
#include "../ui/typography.h"
#include "dect2/dab/dab_fig.h"
#include <functional>

namespace {

struct FicView {
    DabFicAnalysis a;
    DabFigContext ctx;
    std::vector<DabFigDecoded> dec;     // one per record of a.figs
    double stamp = -1e9;
    bool pause = false;
    bool errorsOnly = false;
    bool have = false;
};

FicView& view() { static FicView v; return v; }

void refresh(App& a, FicView& v) {
    v.a = a.engine.dabFic();
    v.ctx = dabFigContext(v.a);
    v.dec.clear();
    v.dec.reserve(v.a.figs.size());
    for (const DabFigRecord& r : v.a.figs) v.dec.push_back(dabFigDecode(r, v.ctx));
    v.have = true;
}

// Seconds between two receptions of a FIG, 0 = seen once
double intervalSec(const DabFigRecord& r) { return r.count > 1 ? (double)(r.lastFrame - r.firstFrame) * 0.096 / (double)(r.count - 1) : 0.0; }

std::string everyText(double s) {
    char b[48];
    if (s <= 0) return "once";
    if (s < 1) snprintf(b, sizeof b, "every %.0f ms", s * 1000);
    else if (s < 100) snprintf(b, sizeof b, "every %.1f s", s);
    else snprintf(b, sizeof b, "every %.0f s", s);
    return b;
}

// The decoded lines as nested nodes: a line is the child of the closest line above it with a lower level
void lineTree(const DabFigDecoded& d) {
    std::vector<int> open;      // levels of the nodes that are open
    int skip = 1 << 30;         // lines deeper than this are under a closed node
    for (size_t i = 0; i < d.lines.size(); i++) {
        const DabFigLine& l = d.lines[i];
        while (!open.empty() && open.back() >= l.level) { ImGui::TreePop(); open.pop_back(); }
        if (l.level > skip) continue;
        skip = 1 << 30;
        const bool parent = i + 1 < d.lines.size() && d.lines[i + 1].level > l.level;
        ImGui::PushID((int)i);
        if (parent) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::TreeNodeEx("##n", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", l.text.c_str())) open.push_back(l.level);
            else skip = l.level;
        } else {
            ImGui::TreeNodeEx("##l", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s", l.text.c_str());
        }
        ImGui::PopID();
    }
    while (!open.empty()) { ImGui::TreePop(); open.pop_back(); }
}

} // namespace

void dabFicTab(App& a) {
    FicView& v = view();
    const double now = ImGui::GetTime();
    if (!v.pause && (!v.have || now - v.stamp > 0.5)) { refresh(a, v); v.stamp = now; }
    if (!v.have) refresh(a, v);
    const DabFicAnalysis& fic = v.a;

    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Everything the fast information channel (FIC) carries, decoded FIG by FIG (EN 300 401 clause 5.2 and 8, TS 101 756), after etisnoop.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    // ---- header: how full the FIC is
    {
        const double fibs = (double)fic.fibOk + (double)fic.fibBad;
        const double fill = fic.fibCount ? (double)fic.fillBytes / ((double)fic.fibCount * 30.0) * 100.0 : 0.0;
        sectionHeader(Ic::Gauge, "Fast information channel");
        ImGui::PushFont(a.mono, 0);
        ImGui::Text("%llu frames   %llu blocks ok, %llu bad (%.1f %% bad)   %.0f %% of the capacity used", (unsigned long long)fic.frames, (unsigned long long)fic.fibOk,
                    (unsigned long long)fic.fibBad, fibs > 0 ? 100.0 * (double)fic.fibBad / fibs : 0.0, fill);
        ImGui::PopFont();
        // the 12 FIBs of the last frame: bytes used of the 30 each carries
        const float h = 14 * gUi, cw = std::max(10.f * gUi, std::min(34.f * gUi, (ImGui::GetContentRegionAvail().x - 4 * gUi) / 12.f - 3 * gUi));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        for (int i = 0; i < 12; i++) {
            const float x = p.x + (float)i * (cw + 3 * gUi);
            dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + cw, p.y + h), IM_COL32(40, 42, 46, 255));
            const int u = fic.lastFill[i];
            if (u < 0) dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + cw, p.y + h), IM_COL32(200, 70, 60, 255));
            else dl->AddRectFilled(ImVec2(x, p.y + h * (1.f - (float)u / 30.f)), ImVec2(x + cw, p.y + h), ImGui::GetColorU32(pal::accent()));
        }
        ImGui::Dummy(ImVec2(12 * (cw + 3 * gUi), h + 2 * gUi));
        ImGui::TextDisabled("last frame: bytes used in each of its 12 blocks of 30 (red = bad CRC)");
    }
    ImGui::Spacing();

    if (ImGui::Button(v.pause ? "Resume" : "Pause")) v.pause = !v.pause;
    ImGui::SameLine();
    if (ImGui::Button("Copy as text")) ImGui::SetClipboardText(dabFicText(fic).c_str());
    ImGui::SameLine();
    ImGui::Checkbox("Only FIGs with errors", &v.errorsOnly);
    ImGui::Spacing();

    if (fic.figs.empty()) { ImGui::TextDisabled(a.rx.dab.state == 2 ? "waiting for the first FIGs" : "no signal: nothing in the FIC yet"); return; }

    // ---- totals per FIG type and extension
    if (typo::collapsingHeader("FIG types in the FIC")) {
        uint64_t total = 0;
        for (const DabFicStat& s : fic.stats) total += s.bytes;
        if (ImGui::BeginTable("ficstat", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("FIG", ImGuiTableColumnFlags_WidthFixed, 56 * gUi); ImGui::TableSetupColumn("Content");
            ImGui::TableSetupColumn("Received"); ImGui::TableSetupColumn("Bytes"); ImGui::TableSetupColumn("Share");
            ImGui::TableHeadersRow();
            for (const DabFicStat& s : fic.stats) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d/%d", s.type, s.ext);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(dabFigName(s.type, s.ext));
                ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)s.count);
                ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)s.bytes);
                ImGui::TableNextColumn(); ImGui::Text("%.1f %%", total ? 100.0 * (double)s.bytes / (double)total : 0.0);
            }
            ImGui::EndTable();
        }
    }
    ImGui::Spacing();
    sectionHeader(Ic::Layers, "FIG tree");

    // ---- the tree
    size_t i = 0;
    while (i < fic.figs.size()) {
        const int type = fic.figs[i].type, ext = fic.figs[i].ext;
        size_t j = i;
        uint64_t received = 0;
        bool anyErr = false;
        while (j < fic.figs.size() && fic.figs[j].type == type && fic.figs[j].ext == ext) {
            received += fic.figs[j].count;
            if (j < v.dec.size() && !v.dec[j].errors.empty()) anyErr = true;
            j++;
        }
        if (v.errorsOnly && !anyErr) { i = j; continue; }
        char title[200];
        snprintf(title, sizeof title, "FIG %d/%d  %s   (%zu distinct, %llu received)###g%d_%d", type, ext, dabFigName(type, ext), j - i, (unsigned long long)received, type, ext);
        if (anyErr) ImGui::PushStyleColor(ImGuiCol_Text, pal::badRed());
        const bool open = ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_SpanAvailWidth);
        if (anyErr) ImGui::PopStyleColor();
        if (open) {
            for (size_t k = i; k < j; k++) {
                const DabFigRecord& r = fic.figs[k];
                const DabFigDecoded& d = v.dec[k];
                if (v.errorsOnly && d.errors.empty()) continue;
                char hd[200];
                const std::string head = dabFigHeader(r);
                // the id of the node must not change while the FIG does: the CIF count (0/0) and the clock (0/10) change in every FIG
                const bool latest = r.type == 0 && (r.ext == 0 || r.ext == 10);
                const size_t id = latest ? (size_t)(r.ext + 1) : std::hash<std::string>()(std::string(r.data.begin(), r.data.end())) % 100000000u;
                snprintf(hd, sizeof hd, "%s   x%llu   %s   FIB %d   %zu bytes###i%zu", head.c_str(), (unsigned long long)r.count, everyText(intervalSec(r)).c_str(), r.lastFib, r.data.size() + 1, id);
                const bool bad = !d.errors.empty();
                if (bad) ImGui::PushStyleColor(ImGuiCol_Text, pal::badRed());
                const bool io = ImGui::TreeNodeEx(hd, ImGuiTreeNodeFlags_SpanAvailWidth | (j - i <= 3 ? ImGuiTreeNodeFlags_DefaultOpen : 0));
                if (bad) ImGui::PopStyleColor();
                if (io) {
                    ImGui::PushFont(a.mono, 0);
                    lineTree(d);
                    for (const std::string& e : d.errors) ImGui::TextColored(pal::badRed(), "  ! %s", e.c_str());
                    ImGui::PopFont();
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
        i = j;
    }
}
