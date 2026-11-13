// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tone decoder screens: what is on the audio now (the sub-audible CTCSS tone or DCS code, the strongest voice-band tone, DTMF keys), the audio
// spectrum, the 50 CTCSS tones as bars, a history of everything heard (CTCSS, DCS, DTMF, five-tone calls, SELCAL, two-tone pages) and the gate
// that opens the sound only for a chosen tone or code. Input is an FM channel from a radio or, from a sound-card source, audio.
#include "app.h"
#include "../d40/d40_ui.h"
#include "../ui/msglog_ui.h"
#include "dect2/tones/tones_proto.h"
#include "dect2/tones/tones_rx.h"
#include "dect2/tones/tones_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include "../ui/font_size.h"

namespace {

struct State {
    int view = 0;                 // 0 live, 1 history
    bool loaded = false;
    bool listen = false;
    int gateMode = 0, gateKind = 0, gateTone = 12, gateCode = 0;   // tone: index into the CTCSS list, code: index into the standard list
    bool gateInv = false;
    int demod = -1;               // -1 auto (USB below 30 MHz, FM above), else dect2::TonesDemod
    bool wasRunning = false;
    int pushedListen = -1, pushedGate = -1, pushedDemod = -1;
    float pushedVol = -1; int pushedMute = -1;
};
State S;

constexpr int kCodes[] = {23, 25, 26, 31, 32, 43, 47, 51, 54, 65, 71, 72, 73, 74, 114, 115, 116, 125, 131, 132, 134, 143, 152, 155, 156, 162, 165, 172, 174, 205, 223, 226, 243, 244,
                          245, 251, 261, 263, 265, 271, 306, 311, 315, 331, 343, 346, 351, 364, 365, 371, 411, 412, 413, 423, 431, 432, 445, 464, 465, 466, 503, 506, 516, 532, 546,
                          565, 606, 612, 624, 627, 631, 632, 654, 662, 664, 703, 712, 723, 731, 732, 734, 743, 754};
constexpr int kNumCodes = (int)(sizeof kCodes / sizeof kCodes[0]);

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.listen = plat::prefs().getB("tonesListen", false);
    S.gateMode = (int)std::max(0L, std::min(2L, (long)plat::prefs().getI("tonesGateMode", 0)));
    S.gateKind = (int)std::max(0L, std::min(1L, (long)plat::prefs().getI("tonesGateKind", 0)));
    S.gateTone = (int)std::max(0L, std::min((long)dect2::tones::kCtcssN - 1, (long)plat::prefs().getI("tonesGateTone", 12)));
    S.gateCode = (int)std::max(0L, std::min((long)kNumCodes - 1, (long)plat::prefs().getI("tonesGateCode", 0)));
    S.gateInv = plat::prefs().getB("tonesGateInv", false);
    S.demod = (int)std::max(-1L, std::min((long)dect2::TonesDemodCount - 1, (long)plat::prefs().getI("tonesDemod", -1)));
}
void save() {
    plat::prefs().setI("tonesGateMode", S.gateMode);
    plat::prefs().setI("tonesGateKind", S.gateKind);
    plat::prefs().setI("tonesGateTone", S.gateTone);
    plat::prefs().setI("tonesGateCode", S.gateCode);
    plat::prefs().setB("tonesGateInv", S.gateInv);
    plat::prefs().setI("tonesDemod", S.demod);
    plat::prefs().flush();
}

