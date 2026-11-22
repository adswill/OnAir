// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The "star OnAir on GitHub" card: one small card in the corner, never in the way. The rules (when it may show, how often it returns) are in
// core/include/dect2/engine/star_prompt.h; this file only keeps the saved numbers, draws the card and opens the browser on a click on
// "Star on GitHub". Nothing is sent anywhere. Settings > Startup has the switch (starPromptOn).
#include "../app.h"
#include "dect2/engine/star_prompt.h"
#include <ctime>

namespace {
using dect2::StarKeys;
dect2::StarState gStar;
bool gLoaded = false, gVisible = false;
double gRunSince = -1, gLastAsk = 0;

void load() {
    gLoaded = true;
    plat::Prefs& p = plat::prefs();
    gStar.enabled = p.getB(StarKeys::on, true);
    gStar.launches = (int)p.getI(StarKeys::launches, 0);
    gStar.decoded = p.getB(StarKeys::decoded, false);
    gStar.choice = (int)p.getI(StarKeys::choice, 0);
    gStar.shown = (int)p.getI(StarKeys::shown, 0);
    gStar.later = (int)p.getI(StarKeys::later, 0);
    gStar.lastShownDay = p.getI(StarKeys::lastDay, 0);
}
void save() {
    plat::Prefs& p = plat::prefs();
    p.setI(StarKeys::launches, gStar.launches);
    p.setB(StarKeys::decoded, gStar.decoded);
    p.setI(StarKeys::choice, gStar.choice);
    p.setI(StarKeys::shown, gStar.shown);
    p.setI(StarKeys::later, gStar.later);
    p.setI(StarKeys::lastDay, gStar.lastShownDay);
}
}

// a normal start of the app (not a --shot or test run): counts one launch
void starCardLaunch() {
    if (!gLoaded) load();
    gStar.launches++;
    save();
}

void starCard(App& a, ImVec2 disp) {
    if (!gLoaded) load();
    gStar.enabled = plat::prefs().getB(StarKeys::on, true);   // the Settings switch takes effect at once
    const double t = ImGui::GetTime();
    if (!gVisible) {
        if (gStar.launches < 1 || !gStar.enabled || gStar.choice != 0 || gStar.shownThisSession) return;   // launches 0: a test run
        if (a.setupOpen || a.wizOpen || listenWizardShown()) return;   // the first-run guides come first
        // "a good decode" is approximated by the receiver having run for two minutes in one go
        if (a.engine.running()) { if (gRunSince < 0) gRunSince = t; } else gRunSince = -1;
        if (t < 20 || t - gLastAsk < 5) return;
        gLastAsk = t;
        gStar.today = (long)(time(nullptr) / 86400);
        const bool decoded = gRunSince >= 0 && t - gRunSince >= 120;
        if (dect2::starShouldShow(gStar, decoded ? dect2::StarEvent::Decode : dect2::StarEvent::Frame)) { gVisible = true; save(); }
        return;
    }
    const float u = gUi;
    ImGui::SetNextWindowPos(ImVec2(disp.x - 16 * u, disp.y - 40 * u), ImGuiCond_Always, ImVec2(1.f, 1.f));
    ImGui::SetNextWindowSize(ImVec2(std::min(380.f * u, disp.x - 32 * u), 0));
    ImGui::PushStyleColor(ImGuiCol_Border, pal::accent(0.8f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::Begin("##starcard", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 360 * u);
    ImGui::TextUnformatted(tr("Enjoying OnAir? A star on GitHub helps others find it. Thank you!"));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    bool done = false;
    if (ImGui::Button(tr("Star on GitHub"))) { dect2::openUrl(dect2::starUrl()); dect2::starOnChoice(gStar, dect2::StarChoice::Star); done = true; }
    ImGui::SameLine();
    if (ImGui::Button(tr("Maybe later"))) { dect2::starOnChoice(gStar, dect2::StarChoice::Later); done = true; }
    ImGui::SameLine();
    if (ImGui::Button(tr("Don't ask again"))) { dect2::starOnChoice(gStar, dect2::StarChoice::Never); done = true; }
    ImGui::End();
    if (done) { gVisible = false; save(); }
}
