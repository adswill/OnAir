// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// One signal indicator for every mode, and the CPU meter's "too slow" warning with its one-click fix.
// The adapter below maps each receiver's existing telemetry (lock lamp, SNR / MER, meter bank, ADC level) into meterlogic::Signal;
// the rules are in meter_logic.h (tested). No DSP is touched.
#include "ui2_internal.h"
#include "meter_logic.h"
#include "ui_common.h"
#include <cstring>
#include <memory>

using namespace meterlogic;

namespace {

// colour-blind safe (Okabe-Ito): good blue, locked sky blue, weak orange, none grey. The bar length and the word say it too.
ImVec4 qualityColour(Quality q) {
    switch (q) {
        case Quality::Good: return ImVec4(0.00f, 0.45f, 0.70f, 1);
        case Quality::Locked: return ImVec4(0.34f, 0.71f, 0.91f, 1);
        case Quality::Weak: return ImVec4(0.90f, 0.62f, 0.00f, 1);
        default: return ImVec4(0.52f, 0.52f, 0.54f, 1);
    }
}

Signal signalOf(const App& a) {
    Signal s;
    s.running = a.engine.running();
    if (!s.running) return s;
    if (a.spec.stats.rmsDbfs > -119.f) { s.haveLevel = true; s.levelDb = a.spec.stats.rmsDbfs; }
    const RxTelemetry& rx = a.rx;
    if (a.fmMode) {   // FM: a carrier in the channel, its audio SNR
        s.haveLock = true; s.locked = rx.standard == 6 && rx.fm.carrier;
        if (s.locked) { s.haveSnr = true; s.snrDb = rx.fm.snrDb; }
    } else if (a.dabMode) {   // DAB: no lock lamp of its own here; the ensemble SNR when there is one
        if (!a.dabSnrH.empty() && a.dabSnrH.back() > 0.f) { s.haveSnr = true; s.snrDb = a.dabSnrH.back(); }
    } else if (a.family < 6) {   // DVB-T2, DVB-T, ATSC, ISDB-T: the receiver's lock state, data SNR (MER where it has one)
        s.haveLock = true; s.locked = rx.state == 2;
        if (s.locked && rx.dataValid) { s.haveSnr = true; s.snrDb = (float)rx.dataSnrDb; }
        if (s.locked && rx.plpMerDb > 0 && rx.plpMerDb < 90) { s.haveSnr = true; s.snrDb = (float)rx.plpMerDb; }
    } else if (const ModeUi* mu = modeUi(a.family)) {
        if (mu->meters) {
            std::vector<ModeMeter> m;
            mu->meters(a, m);
            for (const ModeMeter& b : m) {
                s.meters++;
                s.worst = std::max(s.worst, b.level);
                if (b.level == 1) s.anyGood = true;
                if (!s.haveSnr && b.label && (strstr(b.label, "SNR") || strstr(b.label, "MER")) && b.v > -50 && b.v < 90 && b.level > 0) { s.haveSnr = true; s.snrDb = (float)b.v; }
            }
        }
    }
    return s;
}

float barsWidth(float h) { return h * (4 * 0.30f + 3 * 0.12f); }

std::string numbersText(const Signal& s) {
    char b[96];
    std::string t;
    if (s.haveLevel) { snprintf(b, sizeof b, "level %.1f dBFS", s.levelDb); t = b; }
    if (s.haveSnr) { snprintf(b, sizeof b, "SNR %.1f dB", s.snrDb); t += (t.empty() ? "" : "   ") + std::string(b); }
    return t;
}

Spark gSpark;
SlowState gSlow;
Relief gRelief;
uint64_t gLastCpuEvents = 0;

std::vector<Step> reliefSteps(App& a) {
    std::vector<Step> v;
    {   // 1: the waterfall rate (drawing work, nothing is decoded differently)
        const float cur = a.wf.rowsPerSec, next = nextWaterfallRate(cur);
        char b[96]; snprintf(b, sizeof b, "Waterfall slowed to %g rows a second", next);
        v.push_back({b, [&a, next] { if (next < 0) return false; a.wf.setRate(next); return true; }, [&a, cur] { a.wf.setRate(cur); }});
    }
    {   // 2: a smaller spectrum FFT
        const int oldFft = a.specFft, next = nextFft((int)a.spec.dbfs.size());
        char b[96]; snprintf(b, sizeof b, "Spectrum FFT reduced to %d bins", next);
        v.push_back({b, [&a, next] { if (!next) return false; a.specFft = next; a.engine.setSpectrumFft(next); return true; },
                     [&a, oldFft] { a.specFft = oldFft; a.engine.setSpectrumFft(oldFft); }});
    }
    {   // 3: a lower sample rate, where the mode and the radio allow it (restarts the receiver)
        auto prev = std::make_shared<double>(0.0);
        auto now = std::make_shared<double>(0.0);
        v.push_back({"Sample rate lowered", [&a, prev, now] { return lowerSampleRate(a, *prev, *now); }, [&a, prev] { restoreSampleRate(a, *prev); }});
    }
    return v;
}

}  // namespace

