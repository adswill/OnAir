// Per-mode Simple / Advanced view (docs/SIMPLE-ADVANCED.md). See mode_panel.h.
#include "mode_panel.h"
#include "mode_panel_logic.h"
#include "app.h"
#include "i18n/i18n.h"
#include "kit.h"
#include "platform/platform.h"
#include "ui/scale.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace modepanel {
namespace {

bool gInCommand = false;

std::map<std::string, Level>& cache() { static auto* m = new std::map<std::string, Level>; return *m; }

// the default of this profile, decided once at the start (startup()): a profile that already has settings of a former run keeps the tabs
Level profileLevel() {
    return parseLevel(plat::prefs().getS("setupUiLevelProfile", "advanced"), Level::Advanced);
}

Level levelOf(const App& a) {
    if (const char* e = getenv("DECT2_UILEVEL")) {   // test aid (hidden GUI checks, tools/ui_layout_check.sh): simple | advanced for every mode
        const std::string v = e;
        if (v == "simple") return Level::Simple;
        if (v == "advanced") return Level::Advanced;
    }
    const std::string k = prefKey(a.family, a.sub);
    auto it = cache().find(k);
    if (it != cache().end()) return it->second;
    const Level l = parseLevel(plat::prefs().getS(k.c_str(), ""), profileLevel());
    cache()[k] = l;
    return l;
}

}  // namespace

void startup(bool freshProfile) {
    plat::Prefs& p = plat::prefs();
    if (!p.has("setupUiLevelProfile")) p.setS("setupUiLevelProfile", levelText(profileDefault(!freshProfile)));
}

bool available(const App& a) {
    const ModeUi* mu = modeUi(a.family);
    return mu && mu->tab && !mu->noSimple;
}

bool simple(const App& a) { return available(a) && levelOf(a) == Level::Simple; }

void setSimple(App& a, bool s) {
    const Level l = s ? Level::Simple : Level::Advanced;
    const std::string k = prefKey(a.family, a.sub);
    cache()[k] = l;
    plat::prefs().setS(k.c_str(), levelText(l));
}

bool inCommandView() { return gInCommand; }

void levelBar(App& a) {
    const bool s = simple(a);
    const char* l1 = tr("Simple");
    const char* l2 = tr("Advanced");
    const float w = ImGui::CalcTextSize(l1).x + ImGui::CalcTextSize(l2).x + 60 * gUi;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w));
    ImGui::PushID("##modelevel");
    if (pillButton(l1, s)) setSimple(a, true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Simple: the spectrum and one view with the summary, the live data and the key controls of this mode"));
    ImGui::SameLine(0, 4 * gUi);
    if (pillButton(l2, !s)) setSimple(a, false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Advanced: every tab of this mode with all the detail"));
    ImGui::PopID();
}

void panes(std::initializer_list<Pane> list, float minPaneWidth) {
    std::vector<const Pane*> v;
    std::vector<float> weights;
    for (const Pane& p : list) { v.push_back(&p); weights.push_back(p.weight); }
    if (v.empty()) return;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = 8 * gUi;
    const PaneLayout lay = layoutPanes(avail.x, gap, weights, minPaneWidth * gUi);
    const int n = (int)v.size();
    const int rows = (n + lay.perRow - 1) / lay.perRow;
    float minH = 0;
    for (const Pane* p : v) minH = std::max(minH, p->minHeight);
    const float rowH = std::max(minH * gUi, (avail.y - gap * (float)(rows - 1)) / (float)rows);
    for (int i = 0; i < n; i++) {
        if (i % lay.perRow) ImGui::SameLine(0, gap);
        ImGui::BeginChild(v[i]->id, ImVec2(lay.widths[(size_t)i], rowH), ImGuiChildFlags_None);
        if (v[i]->title) kit::sectionHeader(v[i]->title);
        v[i]->draw();
        ImGui::EndChild();
    }
}

// The strip under the level switch: the mode's second summary line and the meters as small coloured read-outs (wrapping)
static void strip(App& a, const ModeUi& mu) {
    std::string l1, l2;
    if (mu.summary) mu.summary(a, l1, l2);
    std::vector<ModeMeter> mm;
    if (mu.meters && a.engine.running()) mu.meters(a, mm);
    if (l2.empty() && mm.empty()) return;
    if (!l2.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", l2.c_str()); ImGui::PopTextWrapPos(); }
    bool first = true;
    for (const ModeMeter& m : mm) {   // "29  SNR dB": the value in the colour of its level, then the name
        char val[48];
        snprintf(val, sizeof val, m.fmt, m.v);
        if (!first) sameLineIf(ImGui::CalcTextSize(val).x + ImGui::CalcTextSize(m.label).x + 36 * gUi, 16 * gUi);
        first = false;
        const ImVec4 col = m.level == 1 ? pal::okGreen() : m.level == 2 ? pal::warnAmber() : m.level == 3 ? pal::badRed() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(val);
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 5 * gUi);
        ImGui::TextDisabled("%s", m.label);
    }
    ImGui::Spacing();
}

void commandView(App& a, const ModeUi& mu) {
    gInCommand = true;
    gForceTab.clear();   // a forced tab (--tab) has no tab in this view
    levelBar(a);
    strip(a, mu);
    if (mu.command) mu.command(a);
    else if (mu.tab) mu.tab(a);
    gInCommand = false;
}

}  // namespace modepanel
