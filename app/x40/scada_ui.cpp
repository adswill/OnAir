// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The screen of the SCADA radio telemetry mode (x40 variant Scada, docs/SCADA-4.1.md): a Messages tab, a Devices tab with the register map
// seen so far, and the CSV / JSON export. Receive only: nothing on this screen sends or replays a frame.
#include "app.h"
#include "dect2/x40/scada_proto.h"
#include "dect2/x40/x40_rx.h"
#include "dect2/common/diag.h"
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace dect2::scada;

struct ScadaView {
    uint64_t version = ~0ull;
    std::vector<Message> msgs;
    std::vector<Device> devs;
    std::string phy;
    char filter[96] = {0};
    bool hideUnknown = false;
    uint64_t selMsg = 0;
    int selDev = -1;
    std::string note;
};
ScadaView& view() { static ScadaView v; return v; }

void refresh(ScadaView& v) {
    Store& st = store();
    const uint64_t ver = st.version.load();
    if (ver == v.version) return;
    std::lock_guard<std::mutex> lk(st.mu);
    v.msgs.assign(st.tr.messages().begin(), st.tr.messages().end());
    v.devs = st.tr.devices();
    v.phy = st.phy;
    v.version = ver;
}

std::string clock(int64_t ms) {
    const std::time_t s = (std::time_t)(ms / 1000);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &s);
#else
    localtime_r(&s, &tmv);
#endif
    char b[16];
    std::strftime(b, sizeof b, "%H:%M:%S", &tmv);
    return b;
}

std::string lower(std::string v) { for (char& ch : v) ch = (char)std::tolower((unsigned char)ch); return v; }
bool contains(const std::string& hay, const char* needle) {
    if (!needle[0]) return true;
    return lower(hay).find(lower(needle)) != std::string::npos;
}

void exportFile(ScadaView& v, bool json) {
    const std::string p = saveFileDialog(json ? "scada_telemetry.json" : "scada_telemetry.csv");
    if (p.empty()) return;
    std::string body;
    { Store& st = store(); std::lock_guard<std::mutex> lk(st.mu); body = json ? st.tr.json() : st.tr.csv(); }
    std::ofstream f(std::filesystem::u8path(p), std::ios::binary);
    f << body;
    if (f) { char b[600]; snprintf(b, sizeof b, tr("saved %s"), p.c_str()); v.note = b; }
    else v.note = DIAG_TEXT(603403, "Writing the SCADA export %s failed (the folder may be read-only or the disk full), so nothing was saved.", p.c_str());
}

