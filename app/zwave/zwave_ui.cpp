// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Z-Wave screens: the frame log (home ID, source > destination, command class and command), the nodes, the networks (home IDs) and the channels of
// the region with what each carried; the region picker. Receive only: S0 and S2 payloads are named as encrypted and stay so.
#include "dect2/zwave/homenet_health.h"
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "ui/topo_view.h"
#include "dect2/zwave/zwave_proto.h"
#include "dect2/zwave/zwave_rx.h"
#include "dect2/zwave/zwave_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                 // 0 frames, 1 nodes, 2 networks, 3 channels
    bool loaded = false;
    std::string region;           // "" automatic
    char filter[64] = "";
    std::string selHome;
    bool hideAcks = false;
};
State S;

bool live(const App& a) { return a.rx.standard == 99; }   // the engine reports its standard code minus one

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.region = plat::prefs().getS("zwRegion", "");
    if (!S.region.empty() && !dect2::zwave::region(S.region)) S.region.clear();
}

std::string clock(int64_t t) {
    if (t <= 0) return "-";
    return dect2::timefmt::clock(t);
}

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string notLive(const App& a, const char* what) { return a.engine.running() ? "starting" : std::string("start the receiver to see ") + what; }

void filters(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Filter"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(170 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##zwf", "home ID, node, command class", S.filter, sizeof S.filter);
    flowNext(10 * gUi);
    ImGui::Checkbox("hide acks", &S.hideAcks);
    flowNext(10 * gUi);
    if (!S.selHome.empty()) {
        ImGui::TextColored(pal::okGreen(), "home ID %s only", S.selHome.c_str());
        ImGui::SameLine(0, 5 * gUi);
        if (ImGui::SmallButton("Show all")) S.selHome.clear();
        flowNext(10 * gUi);
    }
    if (ImGui::Button("Clear")) a.engine.zwave().clearResults();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget every network, node and frame heard so far");
    flowBreak();
}

void framesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", notLive(a, "the frames").c_str()); return; }
    const dect2::ZwaveTelemetry& t = a.rx.zwave;
    filters(a);
    const std::string q = lower(S.filter);
    std::vector<size_t> rows;
    for (size_t i = 0; i < t.log.size(); i++) {
        const auto& f = t.log[i];
        if (S.hideAcks && f.type == "ack") continue;
        if (!S.selHome.empty() && f.home != S.selHome) continue;
        if (!q.empty() && lower(f.home + " " + f.src + " " + f.dst + " " + f.text).find(q) == std::string::npos) continue;
        rows.push_back(i);
    }
    if (!ImGui::BeginTable("##zwlog", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("Rate"); ImGui::TableSetupColumn("Home ID");
    ImGui::TableSetupColumn("Source > destination"); ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Seq"); ImGui::TableSetupColumn("Level");
    ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin((int)rows.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const auto& f = t.log[rows[(size_t)i]];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(f.wallTime).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.2f", f.mhz);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::zwave::rateName(f.rate));
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(f.home.c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::Text("%s > %s", f.src.c_str(), f.dst.c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(f.type.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d%s", f.seq, f.ackReq ? " ack?" : "");
            ImGui::TableNextColumn(); ImGui::Text("%.0f", f.rssi);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.1f dBFS, SNR %.1f dB, %d octets", f.rssi, f.snrDb, f.len);
            ImGui::TableNextColumn();
            if (f.encryption) ImGui::TextDisabled("%s", f.text.c_str()); else ImGui::TextUnformatted(f.text.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.hex.c_str());
        }
    ImGui::EndTable();
    if (t.log.empty()) ImGui::TextDisabled("no frames yet");
    else if (rows.empty()) ImGui::TextDisabled("no frame passes the selection");
}

void nodesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", notLive(a, "the nodes").c_str()); return; }
    const dect2::ZwaveTelemetry& t = a.rx.zwave;
    filters(a);
    const std::string q = lower(S.filter);
    if (!ImGui::BeginTable("##zwn", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Home ID"); ImGui::TableSetupColumn("Node"); ImGui::TableSetupColumn("Role"); ImGui::TableSetupColumn("Sent"); ImGui::TableSetupColumn("Addressed");
    ImGui::TableSetupColumn("Encrypted"); ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Last seen"); ImGui::TableSetupColumn("Last command class", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& n : t.nodes) {
        if (!S.selHome.empty() && n.home != S.selHome) continue;
        if (!q.empty() && lower(n.home + " " + std::to_string(n.id) + " " + n.lastCc + " " + n.role).find(q) == std::string::npos) continue;
        ImGui::TableNextRow();
        const double ago = std::max(0.0, t.timeSec - n.lastSec);
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(n.home.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextColored(ago < 5 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%d", n.id);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(n.role.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Node 1 is the controller of a network when it is first included; a guess from the node ID");
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)n.sent);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)n.received);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)n.encrypted);
        ImGui::TableNextColumn(); if (n.sent) ImGui::Text("%.0f", n.rssi); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (n.sent) ImGui::Text(ago < 1 ? "now" : "%.0f s ago", ago); else ImGui::TextDisabled("addressed only");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(n.lastCc.c_str());
    }
    ImGui::EndTable();
    if (t.nodes.empty()) ImGui::TextDisabled("nothing heard yet");
}

