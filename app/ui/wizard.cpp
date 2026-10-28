// The setup wizard (v2, four screens, every one skippable): 1 the radio (found and explained with a help link each, or no radio / the test
// signal), 2 the location (a city search, a locator or coordinates, or skipped) which sets the country, ITU region, band plans, FM step,
// DAB band III, TV channels, the ADS-B position and the time zone, 3 the interests (which modes come first, the starting mode and a
// starter preset list), 4 a summary with the tour on offer. Re-run from Help > Setup and lessons > Run setup again.
// The step is saved, so a setup closed halfway resumes there. The place tables, the mappings and the interests are in setup_logic.h
// (tested by tests/app/test_setup.cpp); the lesson goals and the radio notes are in learn_logic.h.
#include "../app.h"
#include "../panels/fixit.h"
#include "dect2/geo/maidenhead.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/updater.h"
#include "tour_logic.h"
#include "learn_logic.h"
#include "setup_logic.h"
#include "mode_visibility.h"
#include <set>
#include "../state/presets_app.h"

// The first-run tour is in tour.cpp; the lessons, the glossary and the checkup in learn.cpp.

namespace {

constexpr int kSteps = 4;   // radio, location, interests, done

// the name of a mode by id as the mode list shows it; "" when the id is unknown to this build
std::string modeNameById(const std::string& id, const learn::Region& r) {
    if (id == "fm") return tr("FM radio");
    if (id == "tv") return r.tvId ? modeTuningById(r.tvId)->name : r.tvFamily == 1 ? "ATSC" : r.tvFamily == 4 ? "ISDB-T" : r.tvStd == 2 ? "DVB-T" : "DVB-T2";
    int n = 0;
    const SubMode* t = subModes(n);
    for (int i = 0; i < n; i++) if (id == t[i].id) return t[i].name;
    if (const ModeTuning* mt = modeTuningById(id)) return mt->name;
    return "";
}

bool parseLoc(const char* text, double& lat, double& lon) {
    const std::string s = text;
    if (s.empty()) return false;
    return dect2::fromMaidenhead(s, lat, lon) || dect2::parseCoordinates(s, lat, lon);
}

// what the place typed or chosen sets; recomputed each frame (the tables are small)
setup::Derived derived(const App& a) {
    double lat, lon;
    if (!a.setupLoc[0] || !parseLoc(a.setupLoc, lat, lon)) return setup::deriveSkipped();
    if (a.setupCountry[0]) return setup::derive(setup::countryByCode(a.setupCountry), lat, lon, true);
    return setup::deriveFromPosition(lat, lon);
}

void saveStep(const App& a) { plat::prefs().setI(learn::prefSetupStep(), a.setupStep); plat::prefs().flush(); }

// a fixit entry: why, the steps for this computer, its buttons
void fixBox(const fixit::Entry& e) {
    ImGui::TextColored(pal::warnAmber(), "%s", tr(e.title));
    ImGui::TextUnformatted(tr(e.why));
    int k = 1;
    for (const char* st : fixit::stepsFor(e, fixit::currentOs())) { ImGui::Text("%d.", k++); ImGui::SameLine(); ImGui::TextUnformatted(tr(st)); }
    bool first = true;
    for (int b = 0; b < 3 && e.buttons[b].label; b++) {
        if (!(e.buttons[b].os & fixit::currentOs())) continue;
        if (!first) ImGui::SameLine();
        first = false;
        ImGui::PushID(b);
        if (ImGui::Button(tr(e.buttons[b].label))) {
            if (e.buttons[b].act == fixit::Act::OpenUrl) dect2::openUrl(e.buttons[b].arg);
            else if (e.buttons[b].act == fixit::Act::CopyText) ImGui::SetClipboardText(e.buttons[b].arg);
        }
        ImGui::PopID();
    }
}

// the OS's own note on drivers, in one place
const char* driverNote() {
    switch (fixit::currentOs()) {
    case fixit::OsWin: return N_("Windows: most USB radios (RTL-SDR, HackRF, Airspy) need the WinUSB driver once. Run Zadig, pick the radio, choose WinUSB and press Install. SDRplay radios use the SDRplay API instead.");
    case fixit::OsLinux: return N_("Linux: your account needs permission to open the radio. The udev rules (install-udev-rules.sh, done by the .deb) give it; unplug and replug afterwards. If the kernel's TV driver grabs an RTL-SDR, the rules blacklist it.");
    default: return N_("macOS: USB radios need no driver. If one is not found, try another cable or port (some cables only charge), and allow OnAir if macOS asks about USB accessories. SDRplay radios need the SDRplay API.");
    }
}

void radioStep(App& a) {
    ImGui::TextUnformatted(tr("Your radio. OnAir looks for radios on USB and picks one for you; a radio on the network is added by its address. You can also go on without one."));
    ImGui::Spacing();
    int radios = 0, demo = -1, cur = -1;
    for (int i = 0; i < (int)a.devices.size(); i++) {
        const DeviceInfo& d = a.devices[i];
        if (d.kind == DeviceInfo::Synthetic && demo < 0) demo = i;
        if (!d.isRadio()) continue;
        radios++;
        ImGui::PushID(i);
        if (ImGui::RadioButton(d.name.c_str(), a.devIdx == i)) a.devIdx = i;
        ImGui::SameLine();
        if (ImGui::SmallButton("?")) dect2::openUrl(setup::radioHelpUrl(d.board, d.name));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Open the help page for this radio in your browser"));
        if (d.isNetwork()) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("(network)")); }
        ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::TextDisabled("%s", tr(learn::radioBlurb(d.board, d.name)));
        ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::PopID();
        if (a.devIdx == i) cur = i;
    }
    if (demo >= 0) {
        if (ImGui::RadioButton(tr("No radio yet: use the test signal"), a.devIdx == demo)) a.devIdx = demo;
        ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::TextDisabled("%s", tr("A test signal made inside OnAir (a TV programme, FM and more). You can try everything with it; recordings (IQ files) open from Source."));
        ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    }
    ImGui::Spacing();
    if (!radios) {
        if (const fixit::Entry* e = fixit::byId("no_radio")) fixBox(*e);
        ImGui::Spacing();
    }
    // the last start failed: the matching fix, in plain words
    if (!a.startErr.empty() && (cur >= 0 || !radios)) {
        if (const fixit::Entry* e = fixit::lookup(a.startErr)) { if (std::string(e->id) != "no_radio") { fixBox(*e); ImGui::Spacing(); } }
    }
    if (ImGui::TreeNode(trw("Driver and permission notes for this computer"))) {
        ImGui::TextUnformatted(tr(driverNote()));
        if (const fixit::Entry* e = fixit::byId("usb_permission")) { ImGui::Spacing(); fixBox(*e); }
        ImGui::TreePop();
    }
    ImGui::Spacing();
    if (ImGui::Button(tr("Look again"))) refreshDevices(a);
    ImGui::SameLine();
    if (ImGui::Button(tr("Add a network radio..."))) { a.netAddOpen = true; a.netAddErr.clear(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A radio on another computer or on the network: a PlutoSDR by IP, an rtl_tcp server, a SpyServer, an SDR++ server, SoapyRemote, a Hermes Lite 2 or a Red Pitaya"));
}


void locationStep(App& a) {
    const float u = gUi;
    ImGui::TextUnformatted(tr("Where are you? This sets your country and radio region, the band plans, the FM step, the TV channels, the aircraft map position and the time zone. You can skip it."));
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Search for a city or country"));
    ImGui::SetNextItemWidth(280 * u);
    ImGui::InputTextWithHint("##city", tr("for example Vilnius"), a.setupQuery, sizeof a.setupQuery);
    if (a.setupQuery[0]) {
        const std::vector<setup::Place> hits = setup::search(a.setupQuery, 6);
        if (hits.empty()) ImGui::TextDisabled("%s", tr("No match. Type a locator or coordinates below instead."));
        for (size_t i = 0; i < hits.size(); i++) {
            const setup::Place& p = hits[i];
            const setup::Country* c = setup::countryByCode(p.country);
            char label[160];
            snprintf(label, sizeof label, "%s%s%s##hit%d", p.name.c_str(), p.isCity && c ? ", " : "", p.isCity && c ? tr(c->name) : "", (int)i);
            if (ImGui::Selectable(label)) {
                snprintf(a.setupLoc, sizeof a.setupLoc, "%.2f, %.2f", p.lat, p.lon);
                snprintf(a.setupCountry, sizeof a.setupCountry, "%s", p.country.c_str());
                a.setupQuery[0] = 0;
            }
        }
    }
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Or your position as a Maidenhead locator (KO24pq) or coordinates (54.68, 25.28)"));
    ImGui::SetNextItemWidth(260 * u);
    char before[sizeof a.setupLoc];
    memcpy(before, a.setupLoc, sizeof before);
    ImGui::InputText("##loc", a.setupLoc, sizeof a.setupLoc);
    if (memcmp(before, a.setupLoc, sizeof before)) a.setupCountry[0] = 0;   // typed by hand: the position decides the country
    ImGui::TextDisabled("%s", tr("Privacy: the location stays on this computer unless you turn on sending to a public map yourself."));
    double lat, lon;
    if (a.setupLoc[0]) {
        if (!parseLoc(a.setupLoc, lat, lon)) { ImGui::TextColored(pal::warnAmber(), "%s", tr("Not a locator or coordinates")); return; }
        const setup::Derived d = derived(a);
        const learn::Region& r = learn::regions(nullptr)[learn::clampRegion(d.region)];
        ImGui::Spacing();
        ImGui::TextColored(pal::accent(), "%s", d.known ? tr(d.countryName.c_str()) : tr("Country not recognised"));
        ImGui::SameLine();
        ImGui::TextDisabled("%.3f, %.3f  (%s)", lat, lon, dect2::toMaidenhead(lat, lon, 6).c_str());
        ImGui::TextDisabled(tr("ITU / IARU region %d.  FM %.1f to %.1f MHz, %d kHz step, %d us de-emphasis.  DAB in band III: %s."), d.itu, r.fmLoMhz, r.fmHiMhz, d.fmStepKhz, r.fmDeemphUs, tr(d.dab3 ? "yes" : "no"));
        ImGui::TextDisabled(tr("TV: %s, %d MHz channels.  Band plan: %s.  Time zone: %s."), d.tv.c_str(), d.tvMhz, d.bandPlan.empty() ? tr("automatic") : d.bandPlan.c_str(), d.tz.empty() ? tr("the computer's") : d.tz.c_str());
    } else {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("Skipped: nothing is changed. OnAir keeps its automatic choices from the system language."));
    }
}