bool live(const App& a) { return a.rx.standard == 90; }   // the engine reports its standard code minus one

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::string clockText(int64_t sec) {
    const time_t t = (time_t)sec;
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char b[32];
    snprintf(b, sizeof b, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return b;
}

const char* kindText(int k) {
    switch (k) {
    case dect2::TkCtcss: return "CTCSS";
    case dect2::TkDcs: return "DCS";
    case dect2::TkDtmf: return "DTMF";
    case dect2::TkSeq: return tr("5-tone");
    case dect2::TkSelcal: return "SELCAL";
    default: return tr("2-tone");
    }
}

void subAudible(const App& a, const dect2::TonesTelemetry& t) {
    ImGui::PushFont(a.mono, 30.f);
    if (t.ctcssIdx >= 0) ImGui::TextColored(pal::okGreen(), "%.1f Hz", t.ctcssHz);
    else if (t.dcsCode >= 0) ImGui::TextColored(pal::okGreen(), "D%03d%c", t.dcsCode, t.dcsInv ? 'I' : 'N');
    else ImGui::TextDisabled("--- Hz");
    ImGui::PopFont();
    if (t.ctcssIdx >= 0)
        kvRow(a, tr("sub-audible"), "CTCSS, %s (%.1f Hz nominal)", dect2::tones::ctcssExtended(t.ctcssIdx) ? tr("extended tone") : tr("standard tone"), dect2::tones::ctcssHz(t.ctcssIdx));
    else if (t.dcsCode >= 0) {
        kvRow(a, tr("sub-audible"), "DCS, %s", t.dcsStd ? tr("code of the usual list") : tr("code not in the usual list"));
        if (!t.dcsAlias.empty()) kvRow(a, tr("same waveform"), "%s", t.dcsAlias.c_str());
    } else kvRow(a, tr("sub-audible"), "%s", tr("no CTCSS tone or DCS code"));
}

void liveView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver on an FM channel or a sound card to decode tones")); return; }
    const dect2::TonesTelemetry& t = a.rx.tones;
    subAudible(a, t);
    ImGui::Spacing();
    d40TonesCard(t);   // 4.0: tone system guess, dialled numbers (app/d40)
    if (t.toneHz > 0) kvRow(a, tr("voice-band tone"), "%.1f Hz, %.1f dB", t.toneHz, t.toneDb);
    else kvRow(a, tr("voice-band tone"), "%s", tr("none"));
    kvRow(a, tr("keys"), "%s", t.dtmfString.empty() ? "-" : t.dtmfString.c_str());
    kvRow(a, tr("audio"), "%.1f dB%s", t.audioDb, t.audioInput ? tr(", sound card") : tr(", FM"));
    kvRow(a, tr("gate"), "%s", t.gateMode == 0 ? tr("open (no gate)") : t.gateOpen ? tr("open") : tr("closed"));
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("audio spectrum, 0 - 3.5 kHz"));
    if (!t.spec.empty()) ImGui::PlotLines("##tnspec", t.spec.data(), (int)t.spec.size(), 0, nullptr, -90.f, 0.f, ImVec2(ImGui::GetContentRegionAvail().x, 70 * gUi));
    ImGui::TextDisabled("%s", tr("the 50 CTCSS tones, 67 - 254 Hz"));
    if (!t.ctcssPow.empty()) {   // bars grow from the bottom: dB above -80
        std::vector<float> bars(t.ctcssPow.size());
        for (size_t i = 0; i < bars.size(); i++) bars[i] = std::max(0.f, t.ctcssPow[i] + 80.f);
        ImGui::PlotHistogram("##tnctcss", bars.data(), (int)bars.size(), 0, nullptr, 0.f, 70.f, ImVec2(ImGui::GetContentRegionAvail().x, 60 * gUi));
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The level of each of the 50 tones in the last second of the sub-audible band, left to right from 67.0 to 254.1 Hz. A tone that stands clear of the others for about half a second is taken as the CTCSS tone."));
}

