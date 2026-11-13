// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CW screens: the band presets (the CW segments of 160 to 10 m), the waterfall of the skimmer window (a click picks a signal), the table
// of every signal (frequency, speed, SNR, text) and the focused signal's text in big letters, with the callsigns highlighted; the
// sidetone switch for the focused signal (off until switched on).
#include "app.h"
#include "../d40/d40_ui.h"
#include "ham/ham_events_ui.h"
#include "gfx/gfx.h"
#include "dect2/cw/cw_morse.h"
#include "dect2/cw/cw_rx.h"
#include "dect2/cw/cw_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include "../ui/font_size.h"

namespace {

struct State {
    bool sidetone = false; float pitch = 700, pushedPitch = -1;
    float thrDb = 10;
    bool wasRunning = false;
    float pushedVol = -1; bool pushedMute = false; int pushedTone = -1; float pushedThr = -1;
    bool loaded = false;
    // the waterfall (UI thread): one row per spectrum report, newest at the top
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

bool live(const App& a) { return a.rx.standard == 35; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.sidetone = plat::prefs().getB("cwSidetone", false);
    S.pitch = (float)plat::prefs().getD("cwPitch", 700);
    S.thrDb = (float)plat::prefs().getD("cwThreshold", 10);
}

const ImVec4 kCallCol(1.0f, 0.82f, 0.36f, 1);

double absKhz(const App& a, double offHz) { return a.freqMhz * 1000.0 + offHz / 1000.0; }

// words in a row, callsigns in colour, wrapped at the right edge
void richText(const std::string& text, float maxX) {
    const float spaceW = ImGui::CalcTextSize(" ").x;
    float x = ImGui::GetCursorScreenPos().x;
    const float x0 = x;
    bool first = true;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && text[i] == ' ') i++;
        size_t e = i;
        while (e < text.size() && text[e] != ' ') e++;
        if (e == i) break;
        const std::string w = text.substr(i, e - i);
        const float ww = ImGui::CalcTextSize(w.c_str()).x;
        if (!first) {
            if (x + spaceW + ww > maxX && x > x0) { x = x0; }
            else { ImGui::SameLine(0, spaceW); x += spaceW; }
        }
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
    const dect2::CwBand* b = dect2::cwBands(n);
    for (int i = 0; i < n; i++) {
        const bool here = a.freqMhz >= b[i].loMhz && a.freqMhz <= b[i].hiMhz;
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        if (ImGui::Button(b[i].name)) tuneFreq(a, std::min(b[i].loMhz + 0.007, (b[i].loMhz + b[i].hiMhz) / 2));
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("CW segment %.3f - %.3f MHz (IARU Region 1); the window covers +-7 kHz", b[i].loMhz, b[i].hiMhz);
        flowNext(4 * gUi);
    }
    ImGui::TextDisabled("|"); flowNext(8 * gUi);
    listenControls(a, S.sidetone, "cwSidetone", "cw", "Hear the focused signal: a tone keyed by its decoder, at the pitch chosen here");
    if (S.sidetone) {
        flowNext(6 * gUi); ImGui::SetNextItemWidth(90 * gUi);
        if (ImGui::SliderFloat("##cwpitch", &S.pitch, 400, 1000, "%.0f Hz")) plat::prefs().setD("cwPitch", S.pitch);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pitch of the tone (400 - 1000 Hz)");
    }
    flowNext(8 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Threshold"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::SliderFloat("##cwthr", &S.thrDb, 6, 24, "%.0f dB")) plat::prefs().setD("cwThreshold", S.thrDb);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How far over the noise floor a carrier must be to get a decoder");
    flowNext(8 * gUi);
    if (ImGui::Button("Clear")) a.engine.cw().clearSignals();
    flowEnd();
}

// the newest spectrum row into the waterfall
void follow(const dect2::CwTelemetry& t) {
    if (t.spectrum.empty() || t.specSeq == S.specSeq) return;
    S.specSeq = t.specSeq;
    const int W = (int)t.spectrum.size();
    if (W != S.bins) { S.bins = W; S.px.assign((size_t)W * State::kRows, 0xFF000000u); }
    std::vector<float> srt(t.spectrum);
    std::nth_element(srt.begin(), srt.begin() + W / 2, srt.end());
    const float med = srt[(size_t)W / 2];
    S.lo += (med - 3 - S.lo) * 0.2f;
    S.hi += (med + 35 - S.hi) * 0.2f;
    std::copy_backward(S.px.begin(), S.px.end() - W, S.px.end());
    for (int k = 0; k < W; k++) S.px[(size_t)k] = Waterfall::jet((t.spectrum[(size_t)k] - S.lo) / std::max(1.f, S.hi - S.lo));
    S.dirty = true;
}

void waterfall(App& a, float h) {
    const dect2::CwTelemetry& t = a.rx.cw;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float l = p0.x, r = p0.x + w, top = p0.y, bot = p0.y + h - ImGui::GetTextLineHeight() - 4 * gUi;
    dl->AddRectFilled(p0, ImVec2(r, p0.y + h), IM_COL32(8, 9, 10, 255));
    const double W = t.windowHz > 0 ? t.windowHz : 7000;
    auto X = [&](double hz) { return (float)(l + (hz + W) / (2 * W) * (r - l)); };
    if (gGfx && S.bins > 0 && (!S.wf || S.wfW != S.bins)) { delete S.wf; S.wf = gGfx->createImage(S.bins, State::kRows, 0xFF000000u); S.wfW = S.bins; }
    if (S.wf && S.dirty) { S.wf->update(0, 0, S.bins, State::kRows, S.px.data()); S.dirty = false; }
    if (S.wf && on) dl->AddImage(S.wf->texture(), ImVec2(l, top), ImVec2(r, bot));
    const ImU32 txt = IM_COL32(120, 124, 128, 255);
    char b[48];
    for (int k = -6; k <= 6; k += 2) {   // kHz from the dial, and the absolute frequency under it
        const float x = X(k * 1000.0);
        dl->AddLine(ImVec2(x, bot), ImVec2(x, bot + 4 * gUi), txt);
        snprintf(b, sizeof b, "%.1f", absKhz(a, k * 1000.0));
        dl->AddText(ImVec2(x - ImGui::CalcTextSize(b).x / 2, bot + 3 * gUi), txt, b);
    }
    if (on) {
        for (const auto& s : t.signals) {
            const float x = X(s.offsetHz);
            const bool f = s.id == t.focusId;
            const ImU32 c = f ? IM_COL32(255, 210, 90, 255) : s.locked ? IM_COL32(120, 230, 140, 220) : IM_COL32(160, 160, 160, 160);
            dl->AddTriangleFilled(ImVec2(x - 5 * gUi, top), ImVec2(x + 5 * gUi, top), ImVec2(x, top + 8 * gUi), c);
            if (f) dl->AddLine(ImVec2(x, top), ImVec2(x, bot), IM_COL32(255, 210, 90, 120), 1.f);
            if (s.key) dl->AddCircleFilled(ImVec2(x, top + 12 * gUi), 2.5f * gUi, c);
        }
    } else {
        dl->AddText(ImVec2(l + 10, top + 10), txt, a.engine.running() ? "starting" : "start the receiver to skim the band");
    }
    ImGui::InvisibleButton("##cwwf", ImVec2(w, h));
    if (on && ImGui::IsItemHovered()) {
        const double hz = (ImGui::GetIO().MousePos.x - l) / std::max(1.f, r - l) * 2 * W - W;
        ImGui::SetTooltip("%.2f kHz: click to decode this signal", absKhz(a, hz));
        if (ImGui::IsItemClicked()) a.engine.cw().setFocusHz(hz);
    }
}

void signalTable(App& a, float h) {
    const dect2::CwTelemetry& t = a.rx.cw;
    if (!ImGui::BeginTable("##cwsig", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, h))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("kHz"); ImGui::TableSetupColumn("WPM"); ImGui::TableSetupColumn("SNR"); ImGui::TableSetupColumn(tr("Call and country")); ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& s : t.signals) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char b[48];
        snprintf(b, sizeof b, "%.2f##cws%d", absKhz(a, s.offsetHz), s.id);
        ImGui::PushFont(a.mono, 0);
        if (ImGui::Selectable(b, s.id == t.focusId, ImGuiSelectableFlags_SpanAllColumns)) a.engine.cw().setFocusHz(s.offsetHz);
        ImGui::TableNextColumn();
        if (s.locked) ImGui::Text("%.0f%s", s.wpm, s.farnsworth ? " F" : ""); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%.0f dB", s.snrDb);
        ImGui::TableNextColumn();
        if (!s.calls.empty()) hamCallText(s.calls.back(), a.freqMhz * 1e6 + s.offsetHz, "CW"); else ImGui::TextDisabled("-");   // colour: new DXCC, new on band, not worked, worked (ham/ham_events_ui.h)
        ImGui::TableNextColumn();
        const std::string tail = s.text.size() > 90 ? s.text.substr(s.text.size() - 90) : s.text;
        const float maxX = ImGui::GetCursorScreenPos().x + std::max(40.f, ImGui::GetContentRegionAvail().x);
        // one line: only the words that fit, newest last
        std::string fit = tail;
        while (!fit.empty() && ImGui::CalcTextSize(fit.c_str()).x > maxX - ImGui::GetCursorScreenPos().x) { const size_t sp = fit.find(' '); fit = sp == std::string::npos ? fit.substr(1) : fit.substr(sp + 1); }
        richText(fit, 1e9f);
        ImGui::PopFont();
    }
    ImGui::EndTable();
}

