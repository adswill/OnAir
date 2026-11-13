// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AM stereo (C-QUAM) screens: the medium-wave presets, the stereo mode (automatic, mono, stereo), the audio bandwidth and the squelch; a big
// STEREO / MONO / NO CARRIER indicator, the left and right level bars with a short history, and the quadrature channel's spectrum from
// 0 to 60 Hz with the 25 Hz pilot marked.
#include "app.h"
#include "i18n/i18n.h"
#include "dect2/cquam/cquam_rx.h"
#include "dect2/cquam/cquam_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>

namespace {

struct State {
    int mode = 0;                 // 0 automatic, 1 mono, 2 stereo
    int widthHz = 5000;
    bool squelch = true;
    bool loaded = false, wasRunning = false;
    int pushedMode = -1, pushedWidth = -1, pushedSq = -1; float pushedVol = -1; bool pushedMute = false;
    uint64_t lastSeq = 0;
    std::deque<float> hl, hr, hp;   // level history, 10 per second
};
State S;
constexpr size_t kHist = 300;

bool live(const App& a) { return a.rx.standard == 108; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.mode = std::max(0, std::min(2, (int)plat::prefs().getI("cquamMode", 0)));
    const int w = (int)plat::prefs().getI("cquamWidth", 5000);
    S.widthHz = (w == 4500 || w == 5000 || w == 7500 || w == 10000) ? w : 5000;
    S.squelch = plat::prefs().getB("cquamSquelch", true);
}

void controls(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Band")); flowNext(6 * gUi);
    int n = 0;
    const dect2::CquamPreset* p = dect2::cquamPresets(n);
    for (int i = 0; i < n; i++) {
        const bool here = std::fabs(a.freqMhz - p[i].mhz) < 0.0005;
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        char b[40]; snprintf(b, sizeof b, "%s##cqp%d", p[i].name, i);
        if (ImGui::Button(b)) tuneFreq(a, p[i].mhz);
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.0f kHz", p[i].mhz * 1000);
        flowNext(4 * gUi);
    }
    ImGui::TextDisabled("|"); flowNext(8 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Stereo")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(100 * gUi);
    const char* names[3] = {tr("Auto"), tr("Mono"), tr("Stereo")};
    if (ImGui::BeginCombo("##cqmode", names[S.mode])) {
        for (int v = 0; v < 3; v++)
            if (ImGui::Selectable(names[v], v == S.mode)) { S.mode = v; plat::prefs().setI("cquamMode", v); }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Auto switches to stereo when the 25 Hz pilot is there. Mono plays L + R on both sides. Stereo uses the quadrature part whatever the pilot says."));
    flowNext(8 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Audio width")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(100 * gUi);
    char cur[24]; snprintf(cur, sizeof cur, "%.1f kHz", S.widthHz / 1000.0);
    if (ImGui::BeginCombo("##cqwidth", cur)) {
        for (int v : {4500, 5000, 7500, 10000}) {
            char l[24]; snprintf(l, sizeof l, "%.1f kHz", v / 1000.0);
            if (ImGui::Selectable(l, v == S.widthHz)) { S.widthHz = v; plat::prefs().setI("cquamWidth", v); }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The channel filter, each side of the carrier: 4.5 kHz for 9 kHz channel spacing, 5 kHz for 10 kHz, wider for a quiet band"));
    flowNext(8 * gUi);
    if (ImGui::Checkbox(tr("Squelch"), &S.squelch)) plat::prefs().setB("cquamSquelch", S.squelch);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Mute the noise when the carrier is weaker than 4 dB over it"));
    flowEnd();
}

void indicator(App& a, float h) {
    const dect2::CquamTelemetry& t = a.rx.cquam;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(12, 14, 16, 255), 4 * gUi);
    const char* txt = !on ? (a.engine.running() ? tr("starting") : tr("stopped")) : t.state == 2 ? tr("STEREO") : t.state == 1 ? tr("MONO") : tr("NO CARRIER");
    const ImU32 col = !on || t.state == 0 ? IM_COL32(120, 124, 128, 255) : t.state == 2 ? IM_COL32(110, 220, 130, 255) : IM_COL32(230, 190, 90, 255);
    ImGui::PushFont(a.mono, ImGui::GetFontSize() * 2.6f);
    const ImVec2 sz = ImGui::CalcTextSize(txt);
    dl->AddText(ImVec2(p0.x + 16 * gUi, p0.y + (h - sz.y) * 0.5f), col, txt);
    ImGui::PopFont();
    if (on && t.state >= 1) {
        char b[160];
        snprintf(b, sizeof b, "%s %+.1f Hz    C/N %.0f dB    %s %.1f %%    %s %.0f %%", tr("carrier"), t.cfoHz, t.snrDb, tr("pilot"), t.pilotPct, tr("modulation"), t.modPct);
        ImGui::PushFont(a.mono, 0);
        dl->AddText(ImVec2(p0.x + 16 * gUi + sz.x + 30 * gUi, p0.y + h * 0.5f - ImGui::GetTextLineHeight() * 0.5f), IM_COL32(190, 194, 198, 255), b);
        ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(w, h));
}

void bars(App& a, float h) {
    const dect2::CquamTelemetry& t = a.rx.cquam;
    const bool on = live(a) && t.state >= 1;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(12, 14, 16, 255));
    const float half = h * 0.5f;
    for (int ch = 0; ch < 2; ch++) {
        const float y0 = p0.y + ch * half + 4 * gUi, y1 = y0 + half - 8 * gUi;
        const float db = ch == 0 ? t.leftDb : t.rightDb;
        const float v = on ? std::max(0.f, std::min(1.f, (db + 60.f) / 60.f)) : 0.f;
        const float lw = 22 * gUi, x0 = p0.x + lw + 6 * gUi, x1 = p0.x + w * 0.38f;
        dl->AddText(ImVec2(p0.x + 6 * gUi, y0), IM_COL32(190, 194, 198, 255), ch == 0 ? "L" : "R");
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(30, 34, 38, 255));
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + (x1 - x0) * v, y1), v > 0.95f ? IM_COL32(230, 90, 80, 255) : v > 0.8f ? IM_COL32(230, 190, 90, 255) : IM_COL32(110, 200, 130, 255));
        // history
        const std::deque<float>& hs = ch == 0 ? S.hl : S.hr;
        const float hx0 = x1 + 14 * gUi, hx1 = p0.x + w - 6 * gUi;
        dl->AddRectFilled(ImVec2(hx0, y0), ImVec2(hx1, y1), IM_COL32(20, 22, 26, 255));
        for (size_t i = 1; i < hs.size(); i++) {
            const float xa = hx0 + (hx1 - hx0) * (float)(i - 1) / kHist, xb = hx0 + (hx1 - hx0) * (float)i / kHist;
            const float va = std::max(0.f, std::min(1.f, (hs[i - 1] + 60.f) / 60.f)), vb = std::max(0.f, std::min(1.f, (hs[i] + 60.f) / 60.f));
            dl->AddLine(ImVec2(xa, y1 - va * (y1 - y0)), ImVec2(xb, y1 - vb * (y1 - y0)), ch == 0 ? IM_COL32(120, 190, 250, 230) : IM_COL32(250, 170, 110, 230), 1.4f);
        }
    }
    ImGui::InvisibleButton("##cqbars", ImVec2(w, h));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Left and right audio level (dBFS, the last 30 s on the right)"));
}

void pilotPlot(App& a, float h) {
    const dect2::CquamTelemetry& t = a.rx.cquam;
    const bool on = live(a) && t.state >= 1;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(12, 14, 16, 255));
    const float top = p0.y + 4 * gUi, bot = p0.y + h - ImGui::GetTextLineHeight() - 6 * gUi, l = p0.x + 6 * gUi, r = p0.x + w - 6 * gUi;
    const float bw = (r - l) / dect2::kCquamPilotBins;
    const ImU32 txt = IM_COL32(120, 124, 128, 255);
    char b[40];
    for (int k = 0; k <= 60; k += 10) {
        const float x = l + bw * (k + 0.5f);
        dl->AddLine(ImVec2(x, bot), ImVec2(x, bot + 4 * gUi), txt);
        snprintf(b, sizeof b, "%d Hz", k);
        dl->AddText(ImVec2(x - ImGui::CalcTextSize(b).x / 2, bot + 4 * gUi), txt, b);
    }
    if (on) {
        for (int k = 1; k < dect2::kCquamPilotBins; k++) {   // 0 Hz holds the mean: left out
            const float v = std::max(0.f, std::min(1.f, (t.qSpecDb[k] + 80.f) / 60.f));
            const bool pil = k >= 24 && k <= 26;
            dl->AddRectFilled(ImVec2(l + bw * k + 1, bot - v * (bot - top)), ImVec2(l + bw * (k + 1) - 1, bot), pil ? (t.pilotSeen ? IM_COL32(110, 220, 130, 240) : IM_COL32(230, 190, 90, 220)) : IM_COL32(100, 140, 190, 200));
        }
    }
    dl->AddText(ImVec2(l + bw * 27, top), txt, tr("25 Hz pilot"));
    ImGui::InvisibleButton("##cqpil", ImVec2(w, h));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The quadrature part of the carrier (L - R and the pilot), 0 - 60 Hz. A C-QUAM station has a 25 Hz tone at about 5 percent of the carrier here."));
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    load();
    if (!a.engine.running()) { S.wasRunning = false; return; }
    if (live(a) && a.rx.cquam.seq != S.lastSeq) {
        S.lastSeq = a.rx.cquam.seq;
        auto push = [](std::deque<float>& d, float v) { d.push_back(v); while (d.size() > kHist) d.pop_front(); };
        push(S.hl, a.rx.cquam.leftDb); push(S.hr, a.rx.cquam.rightDb); push(S.hp, a.rx.cquam.pilotDb);
    }
    dect2::CquamReceiver& r = a.engine.cquam();
    if (!S.wasRunning || S.mode != S.pushedMode) { r.setStereoMode(S.mode); S.pushedMode = S.mode; }
    if (!S.wasRunning || S.widthHz != S.pushedWidth) { r.setWidthHz(S.widthHz); S.pushedWidth = S.widthHz; }
    if (!S.wasRunning || (int)S.squelch != S.pushedSq) { r.setSquelch(S.squelch); S.pushedSq = S.squelch; }
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    S.wasRunning = true;
}

