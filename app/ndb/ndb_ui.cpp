// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NDB screens: the Morse ident in large type, the tone it was keyed with, the speed, and the idents read so far.
// The listener hears the keyed tone. HF beacons stay on their own screen.
#include "app.h"
#include "dect2/ndb/ndb_rx.h"
#include "dect2/ndb/ndb_tel.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace {

struct State {
    bool loaded = false;
    bool listen = false;
    int tone = 0;          // 0 auto, 1 400 Hz, 2 1020 Hz, 3 keyed carrier
};
State S;

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.listen = plat::prefs().getB("ndbListen", false);
    S.tone = (int)plat::prefs().getI("ndbTone", 0);
    if (S.tone < 0 || S.tone > 3) S.tone = 0;
}

bool live(const App& a) { return a.rx.standard == 30; }   // the engine reports its standard code minus one

std::string rangeNote(const App& a) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return "";
    const DeviceInfo& d = a.devices[(size_t)a.devIdx];
    if (!d.isRadio() || d.minFreqHz <= 0) return "";
    if (a.freqMhz * 1e6 >= d.minFreqHz) return "";
    char b[480];
    snprintf(b, sizeof b, "This radio is specified from %.6g MHz. OnAir still tries %.0f kHz. Long-wave NDBs need a radio that tunes below 1 MHz "
             "(an SDRplay, an Airspy HF+, or an RTL-SDR in direct sampling) and a long-wave antenna.", d.minFreqHz / 1e6, a.freqMhz * 1e3);
    return b;
}

void tick(App& a) {
    loadState();
    dect2::NdbReceiver& r = a.engine.ndb();
    r.setToneMode(S.tone);
    r.setListen(S.listen);
    r.setVolume(a.volume);
    r.setMuted(a.muted);
}

// Every beacon in the captured band, without retuning: the receiver runs one decoder per carrier
void censusTable(App& a) {
    const dect2::NdbTelemetry& t = a.rx.ndb;
    if (t.census.empty()) return;
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Beacons in the band"));
    if (!ImGui::BeginTable("##ndbc", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) return;
    ImGui::TableSetupColumn(tr("kHz")); ImGui::TableSetupColumn(tr("Ident")); ImGui::TableSetupColumn(tr("Tone")); ImGui::TableSetupColumn(tr("SNR")); ImGui::TableSetupColumn(tr("WPM"));
    ImGui::TableHeadersRow();
    for (const auto& r : t.census) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%.1f", a.freqMhz * 1000.0 + r.offsetHz / 1000.0);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.ident.empty() ? "-" : r.ident.c_str());
        ImGui::TableNextColumn(); if (r.toneHz > 0) ImGui::Text("%d", r.toneHz); else ImGui::TextDisabled(r.toneHz < 0 ? "keyed" : "-");
        ImGui::TableNextColumn(); ImGui::Text("%.0f", r.snrDb);
        ImGui::TableNextColumn(); if (r.wpm > 0) ImGui::Text("%.0f", r.wpm); else ImGui::TextDisabled("-");
    }
    ImGui::EndTable();
}