void focusView(App& a) {
    const dect2::CwTelemetry& t = a.rx.cw;
    const dect2::CwSignal* f = dect2::cwFocus(t);
    if (!f) { ImGui::TextDisabled("click a signal on the waterfall or in the table to read it here"); return; }
    ImGui::PushFont(a.mono, 0);
    ImGui::TextColored(f->key ? pal::okGreen() : pal::grey(0.8f), "%.2f kHz", absKhz(a, f->offsetHz));
    ImGui::SameLine(0, 14 * gUi);
    if (f->locked) ImGui::Text("%.0f WPM%s", f->wpm, f->farnsworth ? " (Farnsworth spacing)" : ""); else ImGui::TextDisabled("measuring the speed");
    ImGui::SameLine(0, 14 * gUi);
    ImGui::Text("SNR %.0f dB", f->snrDb);
    for (const auto& c : f->calls) { ImGui::SameLine(0, 10 * gUi); hamCallText(c, a.freqMhz * 1e6 + f->offsetHz, "CW"); }
    ImGui::PopFont();
    if (ImGui::BeginChild("##cwbig", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        ImGui::PushFont(a.mono, fontBase() * 1.6f);
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
    if (live(a)) { follow(a.rx.cw); hamFeedCw(a, a.rx.cw); }   // ham.decode (ham/ham_events_ui.h)
    dect2::CwReceiver& r = a.engine.cw();
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    if (!S.wasRunning || S.pitch != S.pushedPitch) { r.setSidetoneHz(S.pitch); S.pushedPitch = S.pitch; }
    if (!S.wasRunning || (int)S.sidetone != S.pushedTone) { r.setSidetone(S.sidetone); S.pushedTone = S.sidetone; }
    if (!S.wasRunning || S.thrDb != S.pushedThr) { r.setThresholdDb(S.thrDb); S.pushedThr = S.thrDb; }
    S.wasRunning = true;
}

void tab(App& a) {
    load();
    presets(a);
    ImGui::Spacing();
    const float av = ImGui::GetContentRegionAvail().y;
    waterfall(a, std::max(90.f, av * 0.34f));
    ImGui::Spacing();
    if (!live(a)) return;
    signalTable(a, std::max(80.f, ImGui::GetContentRegionAvail().y * 0.45f));
    ImGui::Spacing();
    d40CwCard(a, a.rx.cw);   // 4.0: stations heard, CQ callers (app/d40)
    focusView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    if (!ImGui::BeginTable("##cwl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Call"); ImGui::TableSetupColumn("kHz", ImGuiTableColumnFlags_WidthStretch);
    int n = 0;
    for (const auto& s : a.rx.cw.signals)
        for (const auto& c : s.calls) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0);
            char b[48];
            snprintf(b, sizeof b, "%s##cwc%d_%d", c.c_str(), s.id, n++);
            if (ImGui::Selectable(b, s.id == a.rx.cw.focusId, ImGuiSelectableFlags_SpanAllColumns)) a.engine.cw().setFocusHz(s.offsetHz);
            ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%.2f", absKhz(a, s.offsetHz));
        }
    ImGui::EndTable();
    if (n == 0) ImGui::TextDisabled("no callsigns yet");
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::CwTelemetry& t = a.rx.cw;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", dect2::cwSummary(t).c_str());
    kv("window", "+-%.0f Hz around the dial, noise floor %.1f dB/bin", t.windowHz, t.floorDb);
    kv("characters", "%llu decoded, %llu not in the table", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv("signal time", "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Signals", !on ? 0 : a.rx.cw.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("Text", !on ? 0 : a.rx.cw.state == 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::cwSummary(a.rx.cw).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "CW";
    if (live(a)) l2 = dect2::cwSummary(a.rx.cw);
}

} // namespace

extern const ModeUi kCwUi;
const ModeUi kCwUi = {
    .sideTitle = "CALLSIGNS",
    .tabName = "CW",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