void interestStep(App& a) {
    ImGui::TextUnformatted(tr("What interests you? Tick what you like, or nothing. This sets which modes are listed first, where OnAir starts and a few starter presets."));
    ImGui::Spacing();
    const setup::InterestDef* I = setup::interests();
    for (int i = 0; i < setup::IntCount; i++) {
        bool on = (a.setupInterests >> i) & 1u;
        ImGui::PushID(i);
        if (ImGui::Checkbox(tr(I[i].label), &on)) a.setupInterests = on ? (a.setupInterests | (1u << i)) : (a.setupInterests & ~(1u << i));
        ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::TextDisabled("%s", tr(I[i].blurb));
        ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::PopID();
    }
}

void doneStep(App& a) {
    const setup::Derived d = derived(a);
    const learn::Region& r = learn::regions(nullptr)[learn::clampRegion(d.region)];
    ImGui::TextUnformatted(tr("All set. This is what OnAir will use:"));
    ImGui::Spacing();
    const bool okDev = a.devIdx >= 0 && a.devIdx < (int)a.devices.size();
    ImGui::TextDisabled(tr("Radio: %s"), okDev ? a.devices[a.devIdx].name.c_str() : tr("none"));
    if (d.havePosition) ImGui::TextDisabled(tr("Place: %s, ITU region %d, %s"), d.known ? tr(d.countryName.c_str()) : tr("unknown country"), d.itu, d.tz.empty() ? tr("the computer's time zone") : d.tz.c_str());
    else ImGui::TextDisabled("%s", tr("Place: skipped"));
    std::string list;
    const setup::InterestDef* I = setup::interests();
    for (int i = 0; i < setup::IntCount; i++) if ((a.setupInterests >> i) & 1u) { if (!list.empty()) list += ", "; list += tr(I[i].label); }
    ImGui::TextDisabled(tr("Interests: %s"), list.empty() ? tr("none ticked") : list.c_str());
    const std::string startName = a.setupInterests ? modeNameById(setup::startModeFor(a.setupInterests), r) : std::string();
    ImGui::Spacing();
    if (!startName.empty()) { ImGui::TextUnformatted(tr("OnAir will start in:")); ImGui::SameLine(); ImGui::TextColored(pal::accent(), "%s", startName.c_str()); }
    else ImGui::TextUnformatted(tr("OnAir keeps the mode it has."));
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Change any of this at any time: Help > Setup and lessons > Run setup again, Mode > Modes shown, View > Language."));
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Want a guided tour? The lessons show the real controls one at a time (tuning, gain, the waterfall, modes, sound, recording). Each takes under a minute and can be skipped."));
}

