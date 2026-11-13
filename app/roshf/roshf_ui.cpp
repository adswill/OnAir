// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ROS (HF) screens: the band presets, a waterfall of the sideband audio with the detected tone band marked, what was measured (sub-mode, baud,
// tone grid, span) and the log of detections. ROS is a proprietary weak-signal chat mode whose framing and FEC are not public: it is detected and
// named, its text is not decoded ("partial, needs a public spec", docs/ROS.md and docs/roshf.md).
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "gfx/gfx.h"
#include "i18n/i18n.h"
#include "dect2/roshf/roshf_rx.h"
#include "dect2/roshf/roshf_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct State {
    bool loaded = false;
    int view = 0;                        // 0 detection, 1 log
    int demod = dect2::HfDemodAuto;
    gfx::Image* tex = nullptr;
    uint64_t texSeq = ~0ull;
    int pushedDemod = -1;
};
State S;

bool live(const App& a) { return a.rx.standard == 206; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.demod = hfDemodLoad("roshfDemod");
}

uint32_t colourOf(uint8_t v) {
    // dark blue, cyan, yellow, white
    const float t = v / 255.f;
    float r, g, b;
    if (t < 0.4f) { const float u = t / 0.4f; r = 0.02f; g = 0.05f + 0.55f * u; b = 0.15f + 0.65f * u; }
    else if (t < 0.75f) { const float u = (t - 0.4f) / 0.35f; r = 0.02f + 0.95f * u; g = 0.6f + 0.35f * u; b = 0.8f - 0.7f * u; }
    else { const float u = (t - 0.75f) / 0.25f; r = 0.97f + 0.03f * u; g = 0.95f + 0.05f * u; b = 0.1f + 0.9f * u; }
    return 0xFF000000u | ((uint32_t)(b * 255) << 16) | ((uint32_t)(g * 255) << 8) | (uint32_t)(r * 255);
}

void presets(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", tr("Partial"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("ROS is detected and its sub-mode named from the public description of its tones. Its text is not decoded: the framing, coding and FEC of ROS are not published."));
    flowNext(8 * gUi);
    ImGui::TextDisabled("%s", tr("Band"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("ROS has no calling frequency in this list: the buttons only jump to the digital part of each band (upper sideband dial)."));
    flowNext(6 * gUi);
    int n = 0;
    const dect2::RoshfPreset* p = dect2::roshfPresets(n);
    for (int i = 0; i < n; i++) {
        const bool here = std::fabs(a.freqMhz - p[i].mhz) < 0.002;
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        char b[40]; snprintf(b, sizeof b, "%s##rosp%d", p[i].name, i);
        if (ImGui::Button(b)) tuneFreq(a, p[i].mhz);
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.4f MHz", p[i].mhz);
        flowNext(4 * gUi);
    }
    ImGui::TextDisabled("|"); flowNext(8 * gUi);
    char cl[40]; snprintf(cl, sizeof cl, "%s##roshf", tr("Clear"));
    if (ImGui::Button(cl)) a.engine.roshf().clearResults();
    flowEnd();
}

void waterfall(App& a) {
    const dect2::RoshfTelemetry& t = a.rx.roshf;
    const size_t W = dect2::kRoshfWaterBins, H = dect2::kRoshfWaterRows;
    if (!gGfx) return;
    if (!S.tex) S.tex = gGfx->createImage((int)W, (int)H, 0xFF000000u);
    if (S.tex && S.texSeq != t.seq) {
        std::vector<uint32_t> px(W * H, 0xFF000000u);
        for (size_t r = 0; r < t.water.size() && r < H; r++)
            for (size_t k = 0; k < W && k < t.water[r].size(); k++) px[r * W + k] = colourOf(t.water[r][k]);
        S.tex->update(0, 0, (int)W, (int)H, px.data());
        S.texSeq = t.seq;
    }
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = std::max(120.f * gUi, std::min(260.f * gUi, ImGui::GetContentRegionAvail().y * 0.5f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(0, 0, 0, 255));
    if (S.tex) dl->AddImage(S.tex->texture(), p, ImVec2(p.x + w, p.y + h));
    const double span = W * dect2::kRoshfBinHz;
    for (int f = 0; f <= 3500; f += 500) {
        const float x = p.x + (float)(f / span) * w;
        dl->AddLine(ImVec2(x, p.y + h - 6 * gUi), ImVec2(x, p.y + h), IM_COL32(200, 200, 200, 200));
        char b[16]; snprintf(b, sizeof b, "%d", f);
        dl->AddText(ImVec2(x + 2, p.y + h - 18 * gUi), IM_COL32(220, 220, 220, 220), b);
    }
    if (t.state == 2 && t.markHiHz > t.markLoHz) {
        const float x0 = p.x + (float)(t.markLoHz / span) * w, x1 = p.x + (float)(t.markHiHz / span) * w;
        dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x1, p.y + 6 * gUi), IM_COL32(255, 200, 60, 230));
        dl->AddRect(ImVec2(x0, p.y), ImVec2(x1, p.y + h), IM_COL32(255, 200, 60, 200), 0, 0, 1.5f);
        dl->AddText(ImVec2(x0 + 4, p.y + 8 * gUi), IM_COL32(255, 220, 120, 255), t.name.c_str());
    }
    ImGui::Dummy(ImVec2(w, h));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Sideband audio, 0 - 3500 Hz above the dial, newest at the top. The yellow band is the tone range of the signal found."));
}