void messagesTab(ScadaView& v) {
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    const float detailH = v.selMsg ? ImGui::GetTextLineHeightWithSpacing() * 4 : 0.f;
    if (ImGui::BeginTable("##scadamsg", 6, fl, ImVec2(0, std::max(80.f, ImGui::GetContentRegionAvail().y - detailH)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(tr("Link")); ImGui::TableSetupColumn(tr("Master > outstation"));
        ImGui::TableSetupColumn(tr("Function")); ImGui::TableSetupColumn(tr("Summary"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(tr("CRC"));
        ImGui::TableHeadersRow();
        static const char* const pn[3] = {"Modbus", "DNP3", "?"};
        for (size_t k = v.msgs.size(); k-- > 0;) {
            const Message& m = v.msgs[k];
            if (v.hideUnknown && m.proto == Unknown) continue;
            if (v.filter[0] && !contains(m.from + " " + m.to + " " + m.func + " " + m.summary + " " + m.link, v.filter)) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((int)m.id);
            if (ImGui::Selectable(clock(m.wallMs).c_str(), v.selMsg == m.id, ImGuiSelectableFlags_SpanAllColumns)) v.selMsg = v.selMsg == m.id ? 0 : m.id;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.3f s", m.timeSec);
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::Text("%s  %s", pn[m.proto], m.link.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s > %s", m.from.c_str(), m.to.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.func.c_str());
            ImGui::TableNextColumn();
            if (m.encrypted) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", m.summary.c_str());
            else if (m.rttMs >= 0) ImGui::Text("%s  (%.0f ms)", m.summary.c_str(), m.rttMs);
            else ImGui::TextUnformatted(m.summary.c_str());
            ImGui::TableNextColumn();
            if (m.crc == 1) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "%s", tr("OK"));
            else if (m.crc == 0) ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.35f, 1), "%s", tr("failed"));
            else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
    if (!v.selMsg) return;
    for (const Message& m : v.msgs) {
        if (m.id != v.selMsg) continue;
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("Raw bytes"));
        ImGui::SameLine();
        ImGui::TextUnformatted(hex(m.raw.data(), m.raw.size(), 300).c_str());
        if (m.pair) ImGui::TextDisabled("%s %llu", m.dir == 1 ? tr("answered by message") : tr("answers message"), (unsigned long long)m.pair);
        ImGui::PopTextWrapPos();
    }
}

void devicesTab(ScadaView& v) {
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    const float avail = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginTable("##scadadev", 8, fl, ImVec2(0, std::max(80.f, avail * 0.45f)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Protocol")); ImGui::TableSetupColumn(tr("Address")); ImGui::TableSetupColumn(tr("Role"));
        ImGui::TableSetupColumn(tr("Last heard")); ImGui::TableSetupColumn(tr("Last poll")); ImGui::TableSetupColumn(tr("Polls / replies"));
        ImGui::TableSetupColumn(tr("Response")); ImGui::TableSetupColumn(tr("Notes"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)v.devs.size(); i++) {
            const Device& d = v.devs[(size_t)i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (ImGui::Selectable(d.proto == Modbus ? "Modbus RTU" : "DNP3", v.selDev == i, ImGuiSelectableFlags_SpanAllColumns)) v.selDev = v.selDev == i ? -1 : i;
            ImGui::PopID();
            ImGui::TableNextColumn(); if (d.addr < 0) ImGui::TextUnformatted("-"); else ImGui::Text("%d", d.addr);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d.role.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f s", d.lastSec);
            ImGui::TableNextColumn(); if (d.lastPollSec < 0) ImGui::TextDisabled("-"); else ImGui::Text("%.1f s", d.lastPollSec);
            ImGui::TableNextColumn(); ImGui::Text("%d / %d", d.polls, d.replies);
            ImGui::TableNextColumn(); if (d.rttN) ImGui::Text("%.0f ms", d.rttSumMs / d.rttN); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            std::string n;
            if (d.exceptions) n += std::to_string(d.exceptions) + " " + tr("exceptions") + "  ";
            if (d.unanswered) n += std::to_string(d.unanswered) + " " + tr("unanswered") + "  ";
            if (d.unsolicited) n += std::to_string(d.unsolicited) + " " + tr("unsolicited") + "  ";
            if (d.crcBad) n += std::to_string(d.crcBad) + " " + tr("CRC failed") + "  ";
            if (!d.lastIin.empty() && d.lastIin != "none") n += "IIN: " + d.lastIin;
            if (!d.points.empty()) n += "  " + std::to_string(d.points.size()) + " " + tr("points");
            ImGui::TextUnformatted(n.c_str());
        }
        ImGui::EndTable();
    }
    if (v.selDev < 0 || v.selDev >= (int)v.devs.size()) { ImGui::TextDisabled("%s", tr("Select a device to see its registers and points.")); return; }
    const Device& d = v.devs[(size_t)v.selDev];
    ImGui::TextDisabled("%s", tr("Registers and points seen so far"));
    if (d.points.empty()) { ImGui::TextDisabled("%s", tr("none yet")); return; }
    if (ImGui::BeginTable("##scadapts", 3, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Point")); ImGui::TableSetupColumn(tr("Value"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(tr("Time"));
        ImGui::TableHeadersRow();
        for (const auto& p : d.points) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(pointLabel(d.proto, p.first.first, p.first.second).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.second.value.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f s", p.second.timeSec);
        }
        ImGui::EndTable();
    }
}

} // namespace

// The main tab of the mode (called from x40_ui.cpp)
void scadaTab(App& a) {
    ScadaView& v = view();
    refresh(v);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Receive only. Listening to non-broadcast utility traffic may be restricted in some countries; check local law. Not verified on a live signal."));
    ImGui::PopTextWrapPos();
    const bool on = a.rx.standard == dect2::x40::activeOfVariant(dect2::x40::Scada);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(on ? a.rx.x40.headline.c_str() : tr(a.engine.running() ? "starting" : "stopped")); ImGui::PopFont();
    if (!v.phy.empty()) ImGui::TextDisabled("%s", v.phy.c_str());
    if (ImGui::Button(tr("Export CSV"))) exportFile(v, false);
    ImGui::SameLine();
    if (ImGui::Button(tr("Export JSON"))) exportFile(v, true);
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear"))) {
        { Store& st = store(); std::lock_guard<std::mutex> lk(st.mu); st.tr.clear(); st.phy.clear(); st.version++; }
        a.engine.x40().clearResults(dect2::x40::Scada);
        v.selMsg = 0; v.selDev = -1;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160 * gUi);
    ImGui::InputTextWithHint("##scadaf", tr("filter"), v.filter, sizeof v.filter);
    ImGui::SameLine(); ImGui::Checkbox(tr("Hide not recognised"), &v.hideUnknown);
    if (!v.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", v.note.c_str()); }
    if (ImGui::BeginTabBar("##scadatabs")) {
        if (ImGui::BeginTabItem(trw("Messages"))) { messagesTab(v); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Devices"))) { devicesTab(v); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

// The list on the right: the devices heard
void scadaList(App&) {
    ScadaView& v = view();
    refresh(v);
    if (v.devs.empty()) { ImGui::TextDisabled("%s", tr("nothing yet")); return; }
    for (const Device& d : v.devs)
        ImGui::Text("%s %s  %d/%d", d.proto == Modbus ? "MB" : "DNP3", d.addr < 0 ? "master" : std::to_string(d.addr).c_str(), d.polls, d.replies);
}
