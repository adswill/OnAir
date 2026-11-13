// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mesh topology picture (topo_view.h).
#include "app.h"
#include "ui/topo_view.h"
#include "ui/theme.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

struct ViewState { std::string group; bool inferred = false; std::string sel; };
std::map<std::string, ViewState>& states() { static std::map<std::string, ViewState> m; return m; }

ImU32 snrColour(double snr, bool have) {
    if (!have) return IM_COL32(150, 155, 160, 200);
    if (snr >= 5) return IM_COL32(100, 195, 135, 230);
    if (snr >= -5) return IM_COL32(230, 180, 70, 230);
    return IM_COL32(225, 95, 85, 230);
}

} // namespace

void topoView(App& a, const char* id, const dect2::TopoSnapshot& snap) {
    (void)a;
    using namespace dect2;
    ViewState& vs = states()[id];
    if (snap.empty() || snap.reports.empty()) {
        ImGui::TextDisabled("%s", tr("No relays or routes seen yet. The graph builds up as frames arrive; nothing is sent."));
        return;
    }
    // the network to show: the one with most nodes unless chosen
    const TopoReport* rep = nullptr;
    for (const TopoReport& r : snap.reports) if (r.group == vs.group) rep = &r;
    if (!rep) { for (const TopoReport& r : snap.reports) if (!rep || r.nodes > rep->nodes) rep = &r; vs.group = rep->group; }
    if (snap.reports.size() > 1) {
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::BeginCombo("##topogrp", vs.group.c_str())) {
            for (const TopoReport& r : snap.reports) {
                char b[96]; snprintf(b, sizeof b, "%s  (%d)", r.group.c_str(), r.nodes);
                if (ImGui::Selectable(b, r.group == vs.group)) vs.group = r.group;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }
    ImGui::Checkbox(tr("Show guessed links (dashed)"), &vs.inferred);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Links that are only a statistical guess, for example a deeper copy of a flooded packet following a shallower relay. They are never used for the single-point-of-failure check unless this is on."));
    if (!rep->advice.empty()) ImGui::TextColored(pal::warnAmber(), "%s", rep->advice.c_str());
    ImGui::TextDisabled("%d %s, %d %s, %d %s", rep->nodes, tr("nodes"), rep->links, tr("links"), rep->components, tr("connected parts"));
    if (rep->floodFactor > 0) { ImGui::SameLine(); ImGui::TextDisabled("| %s %.1f (%u %s)", tr("flood amplification"), rep->floodFactor, rep->floodPackets, tr("packets")); }

    // nodes of the group, rings by BFS distance from the root over the evidence links
    std::vector<const TopoNode*> ns;
    std::map<std::string, int> idx;
    for (const TopoNode& n : snap.nodes) if (n.group == rep->group && !n.observer) { idx[n.id] = (int)ns.size(); ns.push_back(&n); }
    const int N = (int)ns.size();
    std::vector<std::vector<int>> adj(N);
    for (const TopoEdge& e : snap.edges) {
        const double w = e.link + e.route + (vs.inferred ? e.inferred : 0);
        auto x = idx.find(e.a), y = idx.find(e.b);
        if (w <= 0 || x == idx.end() || y == idx.end()) continue;
        adj[x->second].push_back(y->second); adj[y->second].push_back(x->second);
    }
    std::vector<int> depth(N, -1);
    int root = -1;
    { auto it = idx.find(rep->root); if (it != idx.end()) root = it->second; }
    std::vector<int> order;
    auto bfs = [&](int s) { depth[s] = 0; order.push_back(s); for (size_t q = order.size() - 1; q < order.size(); q++) for (int v : adj[order[q]]) if (depth[v] < 0) { depth[v] = depth[order[q]] + 1; order.push_back(v); } };
    if (root >= 0) bfs(root);
    int maxD = 0;
    for (int i = 0; i < N; i++) if (depth[i] < 0 && !adj[i].empty()) { bfs(i); }
    for (int i = 0; i < N; i++) if (depth[i] < 0) depth[i] = 0, order.push_back(i);   // nodes with no link yet
    for (int i = 0; i < N; i++) maxD = std::max(maxD, depth[i]);
    std::set<std::string> cut;
    for (const TopoCut& c : rep->cuts) cut.insert(c.id);
    std::map<std::string, double> relayed;
    for (const TopoRelayRank& r : rep->relays) relayed[r.id] = r.share;

    const float sideW = std::min(280.f * gUi, ImGui::GetContentRegionAvail().x * 0.38f);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##topo_g", ImVec2(avail.x - sideW - 6 * gUi, avail.y), true);
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), sz = ImGui::GetContentRegionAvail();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p0.x + sz.x / 2, p0.y + sz.y / 2);
    const float R = std::min(sz.x, sz.y) / 2 - 22 * gUi;
    std::vector<ImVec2> pos(N);
    std::vector<int> perRing(maxD + 1, 0), seenRing(maxD + 1, 0);
    for (int i = 0; i < N; i++) perRing[depth[i]]++;
    for (int i : order) {
        const int d = depth[i];
        if (d == 0 && perRing[0] == 1) { pos[i] = c; continue; }
        const float r = maxD == 0 ? R * 0.6f : R * (float)(d + (perRing[0] == 1 ? 0 : 1)) / (float)(maxD + (perRing[0] == 1 ? 0 : 1));
        const float ang = 6.2831853f * (float)seenRing[d]++ / (float)std::max(1, perRing[d]) + 0.4f * d;
        pos[i] = ImVec2(c.x + r * std::cos(ang), c.y + r * std::sin(ang));
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (const TopoEdge& e : snap.edges) {
        auto x = idx.find(e.a), y = idx.find(e.b);
        if (x == idx.end() || y == idx.end()) continue;
        const double w = e.link + e.route, g = e.inferred;
        if (w <= 0 && !(vs.inferred && g > 0)) continue;
        const float th = 1.f + std::min(3.f, (float)std::log2(1.0 + w + g));
        const ImU32 col = snrColour(e.meanSnr(), e.snrN > 0);
        if (w > 0 && e.link == 0) {   // advertised by a route only: thinner, translucent
            dl->AddLine(pos[x->second], pos[y->second], (col & 0x00FFFFFF) | 0x90000000, th * 0.8f);
        } else if (w > 0) {
            dl->AddLine(pos[x->second], pos[y->second], col, th);
        } else {   // a guess: dashed
            const ImVec2 p = pos[x->second], q = pos[y->second];
            const float len = std::hypot(q.x - p.x, q.y - p.y);
            for (float t = 0; t < len; t += 10 * gUi) dl->AddLine(ImVec2(p.x + (q.x - p.x) * t / len, p.y + (q.y - p.y) * t / len), ImVec2(p.x + (q.x - p.x) * std::min(len, t + 5 * gUi) / len, p.y + (q.y - p.y) * std::min(len, t + 5 * gUi) / len), col, 1.f);
        }
        if (e.ambiguous) { const ImVec2 m((pos[x->second].x + pos[y->second].x) / 2, (pos[x->second].y + pos[y->second].y) / 2); dl->AddText(m, IM_COL32(230, 180, 70, 255), "?"); }
    }
    int hover = -1;
    for (int i = 0; i < N; i++) {
        const TopoNode& n = *ns[i];
        const bool router = relayed.count(n.id) || n.role == "router" || n.role == "coordinator" || n.role == "controller" || n.role == "ROUTER";
        const float r = (router ? 8.f : 6.f) * gUi;
        const ImU32 fill = n.placeholder ? IM_COL32(110, 115, 120, 255) : router ? IM_COL32(90, 170, 220, 255) : IM_COL32(150, 190, 150, 255);
        if (router) dl->AddRectFilled(ImVec2(pos[i].x - r, pos[i].y - r), ImVec2(pos[i].x + r, pos[i].y + r), fill, 2.f);
        else dl->AddCircleFilled(pos[i], r, fill);
        if (cut.count(n.id)) dl->AddCircle(pos[i], r + 4 * gUi, IM_COL32(225, 80, 70, 255), 0, 2.f);
        if (i == root) dl->AddCircle(pos[i], r + 3 * gUi, IM_COL32(240, 240, 240, 220), 0, 1.5f);
        if (std::hypot(mouse.x - pos[i].x, mouse.y - pos[i].y) < r + 4 * gUi) hover = i;
        if (n.label.size() <= 14) dl->AddText(ImVec2(pos[i].x + r + 2, pos[i].y - 7 * gUi), IM_COL32(210, 215, 220, 220), n.label.c_str());
    }
    ImGui::Dummy(sz);
    if (hover >= 0 && ImGui::IsWindowHovered()) {
        const TopoNode& n = *ns[hover];
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(n.label.c_str());
        if (n.label != n.id) ImGui::TextDisabled("%s", n.id.c_str());
        if (!n.role.empty()) ImGui::Text("%s: %s", tr("role"), n.role.c_str());
        ImGui::Text("%s %u, %s %.0f, %s %u", tr("started"), n.originated, tr("relayed"), n.relayed, tr("heard directly"), n.heard);
        if (n.snrN) ImGui::Text("%s %.1f dB", tr("mean SNR"), n.snrSum / n.snrN);
        if (n.placeholder) ImGui::TextDisabled("%s", tr("Only a relay byte or hash was seen: the node itself is not known yet."));
        for (const TopoCut& cu : rep->cuts) if (cu.id == n.id) ImGui::TextColored(pal::badRed(), "%s %d", tr("its loss would cut off nodes:"), cu.orphans);
        ImGui::EndTooltip();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##topo_r", ImVec2(sideW, avail.y));
    auto labelOf = [&](const std::string& nid) { auto it = idx.find(nid); return it == idx.end() ? nid : ns[it->second]->label; };
    ImGui::TextColored(pal::heading(), "%s", tr("Busiest relays"));
    if (rep->relays.empty()) ImGui::TextDisabled("-");
    for (const TopoRelayRank& r : rep->relays) { ImGui::Text("%s", labelOf(r.id).c_str()); ImGui::SameLine(sideW * 0.6f); ImGui::Text("%.0f %%", r.share * 100); }
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", tr("Single points of failure"));
    if (rep->cuts.empty()) ImGui::TextDisabled("%s", tr("none found (yet)"));
    for (const TopoCut& cu : rep->cuts) { ImGui::TextColored(pal::badRed(), "%s", labelOf(cu.id).c_str()); ImGui::SameLine(sideW * 0.6f); ImGui::Text("%d %s", cu.orphans, tr("cut off")); }
    ImGui::Spacing();
    ImGui::TextWrapped("%s", tr("Square: relays or routes. Ringed red: if it fails, nodes lose their way to the root (white ring). Line colour: link SNR. Thin: only named by a route. ? : the relay byte fitted several nodes."));
    ImGui::EndChild();
}