void signalBars(ImDrawList* dl, ImVec2 p, float h, int quality) {
    const Quality q = (Quality)std::clamp(quality, 0, 3);
    const int on = qualitySegments(q);
    const float w = h * 0.30f, gap = h * 0.12f;
    const ImU32 col = ImGui::ColorConvertFloat4ToU32(qualityColour(q)), off = ImGui::ColorConvertFloat4ToU32(ImVec4(0.5f, 0.5f, 0.5f, 0.30f));
    for (int i = 0; i < 4; i++) {
        const float sh = h * (0.40f + 0.20f * (float)i), x = p.x + (float)i * (w + gap);
        const ImVec2 a0(x, p.y + h - sh), b0(x + w, p.y + h);
        if (i < on) dl->AddRectFilled(a0, b0, col, 1.5f);
        else dl->AddRectFilled(a0, b0, off, 1.5f);
    }
}

float signalStripWidth(App&) {
    const float h = ImGui::GetFontSize();
    return barsWidth(h) + 6 * gUi + ImGui::CalcTextSize("No signal").x;
}

void signalStripCell(App& a, float x, float y, float h) {
    const Signal s = signalOf(a);
    const Quality q = classify(s);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bh = ImGui::GetFontSize();
    signalBars(dl, ImVec2(x, y + (h - bh) * 0.5f), bh, (int)q);
    const char* word = tr(qualityWord(q));
    dl->AddText(ImVec2(x + barsWidth(bh) + 6 * gUi, y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                ImGui::ColorConvertFloat4ToU32(q == Quality::None ? ui2::T.dim : ui2::T.bright), word);
    const float w = signalStripWidth(a);
    if (ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(x + w, y + h)) && ImGui::IsWindowHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("%s: %s", tr("Signal"), word);
        const std::string n = numbersText(s);
        if (!n.empty()) ImGui::TextDisabled("%s", n.c_str());
        ImGui::TextDisabled("%s", tr("No signal / Weak / Locked / Good: the same four steps in every mode."));
        ImGui::EndTooltip();
    }
}

void signalRow(App& a) {
    const Signal s = signalOf(a);
    const Quality q = classify(s);
    const float h = ImGui::GetFontSize() * 1.1f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    signalBars(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + 1), h, (int)q);
    ImGui::Dummy(ImVec2(barsWidth(h), h + 2));
    ImGui::SameLine(0, 8 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(q == Quality::None ? ui2::T.dim : qualityColour(q), "%s", tr(qualityWord(q)));
    const std::string n = numbersText(s);
    if (!n.empty()) { ImGui::SameLine(0, 12 * gUi); ImGui::TextDisabled("%s", n.c_str()); }
}

void cpuReliefTick(App& a, float cpuPct, int cores) {
    const SampleLoss l = a.engine.sampleLoss();
    const bool run = a.engine.running();
    const bool drops = run && l.live && l.cpuEvents > gLastCpuEvents && gLastCpuEvents > 0;
    gLastCpuEvents = run ? l.cpuEvents : 0;
    gSpark.push(cpuPct);
    if (!run) { gSlow = SlowState(); return; }
    gSlow.update(cpuPct, cores, l.live ? l.loadPct : 0.f, drops);
}

void cpuSparkDraw(ImDrawList* dl, ImVec2 p, ImVec2 size, ImU32 col, ImU32 edge, int cores) {
    const float full = 100.f * (float)std::max(1, cores);
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), edge & 0x40FFFFFF, 2.f);
    const float ty = p.y + size.y * (1.f - kBusyShare);
    dl->AddLine(ImVec2(p.x, ty), ImVec2(p.x + size.x, ty), edge, 1.f);   // the "too slow" line
    if (gSpark.count < 2) return;
    ImVec2 prev;
    for (int i = 0; i < gSpark.count; i++) {
        const float x = p.x + size.x * (float)(i + (Spark::N - gSpark.count)) / (float)(Spark::N - 1);
        const float y = p.y + size.y * (1.f - std::clamp(gSpark.at(i) / full, 0.f, 1.f));
        if (i) dl->AddLine(prev, ImVec2(x, y), col, 1.4f);
        prev = ImVec2(x, y);
    }
}

bool cpuReliefStrip(App& a, float limitX) {
    const bool undo = gRelief.canUndo(), slow = gSlow.slow && a.engine.running();
    if (!undo && !slow) return false;
    const float room = limitX - ImGui::GetCursorScreenPos().x;
    if (room < 90 * gUi) return false;
    bool drew = false;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5 * gUi, 0));
    if (undo) {
        ImGui::TextColored(pal::okGreen(), "%s", tr(gRelief.last().c_str())); drew = true;
        ImGui::SameLine(0, 6 * gUi);
        if (ImGui::SmallButton(tr("Undo"))) { gRelief.undoAll(); gSlow = SlowState(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Put everything the fix changed back as it was."));
    }
    if (slow) {
        if (drew) ImGui::SameLine(0, 10 * gUi);
        ImGui::TextColored(pal::warnAmber(), "%s", tr("Too slow")); drew = true;
        ImGui::SameLine(0, 6 * gUi);
        if (ImGui::SmallButton(undo ? tr("Fix more") : tr("Fix"))) {
            const std::string said = gRelief.fix(reliefSteps(a));
            if (said.empty()) a.engine.log(tr("Nothing more to turn down. Try a narrower mode or close other programs."));
            else { a.engine.log(said); gSlow = SlowState(); gSlow.under = 0; }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Eases the load one step at a time: the waterfall rate first, then the spectrum FFT size, then the sample rate. You can undo it."));
    }
    ImGui::PopStyleVar();
    return drew;
}
