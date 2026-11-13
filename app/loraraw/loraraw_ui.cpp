// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRa (raw) screens: the frame list (time, spreading factor, bandwidth, coding rate, length, CRC, SNR, level, carrier offset, payload as hex and text),
// the decoder list, the receiver details, the frequency presets (EU868, US915, 433 MHz) and the settings: spreading factor, bandwidth, header mode, coding
// rate, CRC, length, sync word. Spreading factor and bandwidth can be left on Auto (every combination is searched).
// Receive only: nothing is ever sent, nothing is decrypted or interpreted; the payload is shown as the bytes it is.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "dect2/loraraw/loraraw_rx.h"
#include "dect2/loraraw/loraraw_tel.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct Preset { const char* name; double mhz; };
const Preset kPresets[] = {
    {"EU868 channel 1 (868.1)", 868.1}, {"EU868 channel 2 (868.3)", 868.3}, {"EU868 channel 3 (868.5)", 868.5}, {"EU868 867.1", 867.1}, {"EU868 high power (869.525)", 869.525},
    {"US915 uplink 1 (902.3)", 902.3}, {"US915 uplink 2 (902.5)", 902.5}, {"US915 500 kHz (903.0)", 903.0}, {"US915 ISM (915.0)", 915.0}, {"US915 downlink (923.3)", 923.3},
    {"433 ISM (433.175)", 433.175}, {"433 ISM (433.920)", 433.92}, {"433 ISM (434.000)", 434.0},
};
const char* const kCr[] = {"4/5", "4/6", "4/7", "4/8"};
const char* const kBwName[] = {"62.5 kHz", "125 kHz", "250 kHz", "500 kHz"};
const int kBwKhz[] = {62, 125, 250, 500};

struct State {
    bool loaded = false;
    dect2::LoraRawSettings cfg;
    int syncChoice = 0;                  // 0 0x12, 1 0x34, 2 0x2B, 3 any of the three, 4 custom
    char syncHex[8] = "12";
    int view = 0;                        // 0 frames, 1 decoders
    bool hideBad = false;
    char filter[64] = "";
    int sel = -1;
};
State S;

void saveState() {
    auto& p = plat::prefs();
    p.setB("lrAutoSf", S.cfg.autoSf); p.setI("lrSf", S.cfg.sf); p.setB("lrAutoBw", S.cfg.autoBw); p.setI("lrBw", S.cfg.bwKhz);
    p.setI("lrCr", S.cfg.cr); p.setB("lrCrc", S.cfg.crc); p.setB("lrImplicit", S.cfg.implicit); p.setI("lrLen", S.cfg.length);
    p.setI("lrSync", S.cfg.sync); p.setI("lrPre", S.cfg.preamble); p.setI("lrLdro", S.cfg.ldro);
    p.flush();
}

void syncChoiceFromCfg() {
    S.syncChoice = S.cfg.sync == 0x12 ? 0 : S.cfg.sync == 0x34 ? 1 : S.cfg.sync == 0x2B ? 2 : S.cfg.sync < 0 ? 3 : 4;
    if (S.syncChoice == 4) snprintf(S.syncHex, sizeof S.syncHex, "%02X", S.cfg.sync);
}

void loadState(App& a) {
    if (S.loaded) return;
    S.loaded = true;
    auto& p = plat::prefs();
    S.cfg.autoSf = p.getB("lrAutoSf", true); S.cfg.sf = (int)p.getI("lrSf", 7); S.cfg.autoBw = p.getB("lrAutoBw", true); S.cfg.bwKhz = (int)p.getI("lrBw", 125);
    S.cfg.cr = (int)p.getI("lrCr", 5); S.cfg.crc = p.getB("lrCrc", true); S.cfg.implicit = p.getB("lrImplicit", false); S.cfg.length = (int)p.getI("lrLen", 16);
    S.cfg.sync = (int)p.getI("lrSync", 0x12); S.cfg.preamble = (int)p.getI("lrPre", 8); S.cfg.ldro = (int)p.getI("lrLdro", -1);
    S.cfg.sf = std::max(5, std::min(12, S.cfg.sf));
    syncChoiceFromCfg();
    a.engine.loraraw().setSettings(S.cfg);
}

