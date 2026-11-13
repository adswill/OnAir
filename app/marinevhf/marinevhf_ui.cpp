// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Marine VHF screens: pick a channel by number (with its name and use), up / down, the channel plan of a region (international, Europe, Baltic,
// USA, Canada), a squelch, de-emphasis or flat audio, the whole-span activity list with a click to listen, a scan of chosen channels with a
// priority watch on 16, the call log of every channel, recording of every transmission, and links to the DSC (channel 70) and AIS modes.
// With a wide radio every channel of the span is followed at once; with a narrow one the scan steps the radio from channel to channel.
#include "app.h"
#include "ui/ui_common.h"
#include "dect2/common/timecompat.h"
#include "dect2/marinevhf/marinevhf_channels.h"
#include "dect2/marinevhf/marinevhf_rx.h"
#include "dect2/marinevhf/marinevhf_scan.h"
#include "dect2/marinevhf/marinevhf_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace mv = dect2::marinevhf;

namespace {

struct State {
    bool loaded = false;
    int region = 0;
    int squelch = 50;
    bool deemph = true, prio = true, scan = false, rec = false, onlyActive = false;
    float dwell = 0.4f, hang = 2.0f;
    std::string scanList;            // "16,6,13,22A": the labels
    char recDir[400] = "";
    std::string listen;              // the label of the channel that plays ("" = automatic)
    std::string sel;                 // the label of the channel shown in the details
    bool dirty = true;
    bool wasRunning = false;
    float pushedVol = -1; bool pushedMute = false;
    int pendNum = 0; char pendSuf = 0;                      // a jump from another screen (the DSC call names a channel)
    mv::ChannelScan single;                                 // a narrow radio: the scan steps the radio
    bool singleOn = false;
    int singleKey = -2;
    int view = 0;                                           // 0 channels, 1 call log
};
State S;

bool live(const App& a) { return a.rx.standard == 156; }   // family 155, engine standard 157, active standard 156

mv::Region region() { return (mv::Region)std::max(0, std::min((int)mv::kRegionCount - 1, S.region)); }

void save() {
    plat::Prefs& d = plat::prefs();
    d.setI("mvRegion", S.region); d.setI("mvSquelch", S.squelch);
    d.setB("mvDeemph", S.deemph); d.setB("mvPrio", S.prio); d.setB("mvScan", S.scan); d.setB("mvRec", S.rec); d.setB("mvOnlyActive", S.onlyActive);
    d.setD("mvDwell", S.dwell); d.setD("mvHang", S.hang);
    d.setS("mvScanList", S.scanList); d.setS("mvRecDir", S.recDir); d.setS("mvListen", S.listen);
    d.flush();
    S.dirty = true;
}

void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.region = (int)d.getI("mvRegion", 0); S.squelch = (int)d.getI("mvSquelch", 50);
    S.deemph = d.getB("mvDeemph", true); S.prio = d.getB("mvPrio", true); S.scan = d.getB("mvScan", false); S.rec = d.getB("mvRec", false);
    S.onlyActive = d.getB("mvOnlyActive", false);
    S.dwell = (float)d.getD("mvDwell", 0.4); S.hang = (float)d.getD("mvHang", 2.0);
    S.scanList = d.getS("mvScanList", "16,6,9,12,13,14,67,68,69,71,72,73,74,77");
    S.listen = d.getS("mvListen", "");
    const std::string dir = d.getS("mvRecDir", recordingsSub("audio-recordings") + "/marine-vhf");
    snprintf(S.recDir, sizeof S.recDir, "%s", dir.c_str());
}

std::vector<std::pair<int, char>> scanChannels() {
    std::vector<std::pair<int, char>> v;
    std::stringstream ss(S.scanList);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        const mv::Channel* c = mv::findLabel(region(), tok);
        if (c && c->voice()) v.push_back({c->number, c->suffix});
    }
    return v;
}

bool inScanList(const mv::Channel& c) {
    for (auto& p : scanChannels()) if (p.first == c.number && p.second == c.suffix) return true;
    return false;
}

void setScanMember(const mv::Channel& c, bool on) {
    std::vector<std::string> labels;
    for (auto& p : scanChannels()) { const mv::Channel* q = mv::findChannel(region(), p.first, p.second); if (q && !(q == &c && !on)) labels.push_back(q->label()); }
    if (on && !inScanList(c)) labels.push_back(c.label());
    std::string s;
    for (auto& l : labels) { if (!s.empty()) s += ","; s += l; }
    S.scanList = s;
    save();
}