void historyView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see what is heard")); return; }
    const dect2::TonesTelemetry& t = a.rx.tones;
    if (ImGui::Button(tr("Copy"))) {
        std::string txt;
        for (const auto& e : t.events) txt += clockText(e.wall) + "\t" + kindText(e.kind) + "\t" + e.text + "\t" + e.detail + "\n";
        ImGui::SetClipboardText(txt.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear"))) a.engine.tones().clearResults();
    {   // Save... / Auto-save to file (oldest first)
        std::vector<dect2::msglog::Row> rows;
        for (const auto& e : t.events) {
            dect2::msglog::Row r;
            r.epoch = e.wall; r.mode = kindText(e.kind); r.freqHz = a.freqMhz * 1e6; r.text = e.text + (e.detail.empty() ? "" : "  " + e.detail);
            rows.push_back(std::move(r));
        }
        std::stable_sort(rows.begin(), rows.end(), [](const dect2::msglog::Row& x, const dect2::msglog::Row& y) { return x.epoch < y.epoch; });
        msglog_ui::bar("tones", "tones", rows);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("CTCSS %d, DCS %d, DTMF %d, %s %d, SELCAL %d, %s %d", t.counts[dect2::TkCtcss], t.counts[dect2::TkDcs], t.counts[dect2::TkDtmf], tr("5-tone"), t.counts[dect2::TkSeq],
                        t.counts[dect2::TkSelcal], tr("2-tone"), t.counts[dect2::TkTwoTone]);
    if (!ImGui::BeginTable("##tnhist", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr(dect2::timefmt::headComputer())); ImGui::TableSetupColumn(tr("Type")); ImGui::TableSetupColumn(tr("Heard"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(tr("Length")); ImGui::TableSetupColumn(tr("Details"));
    ImGui::TableHeadersRow();
    for (const auto& e : t.events) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(e.wall).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(kindText(e.kind));
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0);
        if (e.active) ImGui::TextColored(pal::okGreen(), "%s", e.text.c_str()); else ImGui::TextUnformatted(e.text.c_str());
        ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::Text("%.1f s", e.durSec);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", e.detail.c_str());
    }
    ImGui::EndTable();
    if (t.events.empty()) ImGui::TextDisabled("%s", tr("nothing heard yet"));
}

void tab(App& a) {
    load();
    listenControls(a, S.listen, "tonesListen", "tones", tr("Hear the channel (the sub-audible tones are taken out); with a gate set under Receiver the sound opens only for the chosen tone or code"));
    subNav("tnv", S.view, {tr("Live"), tr("History")});
    if (S.view == 0) liveView(a); else historyView(a);
}

void tick(App& a) {
    load();
    if (!a.engine.running()) { S.wasRunning = false; return; }
    dect2::TonesReceiver& r = a.engine.tones();
    if (!S.wasRunning || (int)S.listen != S.pushedListen) { r.setListen(S.listen); S.pushedListen = S.listen; }
    const int gate = S.gateMode | S.gateKind << 2 | S.gateTone << 4 | S.gateCode << 10 | (S.gateInv ? 1 << 18 : 0);
    if (!S.wasRunning || gate != S.pushedGate) { r.setGate(S.gateMode, S.gateKind, dect2::tones::ctcssHz(S.gateTone), kCodes[S.gateCode], S.gateInv); S.pushedGate = gate; }
    if (!S.wasRunning || a.volume != S.pushedVol || (int)a.muted != S.pushedMute) { r.setVolume(a.volume); r.setMuted(a.muted); S.pushedVol = a.volume; S.pushedMute = a.muted; }
    const int dm = S.demod >= 0 ? S.demod : dect2::tonesAutoDemod(a.freqMhz * 1e6);
    if (!S.wasRunning || dm != S.pushedDemod) { r.setDemod(dm); S.pushedDemod = dm; }
    S.wasRunning = true;
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::TonesTelemetry& t = a.rx.tones;
    ImGui::PushFont(a.mono, 0);
    int shown = 0;
    for (const auto& e : t.events) {
        if (shown++ >= 8) break;
        ImGui::TextDisabled("%s", kindText(e.kind));
        ImGui::SameLine();
        if (e.active) ImGui::TextColored(pal::okGreen(), "%s", e.text.c_str()); else ImGui::TextUnformatted(e.text.c_str());
    }
    ImGui::PopFont();
    if (!shown) ImGui::TextDisabled("%s", tr("nothing heard yet"));
}

void receiver(App& a) {
    load();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::TonesTelemetry& t = a.rx.tones;
    kvRow(a, tr("state"), "%s", dect2::tonesSummary(t).c_str());
    if (t.audioInput) kvRow(a, tr("input"), "%.1f kHz, %s", t.inputRate / 1e3, tr("sound card audio"));
    else if (t.demod != dect2::TonesFm) kvRow(a, tr("input"), "%.3f Msps, %s %s", t.inputRate / 1e6, dect2::tonesDemodName(t.demod), tr("channel"));
    else kvRow(a, tr("input"), "%.3f Msps, %s, %+.0f Hz, SNR %.1f dB", t.inputRate / 1e6, tr("FM channel"), t.cfoHz, t.snrDb);
    kvRow(a, tr("signal time"), "%.1f s", t.timeSec);
    ImGui::Spacing();
    {
        int sel = S.demod + 1;
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::Combo(tr("Demodulation##tndemod"), &sel, (std::string(tr("auto (USB on HF, FM above 30 MHz)")) + '\0' + "FM" + '\0' + "USB" + '\0' + "LSB" + '\0' + "AM" + '\0').c_str())) { S.demod = sel - 1; save(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How the radio's channel is turned into audio. SELCAL on the aeronautical HF channels (for example 8.879 MHz) is sent in USB: tune the dial frequency. FM for VHF / UHF radios, AM for the VHF airband."));
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Gate"));
    bool ch = false;
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::Combo(tr("Open the sound##tnmode"), &S.gateMode, (std::string(tr("always")) + '\0' + tr("on any tone or code") + '\0' + tr("only on the chosen tone or code") + '\0').c_str())) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Like the squelch of a radio with CTCSS or DCS: while the gate is closed the Listen sound is muted. The decoders keep running either way."));
    if (S.gateMode == 2) {
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::Combo("##tnkind", &S.gateKind, "CTCSS\0DCS\0")) ch = true;
        ImGui::SameLine();
        if (S.gateKind == 0) {
            std::string items;
            for (int i = 0; i < dect2::tones::kCtcssN; i++) { char b[32]; snprintf(b, sizeof b, "%.1f Hz", dect2::tones::ctcssHz(i)); items += b; items += '\0'; }
            ImGui::SetNextItemWidth(120 * gUi);
            if (ImGui::Combo("##tntone", &S.gateTone, items.c_str(), 14)) ch = true;
        } else {
            std::string items;
            for (int i = 0; i < kNumCodes; i++) { char b[16]; snprintf(b, sizeof b, "D%03d", kCodes[i]); items += b; items += '\0'; }
            ImGui::SetNextItemWidth(100 * gUi);
            if (ImGui::Combo("##tncode", &S.gateCode, items.c_str(), 14)) ch = true;
            ImGui::SameLine();
            if (ImGui::Checkbox(tr("inverted"), &S.gateInv)) ch = true;
        }
    }
    if (ch) save();
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("DCS polarity (N or I) follows the deviation sign of the receiver; the same waveform can read as two codes, the second is shown as the alias. Five-tone tone lengths are vendor specific: when several standards fit, the one whose usual tone length is closest is listed first."));
    ImGui::PopTextWrapPos();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Audio"), !on ? 0 : a.rx.tones.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("CTCSS / DCS", !on ? 0 : (a.rx.tones.ctcssIdx >= 0 || a.rx.tones.dcsCode >= 0) ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Gate"), !on ? 0 : a.rx.tones.gateMode == 0 ? 2 : a.rx.tones.gateOpen ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::tonesSummary(a.rx.tones).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = tr("Tone decoders");
    if (live(a)) l2 = dect2::tonesSummary(a.rx.tones);
}

void synth(App&, bool&) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("The test signal repeats every 40 s: CTCSS 123.0 Hz under speech-like noise, DCS D023N, DTMF 159#, a ZVEI1 call 12345, SELCAL AM-CP and a two-tone page."));
    ImGui::PopTextWrapPos();
}

} // namespace

extern const ModeUi kTonesUi;
const ModeUi kTonesUi = {
    .sideTitle = "HEARD",
    .tabName = "Tones",
    .tabIcon = Ic::Wave,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .synth = synth,
    .tick = tick,
};
