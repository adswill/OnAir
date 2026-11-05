// Settings (menu bar > Settings, Ctrl+,): one window with every preference gathered in categories, a search across all of them, a reset per
// setting and per category, a marker for what differs from the default, and an Advanced foldout per category. The rows are in
// settings_table.h; here each row is bound to the field that holds its value (the same fields the old menus use, saved by
// app/state/prefs.cpp), so a row without a field is not shown. Docs: docs/SETTINGS.md.
#include <algorithm>
#include "../app.h"
#include "../i18n/i18n.h"
#include "../platform/platform.h"
#include "../platform/ffmpeg_libs.h"
#include "../ui/scale.h"
#include "settings_table.h"
#include "setup_logic.h"
#include "kit/mode_panel.h"
#include "ui2_internal.h"
#include "dect2/common/u8file.h"
#include "dect2/engine/updater.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <type_traits>

using namespace settings;

namespace {

struct Bind {
    double (*gn)(App&) = nullptr;
    void (*sn)(App&, double) = nullptr;
    std::string (*gs)(App&) = nullptr;
    void (*ss)(App&, const std::string&) = nullptr;
};

#define NB(k, expr) m[k] = Bind{[](App& a) -> double { return (double)(expr); }, [](App& a, double v) { using T = std::remove_reference_t<decltype(expr)>; expr = (T)v; }, nullptr, nullptr}
#define SB(k, expr) m[k] = Bind{nullptr, nullptr, [](App& a) -> std::string { return (expr); }, [](App& a, const std::string& v) { (expr) = v; }}
#define PD(k) m[k] = Bind{[](App&) -> double { return plat::prefs().getD(k, 0); }, [](App&, double v) { plat::prefs().setD(k, v); }, nullptr, nullptr}

const std::map<std::string, Bind>& binds() {
    static const std::map<std::string, Bind> mm = [] {
        std::map<std::string, Bind> m;
        NB("powerLowMode", a.powerLowMode); NB("powerHidden", a.powerHidden); NB("reduceCpuHidden", a.reduceCpuHidden);
        NB("discordOn", a.discordOn); NB("discordFreq", a.discordFreq);
        NB("uiTheme", a.uiTheme); NB("uiVariant", a.uiVariant); NB("layoutUnified", a.layoutUnified); NB("uiTextScale", a.uiTextScale);
        NB("touchUi", a.touchUi); NB("touchScale", a.touchScale); NB("perfOverlay", a.perfOverlay);
        NB("wfMap3", a.wf.map); NB("wfAutoFloor", a.wf.autoTrack); NB("wfGamma", a.wf.gamma); NB("wfBright", a.wf.brightness);
        NB("wfContrast", a.wf.contrast); NB("wfClock", a.wf.clockLabels); NB("wfAvg", a.wf.rowAverage); NB("wfLowPass", a.wfLowPass);
        NB("specAutoDb", a.specAutoDb); NB("specPerHz", a.specPerHz); NB("specDbm", a.specDbm); NB("specFill", a.specFill);
        NB("specGrid", a.specGrid); NB("specPeaks", a.specPeaks); NB("specReadout", a.specReadout); NB("specLine", a.specLine);
        NB("specAvgSec", a.specAvgSec); NB("holdDecay", a.holdDecay);
        NB("tuningMode", a.tuningMode); NB("centreTuning", a.centreTuning); NB("dragMovesTuned", a.dragMovesTuned);
        NB("specSnapHz", a.specSnapHz); NB("bwAuto", a.bwAuto);
        NB("popTop", a.popTop); NB("subsOn", a.subsOn);
        m["recFormat"] = Bind{[](App& a) -> double { return a.recFormat; }, [](App& a, double v) { a.recFormat = (int)v; plat::prefs().setI("recFormat", (long)v); }, nullptr, nullptr};
        NB("fmMono", a.fmMono); NB("fmStereo", a.fmStereo); NB("fmDeemph", a.fmDeemph); NB("fmRegion", a.fmRegion);
        NB("fmSquelch", a.fmSquelch); NB("ttxPage", a.ttxPage); SB("tvLang", a.tvLang);
        PD("adsbLat"); PD("adsbLon");
        NB("updCheck", a.updCheck); NB("updBeta", a.updPre); NB("updAuto", a.updAuto); NB("updSrc", a.updSrc);
        NB("updMirrorOn", a.updMirrorOn); SB("updMirror", a.updMirror);
        SB("udpHost", std::string(a.udpHost)); NB("udpPort", a.out.port); NB("udpRtp", a.out.rtp);
        return m;
    }();
    return mm;
}
// udpHost is a char buffer: the setter above assigns a std::string, so give it its own
void putUdpHost(App& a, const std::string& v) { snprintf(a.udpHost, sizeof a.udpHost, "%s", v.c_str()); }

const Bind* bindOf(const Info& i) {
    auto f = binds().find(i.key);
    return f == binds().end() ? nullptr : &f->second;
}
bool isCustom(const Info& i) { return i.kind == Kind::Custom || i.kind == Kind::Link; }

Val readVal(App& a, const Info& i) {
    Val v = defaultOf(i);
    const Bind* b = bindOf(i);
    if (!b) return v;
    if (i.kind == Kind::Text) { if (b->gs) v.s = b->gs(a); }
    else if (b->gn) v.d = b->gn(a);
    return v;
}
void writeVal(App& a, const Info& i, const Val& v) {
    const Bind* b = bindOf(i);
    if (!b) return;
    if (i.kind == Kind::Text) {
        if (!strcmp(i.key, "udpHost")) putUdpHost(a, v.s);
        else if (b->ss) b->ss(a, v.s);
    } else if (b->sn) b->sn(a, clampTo(i, v.d));
    savePrefsSoon(a);
}

// the three custom rows that have a default to go back to
bool customChanged(const App& a, const Info& i) {
    if (!strcmp(i.key, "themeMode")) return a.lightUi || a.highContrast;
    if (!strcmp(i.key, "uiLang")) return !a.uiLang.empty();
    if (!strcmp(i.key, "uiTypography")) return plat::prefs().getI("uiTypography", 0) != 0;
    if (!strcmp(i.key, ffmpeglibs::kPrefKey)) return !plat::basePrefs().getS(ffmpeglibs::kPrefKey, "").empty();
    return false;
}
// Advanced > FFmpeg libraries: the last folder tried in this run (ffmpegRow); a reset of the row or of every setting forgets it
ffmpeglibs::Check gFfTried;
bool gFfHasTried = false;
void ffmpegForget() { gFfTried = ffmpeglibs::Check(); gFfHasTried = false; }
// the FFmpeg choice saved differs from the FFmpeg in use: it applies at the next start
bool ffmpegPending() {
    const ffmpeglibs::Status& st = ffmpeglibs::status();
    return plat::basePrefs().getS(ffmpeglibs::kPrefKey, "") != (st.external ? st.chosen : std::string());
}
// the restart request: the notice and a button that closes OnAir normally and starts it again
void ffmpegRestartButton() {
    if (ImGui::Button(tr("Restart OnAir"))) { ffmpeglibs::requestRestart(); glfwSetWindowShouldClose(gWindow, 1); }
}

void customReset(App& a, const Info& i) {
    if (!strcmp(i.key, "themeMode")) { a.lightUi = false; a.highContrast = false; savePrefs(a); }
    else if (!strcmp(i.key, "uiLang")) setUiLanguage(a, "");
    else if (!strcmp(i.key, "uiTypography")) plat::prefs().setI("uiTypography", 0);
    else if (!strcmp(i.key, ffmpeglibs::kPrefKey)) { plat::basePrefs().setS(ffmpeglibs::kPrefKey, ""); plat::basePrefs().flush(); ffmpegForget(); }
}

// text that wraps to the width left in the cell, also inside a long word such as a folder path (ImGui's TextWrapped breaks only at
// spaces): a line ends at the last space, else after the last / or \\, else at the last character that fits
void wrapAny(const std::string& text, const ImVec4* col = nullptr) {
    const float w = std::max(40.f, ImGui::GetContentRegionAvail().x);
    if (col) ImGui::PushStyleColor(ImGuiCol_Text, *col);
    size_t at = 0;
    while (at < text.size()) {
        size_t end = at, brk = std::string::npos, slash = std::string::npos;
        while (end < text.size()) {
            size_t n = end + 1;
            while (n < text.size() && ((unsigned char)text[n] & 0xC0) == 0x80) n++;   // whole UTF-8 characters
            if (end > at && ImGui::CalcTextSize(text.data() + at, text.data() + n).x > w) break;
            if (text[end] == ' ') brk = end;
            if (text[end] == '/' || text[end] == '\\') slash = n;
            end = n;
        }
        size_t cut = end, next = end;
        if (end < text.size()) {
            if (brk != std::string::npos && brk > at) { cut = brk; next = brk + 1; }
            else if (slash != std::string::npos && slash > at) cut = next = slash;
        }
        ImGui::TextUnformatted(text.data() + at, text.data() + cut);
        at = next;
    }
    if (text.empty()) ImGui::NewLine();
    if (col) ImGui::PopStyleColor();
}

// Advanced > FFmpeg libraries: the folder and version of the FFmpeg in use, and another FFmpeg installation for the next start
// (platform/ffmpeg_libs.h). A chosen folder is tried at once; when it does not work, the row says why and what to do, and OnAir keeps its own.
void ffmpegRow() {
    static const ffmpeglibs::Info now = ffmpeglibs::info();
    ffmpeglibs::Check& tried = gFfTried;
    bool& hasTried = gFfHasTried;
    const ffmpeglibs::Status& st = ffmpeglibs::status();
    const ImVec4 warn(1.f, 0.75f, 0.3f, 1);
    const std::string saved = plat::basePrefs().getS(ffmpeglibs::kPrefKey, "");
    // back to OnAir's own FFmpeg (Reset) while another one is still loaded: show OnAir's own, which the restart brings back
    static const ffmpeglibs::Info ownFf = ffmpeglibs::own();
    const ffmpeglibs::Info& shown = (saved.empty() && st.external) ? ownFf : now;
    wrapAny(std::string(tr("Folder:")) + " " + (shown.folder.empty() ? std::string(tr("OnAir's own FFmpeg")) : shown.folder));
    wrapAny(std::string(tr("Version:")) + " " + shown.version);
    if (!saved.empty())   // only with another FFmpeg chosen
        wrapAny(tr("OnAir cannot guarantee stability when another FFmpeg library is used."), &warn);
    if (ImGui::Button(tr("Choose FFmpeg folder..."))) {
        const std::string d = plat::openFolderDialog();
        if (!d.empty()) {
            tried = ffmpeglibs::check(d);
            hasTried = true;
            plat::basePrefs().setS(ffmpeglibs::kPrefKey, tried.ok ? d : "");   // a folder that does not work: back to OnAir's own FFmpeg
            plat::basePrefs().flush();
        }
    }
    const std::string problem = hasTried ? tried.problem : st.problem, solution = hasTried ? tried.solution : st.solution;
    if (!problem.empty()) {
        wrapAny(problem, &warn);
        wrapAny(std::string(tr("Possible solution:")) + " " + solution);
        wrapAny(tr("OnAir uses its own FFmpeg."));
    }
    if (ffmpegPending()) wrapAny(tr("Restart OnAir to use this choice."), &warn);   // the Restart button is under "Reset every setting"
}

bool changedNow(App& a, const Info& i) {
    if (isCustom(i)) return customChanged(a, i);
    return bindOf(i) && !isDefault(i, readVal(a, i));
}
void resetOne(App& a, const Info& i) {
    if (isCustom(i)) { customReset(a, i); return; }
    writeVal(a, i, defaultOf(i));
}

std::string settingsFilePath() { return plat::dataDir() + "/onair-settings-export.txt"; }

std::string exportAll(App& a) {
    const std::vector<Info>& t = table();
    std::vector<Val> v;
    for (const Info& i : t) v.push_back(isCustom(i) ? Val() : readVal(a, i));
    return exportText(t, v);
}
int importAll(App& a, const std::string& text) {
    const std::vector<Info>& t = table();
    std::vector<Val> v;
    for (const Info& i : t) v.push_back(isCustom(i) ? Val() : readVal(a, i));
    const int n = importText(t, v, text);
    for (size_t k = 0; k < t.size(); k++) if (!isCustom(t[k]) && bindOf(t[k])) writeVal(a, t[k], v[k]);
    savePrefs(a);
    return n;
}

struct Ui {
    char query[64] = "";
    int cat = 0;
    bool focusSearch = false;
    std::string status;
    std::map<std::string, double> pendNum;
    std::map<std::string, std::string> editText;
    std::map<std::string, bool> editActive;
    bool wasOpen = false;
};
Ui U;

void customRow(App& a, const Info& i) {
    const std::string k = i.key;
    if (k == "uiLang") languageButton(a);
    else if (k == "setupRun") { if (ImGui::Button(tr("Run setup again..."))) setupWizardOpen(a); }
    else if (k == "easyMode") {
        bool on = modepanel::easyModeOn();
        if (ImGui::Checkbox(tr("Easy mode"), &on)) modepanel::setEasyMode(on);
    } else if (k == "regionPreset") {
        const std::string cur = setup::presetOfCountry(plat::prefs().getS("locCountry", ""));
        std::string picked;
        if (regionPresetRow(cur, picked)) setupApplyPreset(a, picked, (int)plat::prefs().getI("setupUnits", 0));
    }
    else if (k == "pluginsLink") { if (ImGui::Button(tr("Open Plug-ins..."))) a.pluginsOpen = true; }
    else if (k == "automationLink") {
        if (ImGui::Button(tr("Open Automations..."))) a.automationOpen = true;
        ImGui::TextDisabled("%s", plat::prefs().getB("automationAllowRun", false) ? tr("Running programs is allowed.") : tr("Running programs is off."));
    }
    else if (k == "modeVisLink") { if (ImGui::Button(tr("Open Modes shown..."))) a.modeVisOpen = true; }
    else if (k == "themeMode") ui2::themeChoice(a, false);
    else if (k == "accent") ui2::accentMenu();
    else if (k == "uiTypography") ui2::typographyChoice(false);   // its own heading says it applies after a restart
    else if (k == "timeDisplay") ui2::timeDisplayChoice(false);
    else if (k == "recordingsFolder") {
        static char buf[1024]; static bool init = false;
        if (!init) { init = true; snprintf(buf, sizeof buf, "%s", chosenRecordingsDir().c_str()); }
        const std::string cur = recordingsDir();
        ImGui::TextWrapped("%s", cur.c_str());
        const unsigned long long fb = recdir::freeBytes(cur);
        if (fb != UINT64_MAX) ImGui::TextDisabled("%s %.1f GB", tr("Free disk space:"), (double)fb / 1e9);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##recdir", tr("Default (inside the data folder)"), buf, sizeof buf);
        if (ImGui::Button(tr("Choose..."))) { const std::string d = plat::openFolderDialog(); if (!d.empty()) { snprintf(buf, sizeof buf, "%s", d.c_str()); setRecordingsDir(d); } }
        ImGui::SameLine();
        if (ImGui::Button(tr("Use typed path"))) setRecordingsDir(buf);
        ImGui::SameLine();
        if (ImGui::Button(tr("Reset to default"))) { buf[0] = 0; setRecordingsDir(""); }
        ImGui::SameLine();
        if (ImGui::Button(tr("Open"))) revealFolder(recordingsDir());
        if (!chosenRecordingsDir().empty() && recdir::normalise(chosenRecordingsDir()) != cur) ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1), "%s", tr("That folder cannot be written: the default is used."));
    }
    else if (k == ffmpeglibs::kPrefKey) ffmpegRow();
    else if (k == "dataFolder") {
        const std::string d = plat::dataDir();
        ImGui::TextWrapped("%s", d.c_str());
        if (ImGui::Button(tr("Open folder"))) dect2::openUrl(d);
        ImGui::SameLine();
        if (ImGui::Button(tr("Copy path"))) ImGui::SetClipboardText(d.c_str());
    } else if (k == "privacyInfo") {
        ImGui::TextWrapped("%s", tr(i.tip));
    } else if (k == "saveLog") {
        if (ImGui::Button(tr("Save log"))) {
            size_t n = 0;
            const std::vector<std::string> lines = a.engine.logSnapshot(n);
            const std::string path = plat::dataDir() + "/onair-log.txt";
            std::ofstream f(dect2::u8file(path));
            for (const std::string& l : lines) f << l << "\n";
            U.status = f ? std::string(tr("Saved ")) + path : std::string(tr("Could not write ")) + path;
        }
    } else if (k == "exportImport") {
        const std::string path = settingsFilePath();
        if (ImGui::Button(tr("Export"))) {
            std::ofstream f(dect2::u8file(path));
            f << exportAll(a);
            U.status = f ? std::string(tr("Exported to ")) + path : std::string(tr("Could not write ")) + path;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Import"))) {
            std::ifstream f(dect2::u8file(path));
            std::stringstream ss; ss << f.rdbuf();
            if (!f) U.status = std::string(tr("No file at ")) + path;
            else { char b[96]; snprintf(b, sizeof b, tr("Imported %d settings"), importAll(a, ss.str())); U.status = b; }
        }
        ImGui::TextDisabled("%s", path.c_str());
    } else if (k == "resetAll") {
        static bool sure = false;
        if (!sure) { if (ImGui::Button(tr("Reset every setting..."))) sure = true; }
        else {
            ImGui::TextWrapped("%s", tr("Every setting in this window goes back to its default. Continue?"));
            if (ImGui::Button(tr("Yes, reset all"))) { for (const Info& x : table()) resetOne(a, x); savePrefs(a); sure = false; U.status = tr("All settings reset");
                if (ffmpegPending()) U.status += std::string(" ") + tr("Restart OnAir to use its own FFmpeg."); }
            ImGui::SameLine();
            if (ImGui::Button(tr("Cancel"))) sure = false;
        }
        if (!sure && ffmpegPending()) ffmpegRestartButton();   // an FFmpeg choice (or its reset) applies after a restart
    }
}