void setScanGroup(unsigned flags, bool all) {
    std::string s;
    for (const mv::Channel& c : mv::channels(region())) {
        if (!c.voice()) continue;
        if (all || (c.flags & flags)) { if (!s.empty()) s += ","; s += c.label(); }
    }
    S.scanList = s;
    save();
}

double listenMhz(const mv::Channel& c) { return c.duplex() ? c.coastMhz : c.shipMhz; }

// is the frequency inside the span the radio takes in now?
bool covered(const dect2::MarineVhfTelemetry& t, double mhz) {
    if (t.centerHz <= 0 || t.inputRate <= 0) return true;   // not known: do not move the dial
    return std::fabs(mhz * 1e6 - t.centerHz) < t.inputRate * 0.42;
}

// the dial onto a channel outside the span taken in (on the 25 kHz raster: the receiver puts the DC spike between two channels)
void tuneDialTo(App& a, double mhz) {
    a.freqMhz = std::round(mhz * 1e6 / 25e3) * 25e3 / 1e6;
    a.tune.centerHz = a.freqMhz * 1e6;
    if (a.engine.running()) { a.engine.retuneReset(a.tune); a.peak.clear(); }
    savePrefs(a);
}

const mv::Channel* selected() { return mv::findLabel(region(), S.sel.empty() ? S.listen : S.sel); }

void pick(App& a, const mv::Channel& c) {
    S.sel = c.label();
    if (c.voice()) {
        S.listen = c.label();
        if (live(a) && !covered(a.rx.marinevhf, listenMhz(c))) tuneDialTo(a, listenMhz(c));
        else if (!a.engine.running()) tuneFreq(a, listenMhz(c));   // stopped: the channel sets the frequency too
    }
    save();
}