void networksView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", notLive(a, "the networks").c_str()); return; }
    const dect2::ZwaveTelemetry& t = a.rx.zwave;
    if (!ImGui::BeginTable("##zwp", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Home ID"); ImGui::TableSetupColumn("Nodes"); ImGui::TableSetupColumn("Frames"); ImGui::TableSetupColumn("Encrypted");
    ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Last seen", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& n : t.networks) {
        ImGui::TableNextRow();
        ImGui::PushID((int)n.homeId);
        const double ago = std::max(0.0, t.timeSec - n.lastSec);
        ImGui::TableNextColumn();
        if (ImGui::Selectable("##row", S.selHome == n.home, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.selHome = S.selHome == n.home ? std::string() : n.home;
        ImGui::SameLine(0, 0);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(ago < 5 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", n.home.c_str()); ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("click to show only this network's frames and nodes");
        ImGui::TableNextColumn(); ImGui::Text("%d", n.nodes);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)n.frames);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)n.encrypted);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", n.rssi);
        ImGui::TableNextColumn(); ImGui::Text(ago < 1 ? "now" : "%.0f s ago", ago);
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (t.networks.empty()) ImGui::TextDisabled("nothing heard yet");
}

void channelsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", notLive(a, "the channels").c_str()); return; }
    const dect2::ZwaveTelemetry& t = a.rx.zwave;
    if (!ImGui::BeginTable("##zwc", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("Rate"); ImGui::TableSetupColumn("In the band taken in"); ImGui::TableSetupColumn("Frames"); ImGui::TableSetupColumn("Failed checks");
    ImGui::TableHeadersRow();
    for (const auto& c : t.chans) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%.2f", c.mhz);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::zwave::rateName(c.rate));
        ImGui::TableNextColumn(); if (c.covered) ImGui::TextColored(pal::okGreen(), "yes"); else ImGui::TextDisabled("no");
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.frames);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.bad);
    }
    ImGui::EndTable();
    ImGui::Spacing();
    std::string cc;
    for (size_t i = 0; i < t.perCc.size() && i < 10; i++) { char b[64]; snprintf(b, sizeof b, "%s%s %llu", cc.empty() ? "" : ", ", t.perCc[i].first.c_str(), (unsigned long long)t.perCc[i].second); cc += b; }
    kvRow(a, "command classes", "%s", cc.empty() ? "-" : cc.c_str());
}

void applyRegion(App& a) { a.engine.zwave().setRegion(S.region); }