void detection(App& a) {
    const dect2::RoshfTelemetry& t = a.rx.roshf;
    waterfall(a);
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(150 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::roshfSummary(t).c_str());
    if (t.state == 2) {
        kv(tr("sub-mode"), "%s%s", t.name.c_str(), t.weak ? tr(" (perhaps: few symbols)") : "");
        kv(tr("symbol rate"), "%.1f baud", t.baud);
        kv(tr("tone grid"), "%.2f Hz, %d positions used", t.spacingHz, t.distinctTones);
        kv(tr("tone range"), "%.0f - %.0f Hz (%.0f Hz wide)", t.loHz, t.hiHz, t.spanHz);
        kv(tr("score"), "%.2f", t.score);
        kv(tr("tuning"), "%+.0f Hz from the usual 1550 Hz centre", t.cfoHz);
        if (!t.why.empty()) { ImGui::TextDisabled("%s", t.why.c_str()); }
    }
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", tr("Text decoding: partial, needs a public spec"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("The framing, coding and FEC of ROS are not published, so nothing can be read from it. OnAir identifies the signal and its sub-mode (baud and bandwidth) and marks it on the waterfall; use the ROS program itself to read the text."));
    ImGui::PopTextWrapPos();
}

void logView(App& a) {
    const auto& ev = a.rx.roshf.events;
    if (!ImGui::BeginTable("##rosl", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Sub-mode")); ImGui::TableSetupColumn(tr("Baud")); ImGui::TableSetupColumn(tr("Tones (Hz)"));
    ImGui::TableSetupColumn(tr("Score")); ImGui::TableSetupColumn(tr("Detail"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const auto& e : ev) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(e.utc).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(e.name.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.1f", e.baud);
        ImGui::TableNextColumn(); ImGui::Text("%.0f - %.0f", e.loHz, e.hiHz);
        ImGui::TableNextColumn(); ImGui::Text("%.2f%s", e.score, e.weak ? "?" : "");
        ImGui::TableNextColumn(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(e.why.c_str()); ImGui::PopTextWrapPos();
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (ev.empty()) ImGui::TextDisabled("%s", tr("no detections yet"));
}

void tick(App& a) {
    load();
    if (!a.engine.running()) { S.pushedDemod = -1; return; }
    const int d = dect2::hfDemodResolve(S.demod, a.freqMhz);
    if (d != S.pushedDemod) { a.engine.roshf().setDemod(d); S.pushedDemod = d; }
}

void tuner(App& a, bool&) {
    load();
    hfDemodControl(a, S.demod, "roshfDemod", "roshf");
}

void tab(App& a) {
    load();
    presets(a);
    ImGui::Spacing();
    subNav("rosv", S.view, {"Detection", "Log"});
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (S.view == 0) detection(a); else logView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const auto& ev = a.rx.roshf.events;
    if (!ImGui::BeginTable("##rosl2", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Signal"), ImGuiTableColumnFlags_WidthStretch);
    for (const auto& e : ev) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(e.utc).c_str());
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(e.name.c_str()); ImGui::PopFont();
    }
    ImGui::EndTable();
    if (ev.empty()) ImGui::TextDisabled("%s", tr("no detections yet"));
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::RoshfTelemetry& t = a.rx.roshf;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::roshfSummary(t).c_str());
    kv(tr("detections"), "%llu (%llu %s)", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, tr("signals not ROS"));
    kv(tr("band over floor"), "%.0f dB", t.snrDb);
    kv(tr("input"), "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv(tr("signal time"), "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Signal"), !on ? 0 : a.rx.roshf.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("ROS", !on ? 0 : a.rx.roshf.state == 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::roshfSummary(a.rx.roshf).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "ROS (HF)";
    if (live(a)) l2 = dect2::roshfSummary(a.rx.roshf);
}

} // namespace

extern const ModeUi kRoshfUi;
const ModeUi kRoshfUi = {
    .sideTitle = "DETECTIONS",
    .tabName = "ROS",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
