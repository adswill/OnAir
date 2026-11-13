// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDES screens: the ASM frames (decoded: GMSK 9.6 kbit/s, AIS framing, CRC-16 checked, payload as hex) and the VDE-TER burst log (detected only: band,
// bandwidth, level, modulation class; the content is not decoded).
#include "app.h"
#include "dect2/vdes/vdes_rx.h"
#include "dect2/vdes/vdes_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                        // 0 ASM, 1 VDE-TER
    uint64_t selKey = 0;
};
State S;

bool live(const App& a) { return a.engine.running() && a.rx.standard == 64; }

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::string clock(int64_t t, double sigSec) {
    char b[24];
    if (t <= 0) { snprintf(b, sizeof b, "%.1f s", sigSec); return b; }
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

uint64_t keyOf(const dect2::VdesAsm& f) { return (uint64_t)(f.timeSec * 1000.0) * 31u + (uint64_t)f.bits; }

void asmView(App& a) {
    const bool on = live(a);
    const std::vector<dect2::VdesAsm> none;
    const std::vector<dect2::VdesAsm>& fr = on ? a.rx.vdes.asms : none;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("ASM (application specific messages) on channels 2027 and 2028 (161.950 and 162.000 MHz): demodulated and CRC checked. The payload is shown as it was sent; "
                        "it is read as an AIS message only when its first bits look like one.");
    ImGui::PopTextWrapPos();
    if (ImGui::SmallButton("Clear")) { a.engine.vdes().clearResults(); S.selKey = 0; }
    {   // positions of AIS-like frames: on the Map tab (AIS ships layer) and, as !AIVDM sentences, on the AIS data feed (UDP) for a chart program
        static bool nmea = plat::prefs().getB("vdesNmea", true), pushed = false;
        ImGui::SameLine(0, 10 * gUi);
        const bool ch = ImGui::Checkbox("Send as NMEA (UDP)", &nmea);
        if (ch) plat::prefs().setB("vdesNmea", nmea);
        if (ch || !pushed) { a.engine.vdes().setForwardNmea(nmea); pushed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frames that are AIS messages go out as !AIVDM sentences, like the AIS mode's, to the host and port of 'NMEA !AIVDM (UDP)' in Settings, Data feeds (switch that on there). Decoded positions are on the Map tab.");
        size_t np = 0;
        for (const dect2::VdesAsm& f : fr) if (f.hasPos) np++;
        ImGui::SameLine(0, 10 * gUi);
        ImGui::TextDisabled("%zu with a position", np);
    }
    const float h = ImGui::GetContentRegionAvail().y;
    const float listH = S.selKey ? h * 0.55f : h;
    if (ImGui::BeginTable("##vdt", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, listH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Channel"); ImGui::TableSetupColumn("Bits"); ImGui::TableSetupColumn("SNR");
        ImGui::TableSetupColumn("AIS type"); ImGui::TableSetupColumn("Position"); ImGui::TableSetupColumn("Payload", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const dect2::VdesAsm& f : fr) {
            const uint64_t k = keyOf(f);
            ImGui::TableNextRow();
            ImGui::PushID((int)k);
            ImGui::TableNextColumn();
            if (ImGui::Selectable(clock(f.wallTime, f.timeSec).c_str(), S.selKey == k, ImGuiSelectableFlags_SpanAllColumns)) S.selKey = S.selKey == k ? 0 : k;
            ImGui::TableNextColumn(); ImGui::Text("ASM %c", f.channel == 'B' ? '2' : '1');
            ImGui::TableNextColumn(); ImGui::Text("%d", f.bits);
            ImGui::TableNextColumn(); ImGui::Text("%.1f dB", f.snrDb);
            ImGui::TableNextColumn(); if (f.aisType) ImGui::Text("%d", f.aisType); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); if (f.hasPos) ImGui::Text("%.4f %.4f", f.lat, f.lon); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(f.hex.substr(0, 96).c_str()); ImGui::PopFont();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (fr.empty()) ImGui::TextDisabled("%s", on ? "no ASM frame yet" : a.engine.running() ? "starting" : "stopped");
    if (!S.selKey) return;
    for (const dect2::VdesAsm& f : fr)
        if (keyOf(f) == S.selKey) {
            ImGui::Separator();
            if (ImGui::BeginChild("##vdd", ImVec2(0, 0))) {
                kvRow(a, "channel", "ASM %c, %.3f MHz", f.channel == 'B' ? '2' : '1', (f.channel == 'B' ? dect2::kVdesAsm2Hz : dect2::kVdesAsm1Hz) / 1e6);
                kvRow(a, "received", "%s, SNR %.1f dB, offset %+.0f Hz", clock(f.wallTime, f.timeSec).c_str(), f.snrDb, f.cfoHz);
                kvRow(a, "length", "%d bits (CRC-16 good)", f.bits);
                if (f.aisType) kvRow(a, "as AIS", "message type %d, MMSI %u", f.aisType, f.aisMmsi);
                if (f.hasPos) kvRow(a, "position", "%.5f %.5f%s%s", f.lat, f.lon, f.sogKt >= 0 && f.sogKt < 102.3f ? (", " + std::to_string((int)std::lround(f.sogKt)) + " kn").c_str() : "", f.cogDeg >= 0 && f.cogDeg < 360 ? (", course " + std::to_string((int)std::lround(f.cogDeg))).c_str() : "");
                ImGui::TextDisabled("hex");
                ImGui::SameLine();
                if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(f.hex.c_str());
                ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(f.hex.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
            }
            ImGui::EndChild();
            return;
        }
}

void vdeView(App& a) {
    const bool on = live(a);
    const std::vector<dect2::VdeBurst> none;
    const std::vector<dect2::VdeBurst>& bs = on ? a.rx.vdes.bursts : none;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("VDE-TER (the 100 kHz channels at 157.2 and 161.8 MHz, 25 / 50 / 100 kHz bursts): detected only. Each burst is logged with its band, bandwidth, level and a modulation "
                        "class from the symbol statistics (assumes root-raised-cosine 0.3 at 19.2 kbaud per 25 kHz); the content is not decoded. Tune near 157.2 or 161.8 MHz to see that band.");
    ImGui::PopTextWrapPos();
    if (on) {
        const dect2::VdesTelemetry& t = a.rx.vdes;
        ImGui::Text("bursts %llu:  25 kHz %llu, 50 kHz %llu, 100 kHz %llu;  pi/4-QPSK %llu, 8PSK %llu, 16QAM %llu, unknown %llu", (unsigned long long)t.vdeBursts,
                    (unsigned long long)t.vdeByBw[0], (unsigned long long)t.vdeByBw[1], (unsigned long long)t.vdeByBw[2], (unsigned long long)t.vdeByMod[1],
                    (unsigned long long)t.vdeByMod[2], (unsigned long long)t.vdeByMod[3], (unsigned long long)t.vdeByMod[0]);
    }
    if (ImGui::BeginTable("##vdb", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("Width"); ImGui::TableSetupColumn("Modulation");
        ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("SNR"); ImGui::TableSetupColumn("Length"); ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const dect2::VdeBurst& b : bs) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(b.wallTime, b.timeSec).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.4f", b.centreMhz);
            ImGui::TableNextColumn(); ImGui::Text("%d kHz", b.bwKhz);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::vdeModName(b.mod));
            ImGui::TableNextColumn(); ImGui::Text("%.1f dBFS", b.rssiDb);
            ImGui::TableNextColumn(); ImGui::Text("%.1f dB", b.snrDb);
            ImGui::TableNextColumn(); ImGui::Text("%.1f ms", b.durMs);
            ImGui::TableNextColumn(); ImGui::Text("%+.0f Hz", b.cfoHz);
        }
        ImGui::EndTable();
    }
    if (bs.empty()) ImGui::TextDisabled("%s", on ? "no burst yet" : a.engine.running() ? "starting" : "stopped");
}

void tab(App& a) {
    subNav("vdv", S.view, {"ASM messages", "VDE-TER bursts"});
    if (S.view == 0) asmView(a); else vdeView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::VdesTelemetry& t = a.rx.vdes;
    if (!ImGui::BeginTable("##vdl", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) return;
    ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Frames");
    struct Row { const char* n; bool inView; float lvl; uint64_t cnt; };
    const Row rows[4] = {{"ASM 1  161.950", t.asmInView, t.asmLevelDb[0], t.asmOk[0]}, {"ASM 2  162.000", t.asmInView, t.asmLevelDb[1], t.asmOk[1]},
                         {"VDE-TER 157.2", t.bandInView[0], t.vdeLevelDb[0], 0}, {"VDE-TER 161.8", t.bandInView[1], t.vdeLevelDb[1], 0}};
    for (int i = 0; i < 4; i++) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(rows[i].n);
        ImGui::TableNextColumn();
        if (rows[i].inView) ImGui::Text("%.0f dB", rows[i].lvl); else ImGui::TextDisabled("not in band");
        ImGui::TableNextColumn();
        if (i < 2) ImGui::Text("%llu", (unsigned long long)rows[i].cnt);
        else if (rows[i].inView) ImGui::Text("%llu", (unsigned long long)t.vdeBursts);
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::VdesTelemetry& t = a.rx.vdes;
    kvRow(a, "state", "%s", dect2::vdesSummary(t).c_str());
    kvRow(a, "ASM (decoded)", "%llu frames good, %llu bursts failed their checks", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kvRow(a, "VDE-TER (detected)", "%llu bursts: %llu pi/4-QPSK, %llu 8PSK, %llu 16QAM, %llu unclassified", (unsigned long long)t.vdeBursts,
          (unsigned long long)t.vdeByMod[1], (unsigned long long)t.vdeByMod[2], (unsigned long long)t.vdeByMod[3], (unsigned long long)t.vdeByMod[0]);
    kvRow(a, "bands", "ASM %s, 157.2 MHz %s, 161.8 MHz %s", t.asmInView ? "in view" : "outside", t.bandInView[0] ? "in view" : "outside", t.bandInView[1] ? "in view" : "outside");
    kvRow(a, "last ASM", "SNR %.1f dB, offset %+.0f Hz", t.snrDb, t.cfoHz);
    kvRow(a, "input", "%.3f Msps, dial %s", t.inputRate / 1e6, t.dialMhz > 0 ? (std::to_string(t.dialMhz) + " MHz").c_str() : "unknown (a file): 161.9 MHz assumed");
    kvRow(a, "signal time", "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("ASM", !on ? 0 : a.rx.vdes.dataValid ? 1 : 0); flowNext(10 * gUi);
    lamp("VDE-TER", !on ? 0 : (a.rx.vdes.timeSec - a.rx.vdes.lastVdeSec < 30) ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::vdesSummary(a.rx.vdes).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "VDES";
    if (live(a)) l2 = dect2::vdesSummary(a.rx.vdes);
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("simulated");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));
    static const char* const names[3] = {"ASM and VDE-TER", "ASM only", "VDE-TER only"};
    const int cur = std::max(0, std::min(2, sc.modeOpt[0]));
    if (ImGui::BeginCombo("##vdsy", names[cur])) {
        for (int k = 0; k < 3; k++) if (ImGui::Selectable(names[k], cur == k)) { sc.modeOpt[0] = k; changed = true; }
        ImGui::EndCombo();
    }
}

} // namespace

extern const ModeUi kVdesUi;
const ModeUi kVdesUi = {
    .sideTitle = "CHANNELS",
    .tabName = "Messages",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .synth = synth,
};
