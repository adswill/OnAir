// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The tours: the first-run tour (the whole window, in order) and the four short guided tours of Help > Guided tours. Each step dims the window except the part it describes, with a small flat callout next to it.
// The steps and the rules are in tour_logic.h (tested); the parts of the window report themselves with tourMark() every frame, and a step whose
// part is not on screen (another layout, a mode without it) is skipped.
#include "../app.h"
#include "tour_logic.h"

namespace {
tour::Anchors gAnchors;
bool gNoMore = true;   // "Don't show again": on by default; unticked, the tour comes back at the next start
int gLastStep = -99;   // the step of a guided tour last written to prefs (-99: none yet)

// the tour on screen: null for the first-run tour
const tour::Tour* curTour(const App& a) { return a.wizTour < 0 ? nullptr : tour::tours(nullptr) + a.wizTour; }

// the step of a guided tour goes to prefs, so starting that tour again resumes there
void rememberStep(App& a) {
    if (a.wizTour < 0 || a.wizStep == gLastStep) return;
    gLastStep = a.wizStep;
    plat::prefs().setI(tour::prefKeyTour(), a.wizTour);
    plat::prefs().setI(tour::prefKeyStep(), a.wizStep);
    plat::prefs().flush();
}
}

void tourMark(const char* id, ImVec2 lo, ImVec2 hi) {
    gAnchors.set(id, tour::Rect{lo.x, lo.y, hi.x, hi.y}, ImGui::GetFrameCount());
}

bool tourAnchor(const char* id, ImVec2& lo, ImVec2& hi) {
    if (!gAnchors.has(id, ImGui::GetFrameCount())) return false;
    const tour::Rect r = gAnchors.get(id);
    lo = ImVec2(r.x0, r.y0); hi = ImVec2(r.x1, r.y1);
    return true;
}

static void tourClose(App& a, bool done = false) {
    a.wizOpen = false;
    if (a.wizTour >= 0) {   // a guided tour: finishing forgets the place, skipping keeps it so the tour resumes there
        if (done) { plat::prefs().setI(tour::prefKeyTour(), -1); plat::prefs().flush(); }
        return;
    }
    if (gNoMore) { plat::prefs().setB(tour::prefKey(), true); plat::prefs().flush(); }
}

void wizEnter(App& a, int step) {
    a.wizStep = std::max(0, std::min(tour::count(curTour(a)) - 1, step));
    a.wizStepT = ImGui::GetTime();
}

void wizAction(App& a, int step) {
    if (step != 2) return;
    for (int i = 0; i < (int)a.devices.size(); i++) if (a.devices[i].kind == DeviceInfo::Synthetic) a.devIdx = i;
    setFamily(a, 0);
    a.stdMode = 2;                 // DVB-T: the synthetic DVB-T signal carries a real test programme
    a.tune.synth.dvbt = true; a.tune.synth.demoTv = true;
    if (a.engine.running()) a.engine.stop();
    a.wizStart = true;
    a.playReq = 1;                 // play the demo programme as soon as the receiver has found it
}

void tourStart(App& a) {
    a.wizTour = -1;   // the first-run tour
    a.wizOpen = true;
    a.wizX = -1;
    a.wizStep = -1;   // resolved on the first frame, when the anchors are known
    a.wizStepT = ImGui::GetTime();
    gNoMore = true;
}

void tourStartTour(App& a, int which) {
    int nt = 0;
    const tour::Tour* ts = tour::tours(&nt);
    if (which < 0 || which >= nt) return;
    a.wizTour = which;
    a.wizOpen = true;
    a.wizX = -1;
    // resume where this tour was left, when it was the one last started and not finished
    int at = -1;
    if (plat::prefs().getI(tour::prefKeyTour(), -1) == which) at = (int)plat::prefs().getI(tour::prefKeyStep(), -1);
    a.wizStep = at >= 0 && at < tour::count(&ts[which]) ? at : -1;
    a.wizStepT = ImGui::GetTime();
    gLastStep = -99;
}

