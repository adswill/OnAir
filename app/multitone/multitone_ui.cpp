// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Multitone paging screens: the transmissions heard (carrier, deviation, tones and spectral lines of the audio), the FSK data found in them
// (baud rate, bit stream, 32-bit words against the POCSAG BCH check) and the steady tones heard so far.
// Receive only. Multitone's own format is not public, so this is a detector: everything is labelled unverified and no message text is shown.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "../ui/typography.h"
#include "dect2/multitone/multitone_rx.h"
#include "dect2/multitone/multitone_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                        // 0 transmissions, 1 data, 2 tones
    uint64_t selTx = 0;
};
State S;

bool live(const App& a) { return a.rx.standard == 76; }   // the engine reports its standard code minus one

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

void note() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only: pager messages are private; nothing is decrypted and nothing is transmitted. Multitone Electronics (UK) pagers (Mk IV to VII, mostly hospitals and "
                        "industrial sites) use narrowband FM with FFSK and a code like POCSAG 1200 with headers of their own; that format is not public, so this screen is a detector, "
                        "not a decoder. It shows what is measured: the carrier, the tones and lines in the audio, and for FSK data the baud rate, the bits and how many 32-bit words "
                        "pass the POCSAG BCH(31,21) check. All of it is unverified, and no message text is produced. Standard POCSAG is flagged by its sync word: use the POCSAG mode for it.");
    ImGui::PopTextWrapPos();
}

const dect2::MtTx* findTx(const dect2::MultitoneTelemetry& t, uint64_t id) {
    for (const auto& x : t.tx) if (x.id == id) return &x;
    return nullptr;
}
const dect2::MtBurst* findBurst(const dect2::MultitoneTelemetry& t, uint64_t id) {
    for (const auto& b : t.bursts) if (b.id == id) return &b;
    return nullptr;
}

void burstDetail(const App& a, const dect2::MtBurst& b) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(b.syncFound || b.bestRun >= 2 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", b.verdict.c_str());
    ImGui::PopTextWrapPos();
    if (b.ffsk) kvRow(a, "audio FFSK", "tones %.1f Hz (0) and %.1f Hz (1), one symbol every two half cycles of the high tone", b.toneLoHz, b.toneHiHz);
    if (b.syncCount > 0) kvRow(a, "0x605A", "the 16-bit word is in the stream %d times%s", b.syncCount, b.afterSync.empty() ? "" : (std::string("; words after the first pair: ") + b.afterSync).c_str());
    kvRow(a, "baud", "%.1f%s%s  (edge coherence %.2f)", b.baud, b.nearestStd.empty() ? "" : ", near the standard ", b.nearestStd.c_str(), b.coherence);
    kvRow(a, "bits", "%d sliced%s", b.nBits, b.manchester ? (std::string(", Manchester decoded to ") + std::to_string(b.dataBits) + " bits").c_str() : "");
    if (b.repeatBits > 0) kvRow(a, "repeats", "the bit stream repeats every %d bits", b.repeatBits);
    if (b.dupBits > 1) kvRow(a, "bit rate", "every bit is sent %d times: %.1f baud of data", b.dupBits, b.dataBaud);
    if (b.markerBits > 0) {
        kvRow(a, "marker", "%d bits %s, %d times, one every %d bits", b.markerBits, b.marker.c_str(), b.markerCount, b.markerGap);
        std::string w;
        for (size_t i = 0; i < b.frameWords.size() && i < 12; i++) { if (i) w += "  "; w += b.frameWords[i]; }
        kvRow(a, "words after it", "%s%s  (%d different, %d repeat the one before, %d a first byte and a value sent twice)", w.c_str(), b.frameWords.size() > 12 ? " ..." : "", b.distinctWords, b.doubledWords, b.halfEqualWords);
    }
    kvRow(a, "BCH words", "%d valid of %d, longest run %d, bit offset %d", b.validWords, b.totalWords, b.bestRun, b.align);
    kvRow(a, "stream (hex)", "%s", b.bitsHex.c_str());
    if (!b.words.empty()) {
        std::string w;
        for (size_t i = 0; i < b.words.size(); i++) { char c[24]; snprintf(c, sizeof c, "%08X%s ", b.words[i], b.wordOk[i] ? "*" : ""); w += c; if ((i & 3) == 3) w += "\n"; }
        kvRow(a, "words", "%s", w.c_str());
        ImGui::TextDisabled("* passes the BCH(31,21) + parity check");
    }
}