void apply(App& a) { a.engine.loraraw().setSettings(S.cfg); saveState(); }

bool live(const App& a) { return a.rx.standard == 72; }   // the engine reports its standard code minus one

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

void receiveNote() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only: OnAir never transmits. Plain LoRa between two radios (not LoRaWAN, not a mesh): the payload is shown as the bytes it is, "
                        "nothing is decrypted or interpreted. Listening to LoRa traffic that is not meant for you may be restricted where you live: check the rules "
                        "before you use what you receive. Set the spreading factor and bandwidth of the link, or leave them on Auto to search them all (more CPU). "
                        "Needs a radio with 1 Msps or more (an RTL-SDR is enough at 125 / 250 kHz).");
    ImGui::PopTextWrapPos();
}

std::string settingText(const dect2::LoraRawSettings& c) {
    char b[200];
    char sf[24], bw[24], sync[24];
    if (c.autoSf) snprintf(sf, sizeof sf, "SF7-12 (auto)"); else snprintf(sf, sizeof sf, "SF%d", c.sf);
    if (c.autoBw) snprintf(bw, sizeof bw, "125/250/500 kHz (auto)"); else snprintf(bw, sizeof bw, "%s", c.bwKhz == 62 ? "62.5 kHz" : (std::to_string(c.bwKhz) + " kHz").c_str());
    if (c.sync < 0) snprintf(sync, sizeof sync, "any of 0x12 0x34 0x2B"); else snprintf(sync, sizeof sync, "0x%02X", c.sync);
    if (c.implicit) snprintf(b, sizeof b, "%s, %s, implicit header: %d bytes, CR 4/%d, %s, sync %s", sf, bw, c.length, c.cr, c.crc ? "CRC" : "no CRC", sync);
    else snprintf(b, sizeof b, "%s, %s, explicit header, sync %s", sf, bw, sync);
    return b;
}

// ---------------------------------------------------------------- frames

void framesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see LoRa frames"); return; }
    const dect2::LoraRawTelemetry& t = a.rx.loraraw;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", settingText(t.cfg).c_str());
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Filter"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(150 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##lrf", "text or hex", S.filter, sizeof S.filter);
    flowNext(10 * gUi);
    ImGui::Checkbox("hide CRC errors", &S.hideBad);
    flowNext(10 * gUi);
    const auto& log = t.log;
    std::vector<size_t> rows;
    const std::string f = lower(S.filter);
    for (size_t i = 0; i < log.size(); i++) {
        if (S.hideBad && log[i].hasCrc && !log[i].crcOk) continue;
        if (!f.empty() && lower(log[i].ascii).find(f) == std::string::npos && lower(log[i].hex).find(f) == std::string::npos) continue;
        rows.push_back(i);
    }
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (size_t i : rows) {
            const auto& r = log[i];
            char b[900];
            snprintf(b, sizeof b, "%s\tSF%d\t%.0f kHz\t4/%d\t%d\t%s\t%.1f\t%.1f\t%+.2f\t%s\t%s\n", clock(r.wallTime).c_str(), r.sf, r.bwHz / 1e3, r.cr, r.len,
                     !r.hasCrc ? "none" : r.crcOk ? "ok" : "bad", r.snrDb, r.levelDb, r.cfoHz / 1e3, r.hex.c_str(), r.ascii.c_str());
            txt += b;
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the listed frames (tab separated)");
    flowNext(8 * gUi);
    if (ImGui::Button("Clear")) { a.engine.loraraw().clearResults(); S.sel = -1; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget every frame heard so far");
    flowBreak();
    if (!ImGui::BeginTable("##lrlog", 11, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("SF"); ImGui::TableSetupColumn("BW"); ImGui::TableSetupColumn("CR"); ImGui::TableSetupColumn("Len");
    ImGui::TableSetupColumn("CRC"); ImGui::TableSetupColumn("SNR dB"); ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Offset kHz");
    ImGui::TableSetupColumn("Payload hex"); ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin((int)rows.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const auto& r = log[rows[(size_t)i]];
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableNextColumn();
            if (ImGui::Selectable("##row", S.sel == (int)rows[(size_t)i], ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.sel = (int)rows[(size_t)i];
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(clock(r.wallTime).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", r.sf);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", r.bwHz / 1e3);
            ImGui::TableNextColumn(); ImGui::Text("4/%d", r.cr);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.len);
            ImGui::TableNextColumn();
            if (!r.hasCrc) ImGui::TextDisabled("none"); else if (r.crcOk) ImGui::TextColored(pal::okGreen(), "ok"); else ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "bad");
            ImGui::TableNextColumn(); ImGui::Text("%.1f", r.snrDb);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", r.levelDb);
            ImGui::TableNextColumn(); ImGui::Text("%+.2f", r.cfoHz / 1e3);
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(r.hex.c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(r.ascii.c_str()); ImGui::PopFont();
            ImGui::PopID();
        }
    ImGui::EndTable();
    if (log.empty()) ImGui::TextDisabled("no frames yet");
    else if (rows.empty()) ImGui::TextDisabled("no frame passes the filter");
}

void decodersView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver first"); return; }
    const dect2::LoraRawTelemetry& t = a.rx.loraraw;
    if (!ImGui::BeginTable("##lrdec", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("SF"); ImGui::TableSetupColumn("Bandwidth"); ImGui::TableSetupColumn("Sync"); ImGui::TableSetupColumn("In band"); ImGui::TableSetupColumn("Preambles");
    ImGui::TableSetupColumn("Frames"); ImGui::TableSetupColumn("CRC errors"); ImGui::TableSetupColumn("Header / sync failed", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& d : t.decoders) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%d", d.sf);
        ImGui::TableNextColumn(); ImGui::Text("%.1f kHz", d.bwHz / 1e3);
        ImGui::TableNextColumn(); ImGui::Text("0x%02X", d.sync);
        ImGui::TableNextColumn(); if (d.inBand) ImGui::TextColored(pal::okGreen(), "yes"); else ImGui::TextDisabled("no");
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)d.preambles);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)d.frames);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)d.crcBad);
        ImGui::TableNextColumn(); ImGui::Text("%llu / %llu", (unsigned long long)d.headerBad, (unsigned long long)d.syncBad);
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------- settings

