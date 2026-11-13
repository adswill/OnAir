// Mesh (LoRa) screens: the map of node and station positions, the selected node's card and the chat per channel.
// Split from mesh_ui.cpp; the shared state is declared in mesh_internal.h.
#include "map/my_location.h"
#include "mesh_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

using namespace meshui;

namespace meshui {

// ---------------------------------------------------------------- map

void mapView(App& a, ImVec2 size) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const double lat0 = S.map.lat, lon0 = S.map.lon;
    adsbmap::draw(S.map, size);
    if (S.map.lat != lat0 || S.map.lon != lon0) S.mapFit = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    ImVec2 click; const bool clicked = adsbmap::clickedAt(click);
    const dect2::MeshNode* tip = nullptr; float tipD = 14 * gUi;
    int shown = 0;
    if (on) {
        for (const auto& n : t.nodes) {
            if (!n.hasPosition) continue;
            shown++;
            const ImVec2 q = adsbmap::project(n.lat, n.lon);
            const bool sel = S.sel == nodeKey(n);
            const ImVec4 c = protoColour(n.protocol);
            const ImU32 col = ImGui::ColorConvertFloat4ToU32(c);
            dl->AddCircleFilled(q, 6 * gUi, col, 20);
            dl->AddCircle(q, 6 * gUi, sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 24, 26, 220), 20, sel ? 2.f : 1.2f);
            const std::string lb = !n.shortName.empty() ? n.shortName : !n.longName.empty() ? n.longName : n.id;
            const ImVec2 ts = ImGui::CalcTextSize(lb.c_str());
            dl->AddRectFilled(ImVec2(q.x + 8 * gUi, q.y - ts.y * 0.5f - 1), ImVec2(q.x + 12 * gUi + ts.x, q.y + ts.y * 0.5f + 1), IM_COL32(10, 12, 14, 190), 2.f);
            dl->AddText(ImVec2(q.x + 10 * gUi, q.y - ts.y * 0.5f), IM_COL32(230, 233, 236, 255), lb.c_str());
            const float d = std::hypot(q.x - mouse.x, q.y - mouse.y);
            if (d < tipD) { tipD = d; tip = &n; }
            if (clicked && std::hypot(q.x - click.x, q.y - click.y) < 14 * gUi) S.sel = nodeKey(n);
        }
    }
    const dect2::MeshAprsStation* tipA = nullptr;
    if (on) {
        for (const auto& s : t.aprsStations) {
            if (!s.st.hasPos) continue;
            shown++;
            const ImVec2 q = adsbmap::project(s.st.lat, s.st.lon);
            const bool sel = S.sel == aprsKey(s);
            const ImU32 col = ImGui::ColorConvertFloat4ToU32(protoColour(s.protocol));
            const float r = 6 * gUi;
            // a diamond: APRS stations and objects, apart from the round mesh nodes
            dl->AddQuadFilled(ImVec2(q.x, q.y - r), ImVec2(q.x + r, q.y), ImVec2(q.x, q.y + r), ImVec2(q.x - r, q.y), col);
            dl->AddQuad(ImVec2(q.x, q.y - r), ImVec2(q.x + r, q.y), ImVec2(q.x, q.y + r), ImVec2(q.x - r, q.y), sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 24, 26, 220), sel ? 2.f : 1.2f);
            const ImVec2 ts = ImGui::CalcTextSize(s.st.call.c_str());
            dl->AddRectFilled(ImVec2(q.x + 8 * gUi, q.y - ts.y * 0.5f - 1), ImVec2(q.x + 12 * gUi + ts.x, q.y + ts.y * 0.5f + 1), IM_COL32(10, 12, 14, 190), 2.f);
            dl->AddText(ImVec2(q.x + 10 * gUi, q.y - ts.y * 0.5f), IM_COL32(230, 233, 236, 255), s.st.call.c_str());
            const float d = std::hypot(q.x - mouse.x, q.y - mouse.y);
            if (d < tipD) { tipD = d; tipA = &s; tip = nullptr; }
            if (clicked && std::hypot(q.x - click.x, q.y - click.y) < 14 * gUi) S.sel = aprsKey(s);
        }
    }
    dl->PopClipRect();
    if (tipA && ImGui::IsWindowHovered()) {
        const std::string mineA = mylocmap::distanceBearingText(tipA->st.lat, tipA->st.lon);
        ImGui::SetTooltip("%s  %s  %s\n%.5f, %.5f%s%s\n%s\nSNR %.1f dB, heard %s ago", tipA->st.call.c_str(), protoName(tipA->protocol), symbolText(tipA->st).c_str(), tipA->st.lat, tipA->st.lon,
                          mineA.empty() ? "" : "  ", mineA.c_str(), tipA->st.comment.c_str(), tipA->lastSnrDb, ago(t.timeSec, tipA->st.lastHeardSec).c_str());
    }
    if (tip && ImGui::IsWindowHovered()) {
        const std::string mineN = mylocmap::distanceBearingText(tip->lat, tip->lon);
        ImGui::SetTooltip("%s  %s\n%s  %s\n%.5f, %.5f  %.0f m%s%s\nSNR %.1f dB, heard %s ago", nodeLabel(*tip).c_str(), tip->id.c_str(), protoName(tip->protocol),
                          tip->hwModel.c_str(), tip->lat, tip->lon, tip->altM, mineN.empty() ? "" : "  ", mineN.c_str(), tip->lastSnrDb, ago(t.timeSec, tip->lastHeard).c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in"));
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out"));
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("fit")) S.mapFit = true;
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never a node's position");
    plat::prefs().setI("meshZoom", S.map.zoom);
    if (!on || shown == 0) {
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
        adsbmap::legend(size.x, "%s", on ? "no node or station has sent its position yet" : "start the receiver");
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));   // an item after SetCursorScreenPos: ImGui otherwise reports "extend window/parent boundaries"
}

