// EDACS trunking screens: lock, good / bad frame counts, the frame list and a table of the most frequent data words, then the calls, talkgroups and
// site information read from the control channel, the voice channel table with its activity, and the following of one analog voice channel.
// Control data, analog voice, and ProVoice through the user-installed IMBE plug-in; nothing encrypted is handled. The data words are shown raw next to what the
// published message layout makes of them (core/include/dect2/edacs/edacs_msg.h); only frames whose first message passes its BCH check are listed.
#include "app.h"
#include "dect2/edacs/edacs_rx.h"
#include "dect2/edacs/edacs_tel.h"
#include "dect2/common/u8file.h"
#include "platform/platform.h"
#include "trunkpool/trunk1.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool live(const App& a) { return a.rx.standard == 131; }   // family 130, engine standard 132, active standard 131

struct State {
    char table[2048] = "";               // the frequency table: one line per LCN, "freq" (LCN = line number) or "LCN freq", MHz
    bool loaded = false, follow = false, followAct = false, wasRunning = false;
    char groups[128] = "";
    std::string status;
    float pushedVol = -1; bool pushedMute = false;
    int pushedTableHash = 0;
    bool ea = false, esk = false;
    bool dirty = true;
};
State S;

std::string tablePath() { return plat::dataDir() + "/edacs_lcn_table.txt"; }

std::vector<double> parseTable(const char* text, std::string* err) {
    std::vector<double> out;
    std::istringstream in(text);
    std::string line;
    int next = 1, lineNo = 0;
    while (std::getline(in, line)) {
        lineNo++;
        for (char& c : line) if (c == ',' || c == '=' || c == ';' || c == '\t') c = ' ';
        std::istringstream ls(line);
        std::vector<double> v;
        double x;
        while (ls >> x) v.push_back(x);
        if (v.empty()) continue;
        int lcn = next;
        double f = v.back();
        if (v.size() >= 2) lcn = (int)v[0];
        if (f < 1e5) f *= 1e6;                     // MHz
        if (lcn < 1 || lcn > 64 || f < 1e6) { if (err) *err = "line " + std::to_string(lineNo) + " is not a channel"; continue; }
        if ((size_t)lcn > out.size()) out.resize((size_t)lcn, 0.0);
        out[(size_t)lcn - 1] = f;
        next = lcn + 1;
    }
    return out;
}

std::vector<int> parseGroups(const char* text) {
    std::vector<int> g;
    std::string s = text;
    for (char& c : s) if (c == ',' || c == ';') c = ' ';
    std::istringstream in(s);
    int v;
    while (in >> v) g.push_back(v);
    return g;
}

int hashOf(const char* s) { int h = 7; while (*s) h = h * 31 + (unsigned char)*s++; return h; }

void loadTable() {
    if (S.loaded) return;
    S.loaded = true;
    std::ifstream f(std::filesystem::u8path(tablePath()), std::ios::binary);
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    std::snprintf(S.table, sizeof S.table, "%s", ss.str().c_str());
}

void applyTable(App& a) {
    std::string err;
    const std::vector<double> t = parseTable(S.table, &err);
    a.engine.edacs().setLcnTable(t);
    S.pushedTableHash = hashOf(S.table);
    S.status = std::to_string(t.size()) + " channels" + (err.empty() ? "" : ". " + err);
    std::ofstream f(std::filesystem::u8path(tablePath()), std::ios::binary);
    if (f) f << S.table;
}

void tick(App& a) {
    if (!a.engine.running()) { S.wasRunning = false; return; }
    loadTable();
    dect2::EdacsReceiver& r = a.engine.edacs();
    if (!S.wasRunning) { S.dirty = true; S.pushedVol = -1; }
    if (S.dirty) {
        applyTable(a);
        r.setFollow(S.follow); r.setFollowActivity(S.followAct); r.setFollowGroups(parseGroups(S.groups));
        dect2::edacs::Layout lay; lay.extended = S.ea; lay.esk = S.esk ? 0xA0 : 0; r.setLayout(lay);
        S.dirty = false;
    }
    if (a.volume != S.pushedVol || a.muted != S.pushedMute) { r.setVolume(a.volume); r.setMuted(a.muted); S.pushedVol = a.volume; S.pushedMute = a.muted; }
    S.wasRunning = true;
}

