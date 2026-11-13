// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MT63 screens: the calling-frequency presets (80 to 10 m), the bandwidth (automatic, 500 / 1000 / 2000 Hz) and the interleave
// (automatic, short, long), the waterfall of the audio passband with the block the receiver found marked on it, the level of each of the
// 64 carriers, and the text in big letters beside its chat log, callsigns highlighted.
#include "app.h"
#include "gfx/gfx.h"
#include "i18n/i18n.h"
#include "dect2/cw/cw_morse.h"
#include "dect2/mt63/mt63_rx.h"
#include "dect2/mt63/mt63_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct State {
    int bw = 0, intl = 0;                    // 0 automatic
    bool wasRunning = false;
    int pushedBw = -1, pushedIntl = -1;
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

bool live(const App& a) { return a.rx.standard == 96; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.bw = (int)plat::prefs().getI("mt63Bandwidth", 0);
    if (S.bw != 500 && S.bw != 1000 && S.bw != 2000) S.bw = 0;
    S.intl = std::max(0, std::min(2, (int)plat::prefs().getI("mt63Interleave", 0)));
    S.listen = plat::prefs().getB("mt63Listen", false);
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

void controls(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Band")); flowNext(6 * gUi);
    int n = 0;
    const dect2::Mt63Preset* p = dect2::mt63Presets(n);
    for (int i = 0; i < n; i++) {
        const bool here = std::fabs(a.freqMhz - p[i].mhz) < 0.002;
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        char b[40]; snprintf(b, sizeof b, "%s##mt63p%d", p[i].name, i);
        if (ImGui::Button(b)) tuneFreq(a, p[i].mhz);
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.4f MHz", p[i].mhz);
        flowNext(4 * gUi);
    }
    ImGui::TextDisabled("|"); flowNext(8 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Bandwidth")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(100 * gUi);
    char cur[24];
    if (S.bw) snprintf(cur, sizeof cur, "%d Hz", S.bw); else snprintf(cur, sizeof cur, "%s", tr("Auto"));
    if (ImGui::BeginCombo("##mt63bw", cur)) {
        for (int v : {0, 500, 1000, 2000}) {
            char l[24];
            if (v) snprintf(l, sizeof l, "%d Hz", v); else snprintf(l, sizeof l, "%s", tr("Auto"));
            if (ImGui::Selectable(l, v == S.bw)) { S.bw = v; plat::prefs().setI("mt63Bandwidth", v); }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("MT63 comes in three widths, 500, 1000 and 2000 Hz (5, 10 and 20 characters per second). Automatic looks for all three."));
    flowNext(8 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Interleave")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(90 * gUi);
    const char* names[3] = {tr("Auto"), tr("Short"), tr("Long")};
    if (ImGui::BeginCombo("##mt63il", names[S.intl])) {
        for (int v = 0; v < 3; v++)
            if (ImGui::Selectable(names[v], v == S.intl)) { S.intl = v; plat::prefs().setI("mt63Interleave", v); }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The long interleave spreads every character over 64 symbols, the short one over 32: the long one survives longer fades and bursts, the short one has less delay. Automatic decodes both and shows the one with clean codewords."));
    flowNext(8 * gUi);
    listenControls(a, S.listen, "mt63Listen", "mt63", tr("Play the sideband (dial + 200 Hz to 3.8 kHz) on the sound card; decoding is the same either way"));
    flowNext(8 * gUi);
    char cl[40]; snprintf(cl, sizeof cl, "%s##mt63", tr("Clear"));
    if (ImGui::Button(cl)) a.engine.mt63().clearResults();
    flowEnd();
}

void follow(const dect2::Mt63Telemetry& t) {
    if (t.spectrum.empty() || t.specSeq == S.specSeq) return;
    S.specSeq = t.specSeq;
    const int W = (int)t.spectrum.size();
    if (W != S.bins) { S.bins = W; S.px.assign((size_t)W * State::kRows, 0xFF000000u); }
    std::vector<float> srt(t.spectrum);
    std::nth_element(srt.begin(), srt.begin() + W / 2, srt.end());
    const float med = srt[(size_t)W / 2];
    S.lo += (med - 3 - S.lo) * 0.2f;
    S.hi += (med + 36 - S.hi) * 0.2f;
    std::copy_backward(S.px.begin(), S.px.end() - W, S.px.end());
    for (int k = 0; k < W; k++) S.px[(size_t)k] = Waterfall::jet((t.spectrum[(size_t)k] - S.lo) / std::max(1.f, S.hi - S.lo));
    S.dirty = true;
}

void waterfall(App& a, float h) {
    const dect2::Mt63Telemetry& t = a.rx.mt63;
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
    if (on && t.bandwidth > 0 && t.state >= 1) {   // the block the receiver is on
        const float x0 = X(t.centreHz - t.bandwidth / 2.0), x1 = X(t.centreHz + t.bandwidth / 2.0);
        const ImU32 c = t.state == 2 ? IM_COL32(120, 230, 140, 230) : IM_COL32(255, 210, 90, 230);
        dl->AddRect(ImVec2(x0, top + 1), ImVec2(x1, bot - 1), c, 0, 0, 1.5f);
        snprintf(b, sizeof b, "MT63-%d", t.bandwidth);
        dl->AddText(ImVec2(x0 + 4, top + 3), c, b);
    }
    if (!on) dl->AddText(ImVec2(l + 10, top + 10), txt, a.engine.running() ? tr("starting") : tr("start the receiver to read the band"));
    ImGui::InvisibleButton("##mt63wf", ImVec2(w, h));
    if (on && ImGui::IsItemHovered()) {
        const double hz = lo + (ImGui::GetIO().MousePos.x - l) / std::max(1.f, r - l) * (hi - lo);
        ImGui::SetTooltip(tr("%.0f Hz above the dial (%.3f kHz)"), hz, absKhz(a, hz));
    }
}

// the 64 carriers: one bar each
void carriers(App& a, float h) {
    const dect2::Mt63Telemetry& t = a.rx.mt63;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(12, 14, 16, 255));
    if (live(a) && t.carrierDb.size() == dect2::kMt63Carriers) {
        const float bw = w / dect2::kMt63Carriers;
        for (int i = 0; i < dect2::kMt63Carriers; i++) {
            const float v = std::max(0.f, std::min(1.f, (t.carrierDb[(size_t)i] + 30.f) / 30.f));
            dl->AddRectFilled(ImVec2(p0.x + i * bw + 1, p0.y + h - 2 - v * (h - 4)), ImVec2(p0.x + (i + 1) * bw - 1, p0.y + h - 2), t.state == 2 ? IM_COL32(110, 200, 130, 230) : IM_COL32(200, 170, 90, 200));
        }
    }
    ImGui::InvisibleButton("##mt63car", ImVec2(w, h));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The 64 carriers: the level of each, relative to the strongest"));
}

void textView(App& a) {
    const dect2::Mt63Telemetry& t = a.rx.mt63;
    ImGui::PushFont(a.mono, 0);
    if (t.state >= 1 && t.bandwidth) {
        ImGui::TextColored(t.state == 2 ? pal::okGreen() : pal::grey(0.8f), "MT63-%d %s", t.bandwidth, t.state == 2 ? tr(t.interleave == 2 ? "Long" : "Short") : tr("synchronising"));
        ImGui::SameLine(0, 14 * gUi); ImGui::Text("%.0f Hz  %.3f kHz", t.centreHz, absKhz(a, t.centreHz));
        ImGui::SameLine(0, 14 * gUi); ImGui::Text("SNR %.0f dB", t.snrDb);
        ImGui::SameLine(0, 14 * gUi); ImGui::Text("AFC %+.1f Hz", t.cfoHz);
        for (const auto& c : t.calls) { ImGui::SameLine(0, 10 * gUi); ImGui::TextColored(kCallCol, "%s", c.c_str()); }
    } else {
        ImGui::TextDisabled("%s", tr("searching"));
    }
    ImGui::PopFont();
    const float avW = ImGui::GetContentRegionAvail().x;
    const float logW = avW > 700 * gUi ? avW * 0.42f : 0;
    if (logW > 0) {   // the chat log: one row per line
        if (ImGui::BeginTable("##mt63log", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersOuter, ImVec2(logW, 0))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(tr("Line"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            ImGui::PushFont(a.mono, 0);
            for (const auto& l : t.lines) {
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
    if (ImGui::BeginChild("##mt63big", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        ImGui::PushFont(a.mono, ImGui::GetFontSize() * 1.6f);
        richText(t.text, ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x);
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
        ImGui::PopFont();
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    load();
    if (!a.engine.running()) { S.wasRunning = false; return; }
    if (live(a)) follow(a.rx.mt63);
    dect2::Mt63Receiver& r = a.engine.mt63();
    if (!S.wasRunning || S.bw != S.pushedBw) { r.setBandwidth(S.bw); S.pushedBw = S.bw; }
    if (!S.wasRunning || S.intl != S.pushedIntl) { r.setInterleave(S.intl); S.pushedIntl = S.intl; }
    if (!S.wasRunning || (int)S.listen != S.pushedListen) { r.setListen(S.listen); S.pushedListen = S.listen; }
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
    waterfall(a, std::max(90.f, av * 0.34f));
    ImGui::Spacing();
    if (!live(a)) return;
    carriers(a, std::max(36.f * gUi, av * 0.1f));
    ImGui::Spacing();
    textView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const auto& calls = a.rx.mt63.calls;
    if (calls.empty()) { ImGui::TextDisabled("%s", tr("no callsigns yet")); return; }
    ImGui::PushFont(a.mono, 0);
    for (const auto& c : calls) ImGui::TextColored(kCallCol, "%s", c.c_str());
    ImGui::PopFont();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::Mt63Telemetry& t = a.rx.mt63;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::mt63Summary(t).c_str());
    if (t.bandwidth) {
        kv(tr("bandwidth"), "%d Hz", t.bandwidth);
        kv(tr("interleave"), "%s", t.interleave ? tr(t.interleave == 2 ? "Long" : "Short") : "-");
        kv(tr("centre"), "%.1f Hz (%.3f kHz), AFC %+.1f Hz", t.centreHz, absKhz(a, t.centreHz), t.cfoHz);
        kv(tr("codeword quality"), "%.1f (short %.1f, long %.1f)", t.quality, t.qualityShort, t.qualityLong);
    }
    kv(tr("characters"), "%llu, %llu %s", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, tr("unclear"));
    kv(tr("input"), "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv(tr("signal time"), "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Signal"), !on ? 0 : a.rx.mt63.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Text"), !on ? 0 : a.rx.mt63.state == 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::mt63Summary(a.rx.mt63).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "MT63";
    if (live(a)) l2 = dect2::mt63Summary(a.rx.mt63);
}

} // namespace

extern const ModeUi kMt63Ui;
const ModeUi kMt63Ui = {
    .sideTitle = "CALLSIGNS",
    .tabName = "MT63",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
