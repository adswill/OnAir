// MPT1327 trunking screens (work in progress): lock, good and bad codewords, the system identity, the control message list
// and the channel table.
#include "app.h"
#include "dect2/mpt1327/mpt1327_chan.h"
#include "dect2/mpt1327/mpt1327_engine.h"
#include <cstdio>
#include <string>

namespace {

bool live(const App& a) { return a.rx.standard == 128; }   // family 127, stdMode 129, active standard 128

void messageTable(const dect2::MptTelemetry& t, float height) {
    if (!ImGui::BeginTable("mpt_msgs", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, height))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 56 * gUi);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 56 * gUi);
    ImGui::TableSetupColumn("Caller", ImGuiTableColumnFlags_WidthFixed, 80 * gUi);
    ImGui::TableSetupColumn("Called", ImGuiTableColumnFlags_WidthFixed, 80 * gUi);
    ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthFixed, 56 * gUi);
    ImGui::TableSetupColumn("Detail");
    ImGui::TableHeadersRow();
    for (size_t i = t.rows.size(); i-- > 0;) {   // newest first
        const dect2::MptMessageRow& r = t.rows[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%.1f s", r.t);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.type.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.caller.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.called.c_str());
        ImGui::TableNextColumn();
        if (r.channel >= 0) ImGui::Text("%d", r.channel); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.detail.c_str());
    }
    ImGui::EndTable();
}

// Channel plan for the Frequency column: the control channel's number and the spacing (the radio is tuned to the control channel).
int gCtrlCh = -1;
float gSpacingKhz = 12.5f;

void channelTable(const dect2::MptTelemetry& t) {
    if (gCtrlCh < 0) for (const dect2::MptChannelRow& c : t.channels) if (c.controlChannel) { gCtrlCh = c.channel; break; }   // the announced one
    ImGui::SetNextItemWidth(90 * gUi);
    ImGui::InputInt("Control channel number", &gCtrlCh, 0, 0);
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    ImGui::InputFloat("Spacing kHz", &gSpacingKhz, 0, 0, "%.3f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Traffic channels are named by number. With the control channel's number and the channel spacing the frequency follows (a contiguous plan; a duplex gap needs the right number).");
    if (!ImGui::BeginTable("mpt_chans", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthFixed, 60 * gUi);
    ImGui::TableSetupColumn("Frequency", ImGuiTableColumnFlags_WidthFixed, 100 * gUi);
    ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthFixed, 110 * gUi);
    ImGui::TableSetupColumn("Seen", ImGuiTableColumnFlags_WidthFixed, 50 * gUi);
    ImGui::TableSetupColumn("Last called");
    ImGui::TableSetupColumn("Last caller");
    ImGui::TableHeadersRow();
    for (const dect2::MptChannelRow& c : t.channels) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%d", c.channel);
        ImGui::TableNextColumn();
        {
            const double f = dect2::mpt1327::channelFreqHz(c.channel, gCtrlCh, t.centerHz, (double)gSpacingKhz * 1000.0);
            if (f > 0) ImGui::Text("%.4f MHz", f / 1e6); else ImGui::TextDisabled("-");
        }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.controlChannel ? "control channel" : "traffic (GTC)");
        ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)c.count);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.called.empty() ? "-" : c.called.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.caller.empty() ? "-" : c.caller.c_str());
    }
    ImGui::EndTable();
}

void tab(App& a) {
    if (!live(a)) {
        ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "tune to an MPT1327 control channel (VHF / UHF, 12.5 or 25 kHz FM) and start the receiver");
        return;
    }
    const dect2::MptTelemetry& t = a.rx.mpt1327;
    ImGui::TextUnformatted("MPT1327 trunking (WIP): 1200 bit/s FFSK control channel, control messages only");
    ImGui::Spacing();
    {
        const char* items[] = {"Standard sync word", "Ignore sync word (French networks)", "Automatic"};
        int m = (int)a.engine.mpt1327().syncMode();
        ImGui::SetNextItemWidth(260 * gUi);
        if (ImGui::Combo("Framing", &m, items, 3)) a.engine.mpt1327().setSyncMode((dect2::mpt1327::SyncMode)m);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Standard: codewords start after the MPT1327 SYNC word.\nIgnore sync word: codewords are framed by their check bits and followed on the 64 bit grid, for networks whose sync word differs (French).\nAutomatic: standard, switching to the tolerant framing when no sync word is seen for 8 s.");
        if (t.syncTolerant) { ImGui::SameLine(); ImGui::TextDisabled("sync check off"); }
    }
    ImGui::Text("Lock: %s", t.locked ? "locked" : "searching");
    if (t.sysId >= 0) ImGui::Text("System identity: %d", t.sysId);
    else ImGui::TextDisabled("System identity: -");
    ImGui::Text("Codewords good: %llu, bad: %llu", (unsigned long long)t.good, (unsigned long long)t.bad);
    ImGui::Text("Messages: %llu", (unsigned long long)t.messages);
    ImGui::Text("Level: %.0f dBFS", t.levelDb);
    ImGui::Spacing();
    ImGui::SeparatorText("Channel table");
    if (t.channels.empty()) ImGui::TextDisabled("no channel numbers seen yet");
    else channelTable(t);
    ImGui::Spacing();
    ImGui::TextDisabled("Input %.0f Hz, centre %.4f MHz. No traffic channel audio is decoded and nothing encrypted is decoded.", t.inputRate, t.centerHz / 1e6);
    wideCarriersTable(a, 0);   // whole span: every control channel in the span
}

void receiver(App& a) {
    if (!live(a)) return;
    const dect2::MptTelemetry& t = a.rx.mpt1327;
    ImGui::Text("%s", dect2::mpt1327Summary(t).c_str());
    ImGui::Text("good %llu, bad %llu, level %.1f dBFS", (unsigned long long)t.good, (unsigned long long)t.bad, t.levelDb);
}

void list(App& a) {
    if (!live(a)) return;
    const dect2::MptTelemetry& t = a.rx.mpt1327;
    if (t.rows.empty()) { ImGui::TextDisabled("no messages yet"); return; }
    messageTable(t, ImGui::GetContentRegionAvail().y);
}

void tick(App&) {}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State");
    ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::mpt1327Summary(a.rx.mpt1327).c_str() : (a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "MPT1327 trunking (WIP)";
    if (live(a)) l2 = dect2::mpt1327Summary(a.rx.mpt1327);
}

} // namespace

extern const ModeUi kMpt1327Ui;
const ModeUi kMpt1327Ui = {
    .sideTitle = "MESSAGES",
    .tabName = "MPT1327",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