void wizard(App& a, ImVec2 disp) {
    if (!a.wizOpen) return;
    const long frame = ImGui::GetFrameCount();
    const tour::Tour* T = curTour(a);   // null: the first-run tour
    // the parts of the layouts that already report their rectangle through the older table
    auto legacy = [&](const char* id, int tg) { if (!gAnchors.has(id, frame) && a.tgMax[tg].x > a.tgMin[tg].x) tourMark(id, a.tgMin[tg], a.tgMax[tg]); };
    legacy("modes", TgSwitch); legacy("spectrum", TgMain); legacy("panel", TgRight);
    if (a.touchUi) for (int i = 0; i < tour::count(T); i++) tourMark(tour::stepsOf(T, nullptr)[i].anchor, ImVec2(0, 0), disp);   // the touch shell has no such parts: every step is a centred card
    if (tour::visibleCount(gAnchors, frame, T) == 0) {   // nothing to point at yet (the first frames) or at all (a layout without these parts)
        if (ImGui::GetTime() - a.wizStepT > 1.0) tourClose(a);
        return;
    }
    if (ImGui::GetTime() - a.wizStepT < 0.3) return;   // a moment for every part to report itself (some are drawn after this call)
    if (a.wizStep < 0) a.wizStep = tour::next(gAnchors, frame, -1, +1, T);   // the first step that can be shown
    if (!tour::shown(gAnchors, frame, a.wizStep, T)) {   // the part went away (a resized window, another layout): the next one
        const int n = tour::next(gAnchors, frame, a.wizStep, +1, T);
        if (n < 0) { const int p = tour::next(gAnchors, frame, a.wizStep, -1, T); if (p < 0) { tourClose(a); return; } a.wizStep = p; }
        else a.wizStep = n;
    }
    rememberStep(a);
    const tour::Step& st = tour::stepsOf(T, nullptr)[a.wizStep];
    const int pos = tour::position(gAnchors, frame, a.wizStep, T), total = tour::visibleCount(gAnchors, frame, T);
    const bool first = tour::next(gAnchors, frame, a.wizStep, -1, T) < 0, last = tour::next(gAnchors, frame, a.wizStep, +1, T) < 0;
    const float u = gUi;
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const ImU32 acc = ImGui::ColorConvertFloat4ToU32(pal::accent());

    // the part the step describes (the dimming around it is drawn once the callout's place is known, so the callout stays clear of it)
    const tour::Rect r0 = gAnchors.get(st.anchor);
    const float pad = 3.f * u;
    const ImVec2 lo(std::max(0.f, r0.x0 - pad), std::max(0.f, r0.y0 - pad)), hi(std::min(disp.x, r0.x1 + pad), std::min(disp.y, r0.y1 + pad));

    // the callout: beside the part (right, left, below, above), else inside it at the bottom; the last measured height is used for the fit
    static float hMeasured = 150.f;
    const float margin = 12.f * u, gap = 12.f * u;
    const float W = std::min(340.f * u, disp.x - 2 * margin), H = hMeasured;
    ImVec2 p;
    if (a.touchUi) p = ImVec2((disp.x - W) * 0.5f, (disp.y - H) * 0.5f);
    else if (hi.x + gap + W <= disp.x - margin) p = ImVec2(hi.x + gap, lo.y);
    else if (lo.x - gap - W >= margin) p = ImVec2(lo.x - gap - W, lo.y);
    else if (hi.y + gap + H <= disp.y - margin) p = ImVec2((lo.x + hi.x - W) * 0.5f, hi.y + gap);
    else if (lo.y - gap - H >= margin) p = ImVec2((lo.x + hi.x - W) * 0.5f, lo.y - gap - H);
    else p = ImVec2((lo.x + hi.x - W) * 0.5f, hi.y - H - 3 * gap);
    p.x = std::max(margin, std::min(p.x, disp.x - W - margin));
    p.y = std::max(margin, std::min(p.y, disp.y - H - margin));

    {   // the window dimmed, except the part and the callout
        const ImU32 dim = IM_COL32(6, 8, 10, 150);
        const ImVec2 c0 = p, c1(p.x + W, p.y + H);
        auto band = [&](float x0, float y0, float x1, float y1) {   // a dimmed rectangle minus the callout
            if (x1 <= x0 || y1 <= y0) return;
            const float ix0 = std::max(x0, c0.x), iy0 = std::max(y0, c0.y), ix1 = std::min(x1, c1.x), iy1 = std::min(y1, c1.y);
            if (ix0 >= ix1 || iy0 >= iy1) { fg->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), dim); return; }
            fg->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, iy0), dim);
            fg->AddRectFilled(ImVec2(x0, iy1), ImVec2(x1, y1), dim);
            fg->AddRectFilled(ImVec2(x0, iy0), ImVec2(ix0, iy1), dim);
            fg->AddRectFilled(ImVec2(ix1, iy0), ImVec2(x1, iy1), dim);
        };
        band(0, 0, disp.x, lo.y);
        band(0, hi.y, disp.x, disp.y);
        band(0, lo.y, lo.x, hi.y);
        band(hi.x, lo.y, disp.x, hi.y);
            if (!a.touchUi) fg->AddRect(lo, hi, acc, 2.f, 0, 1.5f);
    }
    ImGui::SetNextWindowPos(p);
    ImGui::SetNextWindowSize(ImVec2(W, 0));
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.085f, 0.092f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 0.22f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * u, 14 * u));
    ImGui::Begin("##tour", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    if (T) {   // the name of the guided tour, above its steps
        ImGui::PushStyleColor(ImGuiCol_Text, pal::accent());
        ImGui::TextUnformatted(tr(T->title));
        ImGui::PopStyleColor();
    }
    char idx[24];
    snprintf(idx, sizeof idx, "%d / %d", pos, total);
    ImGui::TextUnformatted(tr(st.title));
    ImGui::SameLine(ImGui::GetWindowWidth() - 16 * u - ImGui::CalcTextSize(idx).x);
    ImGui::PushStyleColor(ImGuiCol_Text, pal::accent());
    ImGui::TextUnformatted(idx);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 2 * u));
    ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 16 * u);
    char body[512];
    snprintf(body, sizeof body, tr(st.text), (int)kNumModes);   // only the Modes text has a %d; the others ignore the argument
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.76f, 0.80f, 1.f));
    ImGui::TextUnformatted(body);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 6 * u));

    if (!T) {   // the first-run tour only: a guided tour is started on purpose, so it has no "don't show again"
        ImGui::Checkbox(tr("Don't show again"), &gNoMore);
        ImGui::Dummy(ImVec2(0, 2 * u));
    }
    const float bh = a.touchUi ? 40.f * u : 0.f;
    bool goBack = false, goNext = false, skip = false;
    ImGui::BeginDisabled(first);
    if (ImGui::Button(tr("Back"), ImVec2(70 * u, bh))) goBack = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
    if (ImGui::Button(last ? tr("Finish") : tr("Next"), ImVec2(80 * u, bh))) goNext = true;
    ImGui::PopStyleColor();
    if (!last) {
        ImGui::SameLine(0, 14 * u);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (ImGui::Button(tr("Skip"), ImVec2(0, bh))) skip = true;
        ImGui::PopStyleColor();
    }
    hMeasured = ImGui::GetWindowHeight();
    ImGui::End();

    if (!io.WantTextInput) {   // keyboard: Left / Right to move, Esc to close
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && !first) goBack = true;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) goNext = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) skip = true;
    }
    if (skip) { tourClose(a); return; }
    if (goBack) { const int n = tour::next(gAnchors, frame, a.wizStep, -1, T); if (n >= 0) wizEnter(a, n); }
    else if (goNext) { const int n = tour::next(gAnchors, frame, a.wizStep, +1, T); if (n >= 0) wizEnter(a, n); else tourClose(a, true); }
}