void tuner(App& a, bool& retune) {
    loadState();
    namespace zw = dect2::zwave;
    ImGui::TextDisabled("Region");
    ImGui::SameLine(0, 5 * gUi);
    const dect2::ZwaveTelemetry& t = a.rx.zwave;
    const zw::Region* cur = zw::region(S.region);
    if (!cur && live(a) && zw::region(t.region)) cur = zw::region(t.region);
    if (!cur) cur = &zw::regions()[0];
    char label[96];
    if (S.region.empty()) snprintf(label, sizeof label, "automatic (%s)", cur->id); else snprintf(label, sizeof label, "%s  %s", cur->id, cur->name);
    ImGui::SetNextItemWidth(std::min(230 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##zwreg", label)) {
        if (ImGui::Selectable("automatic: the region with most channels in the band", S.region.empty())) { S.region.clear(); plat::prefs().setS("zwRegion", ""); plat::prefs().flush(); applyRegion(a); }
        for (const zw::Region& r : zw::regions()) {
            char e[128];
            snprintf(e, sizeof e, "%-4s %s", r.id, r.name);
            if (ImGui::Selectable(e, S.region == r.id)) { S.region = r.id; plat::prefs().setS("zwRegion", S.region); plat::prefs().flush(); applyRegion(a); tuneFreq(a, r.centreMhz); }   // the region's centre, as "Centre on region"
            if (ImGui::IsItemHovered()) {
                std::string tip;
                for (const auto& c : r.chans) { char b[48]; snprintf(b, sizeof b, "%s%.2f MHz %s", tip.empty() ? "" : "\n", c.mhz, zw::rateName(c.rate)); tip += b; }
                ImGui::SetTooltip("%s", tip.c_str());
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 8 * gUi);
    if (ImGui::SmallButton("Centre on region")) { tuneFreq(a, cur->centreMhz); retune = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tune to %.2f MHz: the channels of %s that fit the sample rate lie around it, off the radio's DC spike", cur->centreMhz, cur->name);
    if (live(a) && t.inputRate > 0) {
        ImGui::SameLine(0, 8 * gUi);
        if (t.channelsCovered > 0) ImGui::TextDisabled("%d of %zu channels in the band taken in", t.channelsCovered, t.chans.size());
        else ImGui::TextColored(pal::warnAmber(), "no %s channel in the band taken in", t.region.c_str());
    }
}

// Home network health (3.9, docs/SMART5-iot-3.9.md): for the one home ID the user picks as theirs (dect2/zwave/homenet_health.h)
dect2::HomeNetHealth gHealth;
std::string gHealthHome;
double gHealthLast = -1e18;   // the newest frame time fed
void healthFeed(const dect2::ZwaveTelemetry& t) {
    if (gHealthHome.empty()) gHealthHome = plat::prefs().getS("zwMyHome", "");
    if (gHealthHome.empty()) return;
    double newest = gHealthLast;
    for (auto it = t.log.rbegin(); it != t.log.rend(); ++it) {   // oldest first
        const dect2::ZwFrame& f = *it;
        if (f.sec <= gHealthLast || f.home != gHealthHome) continue;
        dect2::HomeNetFrame h; h.t = f.sec; h.src = f.src; h.dst = f.dst; h.seq = f.seq; h.ackReq = f.ackReq; h.isAck = f.type == "ack"; h.rssi = f.rssi;
        gHealth.add(h);
        newest = std::max(newest, f.sec);
    }
    gHealthLast = newest;
    gHealth.tick(t.log.empty() ? gHealthLast : std::max(gHealthLast, t.log.front().sec));
}
void healthView(App& a) {
    { ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("%s", tr("Pick your own home ID: its nodes get retransmissions, missing acknowledgements, signal trend and silence. Other networks are not followed."));
      ImGui::PopTextWrapPos(); }
    if (live(a)) for (const auto& n : a.rx.zwave.networks) {
        ImGui::SameLine();
        if (ImGui::RadioButton(n.home.c_str(), gHealthHome == n.home)) { gHealthHome = n.home; plat::prefs().setS("zwMyHome", n.home); plat::prefs().flush(); gHealth = dect2::HomeNetHealth(); gHealthLast = -1e18; }
    }
    if (gHealthHome.empty()) { ImGui::TextDisabled("%s", tr("no home ID picked")); return; }
    if (!ImGui::BeginTable("##zwh", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    for (const char* c : {"Node", "Frames", "Retries", "No ack", "Level", "Every", "Note"}) ImGui::TableSetupColumn(tr(c));
    ImGui::TableHeadersRow();
    for (const auto& [id, n] : gHealth.nodes()) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(id.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)n.frames);
        ImGui::TableNextColumn(); ImGui::Text("%.0f %%", n.retryPct());
        ImGui::TableNextColumn(); if (n.ackAsked) ImGui::Text("%.0f %%", n.missPct()); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (n.rssiNow > -199) ImGui::Text("%.0f (%+.0f)", n.rssiNow, n.rssiNow - n.rssiFirst); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (n.intervalSec > 0) ImGui::Text("%.0f s", n.intervalSec); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (!n.text.empty()) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.25f, 1), "%s", n.text.c_str());
    }
    ImGui::EndTable();
}

void tick(App& a) {
    loadState();
    if (live(a)) healthFeed(a.rx.zwave);
    static bool applied = false;
    if (!applied) { applied = true; applyRegion(a); }
}

void tab(App& a) {
    subNav("zwv", S.view, {"Frames", "Nodes", "Networks", "Channels", tr("Topology"), tr("Health")});
    if (S.view == 0) framesView(a);
    else if (S.view == 1) nodesView(a);
    else if (S.view == 4) { if (live(a)) topoView(a, "zw", a.rx.zwave.topo); else ImGui::TextDisabled("%s", notLive(a, "the topology").c_str()); }
    else if (S.view == 2) networksView(a);
    else if (S.view == 5) healthView(a);
    else channelsView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::ZwaveTelemetry& t = a.rx.zwave;
    if (!ImGui::BeginTable("##zwl", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Home ID", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Nodes"); ImGui::TableSetupColumn("Level");
    for (const auto& n : t.networks) {
        ImGui::TableNextRow();
        ImGui::PushID((int)n.homeId);
        ImGui::TableNextColumn();
        if (ImGui::Selectable("##row", S.selHome == n.home, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.selHome = S.selHome == n.home ? std::string() : n.home;
        ImGui::SameLine(0, 0);
        ImGui::PushFont(a.mono, 0);
        ImGui::TextColored(t.timeSec - n.lastSec < 5 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", n.home.c_str());
        ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::Text("%d", n.nodes);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.0f", n.rssi);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::ZwaveTelemetry& t = a.rx.zwave;
    kvRow(a, "state", "%s", dect2::zwaveSummary(t).c_str());
    kvRow(a, "region", "%s %s", t.region.c_str(), t.regionName.c_str());
    if (t.loMhz > 0) kvRow(a, "band taken in", "%.2f - %.2f MHz, %d of %zu channels", t.loMhz, t.hiMhz, t.channelsCovered, t.chans.size());
    kvRow(a, "signal", "SNR %.1f dB, offset %+.1f kHz, level %.1f dBFS", t.snrDb, t.cfoHz / 1e3, t.levelDb);
    kvRow(a, "frames", "%llu good, %llu failed the check, %llu bursts, %llu acks", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.bursts, (unsigned long long)t.acks);
    kvRow(a, "by rate", "R1 %llu, R2 %llu, R3 %llu", (unsigned long long)t.perRate[0], (unsigned long long)t.perRate[1], (unsigned long long)t.perRate[2]);
    kvRow(a, "encrypted", "S0 %llu, S2 %llu (named only, never decoded)", (unsigned long long)t.encS0, (unsigned long long)t.encS2);
    kvRow(a, "input", "%.3f Msps", t.inputRate / 1e6);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only: nothing is sent, included or answered. Frames are read from the headers every sniffer sees (G.9959 R1, R2, R3 FSK); S0 and S2 "
                        "payloads are encrypted with keys that are not obtainable and stay so. Z-Wave Long Range (O-QPSK) is not decoded.");
    ImGui::PopTextWrapPos();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Frames", !on ? 0 : a.rx.zwave.state >= 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::zwaveSummary(a.rx.zwave).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Z-Wave";
    if (live(a)) l2 = dect2::zwaveSummary(a.rx.zwave);
}

} // namespace

extern const ModeUi kZwaveUi;
const ModeUi kZwaveUi = {
    .sideTitle = "NETWORKS",
    .tabName = "Z-Wave",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