void txView(App& a) {
    note();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to listen for Multitone pagers"); return; }
    const dect2::MultitoneTelemetry& t = a.rx.multitone;
    if (ImGui::Button("Clear")) a.engine.multitone().clearResults();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget every transmission, burst and tone heard so far");
    ImGui::SameLine(0, 8 * gUi);
    ImGui::TextDisabled("%llu transmissions, %llu with FSK data", (unsigned long long)t.txTotal, (unsigned long long)t.burstTotal);
    const float tableH = std::max(120.f * gUi, ImGui::GetContentRegionAvail().y * 0.55f);
    if (ImGui::BeginTable("##mttx", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX, ImVec2(0, tableH))) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("#"); ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("Length"); ImGui::TableSetupColumn("SNR"); ImGui::TableSetupColumn("Deviation");
        ImGui::TableSetupColumn("Kind (unverified)"); ImGui::TableSetupColumn("Tones", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& x : t.tx) {
            ImGui::TableNextRow();
            ImGui::PushID((int)x.id);
            ImGui::TableNextColumn();
            if (ImGui::Selectable("##row", S.selTx == x.id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.selTx = S.selTx == x.id ? 0 : x.id;
            ImGui::SameLine(0, 0);
            ImGui::Text("%llu", (unsigned long long)x.id);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(x.wall).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.2f s", x.durSec);
            ImGui::TableNextColumn(); ImGui::Text("%.1f dB", x.snrDb);
            ImGui::TableNextColumn(); ImGui::Text("%.0f Hz", x.devHz);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(x.kind.c_str());
            ImGui::TableNextColumn(); if (x.tones.empty()) ImGui::TextDisabled("-"); else ImGui::TextUnformatted(x.tones.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (t.tx.empty()) ImGui::TextDisabled("no transmission heard yet");
    const dect2::MtTx* x = findTx(t, S.selTx);
    if (x) {
        ImGui::Separator();
        ImGui::TextWrapped("Transmission %llu: %s, %.2f s, FM deviation about %.0f Hz", (unsigned long long)x->id, x->kind.c_str(), x->durSec, x->devHz);
        if (!x->lineHz.empty()) {
            std::string l;
            for (size_t i = 0; i < x->lineHz.size(); i++) { char c[48]; snprintf(c, sizeof c, "%s%.0f Hz (%.0f dB)", i ? ", " : "", x->lineHz[i], x->lineDb[i]); l += c; }
            kvRow(a, "audio lines", "%s", l.c_str());
        }
        if (!x->tones.empty()) kvRow(a, "steady tones", "%s", x->tones.c_str());
        if (x->periodMs > 0) kvRow(a, "repeats", "the audio repeats every %.0f ms (correlation %.2f)", x->periodMs, x->periodStrength);
        if (const dect2::MtBurst* b = findBurst(t, x->burst)) burstDetail(a, *b);
    }
}

void dataView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to look for FSK data"); return; }
    const dect2::MultitoneTelemetry& t = a.rx.multitone;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("FSK data found in the transmissions, newest first. The words are checked with the POCSAG BCH(31,21) code and even parity at the best of the 32 bit alignments "
                        "and both polarities; a run of two or more valid words is very unlikely by chance (1 in about 4 million). What the words mean is not known.");
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (const auto& b : t.bursts) { char c[300]; snprintf(c, sizeof c, "%s%s\t%.1f baud\t%d bits\t%d valid of %d\t%s\n", clock(b.wall).c_str(), dect2::timefmt::sfx(), b.baud, b.nBits, b.validWords, b.totalWords, b.bitsHex.c_str()); txt += c; }
        ImGui::SetClipboardText(txt.c_str());
    }
    if (t.bursts.empty()) { ImGui::TextDisabled("no FSK data heard yet"); return; }
    ImGui::BeginChild("##mtdata", ImVec2(0, ImGui::GetContentRegionAvail().y), false);
    for (const auto& b : t.bursts) {
        ImGui::PushID((int)b.id);
        char head[160];
        snprintf(head, sizeof head, "%s%s  %.1f baud  %d bits  %d valid of %d words%s", clock(b.wall).c_str(), dect2::timefmt::sfx(), b.baud, b.nBits, b.validWords, b.totalWords, b.syncFound ? "  POCSAG sync" : "");
        if (typo::collapsingHeader(head)) burstDetail(a, b);
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void tonesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to collect tones"); return; }
    const dect2::MultitoneTelemetry& t = a.rx.multitone;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Steady audio tones (60 - 3300 Hz, at least 0.1 s), grouped within 6 Hz. Multitone's tone sequences are not public, so no tone is matched to a pager or a call.");
    ImGui::PopTextWrapPos();
    if (!ImGui::BeginTable("##mttone", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Frequency"); ImGui::TableSetupColumn("Heard"); ImGui::TableSetupColumn("Total time"); ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& x : t.tones) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%.1f Hz", x.hz);
        ImGui::TableNextColumn(); ImGui::Text("%d", x.count);
        ImGui::TableNextColumn(); ImGui::Text("%.2f s", x.totalSec);
        ImGui::TableNextColumn(); ImGui::Text("%.0f s ago", std::max(0.0, t.timeSec - x.lastSec));
    }
    ImGui::EndTable();
    if (t.tones.empty()) ImGui::TextDisabled("no steady tone heard yet");
}

void tab(App& a) {
    subNav("mtv", S.view, {"Transmissions", "FSK data", "Tones"});
    if (S.view == 0) txView(a); else if (S.view == 1) dataView(a); else tonesView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::MultitoneTelemetry& t = a.rx.multitone;
    if (!ImGui::BeginTable("##mtl", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Length"); ImGui::TableSetupColumn("SNR");
    for (const auto& x : t.tx) {
        ImGui::TableNextRow();
        ImGui::PushID((int)x.id);
        ImGui::TableNextColumn();
        if (ImGui::Selectable("##row", S.selTx == x.id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.selTx = S.selTx == x.id ? 0 : x.id;
        ImGui::SameLine(0, 0);
        ImGui::TextUnformatted(x.kind.c_str());
        if (ImGui::IsItemHovered() && !x.tones.empty()) ImGui::SetTooltip("%s", x.tones.c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.1f s", x.durSec);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.0f dB", x.snrDb);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::MultitoneTelemetry& t = a.rx.multitone;
    kvRow(a, "state", "%s", dect2::multitoneSummary(t).c_str());
    kvRow(a, "carrier", "%s, %.1f dB, input %.3f Msps, %.1f dBFS", t.signal ? "up" : "none", t.signal ? t.snrDb : 0.f, t.inputRate / 1e6, t.levelDb);
    kvRow(a, "BCH words", "%llu valid, %llu not valid (FSK data only)", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
    ImGui::Spacing();
    note();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Carrier", !on ? 0 : a.rx.multitone.signal ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::multitoneSummary(a.rx.multitone).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Multitone Paging";
    if (live(a)) l2 = dect2::multitoneSummary(a.rx.multitone);
}

} // namespace

extern const ModeUi kMultitoneUi;
const ModeUi kMultitoneUi = {
    .sideTitle = "TRANSMISSIONS",
    .tabName = "Multitone",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
};