// one row's control; true when the value changed
void control(App& a, const Info& i) {
    const std::string k = i.key;
    Val cur = readVal(a, i);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (i.kind == Kind::Bool) {
        bool v = cur.d != 0;
        if (ImGui::Checkbox("##v1", &v)) { cur.d = v; writeVal(a, i, cur); }
    } else if (i.kind == Kind::Choice) {
        const std::vector<std::string> items = split(i.choices ? i.choices : "", '|');
        const int sel = std::max(0, std::min((int)items.size() - 1, choiceIndex(i, cur.d)));
        if (ImGui::BeginCombo("##v2", items.empty() ? "" : tr(items[(size_t)sel].c_str()))) {
            for (int n = 0; n < (int)items.size(); n++)
                if (ImGui::Selectable(tr(items[(size_t)n].c_str()), n == sel)) { cur.d = choiceValue(i, n); writeVal(a, i, cur); }
            ImGui::EndCombo();
        }
    } else if (i.kind == Kind::Int) {
        int v = (int)cur.d;
        if (i.hi >= i.lo && i.hi - i.lo <= 2000) {
            if (ImGui::SliderInt("##v3", &v, (int)i.lo, (int)i.hi)) { cur.d = v; writeVal(a, i, cur); }
        } else {
            if (ImGui::InputInt("##v4", &v, 0, 0)) U.pendNum[k] = v;
            if (ImGui::IsItemDeactivatedAfterEdit()) { cur.d = U.pendNum[k]; writeVal(a, i, cur); }
        }
    } else if (i.kind == Kind::Float) {
        const bool typed = i.fmt[0] == '!';
        const char* fmt = typed ? i.fmt + 1 : i.fmt;
        if (!typed && i.hi > i.lo) {
            float v = (float)cur.d;
            if (ImGui::SliderFloat("##v5", &v, (float)i.lo, (float)i.hi, fmt)) { cur.d = v; writeVal(a, i, cur); }
        } else {
            double v = cur.d;
            if (ImGui::InputDouble("##v6", &v, 0, 0, fmt)) U.pendNum[k] = v;
            if (ImGui::IsItemDeactivatedAfterEdit()) { cur.d = U.pendNum[k]; writeVal(a, i, cur); }
        }
    } else if (i.kind == Kind::Text) {
        bool& act = U.editActive[k];
        std::string& es = U.editText[k];
        if (!act) es = cur.s;
        char b[512];
        snprintf(b, sizeof b, "%s", es.c_str());
        if (ImGui::InputText("##v7", b, sizeof b)) es = b;
        act = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) { cur.s = es; writeVal(a, i, cur); }
    } else customRow(a, i);
}