void nodeCard(const App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const dect2::MeshNode* n = nullptr;
    if (live(a)) for (const auto& x : t.nodes) if (S.sel == nodeKey(x)) n = &x;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(96 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    const dect2::MeshAprsStation* as = nullptr;
    if (live(a)) for (const auto& x : t.aprsStations) if (S.sel == aprsKey(x)) as = &x;
    if (as) {
        ImGui::TextColored(protoColour(as->protocol), "%s", as->st.call.c_str());
        ImGui::TextDisabled("%s%s", protoName(as->protocol), as->st.isObject ? " object" : " station");
        ImGui::Spacing();
        kv("symbol", "%s", symbolText(as->st).c_str());
        if (as->st.isObject) kv("sent by", "%s", as->st.via.c_str());
        kv("heard", "%s ago, %u frames", ago(t.timeSec, as->st.lastHeardSec).c_str(), as->st.count);
        kv("last", "%s", as->lastType.c_str());
        kv("last SNR", "%.1f dB", as->lastSnrDb);
        if (!as->path.empty()) kv("path", "%s", as->path.c_str());
        if (as->st.hasPos) kv("position", "%.5f %.5f", as->st.lat, as->st.lon);
        if (as->st.hasAlt) kv("altitude", "%.0f m", as->st.altM);
        if (as->st.hasCourse || as->st.hasSpeed) kv("course", "%d deg, %.0f km/h", as->st.courseDeg, as->st.speedKnots * 1.852);
        if (as->batteryPct >= 0) kv("battery", "%d %%", as->batteryPct);
        if (!as->st.comment.empty()) kv("comment", "%s", as->st.comment.c_str());
        return;
    }
    if (!n) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Select a node or station in a table or on the map."); ImGui::PopTextWrapPos(); } return; }
    ImGui::TextColored(protoColour(n->protocol), "%s", nodeLabel(*n).c_str());
    ImGui::TextDisabled("%s", protoName(n->protocol));
    ImGui::Spacing();
    kv("id", "%s", n->id.c_str());
    if (!n->shortName.empty()) kv("short name", "%s", n->shortName.c_str());
    if (!n->hwModel.empty()) kv("hardware", "%s", n->hwModel.c_str());
    if (!n->role.empty()) kv("role", "%s", n->role.c_str());
    kv("heard", "%s ago, %u packets", ago(t.timeSec, n->lastHeard).c_str(), n->packets);
    kv("last SNR", "%.1f dB", n->lastSnrDb);
    if (n->hopsAway >= 0) kv("hops away", "%d", n->hopsAway);
    if (n->hasPosition) {
        kv("position", "%.5f %.5f", n->lat, n->lon);
        kv("altitude", "%.0f m", n->altM);
    }
    if (n->batteryPct > 100) kv("battery", "powered");
    else if (n->batteryPct >= 0) kv("battery", "%.0f %%", n->batteryPct);
    if (n->voltage > 0) kv("voltage", "%.2f V", n->voltage);
    if (n->channelUtilPct >= 0) kv("channel use", "%.1f %%", n->channelUtilPct);
    if (n->airUtilTxPct >= 0) kv("air time tx", "%.2f %%", n->airUtilTxPct);
    if (n->uptimeS >= 0) kv("uptime", "%s", clock((double)n->uptimeS).c_str());
    if (n->hasEnv) {
        kv("temperature", "%.1f C", n->tempC);
        kv("humidity", "%.0f %%", n->humidity);
        kv("pressure", "%.0f hPa", n->pressureHpa);
    }
}

// ---------------------------------------------------------------- chat

void chatView(const App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    std::vector<std::string> chans;      // "<protocol digit><channel>"
    if (on) {
        for (const auto& m : t.messages) {
            const std::string k = std::to_string(m.protocol) + m.channel;
            if (std::find(chans.begin(), chans.end(), k) == chans.end()) chans.push_back(k);
        }
        std::sort(chans.begin(), chans.end());
    }
    if (S.chatChannel > (int)chans.size()) S.chatChannel = 0;
    std::string cur = "All channels";
    if (S.chatChannel > 0) cur = std::string(protoName(chans[S.chatChannel - 1][0] - '0')) + ": " + chans[S.chatChannel - 1].substr(1);
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::BeginCombo("##mchan", cur.c_str())) {
        if (ImGui::Selectable("All channels", S.chatChannel == 0)) S.chatChannel = 0;
        for (size_t i = 0; i < chans.size(); i++) {
            const std::string l = std::string(protoName(chans[i][0] - '0')) + ": " + chans[i].substr(1);
            if (ImGui::Selectable(l.c_str(), S.chatChannel == (int)i + 1)) S.chatChannel = (int)i + 1;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("%zu messages", t.messages.size());
    ImGui::BeginChild("##mchat", ImVec2(0, 0), ImGuiChildFlags_Borders);
    bool any = false;
    if (on) {
        for (const auto& m : t.messages) {
            if (S.chatChannel > 0 && std::to_string(m.protocol) + m.channel != chans[S.chatChannel - 1]) continue;
            any = true;
            ImGui::TextDisabled("%s", clock(m.timeSec).c_str());
            ImGui::SameLine(0, 8 * gUi);
            ImGui::TextColored(protoColour(m.protocol), "%s", protoTag(m.protocol));
            ImGui::SameLine(0, 8 * gUi);
            if (S.chatChannel == 0) { ImGui::TextDisabled("[%s]", m.channel.c_str()); ImGui::SameLine(0, 6 * gUi); }
            const std::string who = m.fromName.empty() ? m.from : m.fromName;
            ImGui::TextColored(ImVec4(0.9f, 0.92f, 0.94f, 1), "%s", who.c_str());
            if ((m.protocol == 1 && m.to != "^all" && !m.to.empty()) || (m.protocol >= 3 && !m.to.empty() && m.to != "*")) { ImGui::SameLine(0, 6 * gUi); ImGui::TextDisabled("to %s", m.to.c_str()); }
            if (m.hops > 0) { ImGui::SameLine(0, 6 * gUi); ImGui::TextDisabled("(%d hop%s)", m.hops, m.hops == 1 ? "" : "s"); }
            ImGui::PushTextWrapPos(0);
            ImGui::Indent(18 * gUi);
            ImGui::TextUnformatted(m.text.c_str());
            ImGui::Unindent(18 * gUi);
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
        }
    }
    if (!any) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", !on ? "start the receiver to see messages" : "no text message yet"); ImGui::PopTextWrapPos(); }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
}

}  // namespace meshui