// the controls: used in the decoder options of the top bar
void settings(App& a, bool& retune) {
    (void)retune;
    loadState(a);
    bool ch = false;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("SF"); ImGui::SameLine(0, 5 * gUi);
    ch |= ImGui::Checkbox("Auto##lrsf", &S.cfg.autoSf);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Search SF7 - SF12 at once (more CPU). Off: only the spreading factor chosen here.");
    if (!S.cfg.autoSf) {
        ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(std::min(70 * gUi, ImGui::GetContentRegionAvail().x));
        char l[8]; snprintf(l, sizeof l, "SF%d", S.cfg.sf);
        if (ImGui::BeginCombo("##lrsfv", l)) {
            for (int s = 5; s <= 12; s++) { snprintf(l, sizeof l, "SF%d", s); if (ImGui::Selectable(l, s == S.cfg.sf)) { S.cfg.sf = s; ch = true; } }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("SF5 and SF6 as on the SX126x chips (not checked against a real radio)");
    }
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("BW"); ImGui::SameLine(0, 5 * gUi);
    ch |= ImGui::Checkbox("Auto##lrbw", &S.cfg.autoBw);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Search 125, 250 and 500 kHz at once. Off: only the bandwidth chosen here.");
    if (!S.cfg.autoBw) {
        ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(std::min(95 * gUi, ImGui::GetContentRegionAvail().x));
        int cur = 1;
        for (int i = 0; i < 4; i++) if (kBwKhz[i] == S.cfg.bwKhz) cur = i;
        if (ImGui::BeginCombo("##lrbwv", kBwName[cur])) {
            for (int i = 0; i < 4; i++) if (ImGui::Selectable(kBwName[i], i == cur)) { S.cfg.bwKhz = kBwKhz[i]; ch = true; }
            ImGui::EndCombo();
        }
    }
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Header"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(95 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##lrhdr", S.cfg.implicit ? "implicit" : "explicit")) {
        if (ImGui::Selectable("explicit", !S.cfg.implicit)) { S.cfg.implicit = false; ch = true; }
        if (ImGui::Selectable("implicit", S.cfg.implicit)) { S.cfg.implicit = true; ch = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Explicit: the frame says its length, coding rate and whether it has a CRC (the usual mode).\n"
                                                  "Implicit: it says nothing, so the coding rate, CRC and length are agreed beforehand and must be set here.");
    if (S.cfg.implicit) {
        flowNext(10 * gUi);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("CR"); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(std::min(60 * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##lrcr", kCr[S.cfg.cr - 5])) {
            for (int i = 0; i < 4; i++) if (ImGui::Selectable(kCr[i], i == S.cfg.cr - 5)) { S.cfg.cr = 5 + i; ch = true; }
            ImGui::EndCombo();
        }
        flowNext(10 * gUi);
        ch |= ImGui::Checkbox("CRC", &S.cfg.crc);
        flowNext(10 * gUi);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Bytes"); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(std::min(80 * gUi, ImGui::GetContentRegionAvail().x));
        ch |= ImGui::InputInt("##lrlen", &S.cfg.length, 0, 0);
        S.cfg.length = std::max(1, std::min(255, S.cfg.length));
    }
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Sync"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(150 * gUi, ImGui::GetContentRegionAvail().x));
    static const char* const kSyncName[] = {"0x12 private", "0x34 LoRaWAN", "0x2B Meshtastic", "any of these", "custom"};
    if (ImGui::BeginCombo("##lrsync", kSyncName[S.syncChoice])) {
        for (int i = 0; i < 5; i++) if (ImGui::Selectable(kSyncName[i], i == S.syncChoice)) {
            S.syncChoice = i;
            if (i == 0) S.cfg.sync = 0x12; else if (i == 1) S.cfg.sync = 0x34; else if (i == 2) S.cfg.sync = 0x2B; else if (i == 3) S.cfg.sync = -1;
            else S.cfg.sync = (int)strtol(S.syncHex, nullptr, 16) & 0xFF;
            ch = true;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The sync word the transmitter's radio is set to: 0x12 is the default of private LoRa links, 0x34 is LoRaWAN, 0x2B Meshtastic.\n"
                                                  "\"any of these\" searches all three (three times the CPU).");
    if (S.syncChoice == 4) {
        ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(std::min(40 * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::InputText("##lrsyncx", S.syncHex, sizeof S.syncHex, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase)) {
            S.cfg.sync = (int)strtol(S.syncHex, nullptr, 16) & 0xFF;
            ch = true;
        }
    }
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("LDRO"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(70 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##lrldro", S.cfg.ldro < 0 ? "auto" : S.cfg.ldro ? "on" : "off")) {
        if (ImGui::Selectable("auto", S.cfg.ldro < 0)) { S.cfg.ldro = -1; ch = true; }
        if (ImGui::Selectable("on", S.cfg.ldro == 1)) { S.cfg.ldro = 1; ch = true; }
        if (ImGui::Selectable("off", S.cfg.ldro == 0)) { S.cfg.ldro = 0; ch = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Low data rate optimisation: radios switch it on when a symbol lasts 16 ms or more (SF11 and SF12 at 125 kHz, SF12 at 250 kHz).\n"
                                                  "Auto does the same; set it by hand when the link's radios were set otherwise.");
    flowBreak();
    if (ch) apply(a);
}

void tuner(App& a, bool& retune) {
    loadState(a);
    ImGui::TextDisabled("Preset");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));
    const char* cur = "frequency";
    for (const auto& p : kPresets) if (std::fabs(p.mhz - a.freqMhz) < 0.0005) cur = p.name;
    if (ImGui::BeginCombo("##lrpre", cur)) {
        for (const auto& p : kPresets) if (ImGui::Selectable(p.name, std::fabs(p.mhz - a.freqMhz) < 0.0005)) { tuneFreq(a, p.mhz); retune = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Common LoRa frequencies: the EU868 LoRaWAN channels and 869.525 MHz, US915, and the 433 MHz ISM band.\n"
                                                  "Any other frequency (137 MHz - 2.5 GHz) can be typed into the frequency box.");
}

void tab(App& a) {
    loadState(a);
    subNav("lrv", S.view, {"Frames", "Settings", "Decoders"});
    if (S.view == 0) { receiveNote(); framesView(a); }
    else if (S.view == 1) { bool rt = false; receiveNote(); ImGui::Spacing(); ImGui::PushID("lrtab"); settings(a, rt); ImGui::PopID(); ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Auto searches every spreading factor (SF7 - SF12) at every bandwidth (125, 250, 500 kHz) that fits the captured band, one demodulator each. "
                            "Fixing one narrows the search and saves CPU. An explicit header carries the coding rate, length and CRC flag; an implicit header carries nothing, "
                            "so set them to what the link uses. Transmitters start at slightly different frequencies: the carrier offset of every frame is shown.");
        ImGui::PopTextWrapPos(); }
    else decodersView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::LoraRawTelemetry& t = a.rx.loraraw;
    if (!ImGui::BeginTable("##lrl", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("SF"); ImGui::TableSetupColumn("SNR");
    for (size_t i = 0; i < t.log.size() && i < 60; i++) {
        const auto& r = t.log[i];
        ImGui::TableNextRow();
        const bool bad = r.hasCrc && !r.crcOk;
        ImGui::TableNextColumn();
        ImGui::PushFont(a.mono, 0);
        ImGui::TextColored(bad ? ImVec4(0.95f, 0.45f, 0.35f, 1) : (t.timeSec - r.sec < 3 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text)), "%s", r.ascii.empty() ? "-" : r.ascii.c_str());
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%d bytes, SF%d %.0f kHz 4/%d%s", r.hex.c_str(), r.len, r.sf, r.bwHz / 1e3, r.cr, bad ? ", CRC error" : "");
        ImGui::TableNextColumn(); ImGui::TextDisabled("%d", r.sf);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.0f", r.snrDb);
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::LoraRawTelemetry& t = a.rx.loraraw;
    kvRow(a, "state", "%s", dect2::loraRawSummary(t).c_str());
    kvRow(a, "settings", "%s", settingText(t.cfg).c_str());
    kvRow(a, "frames", "%llu good, %llu CRC errors, %llu preambles, %llu bad headers", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
          (unsigned long long)t.preambles, (unsigned long long)t.headerBad);
    kvRow(a, "last frame", "SNR %.1f dB, offset %+.2f kHz", t.snrDb, t.cfoHz / 1e3);
    std::string sf;
    for (int i = 5; i <= 12; i++) if (t.perSf[i]) { char b[32]; snprintf(b, sizeof b, "%sSF%d: %llu", sf.empty() ? "" : ", ", i, (unsigned long long)t.perSf[i]); sf += b; }
    kvRow(a, "by SF", "%s", sf.empty() ? "-" : sf.c_str());
    kvRow(a, "by bandwidth", "125 kHz %llu, 250 kHz %llu, 500 kHz %llu", (unsigned long long)t.perBw[0], (unsigned long long)t.perBw[1], (unsigned long long)t.perBw[2]);
    size_t n = 0;
    for (const auto& d : t.decoders) if (d.inBand) n++;
    kvRow(a, "demodulators", "%zu running (%zu listed)", n, t.decoders.size());
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
    ImGui::Spacing();
    receiveNote();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Frames", !on ? 0 : a.rx.loraraw.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::loraRawSummary(a.rx.loraraw).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "LoRa (raw)";
    if (live(a)) l2 = dect2::loraRawSummary(a.rx.loraraw);
}

void synth(App& a, bool& changed) {
    ImGui::TextDisabled("Test signal");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(220 * gUi, ImGui::GetContentRegionAvail().x));
    int& o = a.tune.synth.modeOpt[0];
    if (ImGui::BeginCombo("##lrsyn", o == 1 ? "implicit header (SF8, 250 kHz, 4/6, 12 bytes, CRC)" : "nine frames, SF7 - SF12, explicit header")) {
        if (ImGui::Selectable("nine frames, SF7 - SF12, explicit header", o == 0)) { o = 0; changed = true; }
        if (ImGui::Selectable("implicit header (SF8, 250 kHz, 4/6, 12 bytes, CRC)", o == 1)) { o = 1; changed = true; }
        ImGui::EndCombo();
    }
}

} // namespace

extern const ModeUi kLoraRawUi;
const ModeUi kLoraRawUi = {
    .sideTitle = "FRAMES",
    .tabName = "LoRa",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .decoder = settings,
    .synth = synth,
};