void helpButton() {
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 24 * gUi);
    if (ImGui::SmallButton("?##setuphelp")) ImGui::OpenPopup("##setuphelppop");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("What the setup does"));
    if (ImGui::BeginPopup("##setuphelppop")) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34);
        ImGui::TextUnformatted(tr("The setup has four short screens and every one can be skipped. Nothing is forced.\n\nRadio: OnAir looks for radios on USB and picks one. \"?\" beside a radio opens its help page. Without a radio, the test signal lets you try OnAir.\n\nLocation: a city, a locator or coordinates give your country, the ITU region (1 Europe and Africa, 2 the Americas, 3 Asia and the Pacific), the band plan, the FM step, DAB, the TV channels, the aircraft map position and the time zone. The position never leaves this computer.\n\nInterests: decide which modes are starred and listed first, where OnAir starts and a few starter presets. Everything stays available.\n\nRun it again any time from Help > Setup and lessons > Run setup again."));
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
}

}  // namespace

void setupWizardOpen(App& a) {
    a.setupOpen = true;
    // first run: resume a setup closed halfway; from the Help menu: from the start, with the answers of the last run
    a.setupStep = plat::prefs().getB("setupDone", false) ? 0 : learn::setupResumeStep(plat::prefs().getI(learn::prefSetupStep(), 0), kSteps);
    a.setupRegion = learn::clampRegion(plat::prefs().getI("setupRegion", 0));
    a.setupGoals = (unsigned)plat::prefs().getI("setupGoals", 0);
    a.setupInterests = (unsigned)plat::prefs().getI("setupInterests", 0);
    a.setupAntenna = (int)std::max(0L, std::min((long)learn::AntCount - 1, plat::prefs().getI(learn::prefAntenna(), learn::AntWhip)));
    snprintf(a.setupLoc, sizeof a.setupLoc, "%s", plat::prefs().getS("setupLoc", "").c_str());
    snprintf(a.setupCountry, sizeof a.setupCountry, "%s", plat::prefs().getS("setupCountry", "").c_str());
    a.setupQuery[0] = 0;
    if (!plat::prefs().getB("setupDone", false)) saveStep(a);   // the step key marks an unfinished setup, so it resumes even when the settings are saved at exit
}

