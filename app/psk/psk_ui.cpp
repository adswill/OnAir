// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// PSK31 screens: the calling-frequency presets (80 to 10 m), the modulation (BPSK31 / 63, QPSK31 / 63), the waterfall of the audio
// passband (a click picks a signal), the table of every signal (audio frequency, SNR, AFC, the newest text), and the focused signal's
// chat log (one row per line, with the time) beside its text in big letters, callsigns highlighted.
#include "app.h"
#include "gfx/gfx.h"
#include "dect2/cw/cw_morse.h"
#include "dect2/psk/psk_rx.h"
#include "dect2/psk/psk_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct State {
    int mode = 0;
    bool autoOn = true;
    int pushedAuto = -1;
    float thrDb = 5;
    bool wasRunning = false;
    int pushedMode = -1; float pushedThr = -1;
    bool listen = false; int pushedListen = -1; float pushedVol = -1; bool pushedMute = false;   // "Listen": off until switched on
    bool loaded = false;
    static constexpr int kRows = 160;
    int bins = 0;
    std::vector<uint32_t> px;
    uint64_t specSeq = 0;
    float lo = -120, hi = -60;
    gfx::Image* wf = nullptr;
    int wfW = 0;
    bool dirty = false;
};
State S;

bool live(const App& a) { return a.rx.standard == 37; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.mode = std::max(0, std::min(dect2::kPskModes - 1, (int)plat::prefs().getI("pskMode", 0)));
    S.autoOn = plat::prefs().getI("pskAuto", 1) != 0;
    S.thrDb = (float)plat::prefs().getD("pskThreshold", 5);
    S.listen = plat::prefs().getB("pskListen", false);
}

const ImVec4 kCallCol(1.0f, 0.82f, 0.36f, 1);

double absKhz(const App& a, double offHz) { return a.freqMhz * 1000.0 + offHz / 1000.0; }

// words in a row, callsigns in colour, wrapped at the right edge; a line end starts a new row
void richText(const std::string& text, float maxX) {
    const float spaceW = ImGui::CalcTextSize(" ").x;
    const float x0 = ImGui::GetCursorScreenPos().x;
    float x = x0;
    bool first = true, newLine = false;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\n')) { if (text[i] == '\n') newLine = true; i++; }
        size_t e = i;
        while (e < text.size() && text[e] != ' ' && text[e] != '\n') e++;
        if (e == i) break;
        const std::string w = text.substr(i, e - i);
        const float ww = ImGui::CalcTextSize(w.c_str()).x;
        if (!first) {
            if (newLine || (x + spaceW + ww > maxX && x > x0)) x = x0;
            else { ImGui::SameLine(0, spaceW); x += spaceW; }
        }
        newLine = false;
        if (dect2::cw::isCallsign(w)) ImGui::TextColored(kCallCol, "%s", w.c_str());
        else ImGui::TextUnformatted(w.c_str());
        x += ww;
        first = false;
        i = e;
    }
    if (first) ImGui::TextUnformatted("");
}