std::string timeText(int64_t wall) {
    if (!wall) return "";
    struct tm tmv {};
    dect2::localTime((time_t)wall, &tmv);
    char b[32];
    snprintf(b, sizeof b, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return b;
}

std::string agoText(double s) {
    char b[32];
    if (s < 0) return "";
    if (s < 90) snprintf(b, sizeof b, "%.0f s ago", s);
    else if (s < 5400) snprintf(b, sizeof b, "%.0f min ago", s / 60);
    else snprintf(b, sizeof b, "%.1f h ago", s / 3600);
    return b;
}

std::string flagsText(const mv::Channel& c) {
    std::string s;
    auto add = [&](unsigned f, const char* t) { if (c.has(f)) { if (!s.empty()) s += ", "; s += t; } };
    add(mv::kDistress, "distress"); add(mv::kCalling, "calling"); add(mv::kSar, "SAR"); add(mv::kBridge, "bridge to bridge"); add(mv::kPort, "port operations / VTS");
    add(mv::kIntership, "intership"); add(mv::kPublic, "public correspondence"); add(mv::kSafetyBroadcast, "safety broadcasts"); add(mv::kGovernment, "Coast Guard / government");
    add(mv::kLowPower, "low power");
    return s;
}

// ---------------------------------------------------------------- the controls

void controls(App& a) {
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Plan"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(150 * gUi);
    if (ImGui::BeginCombo("##mvreg", mv::regionName(region()))) {
        for (int i = 0; i < mv::kRegionCount; i++) if (ImGui::Selectable(mv::regionName((mv::Region)i), i == S.region)) { S.region = i; S.sel.clear(); S.listen.clear(); save(); }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "The channel plan: international (ITU Appendix 18), Europe, Baltic Sea, USA or Canada. The numbers and the A / B variants differ.");
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Channel"); ImGui::SameLine(0, 5 * gUi);
    const mv::Channel* cur = selected();
    if (ImGui::ArrowButton("##mvdown", ImGuiDir_Down)) { const mv::Channel* n = mv::stepChannel(region(), cur, -1); if (n) pick(a, *n); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "The previous voice channel");
    ImGui::SameLine(0, 3 * gUi);
    ImGui::SetNextItemWidth(300 * gUi);
    if (ImGui::BeginCombo("##mvch", cur ? cur->title().c_str() : "Automatic: the strongest open channel", ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("Automatic: the strongest open channel", !cur)) { S.sel.clear(); S.listen.clear(); save(); }
        for (const mv::Channel& c : mv::channels(region())) {
            char b[200];
            snprintf(b, sizeof b, "%-4s %s%s   %.3f MHz", c.label().c_str(), c.name.c_str(), c.voice() ? "" : " (data)", c.shipMhz);
            if (ImGui::Selectable(b, cur == &c)) pick(a, c);
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Pick a channel by number. The radio follows every channel of its span at once; this chooses the one you hear.");
    ImGui::SameLine(0, 3 * gUi);
    if (ImGui::ArrowButton("##mvup", ImGuiDir_Up)) { const mv::Channel* n = mv::stepChannel(region(), cur, +1); if (n) pick(a, *n); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "The next voice channel");
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Squelch"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(120 * gUi);
    ImGui::SliderInt("##mvsq", &S.squelch, 0, 100, "%d %%");
    if (ImGui::IsItemDeactivatedAfterEdit()) save();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Quieting squelch: opens when the FM noise drops, whatever the signal strength. 0 stays open, higher wants a cleaner signal.");
    flowNext(10 * gUi);
    if (ImGui::Checkbox("De-emphasis##mvde", &S.deemph)) save();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Marine radios send pre-emphasised audio (6 dB per octave, 750 us); this takes it out. Off gives flat audio, brighter and noisier.");
    flowNext(10 * gUi);
    if (ImGui::Checkbox("Watch channel 16##mvpr", &S.prio)) save();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Priority watch: when 16 opens it takes over from the channel you hear, and the radio goes back when it has been quiet for the hang time.");
    flowNext(10 * gUi);
    if (ImGui::Checkbox("Scan##mvscan", &S.scan)) save();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Play the first of the chosen channels that opens, stay on it until it has been quiet for the hang time, then go on. A narrow radio steps from channel to channel.");
    if (S.scan) {
        ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(90 * gUi);
        ImGui::SliderFloat("##mvdw", &S.dwell, 0.1f, 3.f, "dwell %.1f s");
        if (ImGui::IsItemDeactivatedAfterEdit()) save();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "A narrow radio: how long it listens on each channel before it moves on.");
        ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(90 * gUi);
        ImGui::SliderFloat("##mvhg", &S.hang, 0.f, 6.f, "hang %.1f s");
        if (ImGui::IsItemDeactivatedAfterEdit()) save();
        ImGui::SameLine(0, 5 * gUi);
        if (ImGui::Button("Channels...##mvsl")) ImGui::OpenPopup("##mvscanpop");
        if (ImGui::BeginPopup("##mvscanpop")) {
            if (ImGui::SmallButton("All voice")) setScanGroup(0, true);
            ImGui::SameLine(); if (ImGui::SmallButton("Safety")) setScanGroup(mv::kDistress | mv::kSar | mv::kBridge | mv::kCalling, false);
            ImGui::SameLine(); if (ImGui::SmallButton("Port ops")) setScanGroup(mv::kPort, false);
            ImGui::SameLine(); if (ImGui::SmallButton("Intership")) setScanGroup(mv::kIntership, false);
            ImGui::SameLine(); if (ImGui::SmallButton("Public")) setScanGroup(mv::kPublic, false);
            ImGui::SameLine(); if (ImGui::SmallButton("Coast Guard")) setScanGroup(mv::kGovernment, false);
            ImGui::SameLine(); if (ImGui::SmallButton("None")) { S.scanList.clear(); save(); }
            ImGui::Separator();
            ImGui::BeginChild("##mvsc", ImVec2(420 * gUi, 260 * gUi));
            for (const mv::Channel& c : mv::channels(region())) {
                if (!c.voice()) continue;
                bool on = inScanList(c);
                char b[160]; snprintf(b, sizeof b, "%s  %s##mvsm%s", c.label().c_str(), c.name.c_str(), c.label().c_str());
                if (ImGui::Checkbox(b, &on)) setScanMember(c, on);
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }
    }
    flowNext(10 * gUi);
    if (ImGui::Checkbox("Record##mvrec", &S.rec)) save();
    ImGui::SameLine(); kit::recordingsButton("mv", "audio-recordings/marine-vhf");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "One WAV file per transmission, for every channel of the span: time, channel and frequency in the name.");
    if (S.rec) {
        ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(230 * gUi);
        ImGui::InputText("##mvdir", S.recDir, sizeof S.recDir);
        if (ImGui::IsItemDeactivatedAfterEdit()) save();
        ImGui::SameLine(0, 3 * gUi);
        if (ImGui::SmallButton("...##mvdb")) { const std::string p = plat::openFolderDialog(); if (!p.empty()) { snprintf(S.recDir, sizeof S.recDir, "%s", p.c_str()); save(); } }
        if (live(a)) { ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("%d saved", a.rx.marinevhf.recordedFiles); }
    }
    flowEnd();
}

void links(App& a, const mv::Channel& c) {
    if (c.use == mv::Use::Dsc) {
        if (ImGui::SmallButton("Open the DSC mode (digital selective calling)##mvdsc")) selectMode(a, 15, "dsc");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Channel 70 carries data, not voice: the DSC mode decodes distress alerts and calls and names the channel to go to.");
    } else if (c.use == mv::Use::Ais || c.use == mv::Use::LongRangeAis) {
        if (ImGui::SmallButton("Open the AIS mode##mvais")) selectMode(a, 14);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "AIS is data, not voice: the AIS mode shows the ships on 161.975 and 162.025 MHz.");
    }
}

void detail(App& a) {
    const mv::Channel* c = selected();
    if (!c) { ImGui::TextDisabled("%s", "Automatic: the strongest open channel plays. Click a channel in the list to listen to it."); return; }
    ImGui::PushFont(a.mono, 0);
    ImGui::Text("Ch %s  %s", c->label().c_str(), c->name.c_str());
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", c->purpose.c_str());
    ImGui::PopTextWrapPos();
    if (c->duplex()) ImGui::Text("Ship transmits %.3f MHz, coast station %.3f MHz (duplex)", c->shipMhz, c->coastMhz);
    else ImGui::Text("%.3f MHz (simplex)", c->shipMhz);
    const std::string f = flagsText(*c);
    ImGui::TextDisabled("%s%.4g W%s%s", c->powerW <= 1 ? "limited to " : "up to ", c->powerW, f.empty() ? "" : "   ", f.c_str());
    links(a, *c);
}

// ---------------------------------------------------------------- the lists

void activity(App& a) {
    const bool on = live(a);
    const dect2::MarineVhfTelemetry* t = on ? &a.rx.marinevhf : nullptr;
    if (ImGui::Checkbox("Only channels that are active or heard##mvoa", &S.onlyActive)) save();
    ImGui::SameLine(0, 10 * gUi);
    if (t) ImGui::TextDisabled("%d carriers followed, %llu calls", t->carriers, (unsigned long long)t->callsTotal);
    else ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver: a wide radio (8 Msps) follows every channel at once");
    if (!ImGui::BeginTable("##mvact", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, std::max(160.f * gUi, ImGui::GetContentRegionAvail().y - 4 * gUi)))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("", 0, 18 * gUi); ImGui::TableSetupColumn("Ch"); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("State"); ImGui::TableSetupColumn("Calls"); ImGui::TableSetupColumn("Total"); ImGui::TableSetupColumn("Last heard");
    ImGui::TableHeadersRow();
    const auto& chs = mv::channels(region());
    const mv::Channel* sel = selected();
    for (size_t i = 0; i < chs.size(); i++) {
        const mv::Channel& c = chs[i];
        const dect2::MarineVhfChannelRow* r = (t && t->region == S.region && i < t->rows.size()) ? &t->rows[i] : nullptr;
        if (S.onlyActive && !(r && (r->active || r->open || r->calls))) continue;
        ImGui::TableNextRow();
        ImGui::PushID((int)i);
        ImGui::TableNextColumn();
        if (r && r->playing) ImGui::TextColored(pal::okGreen(), ">"); else ImGui::TextUnformatted(" ");
        ImGui::TableNextColumn();
        ImGui::PushFont(a.mono, 0);
        const bool pri = c.number == 16 && !c.suffix;
        if (pri) ImGui::PushStyleColor(ImGuiCol_Text, pal::badRed());
        if (ImGui::Selectable(c.label().c_str(), sel == &c, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) pick(a, c);
        if (pri) ImGui::PopStyleColor();
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", c.title().c_str(), c.purpose.c_str());
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::Text("%.3f", r && r->mhz > 0 ? r->mhz : c.shipMhz); ImGui::PopFont();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(c.name.c_str());
        if (!c.voice()) { ImGui::SameLine(0, 4 * gUi); links(a, c); }
        ImGui::TableNextColumn();
        if (r && (r->active || r->open) && r->levelDb > -150) {
            const float frac = std::max(0.f, std::min(1.f, ((float)r->levelDb + 60.f) / 50.f));
            char ov[24]; snprintf(ov, sizeof ov, "%.0f dB", r->levelDb);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, r->open ? pal::accent() : pal::grey(0.45f));
            ImGui::ProgressBar(frac, ImVec2(80 * gUi, 0), ov);
            ImGui::PopStyleColor();
        } else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        if (r && r->open) ImGui::TextColored(pal::okGreen(), "OPEN");
        else if (r && r->active) ImGui::TextDisabled("carrier");
        else if (!c.voice()) ImGui::TextDisabled("data");
        else ImGui::TextDisabled(" ");
        if (r && r->inScan) { ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("scan"); }
        ImGui::TableNextColumn(); if (r && r->calls) ImGui::Text("%d", r->calls);
        ImGui::TableNextColumn(); if (r && r->calls) ImGui::Text("%.0f s", r->totalSec);
        ImGui::TableNextColumn();
        if (r && r->open) ImGui::TextColored(pal::okGreen(), "now");
        else if (r && r->calls) ImGui::TextDisabled("%s", agoText(r->lastHeardAgoSec).c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void callLog(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver"); return; }
    const dect2::MarineVhfTelemetry& t = a.rx.marinevhf;
    if (t.calls.empty()) { ImGui::TextDisabled("%s", "no call yet"); return; }
    if (!ImGui::BeginTable("##mvcalls", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, std::max(160.f * gUi, ImGui::GetContentRegionAvail().y - 4 * gUi)))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Ch"); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("Length"); ImGui::TableSetupColumn("Peak");
    ImGui::TableSetupColumn("Mean"); ImGui::TableSetupColumn("Recording", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const dect2::MarineVhfCall& c : t.calls) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(timeText(c.wallTime).c_str());
        ImGui::TableNextColumn();
        char l[16]; snprintf(l, sizeof l, "%d%c", c.number, c.suffix ? c.suffix : ' ');
        ImGui::PushID((int)c.serial);
        if (ImGui::Selectable(c.number ? l : "?", false, ImGuiSelectableFlags_SpanAllColumns)) { const mv::Channel* ch = mv::findChannel(region(), c.number, c.suffix); if (ch) pick(a, *ch); }
        ImGui::PopID();
        ImGui::TableNextColumn(); ImGui::Text("%.3f", c.mhz);
        ImGui::TableNextColumn(); ImGui::Text("%.1f s", c.durationSec);
        ImGui::TableNextColumn(); ImGui::Text("%.0f dB", c.peakDb);
        ImGui::TableNextColumn(); ImGui::Text("%.0f dB", c.meanDb);
        ImGui::TableNextColumn();
        if (!c.file.empty()) { const std::string f = std::filesystem::path(c.file).filename().string(); ImGui::TextDisabled("%s", f.c_str()); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.file.c_str()); }
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------- the engine side

void tick(App& a) {
    load();
    if (!a.engine.running()) { S.wasRunning = false; S.singleOn = false; return; }
    dect2::MarineVhfReceiver& r = a.engine.marineVhf();
    const bool lv = live(a);
    if (S.pendNum) {   // a jump from the DSC screen: pick that channel
        if (const mv::Channel* c = mv::findChannel(region(), S.pendNum, S.pendSuf)) pick(a, *c);
        else if (const mv::Channel* c2 = mv::findChannel(region(), S.pendNum, 0)) pick(a, *c2);
        S.pendNum = 0; S.pendSuf = 0;
    }
    // a narrow radio: the scan steps the radio, with the receiver following whatever it is tuned to
    bool single = false;
    if (S.scan && lv) {
        const dect2::MarineVhfTelemetry& t = a.rx.marinevhf;
        for (auto& p : scanChannels()) { const mv::Channel* c = mv::findChannel(region(), p.first, p.second); if (c && !covered(t, listenMhz(*c))) single = true; }
        if (S.prio) { const mv::Channel* c16 = mv::findChannel(region(), 16, 0); if (c16 && !covered(t, listenMhz(*c16))) single = true; }
    }
    if (single) {
        const dect2::MarineVhfTelemetry& t = a.rx.marinevhf;
        const auto& chs = mv::channels(region());
        if (!S.singleOn) {
            std::vector<int> keys;
            for (auto& p : scanChannels()) for (size_t i = 0; i < chs.size(); i++) if (chs[i].number == p.first && chs[i].suffix == p.second) keys.push_back((int)i);
            mv::ScanConfig cfg;
            cfg.singleTuner = true; cfg.dwellSec = S.dwell + 0.5; cfg.hangSec = S.hang;   // a retune takes the radio and the carrier search about half a second
            cfg.priorityKey = -1;
            if (S.prio) for (size_t i = 0; i < chs.size(); i++) if (chs[i].number == 16 && !chs[i].suffix) cfg.priorityKey = (int)i;
            cfg.priorityEverySec = 4; cfg.priorityDwellSec = 0.9;
            S.single.setChannels(keys); S.single.setConfig(cfg);
            S.single.start(ImGui::GetTime());
            S.singleOn = true; S.singleKey = -2;
        }
        const int k = S.single.update(ImGui::GetTime(), [&](int key) { return key >= 0 && (size_t)key < t.rows.size() && t.rows[(size_t)key].open; });
        if (k != S.singleKey && k >= 0 && (size_t)k < chs.size()) { S.singleKey = k; tuneDialTo(a, listenMhz(chs[(size_t)k])); }
    } else if (S.singleOn) { S.single.stop(); S.singleOn = false; }
    if (!S.wasRunning || S.dirty) {
        r.setRegion(region());
        r.setSquelch(S.squelch); r.setDeemphasis(S.deemph); r.setPriority16(S.prio && !single);
        r.setScanChannels(scanChannels()); r.setScanTiming(S.dwell, S.hang); r.setScan(S.scan && !single);
        const mv::Channel* l = S.listen.empty() ? nullptr : mv::findLabel(region(), S.listen);
        if (l) r.listen(l->number, l->suffix); else r.listen(0);
        r.setRecording(S.rec, S.recDir);
        S.dirty = false;
    }
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    S.wasRunning = true;
}

void tab(App& a) {
    load();
    controls(a);
    ImGui::Separator();
    detail(a);
    ImGui::Separator();
    subNav("mvv", S.view, {"Channels", "Call log"});
    if (S.view == 0) activity(a); else callLog(a);
}

void list(App& a) {
    load();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::MarineVhfTelemetry& t = a.rx.marinevhf;
    const auto& chs = mv::channels(region());
    if (!ImGui::BeginTable("##mvl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Ch"); ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch);
    for (size_t i = 0; i < t.rows.size() && i < chs.size(); i++) {
        const auto& r = t.rows[i];
        if (!(r.active || r.open || r.calls)) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(chs[i].label().c_str()); ImGui::PopFont();
        ImGui::TableNextColumn();
        if (r.open) ImGui::TextColored(pal::okGreen(), "%s OPEN", chs[i].name.c_str());
        else if (r.active) ImGui::TextDisabled("%s carrier", chs[i].name.c_str());
        else ImGui::TextDisabled("%s  %d calls", chs[i].name.c_str(), r.calls);
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::MarineVhfTelemetry& t = a.rx.marinevhf;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    int open = 0, act = 0;
    for (const auto& r : t.rows) { open += r.open; act += r.active; }
    kv("state", "%s", dect2::marineVhfSummary(t).c_str());
    kv("plan", "%s", mv::regionName((mv::Region)t.region));
    kv("channels", "%d carriers, %d active, %d open", t.carriers, act, open);
    kv("plays", "%s", t.playingNumber ? (std::to_string(t.playingNumber) + (t.playingSuffix ? std::string(1, t.playingSuffix) : std::string())).c_str() : "nothing");
    kv("calls", "%llu, %d recorded", (unsigned long long)t.callsTotal, t.recordedFiles);
    kv("radio centre", "%.4f MHz, %.3f Msps", t.centerHz / 1e6, t.inputRate / 1e6);
    kv("signal time", "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool on = live(a);
    StatusPanel panel;
    statusIqLamp(a);
    lamp("Open", !on ? 0 : a.rx.marinevhf.state == 2 ? 1 : 0); flowNext(10 * gUi);
    statusState(a, on, [&] { return dect2::marineVhfSummary(a.rx.marinevhf); });
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Marine VHF";
    if (live(a)) l2 = dect2::marineVhfSummary(a.rx.marinevhf);
}

} // namespace

// Called from the DSC screen: open the Marine VHF mode on this channel
void marineVhfJump(App& a, int number, char suffix) {
    load();
    S.pendNum = number; S.pendSuf = suffix;
    selectMode(a, 155);
}

extern const ModeUi kMarineVhfUi;
const ModeUi kMarineVhfUi = {
    .sideTitle = "ACTIVE",
    .tabName = "Marine VHF",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