// star the modes of the interests (they are listed first) and add the starter presets that are not there yet
static void applyInterests(App& a, const setup::Derived& d) {
    const learn::Region& r = learn::regions(nullptr)[learn::clampRegion(d.region)];
    std::set<std::string> favs = modevis::parseList(a.modeFavs);
    for (const std::string& id : setup::modesFirst(a.setupInterests)) {
        const std::string name = modeNameById(id, r);
        if (!name.empty()) favs.insert(name);
    }
    a.modeFavs = modevis::joinList(favs);
    auto& st = presetsapp::store();
    bool added = false;
    for (const setup::Starter& s : setup::starterPresets(a.setupInterests, d)) {
        if (!modeTuningById(s.mode) && !subModeById(s.mode)) continue;   // a mode this build does not have
        bool have = false;
        for (const auto& p : st.user) if (p.name == s.name && p.mode == s.mode) have = true;
        if (have) continue;
        dect2::presets::Preset p;
        p.name = s.name; p.mode = s.mode;
        p.values["freqMhz"] = dect2::presets::Value::n(s.mhz);
        const std::string id = st.add(p);
        if (!id.empty()) { st.setFavourite(id, true); added = true; }
    }
    if (added) presetsapp::save();
}

// how = 0 skip (change nothing), 1 finish and start the lessons, 2 finish only, 3 finish and take the window tour
static void setupFinish(App& a, int how) {
    a.setupOpen = false;
    if (how) {
        const setup::Derived d = derived(a);
        if (d.havePosition) {
            a.setupRegion = d.region;
            const learn::Region& r = learn::regions(nullptr)[a.setupRegion];
            a.fmDeemph = r.fmDeemphUs;
            if (!d.bandPlan.empty()) a.bandPlan = d.bandPlan;
            plat::prefs().setB("unitsImperial", r.imperial);
            plat::prefs().setI("fmStepKhz", d.fmStepKhz);
            plat::prefs().setI("ituRegion", d.itu);
            plat::prefs().setS("locCountry", d.country);
            tvPlanFollowLocation();   // the TV channel plans (tvplan.h) start from this country
            plat::prefs().setS("locTimeZone", d.tz);
            plat::prefs().setB("dabBand3", d.dab3);
            plat::prefs().setD("adsbLat", d.lat);
            plat::prefs().setD("adsbLon", d.lon);
        }
        a.setupGoals = setup::goalBitsFor(a.setupInterests);
        plat::prefs().setI("setupRegion", a.setupRegion);
        plat::prefs().setI("setupGoals", (long)a.setupGoals);
        plat::prefs().setI("setupInterests", (long)a.setupInterests);
        plat::prefs().setS("setupLoc", a.setupLoc);
        plat::prefs().setS("setupCountry", a.setupCountry);
        if (a.setupInterests) {
            applyInterests(a, d);
            selectModeById(a, setup::startModeFor(a.setupInterests));
        }
    }
    plat::prefs().setB("setupDone", true);
    plat::prefs().setI(learn::prefSetupStep(), 0);
    savePrefs(a);
    plat::prefs().flush();
    if (how == 1) { plat::prefs().setB(tour::prefKey(), true); lessonsStart(a); }   // the lessons cover the tour's ground, on the real controls
    else if (how == 3 || (how == 0 && !plat::prefs().getB(tour::prefKey(), false))) tourStart(a);
}