void rows(App& a, const std::vector<size_t>& idx, bool showCat) {
    const std::vector<Info>& t = table();
    if (idx.empty()) return;
    if (!ImGui::BeginTable("##rows", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) return;
    ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthStretch, 0.46f);
    ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthFixed, 78.f * gUi);
    for (size_t n : idx) {
        const Info& i = t[n];
        ImGui::PushID(i.key);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        const bool ch = changedNow(a, i);
        if (ch) { ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1), "*"); ImGui::SameLine(0, 4 * gUi); }
        ImGui::TextWrapped("%s", tr(i.label));
        if (i.tip[0] && i.kind != Kind::Custom) { if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(i.tip)); }
        if (!strcmp(i.key, ffmpeglibs::kPrefKey)) {   // FFmpeg libraries: what it does, in a (?)
            ImGui::SameLine(0, 4 * gUi);
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.f);
                ImGui::TextUnformatted(tr("Use the FFmpeg libraries of another FFmpeg installation instead of the FFmpeg that comes with OnAir, "
                                          "for example one with the H.264 and H.265 encoders (libx264, libx265)."));
                ImGui::Spacing();
                ImGui::TextUnformatted(tr("Choose the folder of that installation: OnAir finds the libraries in it or in its bin or lib folder. "
                                          "It must be a shared FFmpeg build with the same major version as OnAir's FFmpeg and not older."));
                ImGui::Spacing();
                ImGui::TextUnformatted(tr("OnAir tries the libraries before it uses them. When they do not work, it says why, "
                                          "gives a possible solution and keeps its own FFmpeg."));
                ImGui::Spacing();
                ImGui::TextUnformatted(tr("FFmpeg is loaded when OnAir starts: a new choice applies after Restart OnAir (under Reset every setting). "
                                          "Reset goes back to OnAir's own FFmpeg."));
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        if (showCat) ImGui::TextDisabled("%s%s", tr(i.cat), i.advanced ? " / Advanced" : "");
        if (i.restart) ImGui::TextDisabled("%s", tr("restart needed"));
        ImGui::TableSetColumnIndex(1);
        control(a, i);
        ImGui::TableSetColumnIndex(2);
        if (ch && (!isCustom(i) || !strcmp(i.key, "themeMode") || !strcmp(i.key, "uiLang") || !strcmp(i.key, "uiTypography") || !strcmp(i.key, ffmpeglibs::kPrefKey)))
            if (ImGui::SmallButton(tr("Reset"))) resetOne(a, i);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

}   // namespace

