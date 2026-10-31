// The voice monitor on screen (voice_monitor_ui.h). English only, like the codec helper.
#include "voice_monitor_ui.h"
#include "../app.h"
#include "../platform/platform.h"
#include "../codec/speech_codec_ui.h"
#include "dect2/audio/audioout.h"
#include "dect2/engine/updater.h"
#include "dect2/voice/voice_monitor.h"
#include "dect2/voice/conversation.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

namespace voiceui {

namespace vm = dect2::voice;
namespace fs = std::filesystem;

namespace {

struct Bufs {   // the text boxes of a mode's filters, kept while being typed in
    char allow[1024] = {}, mute[1024] = {}, prio[1024] = {}, hold[64] = {}, labels[32768] = {}, dir[512] = {}, search[96] = {};
    uint64_t gen = 0;   // the settings these were filled from
    std::string importNote;   // what the last "Import names" did
};

struct UiState {
    bool loaded = false;
    bool open = false;
    std::string mode = "dmr";
    std::map<std::string, Bufs> bufs;
    std::map<std::string, std::string> titles;   // mode -> title, as the strips name them
} S;

const char* kModes[][2] = {{"dmr", "DMR"}, {"p25", "P25"}, {"nxdn", "NXDN"}, {"dpmr", "dPMR"}, {"dstar", "D-STAR"}, {"ysf", "Yaesu System Fusion"}, {"tetra", "TETRA"}, {"m17", "M17"}};

std::string settingsPath() { return plat::dataDir() + "/voice-monitor.txt"; }

const char* titleOf(const std::string& m) {
    for (auto& k : kModes) if (m == k[0]) return k[1];
    auto it = S.titles.find(m);
    return it != S.titles.end() ? it->second.c_str() : m.c_str();
}

ImVec4 verdictColour(vm::Verdict v) {
    switch (v) {
    case vm::Verdict::Play: return pal::okGreen();
    case vm::Verdict::Encrypted: case vm::Verdict::NoCodec: return pal::warnAmber();
    case vm::Verdict::Squelched: return pal::badRed();
    default: return pal::grey(0.85f);
    }
}

ImU32 u32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }

void tag(const char* text, ImVec4 bg) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = tagAt(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + 2), text, u32(ImVec4(bg.x * 0.55f, bg.y * 0.55f, bg.z * 0.55f, 1)));
    ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight() + 4));
}

std::string clockText(int64_t t) {
    struct tm g; dect2::localTime((time_t)t, &g);
    char b[32]; snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

std::string target(const vm::CallEntry& e) {
    std::string s = vm::targetText(e.info);
    if (!e.label.empty()) s += "  " + e.label;
    return s;
}

std::string source(const vm::CallEntry& e) {
    std::string s = vm::sourceText(e.info);
    if (!e.info.alias.empty()) s += " (" + e.info.alias + ")";
    if (!e.srcLabel.empty() && e.srcLabel != e.label) s += "  " + e.srcLabel;
    return s;
}

void fill(Bufs& b, const vm::Settings& s, uint64_t gen) {
    snprintf(b.allow, sizeof b.allow, "%s", s.allow.c_str());
    snprintf(b.mute, sizeof b.mute, "%s", s.mute.c_str());
    snprintf(b.prio, sizeof b.prio, "%s", s.priority.c_str());
    snprintf(b.hold, sizeof b.hold, "%s", s.hold.c_str());
    snprintf(b.labels, sizeof b.labels, "%s", s.labels.c_str());
    snprintf(b.dir, sizeof b.dir, "%s", s.recordDir.c_str());
    b.gen = gen;
}

void apply(vm::Monitor& m, const vm::Settings& s) { m.setSettings(s); save(); }

// an error meter: green to red over 0 .. 10 % bit errors
void errorMeter(float er, float w) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight() * 0.55f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.4f), ImVec2(p.x + w, p.y + h * 1.4f), u32(pal::grey(0.25f)), 2);
    if (er >= 0) {
        const float f = std::min(1.f, er / 0.10f);
        const ImVec4 c = er < 0.02f ? pal::okGreen() : er < 0.05f ? pal::warnAmber() : pal::badRed();
        dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.4f), ImVec2(p.x + std::max(2.f, w * f), p.y + h * 1.4f), u32(c), 2);
    }
    ImGui::Dummy(ImVec2(w, h * 1.8f));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(er >= 0 ? "Bit errors: %.2f %%. Speech gets rough above 3 %% and breaks up above 8 %%." : "Bit errors: not measured yet", er * 100.f);
}