void presets(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Band"); flowNext(6 * gUi);
    int n = 0;
    const dect2::PskPreset* p = dect2::pskPresets(n);
    for (int i = 0; i < n; i++) {
        const bool here = std::fabs(a.freqMhz - p[i].mhz) < 0.002;
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        char b[40]; snprintf(b, sizeof b, "%s##pskp%d", p[i].name, i);
        if (ImGui::Button(b)) tuneFreq(a, p[i].mhz);
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("PSK31 dial frequency %.3f MHz (upper sideband); the signals sit 100 Hz to 3.9 kHz above it", p[i].mhz);
        flowNext(4 * gUi);
    }
    ImGui::TextDisabled("|"); flowNext(8 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Mode"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(100 * gUi);
    if (ImGui::BeginCombo("##pskmode", S.autoOn ? tr("Auto") : dect2::pskModeName(S.mode))) {
        if (ImGui::Selectable(tr("Auto"), S.autoOn)) { S.autoOn = true; plat::prefs().setI("pskAuto", 1); }
        for (int m = 0; m < dect2::kPskModes; m++)
            if (ImGui::Selectable(dect2::pskModeName(m), !S.autoOn && m == S.mode)) { S.mode = m; S.autoOn = false; plat::prefs().setI("pskMode", m); plat::prefs().setI("pskAuto", 0); }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Auto finds BPSK31, PSK63, QPSK and the faster modes from the signals in the band. Pick one to fix it."));
    if (S.autoOn && live(a) && a.engine.running()) {
        const dect2::PskTelemetry& at = a.rx.psk;
        flowNext(4 * gUi);
        ImGui::AlignTextToFramePadding();
        if (at.autoLocked && at.autoMode >= 0) ImGui::TextDisabled(tr("Found: %s"), dect2::pskModeName(at.autoMode));
        else ImGui::TextDisabled("%s", tr("Auto: listening..."));
    }
    flowNext(8 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Threshold"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::SliderFloat("##pskthr", &S.thrDb, 4, 20, "%.0f dB")) plat::prefs().setD("pskThreshold", S.thrDb);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How far over the noise floor a signal must be to get a decoder");
    flowNext(8 * gUi);
    listenControls(a, S.listen, "pskListen", "psk", "Play the sideband (dial + 200 Hz to 3.8 kHz) on the sound card; decoding is the same either way");
    flowNext(8 * gUi);
    if (ImGui::Button("Clear##psk")) a.engine.psk().clearSignals();
    flowEnd();
}

void follow(const dect2::PskTelemetry& t) {
    if (t.spectrum.empty() || t.specSeq == S.specSeq) return;
    S.specSeq = t.specSeq;
    const int W = (int)t.spectrum.size();
    if (W != S.bins) { S.bins = W; S.px.assign((size_t)W * State::kRows, 0xFF000000u); }
    std::vector<float> srt(t.spectrum);
    std::nth_element(srt.begin(), srt.begin() + W / 2, srt.end());
    const float med = srt[(size_t)W / 2];
    S.lo += (med - 3 - S.lo) * 0.2f;
    S.hi += (med + 30 - S.hi) * 0.2f;
    std::copy_backward(S.px.begin(), S.px.end() - W, S.px.end());
    for (int k = 0; k < W; k++) S.px[(size_t)k] = Waterfall::jet((t.spectrum[(size_t)k] - S.lo) / std::max(1.f, S.hi - S.lo));
    S.dirty = true;
}

void waterfall(App& a, float h) {
    const dect2::PskTelemetry& t = a.rx.psk;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float l = p0.x, r = p0.x + w, top = p0.y, bot = p0.y + h - ImGui::GetTextLineHeight() - 4 * gUi;
    dl->AddRectFilled(p0, ImVec2(r, p0.y + h), IM_COL32(8, 9, 10, 255));
    const double lo = t.hiHz > t.loHz ? t.loHz : 100, hi = t.hiHz > t.loHz ? t.hiHz : 3900;
    auto X = [&](double hz) { return (float)(l + (hz - lo) / (hi - lo) * (r - l)); };
    if (gGfx && S.bins > 0 && (!S.wf || S.wfW != S.bins)) { delete S.wf; S.wf = gGfx->createImage(S.bins, State::kRows, 0xFF000000u); S.wfW = S.bins; }
    if (S.wf && S.dirty) { S.wf->update(0, 0, S.bins, State::kRows, S.px.data()); S.dirty = false; }
    if (S.wf && on) dl->AddImage(S.wf->texture(), ImVec2(l, top), ImVec2(r, bot));
    const ImU32 txt = IM_COL32(120, 124, 128, 255);
    char b[48];
    for (int k = 500; k <= 3500; k += 500) {   // audio Hz above the dial
        const float x = X(k);
        dl->AddLine(ImVec2(x, bot), ImVec2(x, bot + 4 * gUi), txt);
        snprintf(b, sizeof b, "%d", k);
        dl->AddText(ImVec2(x - ImGui::CalcTextSize(b).x / 2, bot + 3 * gUi), txt, b);
    }
    if (on) {
        for (const auto& s : t.signals) {
            const float x = X(s.offsetHz);
            const bool f = s.id == t.focusId;
            const ImU32 c = f ? IM_COL32(255, 210, 90, 255) : s.dcd ? IM_COL32(120, 230, 140, 220) : IM_COL32(160, 160, 160, 160);
            dl->AddTriangleFilled(ImVec2(x - 5 * gUi, top), ImVec2(x + 5 * gUi, top), ImVec2(x, top + 8 * gUi), c);
            if (f) dl->AddLine(ImVec2(x, top), ImVec2(x, bot), IM_COL32(255, 210, 90, 120), 1.f);
        }
    } else {
        dl->AddText(ImVec2(l + 10, top + 10), txt, a.engine.running() ? "starting" : "start the receiver to read the band");
    }
    ImGui::InvisibleButton("##pskwf", ImVec2(w, h));
    if (on && ImGui::IsItemHovered()) {
        const double hz = lo + (ImGui::GetIO().MousePos.x - l) / std::max(1.f, r - l) * (hi - lo);
        ImGui::SetTooltip("%.0f Hz (%.3f kHz): click to decode this signal", hz, absKhz(a, hz));
        if (ImGui::IsItemClicked()) a.engine.psk().setFocusHz(hz);
    }
}

void signalTable(App& a, float h) {
    const dect2::PskTelemetry& t = a.rx.psk;
    if (!ImGui::BeginTable("##psksig", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, h))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Hz"); ImGui::TableSetupColumn("SNR"); ImGui::TableSetupColumn("AFC"); ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& s : t.signals) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char b[48];
        snprintf(b, sizeof b, "%.0f##psks%d", s.offsetHz, s.id);
        ImGui::PushFont(a.mono, 0);
        if (s.dcd) ImGui::PushStyleColor(ImGuiCol_Text, pal::okGreen());
        if (ImGui::Selectable(b, s.id == t.focusId, ImGuiSelectableFlags_SpanAllColumns)) a.engine.psk().setFocusHz(s.offsetHz);
        if (s.dcd) ImGui::PopStyleColor();
        ImGui::TableNextColumn(); ImGui::Text("%.0f dB", s.snrDb);
        ImGui::TableNextColumn(); ImGui::Text("%+.1f", s.afcHz);
        ImGui::TableNextColumn();
        std::string tail = s.lines.empty() ? std::string() : s.lines.back().text;
        while (!tail.empty() && ImGui::CalcTextSize(tail.c_str()).x > ImGui::GetContentRegionAvail().x) { const size_t sp = tail.find(' '); tail = sp == std::string::npos ? tail.substr(1) : tail.substr(sp + 1); }
        richText(tail, 1e9f);
        ImGui::PopFont();
    }
    ImGui::EndTable();
}

void focusView(App& a) {
    const dect2::PskTelemetry& t = a.rx.psk;
    const dect2::PskSignal* f = dect2::pskFocus(t);
    if (!f) { ImGui::TextDisabled("click a signal on the waterfall or in the table to read it here"); return; }
    ImGui::PushFont(a.mono, 0);
    ImGui::TextColored(f->dcd ? pal::okGreen() : pal::grey(0.8f), "%.0f Hz  %.3f kHz", f->offsetHz, absKhz(a, f->offsetHz));
    ImGui::SameLine(0, 14 * gUi); ImGui::Text("SNR %.0f dB", f->snrDb);
    ImGui::SameLine(0, 14 * gUi); ImGui::Text("AFC %+.1f Hz", f->afcHz);
    ImGui::SameLine(0, 14 * gUi); ImGui::TextDisabled("%s", f->dcd ? "decoding" : "no lock");
    for (const auto& c : f->calls) { ImGui::SameLine(0, 10 * gUi); ImGui::TextColored(kCallCol, "%s", c.c_str()); }
    ImGui::PopFont();
    const float avW = ImGui::GetContentRegionAvail().x;
    const float logW = avW > 700 * gUi ? avW * 0.42f : 0;
    if (logW > 0) {   // the chat log of the focused signal: one row per line
        if (ImGui::BeginTable("##psklog", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersOuter, ImVec2(logW, 0))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            ImGui::PushFont(a.mono, 0);
            for (const auto& l : f->lines) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const int s = (int)l.timeSec;
                ImGui::TextDisabled("%d:%02d", s / 60, s % 60);
                ImGui::TableNextColumn();
                ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(l.text.c_str()); ImGui::PopTextWrapPos();
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
            ImGui::PopFont();
            ImGui::EndTable();
        }
        ImGui::SameLine();
    }
    if (ImGui::BeginChild("##pskbig", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        ImGui::PushFont(a.mono, ImGui::GetFontSize() * 1.6f);
        richText(f->text, ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x);
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
        ImGui::PopFont();
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    load();
    if (!a.engine.running()) { S.wasRunning = false; return; }
    if (live(a)) follow(a.rx.psk);
    dect2::PskReceiver& r = a.engine.psk();
    if (!S.wasRunning || (int)S.autoOn != S.pushedAuto) { r.setAuto(S.autoOn); S.pushedAuto = S.autoOn; S.pushedMode = -1; }
    if (!S.autoOn && (!S.wasRunning || S.mode != S.pushedMode)) { r.setMode(S.mode); S.pushedMode = S.mode; }
    if (!S.wasRunning || S.thrDb != S.pushedThr) { r.setThresholdDb(S.thrDb); S.pushedThr = S.thrDb; }
    if (!S.wasRunning || (int)S.listen != S.pushedListen) { r.setListen(S.listen); S.pushedListen = S.listen; }
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    S.wasRunning = true;
}

void tab(App& a) {
    load();
    presets(a);
    ImGui::Spacing();
    const float av = ImGui::GetContentRegionAvail().y;
    waterfall(a, std::max(90.f, av * 0.32f));
    ImGui::Spacing();
    if (!live(a)) return;
    signalTable(a, std::max(80.f, ImGui::GetContentRegionAvail().y * 0.4f));
    ImGui::Spacing();
    focusView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    if (!ImGui::BeginTable("##pskl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Call"); ImGui::TableSetupColumn("Hz", ImGuiTableColumnFlags_WidthStretch);
    int n = 0;
    for (const auto& s : a.rx.psk.signals)
        for (const auto& c : s.calls) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0);
            char b[48];
            snprintf(b, sizeof b, "%s##pskc%d_%d", c.c_str(), s.id, n++);
            if (ImGui::Selectable(b, s.id == a.rx.psk.focusId, ImGuiSelectableFlags_SpanAllColumns)) a.engine.psk().setFocusHz(s.offsetHz);
            ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%.0f", s.offsetHz);
        }
    ImGui::EndTable();
    if (n == 0) ImGui::TextDisabled("no callsigns yet");
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::PskTelemetry& t = a.rx.psk;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", dect2::pskSummary(t).c_str());
    kv("window", "dial + %.0f .. %.0f Hz, noise floor %.1f dB/bin", t.loHz, t.hiHz, t.floorDb);
    kv("characters", "%llu decoded, %llu codes not in the table", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv("signal time", "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Signals", !on ? 0 : a.rx.psk.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("Text", !on ? 0 : a.rx.psk.state == 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::pskSummary(a.rx.psk).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "PSK31";
    if (live(a)) l2 = dect2::pskSummary(a.rx.psk);
}

} // namespace

extern const ModeUi kPskUi;
const ModeUi kPskUi = {
    .sideTitle = "CALLSIGNS",
    .tabName = "PSK31",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
