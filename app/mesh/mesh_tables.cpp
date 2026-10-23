// Mesh (LoRa) screens: the node table, telemetry table, packet log and the APRS station table.
// Split from mesh_ui.cpp; the shared state is declared in mesh_internal.h.
#include "dect2/common/time_fmt.h"
#include "mesh_internal.h"
#include "ui/ui_common.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include "dect2/common/u8file.h"

using namespace meshui;

namespace meshui {

// ---------------------------------------------------------------- tables

std::vector<const dect2::MeshNode*> sortedNodes(const App& a) {
    std::vector<const dect2::MeshNode*> v;
    for (const auto& n : a.rx.mesh.nodes) if (shows(a, n.protocol)) v.push_back(&n);
    std::sort(v.begin(), v.end(), [](const dect2::MeshNode* x, const dect2::MeshNode* y) { return x->lastHeard > y->lastHeard; });
    return v;
}

// the width of a fixed column: its header or its widest usual value, whichever is wider (an auto-fit column was sized to its cells
// only, and cut its header short)
float hdrW(const char* header, const char* sample) { return std::max(ImGui::CalcTextSize(header).x, ImGui::CalcTextSize(sample).x); }

// the clock time of a signal time (seconds since the receiver started): the export's time column
double meshEpoch(const dect2::MeshTelemetry& t, double sec, double now) { return sec < 0 ? 0 : now - (t.timeSec - sec); }

std::string meshTime(double epoch) {
    if (epoch <= 0) return "";
    const std::time_t tt = (std::time_t)epoch;
    char b[24];
    snprintf(b, sizeof b, "%s", dect2::timefmt::show((int64_t)tt, dect2::timefmt::Style::DateClock).c_str());
    return b;
}

void nodeTable(const App& a, bool compact) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    if (!compact) {
        static std::string note;
        auto make_x = [&]() {   // built only when needed (once a second for the clip log, and on save)
            resexp::Table x;
            x.cols = {"time", "frequency_hz", "mode", "name", "short_name", "id", "hardware", "role", "lat", "lon", "alt_m", "snr_db", "hops", "battery_pct",
                      "voltage", "battery_time", "temp_c", "humidity_pct", "pressure_hpa", "sensors_time", "heard_s_ago", "packets"};
            const double now = (double)std::time(nullptr);
            for (const dect2::MeshNode* n : sortedNodes(a)) {
                const double ep = meshEpoch(t, n->lastHeard, now);
                x.add({meshTime(ep), resexp::num(a.freqMhz * 1e6, 0), protoName(n->protocol), n->longName, n->shortName, n->id, n->hwModel, n->role,
                       n->hasPosition ? resexp::num(n->lat, 6) : "", n->hasPosition ? resexp::num(n->lon, 6) : "", n->hasPosition ? resexp::num(n->altM, 0) : "",
                       resexp::num(n->lastSnrDb, 1), n->hopsAway >= 0 ? std::to_string(n->hopsAway) : "", n->batteryPct >= 0 ? resexp::num(n->batteryPct, 0) : "",
                       n->voltage >= 0 ? resexp::num(n->voltage, 2) : "", meshTime(meshEpoch(t, n->devSec, now)), n->hasEnv ? resexp::num(n->tempC, 1) : "",
                       n->hasEnv ? resexp::num(n->humidity, 0) : "", n->hasEnv ? resexp::num(n->pressureHpa, 1) : "", meshTime(meshEpoch(t, n->envSec, now)), n->lastHeard >= 0 ? resexp::num(t.timeSec - n->lastHeard, 0) : "", std::to_string(n->packets)}, ep);
            }
            return x;
        };
        resexp::saveButtonLazy(make_x, "mesh-nodes", note, "svmn");
        ImGui::SameLine();
        if (ImGui::SmallButton("Report...")) {   // a short text summary: copied to the clipboard and saved as .txt
            char mhz[32];
            snprintf(mhz, sizeof mhz, "  %.3f MHz", a.freqMhz);
            const std::string rep = dect2::meshReport(t, std::string(kPlans[effRegion(a)].shortName) + (t.custom ? "  (custom LoRa setting)" : "") + mhz, now);
            ImGui::SetClipboardText(rep.c_str());
            const std::string path = saveFileDialog(("mesh-report-" + resexp::stamp() + ".txt").c_str());
            if (!path.empty()) {
                std::ofstream f(dect2::u8file(path), std::ios::binary);
                f << rep;
                note = f ? "report saved to " + path + " and copied" : "could not write " + path;
            } else note = "report copied to the clipboard";
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A short text report: period, frames and CRC errors, packet types, every node with its SNR range,\nhops, battery, sensors and position, and the text messages. Copied to the clipboard and saved as a .txt file.");
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
    const int cols = compact ? 3 : 11;
    if (!ImGui::BeginTable(compact ? "##mn_s" : "##mn_f", cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | (compact ? 0 : ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX),
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    if (compact) {
        ImGui::TableSetupColumn("Node", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Heard"); ImGui::TableSetupColumn("SNR");
    } else {
        ImGui::TableSetupColumn("Net"); ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 160 * gUi); ImGui::TableSetupColumn("Short");
        ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, 110 * gUi); ImGui::TableSetupColumn("Hardware", ImGuiTableColumnFlags_WidthFixed, 170 * gUi);
        ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed, 90 * gUi); ImGui::TableSetupColumn("Heard");
        ImGui::TableSetupColumn("SNR dB", ImGuiTableColumnFlags_WidthFixed, hdrW("SNR dB", "-12.5")); ImGui::TableSetupColumn("Hops", ImGuiTableColumnFlags_WidthFixed, hdrW("Hops", "3/7"));
        ImGui::TableSetupColumn("Battery", ImGuiTableColumnFlags_WidthFixed, hdrW("Battery", "powered")); ImGui::TableSetupColumn("Packets", ImGuiTableColumnFlags_WidthFixed, hdrW("Packets", "99999"));
    }
    ImGui::TableHeadersRow();
    if (on) {
        for (const dect2::MeshNode* n : sortedNodes(a)) {
            ImGui::TableNextRow();
            const bool sel = S.sel == nodeKey(*n);
            ImGui::TableNextColumn();
            ImGui::PushID(nodeKey(*n).c_str());
            if (compact) {
                if (ImGui::Selectable(nodeLabel(*n).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) S.sel = nodeKey(*n);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, n->lastHeard).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.1f", n->lastSnrDb);
            } else {
                if (ImGui::Selectable(protoTag(n->protocol), sel, ImGuiSelectableFlags_SpanAllColumns)) S.sel = nodeKey(*n);
                ImGui::TableNextColumn(); ImGui::TextColored(protoColour(n->protocol), "%s", n->longName.empty() ? "(no name yet)" : n->longName.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(n->shortName.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(n->id.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(n->hwModel.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(n->role.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, n->lastHeard).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.1f", n->lastSnrDb);
                ImGui::TableNextColumn(); if (n->hopsAway >= 0) ImGui::Text("%d", n->hopsAway); else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn();
                if (n->batteryPct > 100) ImGui::TextUnformatted("powered");
                else if (n->batteryPct >= 0) ImGui::Text("%.0f %%", n->batteryPct);
                else if (n->voltage > 0) ImGui::Text("%.2f V", n->voltage);
                else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn(); ImGui::Text("%u", n->packets);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    if (!on || sortedNodes(a).empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", !on ? "start the receiver to see nodes" : subMask(a) == 2 ? "no node heard yet: a MeshCore node advertises itself now and then" : "no node heard yet: a Meshtastic node announces itself every few minutes"); ImGui::PopTextWrapPos(); }
}

// every telemetry and position report, newest first: the history behind the battery, sensor and position values of the node table
void telemetryTable(const App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    std::map<std::string, std::string> names;
    for (const auto& n : t.nodes) names[std::to_string(n.protocol) + n.id] = nodeLabel(n);
    auto nameOf = [&](const dect2::MeshReading& r) { auto it = names.find(std::to_string(r.protocol) + r.id); return it == names.end() ? r.id : it->second; };
    auto opt = [](double v, int dec) { return v >= 0 ? resexp::num(v, dec) : std::string(); };
    {
        static std::string note;
        auto make_x = [&]() {   // built only when needed (once a second for the clip log, and on save)
            resexp::Table x;
            x.cols = {"time", "frequency_hz", "mode", "name", "id", "battery_pct", "voltage", "channel_util_pct", "air_util_tx_pct", "uptime_s", "temp_c", "humidity_pct",
                      "pressure_hpa", "lat", "lon", "alt_m"};
            const double now = (double)std::time(nullptr);
            for (const auto& r : t.readings) {
                const double ep = r.unixTime > 0 ? r.unixTime : meshEpoch(t, r.timeSec, now);
                x.add({meshTime(ep), resexp::num(a.freqMhz * 1e6, 0), protoName(r.protocol), nameOf(r), r.id, opt(r.batteryPct, 0), opt(r.voltage, 2), opt(r.channelUtilPct, 1),
                       opt(r.airUtilTxPct, 1), r.uptimeS >= 0 ? std::to_string(r.uptimeS) : "", r.hasEnv ? resexp::num(r.tempC, 1) : "", r.hasEnv ? resexp::num(r.humidity, 0) : "",
                       r.hasEnv ? resexp::num(r.pressureHpa, 1) : "", r.hasPosition ? resexp::num(r.lat, 6) : "", r.hasPosition ? resexp::num(r.lon, 6) : "",
                       r.hasPosition ? resexp::num(r.altM, 0) : ""}, ep);
            }
            return x;
        };
        resexp::saveButtonLazy(make_x, "mesh-telemetry", note, "svmt");
        ImGui::SameLine();
        ImGui::TextDisabled("%zu reports (the last %d kept); KML / GPX: one point per position", t.readings.size(), 2000);
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
    if (!ImGui::BeginTable("##mt", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Node", ImGuiTableColumnFlags_WidthFixed, 180 * gUi); ImGui::TableSetupColumn("Battery");
    ImGui::TableSetupColumn("Voltage"); ImGui::TableSetupColumn("Ch util"); ImGui::TableSetupColumn("Air tx"); ImGui::TableSetupColumn("Temp");
    ImGui::TableSetupColumn("Humidity"); ImGui::TableSetupColumn("Pressure"); ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    if (on) {
        ImGuiListClipper clip;   // up to 2000 rows: draw only the visible ones
        clip.Begin((int)t.readings.size());
        while (clip.Step())
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
                const dect2::MeshReading& r = t.readings[t.readings.size() - 1 - (size_t)row];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();   // clock time (this is a history), the date in the tooltip
                {
                    const std::string full = meshTime(r.unixTime > 0 ? r.unixTime : meshEpoch(t, r.timeSec, (double)std::time(nullptr)));
                    ImGui::TextUnformatted(full.size() > 11 ? full.c_str() + 11 : clock(r.timeSec).c_str());
                    if (!full.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%s after the start)", full.c_str(), clock(r.timeSec).c_str());
                }
                ImGui::TableNextColumn(); ImGui::TextColored(protoColour(r.protocol), "%s", nameOf(r).c_str());
                ImGui::TableNextColumn(); if (r.batteryPct > 100) ImGui::TextUnformatted("powered"); else if (r.batteryPct >= 0) ImGui::Text("%.0f %%", r.batteryPct);
                ImGui::TableNextColumn(); if (r.voltage >= 0) ImGui::Text("%.2f V", r.voltage);
                ImGui::TableNextColumn(); if (r.channelUtilPct >= 0) ImGui::Text("%.1f %%", r.channelUtilPct);
                ImGui::TableNextColumn(); if (r.airUtilTxPct >= 0) ImGui::Text("%.1f %%", r.airUtilTxPct);
                ImGui::TableNextColumn(); if (r.hasEnv) ImGui::Text("%.1f C", r.tempC);
                ImGui::TableNextColumn(); if (r.hasEnv) ImGui::Text("%.0f %%", r.humidity);
                ImGui::TableNextColumn(); if (r.hasEnv) ImGui::Text("%.1f hPa", r.pressureHpa);
                ImGui::TableNextColumn(); if (r.hasPosition) ImGui::Text("%.5f, %.5f  %.0f m", r.lat, r.lon, r.altM);
            }
    }
    ImGui::EndTable();
}

void packetTable(const App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    {
        static std::string note;
        auto make_x = [&]() {   // built only when needed (once a second for the clip log, and on save)
            resexp::Table x;
            x.cols = {"time", "frequency_hz", "mode", "type", "from", "to", "channel", "bytes", "decrypted", "hop_limit", "hop_start", "snr_db", "level_dbfs", "sf", "bw_hz",
                      "detail", "raw", "packet_id", "relay_node", "next_hop", "via_mqtt", "cfo_hz"};
            const double now = (double)std::time(nullptr);
            for (const dect2::MeshPacket& p : t.packets) {
                const double ep = p.unixTime > 0 ? p.unixTime : meshEpoch(t, p.timeSec, now);
                // Meshtastic header: the packet id ties together the copies of one packet (flood) and the same packet seen by the node or MQTT;
                // relay_node is who sent this copy, next_hop who should send it on (last byte of the node number, 0 = none)
                const bool mt = p.protocol == 1 && p.parsed;
                char pid[12] = "", rel[4] = "", nxt[4] = "";
                if (mt) snprintf(pid, sizeof pid, "%08x", p.packetId);
                if (mt && p.relayNode >= 0) snprintf(rel, sizeof rel, "%02x", p.relayNode);
                if (mt && p.nextHop >= 0) snprintf(nxt, sizeof nxt, "%02x", p.nextHop);
                x.add({meshTime(ep), resexp::num(p.freqHz, 0), protoName(p.protocol), p.crcOk ? p.type : "CRC error", p.from, p.to, p.channel, std::to_string(p.size),
                       p.decrypted ? "yes" : "no", p.hopLimit >= 0 ? std::to_string(p.hopLimit) : "", p.hopStart >= 0 ? std::to_string(p.hopStart) : "",
                       resexp::num(p.snrDb, 1), resexp::num(p.levelDb, 1), std::to_string(p.sf), resexp::num(p.bwHz, 0), p.detail.empty() ? p.note : p.detail, p.raw,
                       pid, rel, nxt, mt ? (p.viaMqtt ? "yes" : "no") : "", resexp::num(p.cfoHz, 0)}, ep);
            }
            return x;
        };
        resexp::saveButtonLazy(make_x, "mesh-packets", note, "svmp");
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
    ImGui::Checkbox("raw payload", &S.rawView);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the frame's bytes as text (other bytes as \\xNN) in the last column, for LoRa APRS, MeshCom\nand frames that did not parse. Hover a row's last column to see it either way.");
    if (!ImGui::BeginTable("##mp", 11, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Net"); ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("From"); ImGui::TableSetupColumn("To");
    ImGui::TableSetupColumn("Bytes"); ImGui::TableSetupColumn("Content"); ImGui::TableSetupColumn("Hops"); ImGui::TableSetupColumn("SNR dB");
    ImGui::TableSetupColumn("SF / BW"); ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    if (on) {
        for (size_t i = t.packets.size(); i-- > 0;) {
            const dect2::MeshPacket& p = t.packets[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(p.timeSec).c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(protoColour(p.protocol), "%s", protoTag(p.protocol));
            ImGui::TableNextColumn();
            if (!p.crcOk) ImGui::TextColored(pal::badRed(), "CRC error");
            else ImGui::TextUnformatted(p.type.empty() ? "?" : p.type.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.from.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.to.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", p.size);
            ImGui::TableNextColumn();
            if (!p.crcOk) ImGui::TextDisabled("-");
            else if (p.decrypted) ImGui::TextColored(pal::okGreen(), "decrypted");
            else ImGui::TextColored(pal::warnAmber(), "%s", p.note.empty() ? "encrypted" : p.note.c_str());
            ImGui::TableNextColumn();
            if (p.hopStart >= 0 && p.hopLimit >= 0) ImGui::Text("%d", p.hopStart - p.hopLimit);
            else if (!p.path.empty() && p.protocol >= 3) ImGui::TextUnformatted(p.path.c_str());
            else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); ImGui::Text("%.1f", p.snrDb);
            ImGui::TableNextColumn(); ImGui::Text("%d / %.1f", p.sf, p.bwHz / 1e3);
            ImGui::TableNextColumn();
            if (S.rawView && !p.raw.empty()) { ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(p.raw.c_str()); ImGui::PopFont(); }
            else ImGui::TextUnformatted(p.detail.c_str());
            if (!p.raw.empty() && ImGui::IsItemHovered()) { wrapTooltip(560 * gUi, p.raw.c_str()); }
        }
    }
    ImGui::EndTable();
    if (!on) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("start the receiver to see packets"); ImGui::PopTextWrapPos(); }
}

std::string aprsKey(const dect2::MeshAprsStation& s) { return "A" + std::to_string(s.protocol) + s.st.call; }

std::string symbolText(const dect2::PacketStation& s) {
    if (!s.symTable) return "-";
    char b[4] = {s.symTable, s.symCode, 0, 0};
    return b;
}

std::vector<const dect2::MeshAprsStation*> sortedStations(const App& a) {
    std::vector<const dect2::MeshAprsStation*> v;
    for (const auto& x : a.rx.mesh.aprsStations) if (shows(a, x.protocol)) v.push_back(&x);
    std::sort(v.begin(), v.end(), [](const dect2::MeshAprsStation* x, const dect2::MeshAprsStation* y) { return x->st.lastHeardSec > y->st.lastHeardSec; });
    return v;
}

void aprsTable(const App& a, bool compact) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    const char* none = !on ? "start the receiver to see stations"
                     : subMask(a) == 4 ? "no LoRa APRS station heard yet: tune 433.775 MHz"
                     : subMask(a) == 8 ? "no MeshCom station heard yet: tune 433.175 MHz"
                     : "no LoRa APRS or MeshCom station heard yet: pick a 70 cm plan (433.775 or 433.175 MHz) in the tuner";
    if (compact) {   // the list beside the main view: call, heard, SNR
        if (ImGui::BeginTable("##maprs_s", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Station", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Heard"); ImGui::TableSetupColumn("SNR");
            ImGui::TableHeadersRow();
            if (on) for (const auto* s : sortedStations(a)) {
                ImGui::TableNextRow();
                ImGui::PushID(aprsKey(*s).c_str());
                ImGui::TableNextColumn();
                if (ImGui::Selectable(s->st.call.c_str(), S.sel == aprsKey(*s), ImGuiSelectableFlags_SpanAllColumns)) S.sel = aprsKey(*s);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, s->st.lastHeardSec).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.1f", s->lastSnrDb);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (!on || sortedStations(a).empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", none); ImGui::PopTextWrapPos(); }
        return;
    }
    if (!ImGui::BeginTable("##maprs", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable,
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Net"); ImGui::TableSetupColumn("Call", ImGuiTableColumnFlags_WidthFixed, hdrW("Call", "A61ABC-15")); ImGui::TableSetupColumn("Sym");
    ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthFixed, hdrW("Position", "25.0805N 155.1403E")); ImGui::TableSetupColumn("Last");
    ImGui::TableSetupColumn("Heard"); ImGui::TableSetupColumn("SNR dB", ImGuiTableColumnFlags_WidthFixed, hdrW("SNR dB", "-12.5"));
    ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, hdrW("Frames", "9999")); ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthFixed, 150 * gUi);
    ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthFixed, 320 * gUi);
    ImGui::TableHeadersRow();
    if (on) {
        for (const auto* s : sortedStations(a)) {
            ImGui::TableNextRow();
            ImGui::PushID(aprsKey(*s).c_str());
            ImGui::TableNextColumn();
            if (ImGui::Selectable(protoTag(s->protocol), S.sel == aprsKey(*s), ImGuiSelectableFlags_SpanAllColumns)) S.sel = aprsKey(*s);
            ImGui::TableNextColumn(); ImGui::TextColored(protoColour(s->protocol), "%s%s", s->st.call.c_str(), s->st.isObject ? " (obj)" : "");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(symbolText(s->st).c_str());
            ImGui::TableNextColumn();
            if (s->st.hasPos) ImGui::Text("%s %s", dect2::aprs::fmtLat(s->st.lat).c_str(), dect2::aprs::fmtLon(s->st.lon).c_str()); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s->lastType.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, s->st.lastHeardSec).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f", s->lastSnrDb);
            ImGui::TableNextColumn(); ImGui::Text("%u", s->st.count);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s->st.isObject ? ("by " + s->st.via).c_str() : s->path.c_str());
            ImGui::TableNextColumn();
            if (s->batteryPct >= 0) { ImGui::Text("battery %d %%", s->batteryPct); ImGui::SameLine(0, 6 * gUi); }
            ImGui::TextUnformatted(s->st.comment.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    if (!on || sortedStations(a).empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", none); ImGui::PopTextWrapPos(); }
}

}  // namespace meshui