void openFolder(const std::string& dir) {
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    dect2::openUrl(dir);
}

std::string recordFolder(const vm::Monitor& m) {
    const vm::Settings s = m.settings();
    return s.recordDir.empty() ? vm::defaultRecordDir() + "/" + m.mode() : s.recordDir;
}

} // namespace

void load() {
    if (S.loaded) return;
    S.loaded = true;
    vm::setDefaultRecordDir(plat::dataDir() + "/voice-recordings");
    std::ifstream f(settingsPath(), std::ios::binary);
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    vm::loadAllSettings(ss.str());
}

void save() {
    std::error_code ec;
    fs::create_directories(fs::path(plat::dataDir()), ec);
    const std::string p = settingsPath(), tmp = p + ".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f << vm::saveAllSettings(); }
    fs::rename(tmp, p, ec);
}

void open(const char* mode) { if (mode) S.mode = mode; S.open = true; }

// ---------------------------------------------------------------- the strip in the mode's tab

void strip(App& a, const char* mode, const char* title, const Link& l) {
    load();
    S.titles[mode] = title;
    vm::Monitor& m = vm::monitor(mode);
    m.housekeep();
    vm::Settings s = m.settings();
    vm::CallEntry now;
    const bool talking = l.live && m.nowTalking(now);
    // the newest call that is not played (encrypted, muted, held out ...), shown when nobody is heard
    vm::CallEntry other;
    bool haveOther = false;
    if (!talking && l.live) { const auto calls = m.calls(); if (!calls.empty()) { other = calls[0]; haveOther = true; } }

    ImGui::PushID(mode);
    // the state line: one tag per fact, the same in every voice mode
    ImGui::AlignTextToFramePadding();
    tag(!l.live ? "stopped" : l.locked ? "locked" : "searching", !l.live ? pal::grey(0.7f) : l.locked ? pal::okGreen() : pal::warnAmber());
    flowNext(6 * gUi);
    {
        char b[64]; snprintf(b, sizeof b, "%s %s", l.codecName, l.codecReady ? "ready" : "missing");
        tag(b, l.codecReady ? pal::okGreen() : pal::warnAmber());
        if (!l.codecReady && l.codecId) {   // one click to the install helper: the exact file name, the folder, the build steps, Rescan
            ImGui::SameLine(0, 4 * gUi);
            if (ImGui::SmallButton("Install codec...")) ImGui::OpenPopup("vmcodec");
            if (ImGui::BeginPopup("vmcodec")) {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32);
                appcodec::codecInstallHelper(l.codecId);
                ImGui::PopTextWrapPos();
                ImGui::EndPopup();
            }
        }
    }
    if (talking || haveOther) {
        const vm::CallEntry& e = talking ? now : other;
        if (e.info.encrypted) { flowNext(6 * gUi); tag("encrypted call: not decoded", pal::warnAmber()); }
        if (e.info.emergency) { flowNext(6 * gUi); tag("EMERGENCY", pal::badRed()); }
    }
    if (!s.hold.empty()) { flowNext(6 * gUi); std::string h = "hold " + s.hold; tag(h.c_str(), pal::accent()); }
    if (s.allowOnly) { flowNext(6 * gUi); tag("allow list only", pal::accent()); }
    if (s.record) { flowNext(6 * gUi); tag("recording", pal::badRed()); }
    flowNext(10 * gUi);
    ImGui::SetNextItemWidth(90 * gUi);
    float vol = s.volume * 100.f;
    if (ImGui::SliderFloat("##vmvol", &vol, 0, 100, "vol %.0f%%")) { s.volume = vol / 100.f; m.setSettings(s); }
    if (ImGui::IsItemDeactivatedAfterEdit()) save();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The volume of %s, on top of the program's volume", title);
    flowNext(6 * gUi);
    if (ImGui::SmallButton(s.record ? "Stop recording" : "Record calls")) { s.record = !s.record; apply(m, s); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Each call that is played is saved as a WAV file with a JSON file of its details in\n%s", recordFolder(m).c_str());
    flowNext(6 * gUi);
    if (ImGui::SmallButton("Voice monitor...")) open(mode);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Calls, history search, talkgroup filters, mute list, priority, squelch and the output device of %s", title);
    flowEnd();

    // the "now talking" card
    const float cardH = ImGui::GetTextLineHeightWithSpacing() * 3.2f;
    ImGui::BeginChild("##vmcard", ImVec2(0, cardH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    if (talking || haveOther) {
        const vm::CallEntry& e = talking ? now : other;
        ImGui::PushFont(a.ui, ImGui::GetFontSize() * 1.45f);
        ImGui::TextColored(talking ? pal::okGreen() : pal::grey(0.75f), "%s", ellipsize(target(e), ImGui::GetContentRegionAvail().x * 0.62f).c_str());
        ImGui::SameLine();
        ImGui::TextColored(pal::grey(0.85f), "from %s", ellipsize(source(e), ImGui::GetContentRegionAvail().x).c_str());
        ImGui::PopFont();
        std::string info = talking ? "talking" : vm::verdictText(e.verdict);
        char b[64];
        snprintf(b, sizeof b, "   %.1f s", e.seconds()); info += b;
        if (!e.info.access.empty()) info += "   " + e.info.access;
        if (e.info.slot) { snprintf(b, sizeof b, "   slot %d", e.info.slot); info += b; }
        if (e.info.carrierHz > 0) { snprintf(b, sizeof b, "   %.4f MHz", e.info.carrierHz / 1e6); info += b; }
        ImGui::TextColored(verdictColour(talking ? vm::Verdict::Play : e.verdict), "%s", info.c_str());
        ImGui::SameLine(0, 12 * gUi);
        const float er = e.errorRate >= 0 ? e.errorRate : l.errorRate;
        errorMeter(er, 80 * gUi);
        ImGui::SameLine(0, 12 * gUi);
        const std::string tok = vm::Monitor::targetToken(e.info);
        if (!tok.empty()) {
            if (s.hold.empty()) { if (ImGui::SmallButton("Hold")) m.holdTarget(tok), save(); if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play only %s until you release it", tok.c_str()); }
            else { if (ImGui::SmallButton("Release hold")) m.releaseHold(), save(); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Mute")) m.muteToken(tok), save();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Never play %s again (the Voice monitor's mute list)", tok.c_str());
        }
    } else {
        ImGui::PushFont(a.ui, ImGui::GetFontSize() * 1.3f);
        ImGui::TextDisabled("%s", !l.live ? "Start the receiver to listen" : !l.locked ? "Looking for a signal" : "Nobody is talking");
        ImGui::PopFont();
        if (l.live) {
            ImGui::TextDisabled("%s", !l.codecReady ? "No voice codec: calls are listed but not heard (see the ? button)." : !s.hold.empty() ? "Held: other talkgroups are not played." : "Scanning: the first call heard is played.");
            ImGui::SameLine(0, 12 * gUi);
            errorMeter(l.errorRate, 80 * gUi);
            if (!s.hold.empty()) { ImGui::SameLine(); if (ImGui::SmallButton("Release hold")) m.releaseHold(), save(); }
        }
    }
    ImGui::EndChild();
    ImGui::PopID();
}

// ---------------------------------------------------------------- the window

namespace {

void callsTab(vm::Monitor& m, const vm::Settings& s) {
    const auto calls = m.calls();
    if (calls.empty()) { ImGui::TextDisabled("No call in the last %.0f s.", s.hangSec); return; }
    if (!ImGui::BeginTable("##vmcalls", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Talkgroup / target", ImGuiTableColumnFlags_WidthStretch, 1.f);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 0.8f);
    ImGui::TableSetupColumn("Access", ImGuiTableColumnFlags_WidthFixed, 90 * gUi);
    ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 34 * gUi);
    ImGui::TableSetupColumn("s", ImGuiTableColumnFlags_WidthFixed, 40 * gUi);
    ImGui::TableSetupColumn("Err %", ImGuiTableColumnFlags_WidthFixed, 44 * gUi);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 150 * gUi);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 96 * gUi);
    ImGui::TableHeadersRow();
    for (const auto& e : calls) {
        ImGui::PushID((int)e.id);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextColored(e.verdict == vm::Verdict::Play ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s%s", e.priority ? "* " : "", target(e).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(source(e).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(e.info.access.empty() ? "-" : e.info.access.c_str());
        ImGui::TableNextColumn(); if (e.info.slot) ImGui::Text("%d", e.info.slot); else ImGui::TextUnformatted("-");
        ImGui::TableNextColumn(); ImGui::Text("%.1f", e.seconds());
        ImGui::TableNextColumn(); if (e.errorRate >= 0) ImGui::Text("%.1f", e.errorRate * 100.f); else ImGui::TextUnformatted("-");
        ImGui::TableNextColumn(); ImGui::TextColored(verdictColour(e.verdict), "%s", vm::verdictText(e.verdict));
        ImGui::TableNextColumn();
        const std::string tok = vm::Monitor::targetToken(e.info);
        if (!tok.empty()) {
            if (ImGui::SmallButton("Hold")) m.holdTarget(tok), save();
            ImGui::SameLine();
            if (ImGui::SmallButton("Mute")) m.muteToken(tok), save();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void historyTab(vm::Monitor& m, Bufs& b) {
    ImGui::SetNextItemWidth(220 * gUi);
    ImGui::InputTextWithHint("##vmsearch", "search: talkgroup, ID, callsign, label, state", b.search, sizeof b.search);
    ImGui::SameLine();
    if (ImGui::SmallButton("Open recordings folder")) openFolder(recordFolder(m));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear history")) m.clearHistory();
    const auto h = m.history(b.search, 500);
    ImGui::SameLine(); ImGui::TextDisabled("%zu calls", h.size());
    if (!ImGui::BeginTable("##vmhist", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 64 * gUi);
    ImGui::TableSetupColumn("Talkgroup / target", ImGuiTableColumnFlags_WidthStretch, 1.f);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 0.8f);
    ImGui::TableSetupColumn("Access", ImGuiTableColumnFlags_WidthFixed, 90 * gUi);
    ImGui::TableSetupColumn("s", ImGuiTableColumnFlags_WidthFixed, 40 * gUi);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 150 * gUi);
    ImGui::TableSetupColumn("Recording", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableHeadersRow();
    for (const auto& e : h) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(e.wallStart).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(target(e).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(source(e).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(e.info.access.empty() ? "-" : e.info.access.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.1f", e.seconds());
        ImGui::TableNextColumn(); ImGui::TextColored(verdictColour(e.played ? vm::Verdict::Play : e.verdict), "%s", e.played ? (e.active ? "playing" : "played") : vm::verdictText(e.verdict));
        ImGui::TableNextColumn();
        if (!e.file.empty()) {
            ImGui::PushID((int)e.id);
            if (ImGui::SmallButton("Show")) openFolder(fs::path(e.file).parent_path().string());
            ImGui::SameLine(); ImGui::TextDisabled("%s", fs::path(e.file).filename().string().c_str());
            ImGui::PopID();
        } else ImGui::TextDisabled("-");
    }
    ImGui::EndTable();
}

void heardTab(vm::Monitor& m) {
    const auto h = m.heard(300);
    ImGui::TextDisabled("%zu radios heard this session. Click a name to hold or mute it.", h.size());
    if (!ImGui::BeginTable("##vmheard", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Last heard", ImGuiTableColumnFlags_WidthFixed, 74 * gUi);
    ImGui::TableSetupColumn("Radio / callsign", ImGuiTableColumnFlags_WidthStretch, 1.f);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.f);
    ImGui::TableSetupColumn("Last talkgroup", ImGuiTableColumnFlags_WidthStretch, 0.8f);
    ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 44 * gUi);
    ImGui::TableSetupColumn("Airtime s", ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
    ImGui::TableSetupColumn("Errors", ImGuiTableColumnFlags_WidthFixed, 60 * gUi);
    ImGui::TableHeadersRow();
    for (const auto& e : h) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(e.lastWall).c_str());
        ImGui::TableNextColumn();
        ImGui::PushID(e.key.c_str());
        const std::string who = vm::sourceText(e.last);
        if (ImGui::Selectable(who.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {}
        if (ImGui::BeginPopupContextItem("##vmheardmenu", ImGuiPopupFlags_MouseButtonLeft | ImGuiPopupFlags_MouseButtonRight)) {
            const std::string tok = e.last.src ? std::to_string(e.last.src) : e.last.srcText;
            if (ImGui::MenuItem("Mute this radio")) m.muteToken(tok), save();
            if (ImGui::MenuItem("Hold on its last talkgroup")) m.holdTarget(vm::Monitor::targetToken(e.last)), save();
            if (ImGui::MenuItem("Copy ID")) ImGui::SetClipboardText(tok.c_str());
            ImGui::EndPopup();
        }
        ImGui::PopID();
        if (e.emergency) { ImGui::SameLine(); ImGui::TextColored(pal::badRed(), "EMERGENCY"); }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(e.name.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(vm::targetText(e.last).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%u", e.calls);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", e.seconds);
        ImGui::TableNextColumn(); if (e.errorRate >= 0) ImGui::Text("%.1f %%", e.errorRate * 100.f); else ImGui::TextDisabled("-");
    }
    ImGui::EndTable();
}

// ---- Conversations: the calls of this mode grouped into threads by talkgroup (or the two radios of a private call), carrier and slot.
struct ConvUi {
    char search[96] = {};
    int gapIdx = 1, sinceIdx = 0, slot = 0;
    float minSec = 0, maxErrPct = 0;
    bool playedOnly = false, withAudio = false, hideEnc = false;
    std::string exported;   // where the last export went
};
std::map<std::string, ConvUi> gConv;

bool writeText(const std::string& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream o(path, std::ios::binary);
    if (!o) return false;
    o << text;
    return (bool)o;
}

void conversationsTab(vm::Monitor& m) {
    ConvUi& u = gConv[m.mode()];
    static const double gaps[] = {30, 60, 300, 900, 3600};
    static const char* gapNames[] = {"30 s", "1 min", "5 min", "15 min", "1 hour"};
    static const int64_t sinces[] = {0, 3600, 6 * 3600, 24 * 3600};
    static const char* sinceNames[] = {"all session", "last hour", "last 6 hours", "last 24 hours"};
    ImGui::SetNextItemWidth(200 * gUi);
    ImGui::InputTextWithHint("##cvsearch", "search: talkgroup, ID, callsign, name, state", u.search, sizeof u.search);
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::BeginCombo("##cvgap", gapNames[u.gapIdx])) {
        for (int i = 0; i < 5; ++i) if (ImGui::Selectable(gapNames[i], u.gapIdx == i)) u.gapIdx = i;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A conversation ends after this much quiet on its talkgroup.");
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::BeginCombo("##cvsince", sinceNames[u.sinceIdx])) {
        for (int i = 0; i < 4; ++i) if (ImGui::Selectable(sinceNames[i], u.sinceIdx == i)) u.sinceIdx = i;
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Played only", &u.playedOnly); ImGui::SameLine();
    ImGui::Checkbox("With recording", &u.withAudio); ImGui::SameLine();
    ImGui::Checkbox("Hide encrypted", &u.hideEnc); ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * gUi); ImGui::SliderFloat("##cvmin", &u.minSec, 0, 20, u.minSec <= 0 ? "any length" : "over %.0f s");
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ImGui::SliderFloat("##cverr", &u.maxErrPct, 0, 20, u.maxErrPct <= 0 ? "any signal" : "errors < %.0f %%");
    ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi);
    if (ImGui::BeginCombo("##cvslot", u.slot ? (u.slot == 1 ? "slot 1" : "slot 2") : "all slots")) {
        if (ImGui::Selectable("all slots", u.slot == 0)) u.slot = 0;
        if (ImGui::Selectable("slot 1", u.slot == 1)) u.slot = 1;
        if (ImGui::Selectable("slot 2", u.slot == 2)) u.slot = 2;
        ImGui::EndCombo();
    }
    vm::ConvFilter f;
    f.query = u.search; f.minSeconds = u.minSec; f.maxErrorRate = u.maxErrPct > 0 ? u.maxErrPct / 100.f : -1.f;
    f.playedOnly = u.playedOnly; f.withAudioOnly = u.withAudio; f.hideEncrypted = u.hideEnc; f.slot = u.slot;
    if (sinces[u.sinceIdx] > 0) f.sinceWall = (int64_t)std::time(nullptr) - sinces[u.sinceIdx];
    std::vector<vm::CallEntry> all = m.history(std::string(), 1000);
    for (const auto& c : m.calls()) {   // a live call that the history does not list yet
        bool have = false;
        for (const auto& h : all) if (h.id == c.id) { have = true; break; }
        if (!have) all.push_back(c);
    }
    const auto conv = vm::groupConversations(all, gaps[u.gapIdx], f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%zu conversations", conv.size());
    ImGui::SameLine();
    const std::string dir = recordFolder(m) + "/exports";
    char stamp[32]; { struct tm g; dect2::localTime(std::time(nullptr), &g); snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec); }
    if (ImGui::SmallButton("Export CSV")) { const std::string p = dir + "/conversations-" + m.mode() + "-" + stamp + ".csv"; u.exported = writeText(p, vm::conversationsCsv(conv)) ? p : "could not write " + p; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Export JSON")) { const std::string p = dir + "/conversations-" + m.mode() + "-" + stamp + ".json"; u.exported = writeText(p, vm::conversationsJson(conv, m.mode())) ? p : "could not write " + p; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Open exports folder")) openFolder(dir);
    if (!u.exported.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", u.exported.c_str()); }
    if (!ImGui::BeginTable("##vmconv", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Conversation", ImGuiTableColumnFlags_WidthStretch, 1.f);
    ImGui::TableSetupColumn("Last heard", ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
    ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 44 * gUi);
    ImGui::TableSetupColumn("Talk s", ImGuiTableColumnFlags_WidthFixed, 56 * gUi);
    ImGui::TableSetupColumn("Talkers", ImGuiTableColumnFlags_WidthStretch, 1.f);
    ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthFixed, 90 * gUi);
    ImGui::TableSetupColumn("Audio", ImGuiTableColumnFlags_WidthFixed, 60 * gUi);
    ImGui::TableHeadersRow();
    int row = 0;
    for (const auto& v : conv) {
        ImGui::PushID(row++);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const std::string head = v.title + (v.label.empty() ? "" : "  " + v.label) + (v.slot ? "  slot " + std::to_string(v.slot) : "");
        const bool openNode = ImGui::TreeNodeEx("##cv", ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_AllowOverlap, "%s", head.c_str());
        if (v.active) { ImGui::SameLine(); ImGui::TextColored(pal::okGreen(), "live"); }
        if (v.encrypted) { ImGui::SameLine(); ImGui::TextColored(pal::warnAmber(), "%zu encrypted", v.encrypted); }
        if (v.emergency) { ImGui::SameLine(); ImGui::TextColored(pal::badRed(), "emergency"); }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(v.lastWall).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%zu", v.calls.size());
        ImGui::TableNextColumn(); ImGui::Text("%.0f", v.talkSeconds);
        ImGui::TableNextColumn();
        { std::string t; for (size_t i = 0; i < v.talkers.size() && i < 3; ++i) t += (i ? ", " : "") + v.talkers[i].who; if (v.talkers.size() > 3) t += " +" + std::to_string(v.talkers.size() - 3); ImGui::TextUnformatted(t.c_str()); }
        ImGui::TableNextColumn();
        if (v.avgError >= 0) ImGui::TextColored(v.avgError < 0.02f ? pal::okGreen() : v.avgError < 0.08f ? pal::warnAmber() : pal::badRed(), "%s %.1f %%", vm::signalLabel(v.avgError), v.avgError * 100.f);
        else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (v.withAudio) ImGui::Text("%zu clip%s", v.withAudio, v.withAudio == 1 ? "" : "s"); else ImGui::TextDisabled("-");
        if (openNode) {
            for (size_t i = 0; i < v.calls.size(); ++i) {
                const auto& c = v.calls[v.calls.size() - 1 - i];   // newest call first inside a thread
                ImGui::PushID((int)c.id);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Indent(18 * gUi); ImGui::TextUnformatted(source(c).c_str());
                ImGui::SameLine(); ImGui::TextDisabled("%s", c.played ? (c.active ? "playing" : "played") : vm::verdictText(c.verdict));
                ImGui::Unindent(18 * gUi);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(c.wallStart).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted("");
                ImGui::TableNextColumn(); ImGui::Text("%.1f", c.seconds());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c.info.access.empty() ? "" : c.info.access.c_str());
                ImGui::TableNextColumn();
                if (c.errorRate >= 0) ImGui::Text("%s %.1f %%", vm::signalLabel(c.errorRate), c.errorRate * 100.f); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                if (!c.file.empty()) { if (ImGui::SmallButton("Play")) dect2::openUrl(c.file); if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens the recording in your sound player:\n%s", c.file.c_str()); }
                else ImGui::TextDisabled("-");
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

bool listBox(const char* label, const char* id, char* buf, size_t n, const char* tip, float h) {
    ImGui::TextUnformatted(label);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    const bool ch = ImGui::InputTextMultiline(id, buf, n, ImVec2(-1, h));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return ch;
}

void filtersTab(vm::Monitor& m, vm::Settings s, Bufs& b) {
    const char* ids = "Numbers (talkgroups or radio IDs), ranges like 3100-3199, callsigns, or prefixes like LY*.\nSeparate them with commas or new lines. A call matches when its target or its source does.";
    bool ch = false;
    if (ImGui::Checkbox("Play only the allow list", &s.allowOnly)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: every call is played (scan). On: only the calls on the allow list.");
    ImGui::SameLine(0, 20 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Hold on"); ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::InputTextWithHint("##vmhold", "nothing (scan)", b.hold, sizeof b.hold)) { s.hold = b.hold; ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only this talkgroup (or callsign) is played. Empty: scan every talkgroup.");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Hang time"); ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    float hang = (float)s.hangSec;
    if (ImGui::SliderFloat("##vmhang", &hang, 0.5f, 10.f, "%.1f s")) { s.hangSec = hang; ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How long the sound stays with a call after its last frame, so the answer in a conversation is heard\nbefore another talkgroup takes over.");
    ImGui::SameLine();
    float hs = (float)s.holdSec;
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Release after"); ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::SliderFloat("##vmholdsec", &hs, 0.f, 300.f, hs < 1.f ? "never" : "%.0f s", ImGuiSliderFlags_Logarithmic)) { s.holdSec = hs < 1.f ? 0.0 : hs; ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The hold lets go by itself after this long without a call on the held talkgroup, so you do not miss the rest of the\nband after forgetting a hold. Never: held until you release it.");
    const float h = std::max(48.f * gUi, (ImGui::GetContentRegionAvail().y - 6 * ImGui::GetTextLineHeightWithSpacing()) / 4.f);
    if (listBox("Allow list", "##vmallow", b.allow, sizeof b.allow, ids, h)) { s.allow = b.allow; ch = true; }
    if (listBox("Mute list (never played)", "##vmmute", b.mute, sizeof b.mute, ids, h)) { s.mute = b.mute; ch = true; }
    if (listBox("Priority (takes the sound from any other call)", "##vmprio", b.prio, sizeof b.prio, ids, h)) { s.priority = b.prio; ch = true; }
    if (ImGui::SmallButton("Import names...")) {
        const std::string path = plat::openFileDialog();
        if (!path.empty()) {
            std::ifstream f(path, std::ios::binary);
            std::stringstream ss; ss << f.rdbuf();
            size_t n = 0;
            const std::string add = vm::importLabels(ss.str(), vm::ListKind::Auto, &n);
            const std::string merged = vm::mergeLabels(s.labels, add);
            if (n == 0) b.importNote = "No usable lines found (expected: ID,name per line, or a RadioID.net user.csv).";
            else if (merged.size() >= sizeof b.labels) b.importNote = "That list is too big (" + std::to_string(n) + " names); trim it to the radios you hear.";
            else { s.labels = merged; snprintf(b.labels, sizeof b.labels, "%s", merged.c_str()); ch = true; b.importNote = "Imported " + std::to_string(n) + " names from " + fs::path(path).filename().string() + "."; }
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add names from a file: a RadioID.net user.csv (radio IDs with callsign and name), a BrandMeister or TGIF talkgroup\nlist, or any file with an ID and a name per line. Names with the same ID are replaced.");
    if (!b.importNote.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", b.importNote.c_str()); }
    if (listBox("Names (one per line: 91=Worldwide, RID 3120123=LY1ABC Jonas)", "##vmlabels", b.labels, sizeof b.labels, "A name for a talkgroup, radio ID or callsign, shown in the card and the lists and searched in the history.", h)) { s.labels = b.labels; ch = true; }
    if (ch) { m.setSettings(s); b.gen = m.settingsGen(); save(); }
}

void soundTab(vm::Monitor& m, vm::Settings s, Bufs& b, const char* title) {
    bool ch = false;
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Volume"); ImGui::SameLine(110 * gUi);
    ImGui::SetNextItemWidth(200 * gUi);
    float vol = s.volume * 100.f;
    if (ImGui::SliderFloat("##vmvol2", &vol, 0, 100, "%.0f %%")) { s.volume = vol / 100.f; ch = true; }
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Squelch"); ImGui::SameLine(110 * gUi);
    ImGui::SetNextItemWidth(200 * gUi);
    if (ImGui::SliderFloat("##vmsq", &s.squelchPct, 0, 20, s.squelchPct <= 0 ? "off" : "above %.1f %% errors")) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frames with more bit errors than this are not played: a call that breaks up stays quiet instead of warbling.");
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Output"); ImGui::SameLine(110 * gUi);
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::BeginCombo("##vmdev", s.device.empty() ? "system default" : s.device.c_str())) {
        if (ImGui::Selectable("system default", s.device.empty())) { s.device.clear(); ch = true; }
        for (const auto& d : dect2::AudioOut::devices()) if (!d.empty() && ImGui::Selectable(d.c_str(), d == s.device)) { s.device = d; ch = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Where the voice of %s goes: another sound card lets two modes play side by side.", title);
    ImGui::Separator();
    if (ImGui::Checkbox("Record every played call", &s.record)) ch = true;
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Folder"); ImGui::SameLine(110 * gUi);
    ImGui::SetNextItemWidth(-90 * gUi);
    if (ImGui::InputTextWithHint("##vmdir", (vm::defaultRecordDir() + "/" + m.mode()).c_str(), b.dir, sizeof b.dir)) { s.recordDir = b.dir; ch = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Open")) openFolder(recordFolder(m));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("One WAV file per call (16 bit, the codec's rate), in a folder per day, named by time, talkgroup and source, with a JSON file of the details "
                        "(talkgroup, source, slot, colour code / NAC / RAN, alias, start and end in UTC, error rate). Encrypted calls are never decoded, so never recorded.");
    ImGui::PopTextWrapPos();
    if (ch) { m.setSettings(s); b.gen = m.settingsGen(); save(); }
}

void helpTab(const char* title) {
    ImGui::PushTextWrapPos(0);
    ImGui::Text("The voice monitor of %s", title);
    ImGui::TextDisabled(
        "Every digital voice mode (DMR, P25, NXDN, dPMR, D-STAR, Yaesu System Fusion, TETRA, M17) has the same monitor.\n\n"
        "Scan and hold: one call is played at a time. With no hold, the first call heard is played and keeps the sound until it has been quiet for "
        "the hang time; then the next one may take over. Hold (in the card or a row) plays only that talkgroup until you release it.\n\n"
        "Priority: a call on the priority list takes the sound from any other call, and no other call takes it from a priority call.\n\n"
        "Mute and allow: muted talkgroups and IDs are never played. With 'Play only the allow list' on, only the listed ones are.\n\n"
        "Squelch: frames with too many bit errors are not played. The error meter in the card shows the bit errors of the call.\n\n"
        "Status: locked means a signal of the mode is being decoded. 'codec missing' means the voice codec plug-in is not installed: the calls "
        "are still listed. Install it with the ? button next to the codec line (Help > Voice codecs lists them all). Encrypted calls are shown "
        "as encrypted and are never decoded.\n\n"
        "Conversations: the calls of a talkgroup (or the two radios of a private call) on one slot, grouped while the quiet between them stays under the gap you choose. Open a row to see every call with its talker, length, signal (bit errors) and recording; Play opens the recording in your sound player. Search and the filters narrow the list, Export CSV / JSON write it into the recordings folder (exports). Encrypted calls are listed as encrypted and have no audio.\n\n"
        "Recording: each played call becomes a WAV file and a JSON file of its details. The history keeps the last 1000 calls of this session "
        "and can be searched by talkgroup, ID, callsign, name or state.");
    ImGui::PopTextWrapPos();
}

} // namespace

void window(App& a) {
    (void)a;
    if (!S.open) return;
    load();
    firstWindowSize(760 * gUi, 520 * gUi);
    if (!ImGui::Begin("Voice monitor", &S.open)) { ImGui::End(); return; }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Mode"); ImGui::SameLine();
    ImGui::SetNextItemWidth(200 * gUi);
    if (ImGui::BeginCombo("##vmmode", titleOf(S.mode))) {
        for (auto& k : kModes) if (ImGui::Selectable(k[1], S.mode == k[0])) S.mode = k[0];
        ImGui::EndCombo();
    }
    vm::Monitor& m = vm::monitor(S.mode);
    m.housekeep();
    const vm::Settings s = m.settings();
    Bufs& b = S.bufs[S.mode];
    if (b.gen != m.settingsGen()) fill(b, s, m.settingsGen());   // changed elsewhere (the card's Hold / Mute, a loaded file)
    ImGui::SameLine(0, 16 * gUi);
    vm::CallEntry now;
    if (m.nowTalking(now)) ImGui::TextColored(pal::okGreen(), "talking: %s from %s", target(now).c_str(), source(now).c_str());
    else ImGui::TextDisabled("nobody talking");
    if (ImGui::BeginTabBar("##vmtabs")) {
        if (ImGui::BeginTabItem("Calls")) { callsTab(m, s); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Conversations")) { conversationsTab(m); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("History")) { historyTab(m, b); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Last heard")) { heardTab(m); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Filters")) { filtersTab(m, s, b); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Sound")) { soundTab(m, s, b, titleOf(S.mode)); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Help")) { helpTab(titleOf(S.mode)); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace voiceui
