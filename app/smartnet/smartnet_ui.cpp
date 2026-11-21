// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Motorola Type II (SmartNet, SmartZone) screens: lock and word counts, the calls followed inside the capture with the sound of one of them, the talkgroups, the sites and patches, the control channel messages. One click on top; the band plan and the call filters under Advanced.
#include "app.h"
#include "i18n/i18n.h"
#include "dect2/smartnet/smartnet_rx.h"
#include "dect2/smartnet/smartnet_tel.h"
#include "platform/platform.h"
#include "trunkpool/trunk1.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace sn = dect2::smartnet;

bool live(const App& a) { return a.rx.standard == 321; }   // family 320, engine standard 322, active standard 321

struct State {
    int band = 0;                        // 0 automatic, then the plans
    // the Custom plan: three ranges like OP25's bp_base / bp_mid / bp_high; a mid or high base of 0 and a spacing of 0 follow the range below
    double baseMhz = 0, spacingKhz = 12.5, midMhz = 0, midSpacingKhz = 0, highMhz = 0, highSpacingKhz = 0;
    int baseOffset = 380, midOffset = 760, highOffset = 760;
    bool loaded = false;
    char groups[160] = "";
    bool follow = true, followPrivate = false, record = false;
    int listen = -1;
    bool dirty = true, wasRunning = false;
    float pushedVol = -1; bool pushedMute = false;
};
State S;

const sn::Band kBands[] = {sn::Band::Auto, sn::Band::Domestic800, sn::Band::Rebanded800, sn::Band::Splinter800, sn::Band::International800, sn::Band::InternationalSplinter800, sn::Band::Us900, sn::Band::Custom};

std::vector<int> parseGroups(const char* text) {
    std::vector<int> g;
    std::string s = text;
    for (char& c : s) if (c == ',' || c == ';') c = ' ';
    std::istringstream in(s);
    int v;
    while (in >> v) g.push_back(v);
    return g;
}

void loadPrefs() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.band = (int)std::clamp(d.getI("snBand", 0), 0L, 7L);
    S.baseMhz = d.getD("snBaseMhz", 0); S.spacingKhz = d.getD("snSpacingKhz", 12.5); S.baseOffset = (int)d.getI("snBaseOffset", 380);
    S.midMhz = d.getD("snMidMhz", 0); S.midSpacingKhz = d.getD("snMidSpacingKhz", 0); S.midOffset = (int)d.getI("snMidOffset", 760);
    S.highMhz = d.getD("snHighMhz", 0); S.highSpacingKhz = d.getD("snHighSpacingKhz", 0); S.highOffset = (int)d.getI("snHighOffset", 760);
}

void savePrefs() {
    plat::Prefs& d = plat::prefs();
    d.setI("snBand", S.band);
    d.setD("snBaseMhz", S.baseMhz); d.setD("snSpacingKhz", S.spacingKhz); d.setI("snBaseOffset", S.baseOffset);
    d.setD("snMidMhz", S.midMhz); d.setD("snMidSpacingKhz", S.midSpacingKhz); d.setI("snMidOffset", S.midOffset);
    d.setD("snHighMhz", S.highMhz); d.setD("snHighSpacingKhz", S.highSpacingKhz); d.setI("snHighOffset", S.highOffset);
    d.flush();
}

std::string mhz(double hz) { if (hz <= 0) return "-"; char b[32]; snprintf(b, sizeof b, "%.4f MHz", hz / 1e6); return b; }
std::string idOf(int v) { return v < 0 ? std::string("-") : std::to_string(v); }

void tick(App& a) {
    loadPrefs();
    if (!a.engine.running()) { S.wasRunning = false; return; }
    dect2::SmartnetReceiver& r = a.engine.smartnet();
    if (!S.wasRunning) { S.dirty = true; S.pushedVol = -1; }
    if (S.dirty) {
        sn::CustomPlan c;
        c.baseHz = S.baseMhz * 1e6; c.baseSpacingHz = S.spacingKhz * 1e3; c.baseOffset = S.baseOffset;
        c.midHz = S.midMhz * 1e6; c.midSpacingHz = S.midSpacingKhz * 1e3; c.midOffset = S.midOffset;
        c.highHz = S.highMhz * 1e6; c.highSpacingHz = S.highSpacingKhz * 1e3; c.highOffset = S.highOffset;
        r.setBand(kBands[std::clamp(S.band, 0, 7)], c);
        r.setFollow(S.follow); r.setFollowPrivate(S.followPrivate); r.setFollowGroups(parseGroups(S.groups)); r.setListenTg(S.listen);
        r.setRecordDir(S.record ? plat::dataDir() + "/recordings/typeii" : std::string());
        S.dirty = false;
    }
    if (a.volume != S.pushedVol || a.muted != S.pushedMute) { r.setVolume(a.volume); r.setMuted(a.muted); S.pushedVol = a.volume; S.pushedMute = a.muted; }
    S.wasRunning = true;
}

