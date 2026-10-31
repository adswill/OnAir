// Mode > Modes shown...: tick the groups and modes the lists should show. Built from the mode table (kModes, kGroupNames), so new modes
// and groups appear here by themselves. The rules are in mode_visibility.h. Hidden modes stay reachable by search, --mode and deep links.
#include "../app.h"
#include "../i18n/i18n.h"
#include "mode_visibility.h"
#include "../platform/platform.h"
#include <cstdio>

namespace {

std::string filePath() { return plat::dataDir() + "/modes-shown.txt"; }

// the rules parsed from the saved strings; reparsed only when they change
const modevis::Rules& rulesOf(const App& a) {
    static std::string sm, sg, sf;
    static modevis::Rules r;
    if (a.modeHidden != sm || a.modeHiddenGroups != sg || a.modeFavs != sf) {
        sm = a.modeHidden; sg = a.modeHiddenGroups; sf = a.modeFavs;
        r.hiddenModes = modevis::parseList(sm); r.hiddenGroups = modevis::parseList(sg); r.favs = modevis::parseList(sf);
    }
    r.favsOnly = a.modeFavsOnly;
    return r;
}

void store(App& a, const modevis::Rules& r) {
    a.modeHidden = modevis::joinList(r.hiddenModes);
    a.modeHiddenGroups = modevis::joinList(r.hiddenGroups);
    a.modeFavs = modevis::joinList(r.favs);
    a.modeFavsOnly = r.favsOnly;
    if (!modevis::writeFile(filePath(), r)) fprintf(stderr, "modes shown: cannot write %s\n", filePath().c_str());   // the copy that survives a reset of settings.conf
    savePrefsSoon(a);   // and the settings file, a moment after the last tick (flushed at exit)
}

std::string groupName(int g) { return kGroupNames[g]; }

std::vector<std::string> groupModes(int g) {
    std::vector<std::string> v;
    for (const auto& m : allModes()) if (m.group == g) v.push_back(m.name);
    return v;
}

}  // namespace

void modeVisibilityLoad(App& a) {
    modevis::Rules r;
    if (modevis::readFile(filePath(), r)) {   // the file is written at every change, so it is the newer of the two
        a.modeHidden = modevis::joinList(r.hiddenModes); a.modeHiddenGroups = modevis::joinList(r.hiddenGroups);
        a.modeFavs = modevis::joinList(r.favs); a.modeFavsOnly = r.favsOnly;
    } else if (!a.modeHidden.empty() || !a.modeHiddenGroups.empty() || !a.modeFavs.empty() || a.modeFavsOnly) {   // from settings.conf only (an older version): make the file
        r.hiddenModes = modevis::parseList(a.modeHidden); r.hiddenGroups = modevis::parseList(a.modeHiddenGroups);
        r.favs = modevis::parseList(a.modeFavs); r.favsOnly = a.modeFavsOnly;
        modevis::writeFile(filePath(), r);
    }
}

std::string modeVisibilityText(const App& a) { return rulesOf(a).hiddenModes.empty() && rulesOf(a).hiddenGroups.empty() && rulesOf(a).favs.empty() && !a.modeFavsOnly ? std::string("default\n") : modevis::toText(rulesOf(a)); }

void modeVisibilityApplyText(App& a, const std::string& text) {
    modevis::Rules r;
    if (text == "default\n") { r = modevis::Rules(); }
    else if (!modevis::fromText(text, r)) return;
    store(a, r);
}

bool modeHiddenNow(const App& a, const ModeDef& m) {
    if (a.modeHidden.empty() && a.modeHiddenGroups.empty() && !a.modeFavsOnly) return false;
    return modevis::hidden(rulesOf(a), groupName(m.group), m.name, modeSelected(a, m));
}

bool modeListed(const App& a, const ModeDef& m, const char* query) {
    const bool searching = query && *query;
    if (!modeMatches(m, query)) return false;
    return searching || !modeHiddenNow(a, m);
}