void tab(App& a) {
    load();
    controls(a);
    ImGui::Spacing();
    const float av = ImGui::GetContentRegionAvail().y;
    indicator(a, std::max(70.f * gUi, av * 0.22f));
    ImGui::Spacing();
    bars(a, std::max(80.f * gUi, av * 0.30f));
    ImGui::Spacing();
    pilotPlot(a, std::max(90.f * gUi, av * 0.30f));
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::CquamTelemetry& t = a.rx.cquam;
    if (t.state == 0) { ImGui::TextDisabled("%s", tr("no carrier yet")); return; }
    ImGui::PushFont(a.mono, 0);
    ImGui::Text("%.3f kHz", a.freqMhz * 1000.0 + t.cfoHz / 1000.0);
    ImGui::TextColored(t.state == 2 ? pal::okGreen() : pal::grey(0.8f), "%s", t.state == 2 ? "C-QUAM" : "AM");
    ImGui::PopFont();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::CquamTelemetry& t = a.rx.cquam;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::cquamSummary(t).c_str());
    kv(tr("carrier"), "%.3f kHz (%+.1f Hz), %.1f dBFS", a.freqMhz * 1000.0 + t.cfoHz / 1000.0, t.cfoHz, t.levelDb);
    kv(tr("carrier to noise"), "%.1f dB (%.0f kHz each side)", t.snrDb, t.widthHz / 1000.0);
    kv(tr("pilot"), "%.1f dB over its neighbours, %.1f %% of the carrier%s", t.pilotDb, t.pilotPct, t.pilotSeen ? "" : ", not found");
    kv(tr("modulation"), "%.0f %% (L + R, peak of the last second)", t.modPct);
    if (t.state == 2) kv(tr("separation"), "L - R is %.1f dB against L + R", t.widthDb);
    kv(tr("stereo phase"), "cos = %.2f", t.lockQuality);
    kv(tr("pilot checks"), "%llu %s, %llu %s", (unsigned long long)t.blocksOk, tr("found"), (unsigned long long)t.blocksBad, tr("not found"));
    kv(tr("input"), "%.3f Msps", t.inputRate / 1e6);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Carrier"), !on ? 0 : a.rx.cquam.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Pilot"), !on ? 0 : a.rx.cquam.pilotSeen ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::cquamSummary(a.rx.cquam).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "AM stereo";
    if (live(a)) l2 = dect2::cquamSummary(a.rx.cquam);
}

} // namespace

extern const ModeUi kCquamUi;
const ModeUi kCquamUi = {
    .sideTitle = "STATION",
    .tabName = "C-QUAM",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