void advanced() {
    if (!ImGui::CollapsingHeader(trw("Advanced"))) return;
    const char* names[] = {tr("Automatic (from the frequency and the control channel)"), "800 MHz domestic", "800 MHz rebanded", "800 MHz domestic splinter", "800 MHz international", "800 MHz international splinter", "900 MHz", tr("Custom (OBT, VHF / UHF)")};
    ImGui::SetNextItemWidth(320 * gUi);
    if (ImGui::Combo(tr("Band plan"), &S.band, names, IM_ARRAYSIZE(names))) { S.dirty = true; savePrefs(); }
    if (S.band == 7) {
        struct Row { const char* label; double* mhz; double* khz; int* off; } rows[3] = {
            {"Range 1", &S.baseMhz, &S.spacingKhz, &S.baseOffset}, {"Range 2", &S.midMhz, &S.midSpacingKhz, &S.midOffset}, {"Range 3", &S.highMhz, &S.highSpacingKhz, &S.highOffset}};
        bool ch = false;
        for (int i = 0; i < 3; i++) {
            ImGui::PushID(i);
            ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(tr(rows[i].label)); ImGui::SameLine();
            ImGui::SetNextItemWidth(110 * gUi);
            ch |= ImGui::InputDouble(tr("First channel (MHz)"), rows[i].mhz, 0, 0, "%.4f");
            ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi);
            ch |= ImGui::InputDouble(tr("Spacing (kHz)"), rows[i].khz, 0, 0, "%.3f");
            ImGui::SameLine(); ImGui::SetNextItemWidth(60 * gUi);
            ch |= ImGui::InputInt(tr("From channel"), rows[i].off, 0, 0);
            ImGui::PopID();
        }
        ImGui::TextDisabled("%s", tr("Ranges 2 and 3 are optional: leave the frequency at 0 to follow the range before. Channels below range 1's start are transmit channels."));
        if (ch) { S.dirty = true; savePrefs(); }
    }
    if (ImGui::Checkbox(tr("Follow calls"), &S.follow)) S.dirty = true;
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Also follow unit-to-unit calls"), &S.followPrivate)) S.dirty = true;
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Record each call"), &S.record)) S.dirty = true;
    ImGui::SetNextItemWidth(240 * gUi);
    if (ImGui::InputText(tr("Only talkgroups"), S.groups, sizeof S.groups)) S.dirty = true;
    ImGui::SameLine(); ImGui::TextDisabled("%s", tr("(empty: all; numbers separated by commas)"));
}