bool modeGroupListed(const App& a, int group, const char* query) {
    for (const auto& m : allModes()) if (m.group == group && modeListed(a, m, query)) return true;
    return false;
}

void modeVisibilityWindow(App& a) {
    if (!a.modeVisOpen) return;
    firstWindowSize(460, 560);
    if (!ImGui::Begin(trw("Modes shown"), &a.modeVisOpen)) { ImGui::End(); return; }
    modevis::Rules r = rulesOf(a);
    bool changed = false;
    ImGui::TextWrapped("%s", tr("Tick the modes you use. The others leave the mode menu, picker, command palette and keys 1-9. A search still finds them (tagged hidden), and --mode works as before. The active mode always stays."));
    static char q[40] = "";
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##modevisq", tr("search modes and groups"), q, sizeof q);
    std::vector<std::string> groups;
    for (int g = 0; g < kNumGroups; g++) groups.push_back(groupName(g));
    if (ImGui::SmallButton(tr("Show all"))) { modevis::showAll(r); changed = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Hide all except..."))) { modevis::hideAll(r, groups); changed = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Reset"))) { modevis::reset(r); changed = true; }
    ImGui::TextDisabled("%s", tr("Presets"));
    for (const std::string& p : modevis::presetNames()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(tr(p.c_str()))) { modevis::applyPreset(r, p, groups); changed = true; }
    }
    if (ImGui::Checkbox(tr("Favourites only"), &r.favsOnly)) changed = true;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr("(star a mode below)"));
    ImGui::Separator();
    ImGui::BeginChild("##modevistree", ImVec2(0, 0));
    for (int g = 0; g < kNumGroups; g++) {
        const std::string gn = groupName(g);
        const std::vector<std::string> mods = groupModes(g);
        if (mods.empty()) continue;
        const bool gMatch = modevis::containsNoCase(gn, q) || modevis::containsNoCase(tr(kGroupNames[g]), q);
        bool any = gMatch;
        for (const auto& m : allModes()) if (m.group == g && modeMatches(m, q)) any = true;
        if (!any) continue;
        const modevis::GroupState st = modevis::groupState(r, gn, mods);
        ImGui::PushID(1000 + g);
        bool on = st != modevis::GroupState::Hidden;
        if (ImGui::Checkbox("##grp", &on)) { modevis::setGroupShown(r, gn, mods, on); changed = true; }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kGroupCols[g]));
        const bool open = ImGui::TreeNodeEx(trw(kGroupNames[g]), (q[0] ? ImGuiTreeNodeFlags_DefaultOpen : 0) | ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::PopStyleColor();
        if (st == modevis::GroupState::Mixed) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("(some hidden)")); }
        if (open) {
            for (const auto& m : allModes()) {
                if (m.group != g || (!gMatch && !modeMatches(m, q))) continue;
                ImGui::PushID(m.name);
                bool shown = !modevis::hiddenByRules(r, gn, m.name);
                ImGui::Indent(14);
                if (ImGui::Checkbox(m.name, &shown)) {
                    if (r.favsOnly && !r.hiddenModes.count(m.name) && !r.hiddenGroups.count(gn)) { if (shown) r.favs.insert(m.name); else r.favs.erase(m.name); }   // under Favourites only a tick is the star
                    else modevis::setModeShown(r, gn, m.name, mods, shown);
                    changed = true;
                }
                ImGui::SameLine();
                const bool fav = r.favs.count(m.name) != 0;
                if (ImGui::SmallButton(fav ? "*" : "-")) { if (fav) r.favs.erase(m.name); else r.favs.insert(m.name); changed = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(fav ? "Favourite: click to remove" : "Click to make a favourite"));
                ImGui::SameLine();
                ImGui::TextDisabled("%s", tr(m.sub));
                ImGui::Unindent(14);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
    if (changed) store(a, r);
}