void setupWizard(App& a, ImVec2 disp) {
    if (!a.setupOpen) return;
    const float u = gUi;
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.45f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(640.f * u, disp.x - 24 * u), 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, disp.y - 24 * u));
    ImGui::PushStyleColor(ImGuiCol_Border, pal::accent(0.8f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::Begin("##setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    static const char* const kNames[kSteps] = {N_("Radio"), N_("Location"), N_("Interests"), N_("Done")};
    a.setupStep = std::max(0, std::min(kSteps - 1, a.setupStep));
    ImGui::TextColored(pal::accent(), "%s  (%d / %d)", tr(kNames[a.setupStep]), a.setupStep + 1, kSteps);
    {   // the steps as dots
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float r = 4 * u, sp = 14 * u;
        ImGui::SameLine(ImGui::GetWindowWidth() - kSteps * sp - 40 * u);
        const ImVec2 c = ImGui::GetCursorScreenPos();
        for (int i = 0; i < kSteps; i++) {
            const ImVec2 p(c.x + i * sp + r, c.y + ImGui::GetTextLineHeight() * 0.5f);
            if (i <= a.setupStep) dl->AddCircleFilled(p, r, ImGui::ColorConvertFloat4ToU32(pal::accent(i == a.setupStep ? 1.f : 0.5f)));
            else dl->AddCircle(p, r, IM_COL32(255, 255, 255, 80));
        }
        ImGui::Dummy(ImVec2(kSteps * sp, ImGui::GetTextLineHeight()));
        helpButton();
    }
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(600.f * u, disp.x - 60 * u));
    switch (a.setupStep) {
    case 0: radioStep(a); break;
    case 1: locationStep(a); break;
    case 2: interestStep(a); break;
    default: doneStep(a); break;
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Separator();
    const float bh = a.touchUi ? 40.f * u : 0.f;
    const int before = a.setupStep;
    if (a.setupStep > 0 && ImGui::Button(tr("Back"), ImVec2(70 * u, bh))) a.setupStep--;
    if (before > 0) ImGui::SameLine();
    const bool last = before == kSteps - 1;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.34f, 0.40f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.27f, 0.45f, 0.53f, 1));
    int finish = -1;
    if (last) {
        if (ImGui::Button(tr("Start the lessons"), ImVec2(0, bh))) finish = 1;
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (ImGui::Button(tr("Window tour instead"), ImVec2(0, bh))) finish = 3;
        ImGui::SameLine();
        if (ImGui::Button(tr("No tour, just start"), ImVec2(0, bh))) finish = 2;
    } else {
        if (ImGui::Button(tr("Next"), ImVec2(90 * u, bh))) a.setupStep++;
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (ImGui::Button(tr("Skip this step"), ImVec2(0, bh))) {
            if (before == 1) { a.setupLoc[0] = 0; a.setupCountry[0] = 0; a.setupQuery[0] = 0; }
            if (before == 2) a.setupInterests = 0;
            a.setupStep++;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Leave this step as it is and go on. Nothing is changed for it."));
        ImGui::PopStyleColor();
    }
    ImGui::SameLine(0, 18 * u);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    if (!last && ImGui::Button(tr("Later"), ImVec2(0, bh))) { a.setupOpen = false; saveStep(a); }   // closes it; the next start resumes here
    if (!last && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Close the setup for now. It continues at this step next time, or from Help > Setup and lessons."));
    if (!last) ImGui::SameLine();
    if (ImGui::Button(tr("Skip setup"), ImVec2(0, bh))) finish = 0;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Skip the whole setup. Nothing is changed; it will not come back on its own."));
    ImGui::PopStyleColor();
    if (!ImGui::GetIO().WantTextInput && ImGui::IsWindowFocused()) {   // keyboard: Enter for Next, Escape for Later
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) && !last) a.setupStep++;
        else if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !last) { a.setupOpen = false; saveStep(a); }
    }
    ImGui::End();
    if (a.setupStep != before) saveStep(a);
    if (finish >= 0) setupFinish(a, finish);
}