void tab(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", tr(a.engine.running() ? "starting" : "tune to a Motorola Type II control channel (3600 bit/s) and start the receiver")); return; }
    const dect2::SmartnetTelemetry& t = a.rx.smartnet;
    ImGui::TextUnformatted(tr("Motorola Type II trunking: 3600 bit/s control channel, calls followed inside the capture"));
    ImGui::Spacing();
    ImGui::Text("%s: %s   %s: %llu   %s: %llu", tr("Lock"), t.locked ? tr("yes") : tr("no"), tr("Good words"), (unsigned long long)t.oswOk, tr("Bad words"), (unsigned long long)t.oswBad);
    ImGui::Text("%s: %s", tr("System"), t.smartZone ? "SmartZone" : (t.dataValid ? "SmartNet" : "-"));
    if (t.systemId >= 0) { ImGui::SameLine(); ImGui::Text("0x%04X", t.systemId); }
    if (t.siteId >= 0) { ImGui::SameLine(); ImGui::Text("%s %d", tr("site"), t.siteId); }
    if (t.features >= 0) { ImGui::SameLine(); ImGui::TextDisabled("%s", sn::featuresText(t.features).c_str()); }
    ImGui::TextDisabled("%s: %s%s   %s: %.0f dBFS, %s: %.1f dB", tr("Band plan"), t.band.c_str(), t.bandAuto ? tr(" (automatic)") : "", tr("Level"), t.levelDb, tr("SNR"), t.snrDb);
    if (!t.note.empty()) ImGui::TextColored(pal::warnAmber(), "%s", tr(t.note.c_str()));
    if (!t.poolStatus.empty()) ImGui::TextDisabled("%s", t.poolStatus.c_str());
    trunk1::section(a);
    advanced();
    ImGui::Spacing();
    ImGui::SeparatorText(tr("Calls"));
    ImGui::TextDisabled("%s", tr("Analog calls as FM; digital calls through the P25 receiver and the user-installed IMBE plug-in (without it they are logged, without sound). Encrypted calls are logged and not followed."));
    if (ImGui::Button(S.listen < 0 ? tr("Listening: newest call") : tr("Listening: one talkgroup (click for newest)"))) { S.listen = -1; S.dirty = true; }
    if (ImGui::BeginTable("sncall", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, 170 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("State")); ImGui::TableSetupColumn(tr("Talkgroup")); ImGui::TableSetupColumn(tr("From")); ImGui::TableSetupColumn(tr("Frequency"));
        ImGui::TableSetupColumn(tr("Type")); ImGui::TableSetupColumn(tr("Seconds")); ImGui::TableSetupColumn(tr("Sound (s)")); ImGui::TableSetupColumn(tr("Notes"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const dect2::SmartnetCall& c : t.calls) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(c.active ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", c.listening ? tr("listening") : c.state.c_str());
            ImGui::TableNextColumn();
            char lbl[40]; snprintf(lbl, sizeof lbl, "%s%d##sncl%llu", c.group ? "" : "unit ", c.tg, (unsigned long long)c.id);
            if (ImGui::Selectable(lbl) && c.group) { S.listen = c.tg; S.dirty = true; }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(idOf(c.src).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mhz(c.freqHz).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s%s%s", c.digital ? tr("digital") : tr("analog"), c.emergency ? tr(" emergency") : "", c.encrypted ? tr(" encrypted") : "");
            ImGui::TableNextColumn(); ImGui::Text("%.1f", c.durationSec);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", c.audioSec);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.note.c_str());
        }
        ImGui::EndTable();
    }
    if (!t.talkgroups.empty() && ImGui::CollapsingHeader(trw("Talkgroups"), ImGuiTreeNodeFlags_DefaultOpen) && ImGui::BeginTable("sntg", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(tr("Talkgroup")); ImGui::TableSetupColumn(tr("Calls")); ImGui::TableSetupColumn(tr("Last unit")); ImGui::TableSetupColumn(tr("Frequency")); ImGui::TableSetupColumn(tr("Type"));
        ImGui::TableHeadersRow();
        for (const dect2::SmartnetTalkgroup& g : t.talkgroups) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(g.active ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%d", g.tg);
            ImGui::TableNextColumn(); ImGui::Text("%u", g.calls);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(idOf(g.lastSrc).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mhz(g.lastFreqHz).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s%s%s", g.digital ? tr("digital") : tr("analog"), g.encrypted ? tr(" encrypted") : "", g.patched ? tr(" patch") : "");
        }
        ImGui::EndTable();
    }
    if ((!t.sites.empty() || !t.patches.empty()) && ImGui::CollapsingHeader(trw("Sites and patches"))) {
        for (const dect2::SmartnetSiteRow& s : t.sites) ImGui::Text("%s %d: %s, %s, %s", s.adjacent ? tr("Adjacent site") : tr("Site"), s.site, sn::siteBandName(s.band), sn::featuresText(s.features).c_str(), mhz(s.controlHz).c_str());
        for (const dect2::SmartnetPatch& p : t.patches) ImGui::Text("%s %d: %s %d", tr("Patch"), p.tg, tr("includes"), p.member);
    }
}

void receiver(App& a) {
    if (!live(a)) return;
    ImGui::TextUnformatted(dect2::smartnetSummary(a.rx.smartnet).c_str());
}

void list(App& a) {
    if (!live(a)) return;
    const dect2::SmartnetTelemetry& t = a.rx.smartnet;
    if (ImGui::BeginTable("snmsg", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(tr("Time (s)")); ImGui::TableSetupColumn("OSW"); ImGui::TableSetupColumn(tr("Message"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const dect2::SmartnetRow& r : t.rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.1f", r.timeSec);
            ImGui::TableNextColumn(); ImGui::Text("%04X %c %03X", r.addr, r.group ? 'G' : 'I', r.cmd);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.text.c_str());
        }
        ImGui::EndTable();
    }
}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State"));
    ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::smartnetSummary(a.rx.smartnet).c_str() : tr(a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Motorola Type II";
    if (live(a)) l2 = dect2::smartnetSummary(a.rx.smartnet);
}

} // namespace

extern const ModeUi kSmartnetUi;
const ModeUi kSmartnetUi = {
    .sideTitle = "MESSAGES",
    .tabName = "Type II",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