std::string mhz(double hz) { if (hz <= 0) return "-"; char b[32]; snprintf(b, sizeof b, "%.4f MHz", hz / 1e6); return b; }
std::string idOf(int v) { return v < 0 ? std::string("?") : std::to_string(v); }

void voiceSection(App& a, const dect2::EdacsTelemetry& t) {
    ImGui::Spacing();
    ImGui::SeparatorText("Voice channels");
    ImGui::TextDisabled("Analog FM calls, and ProVoice calls through the user-installed IMBE plug-in (onair-codec-imbe). Nothing encrypted is handled. Needs a capture wide enough to hold the channels.");
    ImGui::TextUnformatted("Frequency table (MHz): one line per channel, the first line is LCN 1, or \"LCN frequency\"");
    if (ImGui::InputTextMultiline("##edtable", S.table, sizeof S.table, ImVec2(-1, 90 * gUi))) S.dirty = true;
    if (ImGui::Checkbox("Follow calls", &S.follow)) S.dirty = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Also follow carriers no message announced", &S.followAct)) S.dirty = true;
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::InputText("Only groups", S.groups, sizeof S.groups)) S.dirty = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(empty: every call; numbers separated by commas)");
    if (!S.status.empty()) ImGui::TextDisabled("table: %s", S.status.c_str());
    if (!t.note.empty()) ImGui::TextColored(pal::warnAmber(), "%s", t.note.c_str());
    if (t.followLcn) ImGui::Text("Listening to LCN %d%s", t.followLcn, t.audioOpen ? "" : " (waiting for the carrier)");
    if (!t.channels.empty() && ImGui::BeginTable("edchan", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("LCN"); ImGui::TableSetupColumn("Frequency"); ImGui::TableSetupColumn("Level (dBFS)"); ImGui::TableSetupColumn("Floor"); ImGui::TableSetupColumn("State");
        ImGui::TableHeadersRow();
        for (const dect2::EdacsChannelActivity& c : t.channels) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d%s", c.lcn, c.lcn == t.controlLcn ? " (control)" : "");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mhz(c.freqHz).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0f", c.levelDb);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", c.floorDb);
            ImGui::TableNextColumn();
            if (!c.inCapture) ImGui::TextDisabled("outside the capture");
            else if (c.lcn == t.followLcn) ImGui::TextColored(pal::okGreen(), "listening");
            else if (c.active) ImGui::TextColored(pal::okGreen(), "carrier");
            else ImGui::TextDisabled("idle");
        }
        ImGui::EndTable();
    }
}