void tab(App& a) {
    loadState();
    const std::string note = rangeNote(a);
    if (!note.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::warnAmber(), "%s", note.c_str()); ImGui::PopTextWrapPos(); }
    listenControls(a, S.listen, "ndbListen", "ndb", "Play the keyed ident tone (a 400 Hz tone follows the carrier when the beacon sends no modulating tone)");
    flowNext(16 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("tone");
    flowNext(6 * gUi);
    ImGui::SetNextItemWidth(150 * gUi);
    static const char* const tones[] = {"Auto", "400 Hz", "1020 Hz", "Keyed carrier"};
    if (ImGui::Combo("##ndbtone", &S.tone, tones, 4)) plat::prefs().setI("ndbTone", S.tone);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Auto follows whichever of the 400 Hz tone, the 1020 Hz tone or the keyed carrier is actually being keyed");
    flowEnd();

    const dect2::NdbTelemetry& t = a.rx.ndb;
    const bool on = live(a);
    ImGui::Spacing();
    const std::string id = on && !t.ident.empty() ? t.ident : std::string(on ? "..." : "-");
    ImGui::PushFont(a.mono, 0);
    ImGui::SetWindowFontScale(2.4f);
    ImGui::TextUnformatted(id.c_str());
    ImGui::SetWindowFontScale(1.f);
    ImGui::PopFont();

    if (!on) {
        ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped");
    } else {
        ImGui::TextDisabled("%s", dect2::ndbSummary(t).c_str());
        ImGui::Spacing();
        auto row = [](const char* k, const char* v) {
            ImGui::TextDisabled("%s", k);
            kvColumn(130 * gUi);
            ImGui::TextUnformatted(v);
        };
        char b[160];
        snprintf(b, sizeof b, "%s", dect2::ndbToneName(t).c_str());
        row("path", b);
        if (t.wpm > 0) snprintf(b, sizeof b, "%.0f WPM", t.wpm); else snprintf(b, sizeof b, "-");
        row("speed", b);
        snprintf(b, sizeof b, "%s%s", t.key ? "down" : "up", t.longDash ? ", long dash" : "");
        row("key", b);
        if (t.depth > 0) snprintf(b, sizeof b, "%.0f %%", 100.0 * (double)t.depth); else snprintf(b, sizeof b, "-");
        row("depth", b);
        snprintf(b, sizeof b, "%+.0f Hz", t.cfoHz);
        row("carrier offset", b);
        snprintf(b, sizeof b, "%.0f dBFS", t.levelDb);
        row("level", b);
        if (t.snrDb > 0) snprintf(b, sizeof b, "%.0f dB", t.snrDb); else snprintf(b, sizeof b, "-");
        row("keying", b);
        row("reading", t.text.empty() ? "-" : t.text.c_str());
        if (!t.partial.empty()) row("in progress", t.partial.c_str());
    }
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Tuned to the middle of the long-wave beacon band (190-535 kHz). The ident is Morse at about 7 WPM: a 400 Hz or 1020 Hz tone, or the carrier itself keyed. "
                        "A mark longer than about a second is the long dash between idents. The speed follows the same Morse timing as CW (5 to 50 WPM). "
                        "HF propagation beacons are the HF beacons mode, not this one.");
    ImGui::PopTextWrapPos();
    if (on) censusTable(a);
}

void list(App& a) {
    if (resultsClearButton("ndb")) a.engine.ndb().clearResults();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::NdbTelemetry& t = a.rx.ndb;
    if (t.history.empty()) { ImGui::TextDisabled("no ident yet"); return; }
    if (!ImGui::BeginTable("##ndbh", 1, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    for (int i = (int)t.history.size() - 1; i >= 0; --i) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(t.history[(size_t)i].c_str());
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::NdbTelemetry& t = a.rx.ndb;
    ImGui::TextDisabled("state"); kvColumn(130 * gUi); ImGui::TextUnformatted(dect2::ndbSummary(t).c_str());
    ImGui::TextDisabled("idents"); kvColumn(130 * gUi); ImGui::Text("%llu", (unsigned long long)t.blocksOk);
    ImGui::TextDisabled("unknown"); kvColumn(130 * gUi); ImGui::Text("%llu", (unsigned long long)t.blocksBad);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    lamp("Carrier", !on ? 0 : a.rx.ndb.state >= 1 ? 1 : 2); flowNext(10 * gUi);
    lamp("Ident", !on ? 0 : a.rx.ndb.ident.empty() ? 0 : 1); flowNext(10 * gUi);
    lamp("Key", !on ? 0 : a.rx.ndb.key ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0);
    ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::ndbSummary(a.rx.ndb).c_str());
    ImGui::PopFont();
    flowEnd();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "NDB";
    if (live(a)) l2 = dect2::ndbSummary(a.rx.ndb);
}

} // namespace

extern const ModeUi kNdbUi;
const ModeUi kNdbUi = {
    .sideTitle = "IDENTS",
    .tabName = "NDB",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