bool& settingsOpen() { static bool open = false; return open; }

void settingsWindow(App& a) {
    ImGuiIO& io = ImGui::GetIO();
    {   // dev / test only: DECT2_SETTINGS="category index[:search text]" opens the window at start (hidden --shot runs)
        static bool env = false;
        if (!env) { env = true; if (const char* e = getenv("DECT2_SETTINGS")) { settingsOpen() = true; U.cat = atoi(e); if (const char* c = strchr(e, ':')) snprintf(U.query, sizeof U.query, "%s", c + 1); } }
    }
    if (!io.WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Comma)) settingsOpen() = !settingsOpen();
    if (!settingsOpen()) { U.wasOpen = false; return; }
    static bool once = (settingsUnboundRows(), true); (void)once;   // lists on stderr what has no field
    if (!U.wasOpen) { U.wasOpen = true; U.focusSearch = true; U.status.clear(); }
    const std::vector<Info>& t = table();
    if (getenv("DECT2_SETTINGS_DEBUG")) for (const Info& i : t) if (!isCustom(i) && bindOf(i) && changedNow(a, i)) { const Val v = readVal(a, i); fprintf(stderr, "settings: %s differs: %g / %s (default %g)\n", i.key, v.d, v.s.c_str(), i.def); }
    static const std::vector<std::string> cats = categories(t);
    firstWindowSize(880 * gUi, 600 * gUi);
    if (!ImGui::Begin(trw("Settings##settingswin"), &settingsOpen())) { ImGui::End(); return; }
    if (a.touchUi) ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * gUi, 12 * gUi));
    if (U.focusSearch) { ImGui::SetKeyboardFocusHere(); U.focusSearch = false; }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##setsearch", tr("Search all settings"), U.query, sizeof U.query);
    const bool searching = U.query[0] != 0;
    const float footer = ImGui::GetTextLineHeightWithSpacing() * 2 + 8 * gUi;
    ImGui::BeginChild("##setcats", ImVec2(200 * gUi, -footer), ImGuiChildFlags_Borders);
    for (int c = 0; c < (int)cats.size(); c++) {
        // only categories that have a row we can show
        int shown = 0;
        for (const Info& i : t) if (cats[(size_t)c] == i.cat && (isCustom(i) || bindOf(i))) shown++;
        if (!shown) continue;
        char l[96];
        const int n = [&] { int x = 0; for (const Info& i : t) if (cats[(size_t)c] == i.cat && changedNow(a, i)) x++; return x; }();
        if (n) snprintf(l, sizeof l, "%s  (%d)##cat%d", tr(cats[(size_t)c].c_str()), n, c);
        else snprintf(l, sizeof l, "%s##cat%d", tr(cats[(size_t)c].c_str()), c);
        if (ImGui::Selectable(l, !searching && U.cat == c, 0, ImVec2(0, a.touchUi ? 40 * gUi : 0))) { U.cat = c; U.query[0] = 0; }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##setbody", ImVec2(0, -footer));
    const std::string catName = cats[(size_t)std::max(0, std::min((int)cats.size() - 1, U.cat))];
    std::vector<size_t> all = filter(t, U.query, catName);
    std::vector<size_t> plain, adv;
    for (size_t n : all) {
        const Info& i = t[n];
        if (!isCustom(i) && !bindOf(i)) continue;   // no field behind it: not shown
        (i.advanced && !searching ? adv : plain).push_back(n);
    }
    if (searching) {
        ImGui::TextDisabled("%zu %s", plain.size(), tr("matching settings"));
        if (plain.empty()) ImGui::TextWrapped("%s", tr("Nothing matches. Try another word, e.g. colour, update, audio."));
    } else {
        ImGui::TextUnformatted(tr(catName.c_str()));
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Reset category"))) for (const Info& i : t) if (catName == i.cat) resetOne(a, i);
        ImGui::Separator();
    }
    rows(a, plain, searching);
    if (!adv.empty() && ImGui::CollapsingHeader(trw("Advanced##adv"))) rows(a, adv, false);
    ImGui::EndChild();
    ImGui::Separator();
    if (!U.status.empty()) ImGui::TextWrapped("%s", U.status.c_str());
    else ImGui::TextDisabled("%s", tr("* marks a setting that differs from its default. Changes apply at once and are saved."));
    if (a.touchUi) ImGui::PopStyleVar();
    ImGui::End();
}

// for the self-test of the shot runs: rows whose field is missing (they are hidden), listed on stderr
int settingsUnboundRows() {
    int n = 0;
    for (const Info& i : table()) if (!isCustom(i) && !bindOf(i)) { fprintf(stderr, "settings: no field for %s\n", i.key); n++; }
    return n;
}