void messageSection(App& a, const dect2::EdacsTelemetry& t) {
    (void)a;
    ImGui::Spacing();
    ImGui::SeparatorText("Control channel messages");
    ImGui::TextDisabled("Read as the published Standard / Networked layout (TSB-69.3), or Extended Addressing; check bits verified (BCH 40,28).");
    if (t.siteValid) ImGui::Text("Site %s, area %s, control channel LCN %s, highest LCN seen %s", idOf(t.siteId).c_str(), idOf(t.systemId).c_str(), idOf(t.controlLcn).c_str(), idOf(t.nChannels).c_str());
    else ImGui::TextDisabled("Site: not heard");
    ImGui::TextDisabled("%llu words, %llu of them read as a known message type", (unsigned long long)t.messages, (unsigned long long)t.messagesKnown);
    if (ImGui::BeginTable("edcall", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, 150 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("State"); ImGui::TableSetupColumn("LCN"); ImGui::TableSetupColumn("Frequency"); ImGui::TableSetupColumn("Group / unit"); ImGui::TableSetupColumn("From");
        ImGui::TableSetupColumn("Seconds"); ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const dect2::EdacsCall& c : t.calls) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(c.active ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", c.followed ? "listening" : c.active ? "on air" : "ended");
            ImGui::TableNextColumn(); ImGui::Text("%d", c.lcn);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mhz(c.freqHz).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s%s", c.group ? "" : "unit ", idOf(c.gid).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(idOf(c.lid).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f", c.durSec);
            ImGui::TableNextColumn();
            ImGui::Text("%s%s%s", c.fromActivity ? "carrier only " : "", c.emergency ? "emergency " : "", c.digital ? "ProVoice" : "");
        }
        ImGui::EndTable();
    }
    if (!t.talkgroups.empty() && ImGui::BeginTable("edtg", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Group"); ImGui::TableSetupColumn("Calls"); ImGui::TableSetupColumn("Last unit"); ImGui::TableSetupColumn("Last LCN"); ImGui::TableSetupColumn("Frequency");
        ImGui::TableHeadersRow();
        for (const dect2::EdacsTalkgroup& g : t.talkgroups) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(g.active ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%d", g.gid);
            ImGui::TableNextColumn(); ImGui::Text("%u", g.calls);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(idOf(g.lastLid).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", g.lastLcn);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mhz(g.lastFreqHz).c_str());
        }
        ImGui::EndTable();
    }
}

void tab(App& a) {
    if (!live(a)) {
        ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "tune to an EDACS control channel (9600 bit/s) and start the receiver");
        return;
    }
    const dect2::EdacsTelemetry& t = a.rx.edacs;
    ImGui::TextUnformatted("EDACS trunking: 9600 bit/s control channel, analog voice following");
    ImGui::Spacing();
    ImGui::Text("Lock: %s", t.locked ? "yes" : "no");
    ImGui::Text("Good frames: %llu   Bad frames: %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    ImGui::Text("Level: %.0f dBFS, data swing %.0f Hz", t.levelDb, t.deviationHz);
    ImGui::TextDisabled("Data polarity: %s (found from the sync word and the check bits)", t.inverted ? "inverted" : "normal");
    if (t.follow) ImGui::TextDisabled("%d of %d voice slots in use. %s", t.slotsInUse, t.voiceSlots, t.slotsStatus.c_str());
    trunk1::section(a);
    if (ImGui::Checkbox("Extended Addressing", &S.ea)) S.dirty = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("ESK mask (0xA0)", &S.esk)) S.dirty = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(Standard / Networked is the default; EA and ESK only when the system uses them)");
    messageSection(a, t);
    voiceSection(a, t);
    ImGui::Spacing();
    ImGui::TextUnformatted("Data words (28 bits, raw)");
    if (ImGui::BeginTable("edword", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Word");
        ImGui::TableSetupColumn("Count");
        ImGui::TableHeadersRow();
        for (const dect2::EdacsWordCount& w : t.words) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%07X", w.word);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)w.count);
        }
        ImGui::EndTable();
    }
    if (!t.dataValid) ImGui::TextDisabled("listening: no frame with agreeing copies yet");
    wideCarriersTable(a, 0);   // whole span: every control channel in the span
}

void receiver(App& a) {
    if (!live(a)) return;
    const dect2::EdacsTelemetry& t = a.rx.edacs;
    ImGui::Text("%s", dect2::edacsSummary(t).c_str());
    ImGui::Text("good %llu, bad %llu, level %.1f dBFS", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.levelDb);
}

void list(App& a) {
    if (!live(a)) return;
    const dect2::EdacsTelemetry& t = a.rx.edacs;
    if (ImGui::BeginTable("edmsg", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Message A");
        ImGui::TableSetupColumn("Message B");
        ImGui::TableSetupColumn("Copy errors");
        ImGui::TableSetupColumn("Time (s)");
        ImGui::TableSetupColumn("Read as", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const dect2::EdacsFrame& f : t.frames) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%07X", f.msgA);
            ImGui::TableNextColumn(); ImGui::Text("%07X", f.msgB);
            ImGui::TableNextColumn(); ImGui::Text("%d / %d", f.errA, f.errB);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", f.timeSec);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(f.text.c_str());
        }
        ImGui::EndTable();
    }
}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State");
    ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::edacsSummary(a.rx.edacs).c_str() : (a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "EDACS trunking";
    if (live(a)) l2 = dect2::edacsSummary(a.rx.edacs);
}

} // namespace

extern const ModeUi kEdacsUi;
const ModeUi kEdacsUi = {
    .sideTitle = "FRAMES",
    .tabName = "EDACS",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
