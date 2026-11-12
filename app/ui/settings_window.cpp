// Settings (menu bar > Settings, Ctrl+,): one window with every preference gathered in categories, a search across all of them, a reset per
// setting and per category, a marker for what differs from the default, and an Advanced foldout per category. The rows are in
// settings_table.h; here each row is bound to the field that holds its value (the same fields the old menus use, saved by
// app/state/prefs.cpp), so a row without a field is not shown. Docs: docs/SETTINGS.md.
#include "dect2/audio/dolby_audio.h"
#include "../app.h"
#include "../i18n/i18n.h"
#include "../platform/platform.h"
#include "../ui/scale.h"
#include "settings_table.h"
#include "settings_profile.h"
#include "input_map.h"
#include "dect2/audio/audio_control.h"
#include "dect2/audio/audioout.h"
#include "dect2/source/iq_record.h"
#include "setup_logic.h"
#include "kit/mode_panel.h"
#include "../codec/speech_codec_ui.h"
#include "ui2_internal.h"
#include "ui_preview.h"
#include "dect2/common/u8file.h"
#include "dect2/engine/updater.h"
#include "dect2/limits.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <type_traits>
#include <filesystem>

using namespace settings;
namespace adsbui { void unitsFromPrefs(); }   // adsb_ui.cpp: the aircraft units again from the preferences (Settings > Units)

// Choice items are translated one by one (tr(items[n])); the ones no other tr() has are listed here for the catalog check
[[maybe_unused]] static const char* const kChoiceItemTexts[] = {N_("Automatic (the device's speakers)"), N_("Stereo, Dolby Surround (Lt/Rt)"),
    // 4.0 rows
    N_("The last frequency"), N_("A fixed frequency"), N_("Ask"), N_("Always restore"), N_("Never offer"), N_("Never"), N_("While receiving or recording"), N_("Always"), N_("Aviation (kt, ft, nm)"), N_("Every frame"), N_("60 rows/s"), N_("10 rows/s"), N_("5 rows/s"), N_("2 rows/s"), N_("1 row/s"), N_("1 row per 10 s"), N_("1 row per minute"), N_("Soft white"), N_("Amber"), N_("Green"), N_("Cyan"), N_("Yellow"), N_("Orange (CB safe)"), N_("Sky blue (CB safe)"), N_("Pink"), N_("Light blue"), N_("Pale cyan"), N_("Pale steel"), N_("Grey (default)"), N_("As the look"), N_("Offsets from the tuned frequency"), N_("Frequencies"), N_("Fast"), N_("Slow"), N_("Speech to noise"), N_("Full (15 kHz)"), N_("Level"), N_("Off"), N_("Automatic")};

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

// the "remove after" times (Settings > Decoding) go to the trackers in core (dect2/limits.h)
void applyStaleNow() { applyStaleFromPrefs(); }
#define PS(k, def) m[k] = Bind{[](App&) -> double { return plat::prefs().getD(k, def); }, [](App&, double v) { plat::prefs().setD(k, v); applyStaleNow(); }, nullptr, nullptr}

// 4.0: settings kept only in the preferences (the code that uses them reads the key): bool, int, double, text, with an optional action after a change
#define SW_PB(k, def) m[k] = Bind{[](App&) -> double { return plat::prefs().getB(k, def) ? 1.0 : 0.0; }, [](App&, double v) { plat::prefs().setB(k, v != 0); }, nullptr, nullptr}
#define SW_PI(k, def) m[k] = Bind{[](App&) -> double { return (double)plat::prefs().getI(k, def); }, [](App&, double v) { plat::prefs().setI(k, (long)v); }, nullptr, nullptr}
#define SW_PIA(k, def, after) m[k] = Bind{[](App&) -> double { return (double)plat::prefs().getI(k, def); }, [](App& a, double v) { plat::prefs().setI(k, (long)v); after; (void)a; }, nullptr, nullptr}
#define SW_PF(k, def) m[k] = Bind{[](App&) -> double { return plat::prefs().getD(k, def); }, [](App&, double v) { plat::prefs().setD(k, v); }, nullptr, nullptr}
#define SW_PT(k) m[k] = Bind{nullptr, nullptr, [](App&) -> std::string { return plat::prefs().getS(k, ""); }, [](App&, const std::string& v) { plat::prefs().setS(k, v); }}
#define SW_PTA(k, after) m[k] = Bind{nullptr, nullptr, [](App&) -> std::string { return plat::prefs().getS(k, ""); }, [](App& a, const std::string& v) { plat::prefs().setS(k, v); after; (void)a; }}
// a field of App with an action after a change (the engine is told)
#define SW_NBA(k, expr, after) m[k] = Bind{[](App& a) -> double { return (double)(expr); }, [](App& a, double v) { using T = std::remove_reference_t<decltype(expr)>; expr = (T)v; after; }, nullptr, nullptr}
// a switch the engine keeps behind a getter and a setter
#define SW_FB(k, get, set) m[k] = Bind{[](App& a) -> double { return (get) ? 1.0 : 0.0; }, [](App& a, double v) { const bool on = v != 0; set; (void)on; }, nullptr, nullptr}

const std::map<std::string, Bind>& binds() {
    static const std::map<std::string, Bind> mm = [] {
        std::map<std::string, Bind> m;
        NB("powerLowMode", a.powerLowMode); NB("powerHidden", a.powerHidden); NB("reduceCpuHidden", a.reduceCpuHidden);
        NB("discordOn", a.discordOn); NB("discordFreq", a.discordFreq);
        NB("uiTheme", a.uiTheme); NB("uiVariant", a.uiVariant); NB("layoutUnified", a.layoutUnified); NB("uiTextScale", a.uiTextScale);
        NB("touchUi", a.touchUi); NB("touchScale", a.touchScale); NB("perfOverlay", a.perfOverlay);
        NB("wfMap3", a.wf.map); NB("wfAutoFloor", a.wf.autoTrack); NB("wfGamma", a.wf.gamma); NB("wfBright", a.wf.brightness); NB("wfSat", a.wf.sat);
        NB("wfContrast", a.wf.contrast); NB("wfClock", a.wf.clockLabels); NB("wfAvg", a.wf.rowAverage); NB("wfLowPass", a.wfLowPass);
        NB("specAutoDb", a.specAutoDb); NB("specPerHz", a.specPerHz); NB("specDbm", a.specDbm); NB("specFill", a.specFill);
        NB("specGrid", a.specGrid); NB("specPeaks", a.specPeaks); NB("specReadout", a.specReadout); NB("specLine", a.specLine);
        NB("specAvgSec", a.specAvgSec); NB("holdDecay", a.holdDecay);
        NB("tuningMode", a.tuningMode); NB("centreTuning", a.centreTuning); NB("dragMovesTuned", a.dragMovesTuned); NB("specDrag", a.specDrag);
        NB("specSnapHz", a.specSnapHz); NB("bwAuto", a.bwAuto); NB("gpuGlobal", a.gpuGlobal);
        NB("popTop", a.popTop); NB("subsOn", a.subsOn);
        // TV sound (dolby_audio.h): straight into the player's configuration, and the playing sound is opened again
        m["surroundLayout"] = Bind{[](App&) -> double { return (double)plat::prefs().getI("surroundLayout", 0); },
                                   [](App& a, double v) { plat::prefs().setI("surroundLayout", (long)v); dect2::DolbyOutputConfig::global().layout = (int)v; a.engine.player().reopenAudio(); }, nullptr, nullptr};
        m["dolbyPassthrough"] = Bind{[](App&) -> double { return plat::prefs().getB("dolbyPassthrough", false) ? 1.0 : 0.0; },
                                     [](App& a, double v) { plat::prefs().setB("dolbyPassthrough", v != 0); dect2::DolbyOutputConfig::global().passthrough = v != 0; a.engine.player().reopenAudio(); }, nullptr, nullptr};
        m["iecDevice"] = Bind{nullptr, nullptr, [](App&) -> std::string { return plat::prefs().getS("iecDevice", ""); },
                              [](App& a, const std::string& v) { plat::prefs().setS("iecDevice", v); dect2::DolbyOutputConfig::global().setDevice(v); a.engine.player().reopenAudio(); }};
        m["recFormat"] = Bind{[](App& a) -> double { return a.recFormat; }, [](App& a, double v) { a.recFormat = (int)v; plat::prefs().setI("recFormat", (long)v); }, nullptr, nullptr};
        NB("fmMono", a.fmMono); NB("fmStereo", a.fmStereo); NB("fmDeemph", a.fmDeemph); NB("fmRegion", a.fmRegion);
        NB("fmSquelch", a.fmSquelch); NB("ttxPage", a.ttxPage); SB("tvLang", a.tvLang);
        PD("adsbLat"); PD("adsbLon");
        NB("updCheck", a.updCheck); NB("updBeta", a.updPre); NB("updAuto", a.updAuto); NB("updSrc", a.updSrc);
        NB("updMirrorOn", a.updMirrorOn); SB("updMirror", a.updMirror);
        PS("staleVdl2", -1); PS("staleAcars", -1); PS("staleHfdl", -1); PS("staleAdsb", -1); PS("staleAis", -1); PS("staleUat", -1); PS("staleInmsat", -1); PS("staleCustomMin", 120);
        SB("udpHost", std::string(a.udpHost)); NB("udpPort", a.out.port); NB("udpRtp", a.out.rtp);
        // ---- 4.0
        SW_PI("startFreqMode", 0); SW_PF("startFreqMhz", 100); SW_PB("autoStartRx", false); SW_PI("sessionRestore", 0); SW_PI("confirmExit", 0);
        SW_PIA("adsbUnits", 0, adsbui::unitsFromPrefs()); SW_PIA("adsbSpeedUnit", 0, adsbui::unitsFromPrefs());
        NB("specFft", a.specFft);
        m["wfRate"] = Bind{[](App& a) -> double { return a.wf.rowsPerSec; }, [](App& a, double v) { a.wf.setRate((float)v); }, nullptr, nullptr};
        NB("specHold", a.peakHold); NB("minHold", a.minHold); NB("specNfMarker", a.specNfMarker); NB("specLabels", a.specLabels);
        NB("traceColor", a.traceColor); NB("holdColor", a.holdColor); NB("specSmoothTrace", a.specSmoothTrace); NB("specAxisLabels", a.specAxisLabels);
        NB("specLabelSize", a.specLabelSize); NB("specSmoothZoom", a.specSmoothZoom); NB("wfFloorBelow", a.wf.autoBelow); NB("wfFloorSpan", a.wf.autoSpan);
        SW_PF("specDefLo", -110); SW_PF("specDefHi", -30); SW_PF("dbKeyStep", 5);
        SW_NBA("agcOn", a.agcOn, if (a.agcOn) agcReset(a));
        SW_FB("dcRemove", a.engine.iqFix().dc.load(), a.engine.iqFix().dc = on); SW_FB("iqCorrect", a.engine.iqFix().iq.load(), a.engine.iqFix().iq = on);
        SW_FB("offsetTune", a.engine.autoOffset(), a.engine.setAutoOffset(on)); SW_FB("spurNotch", a.engine.spurNotch(), a.engine.setSpurNotch(on));
        SW_FB("smoothGaps", a.engine.player().conceal(), a.engine.player().setConceal(on));
        NB("nbOn", a.nbOn); NB("nbThr", a.nbThr); NB("nbWidthUs", a.nbWidthUs);
        SW_PI("aclDefAgc", 0); SW_PI("aclDefSq", 0); SW_PF("aclDefSqDb", -50);
        NB("fmBass", a.fmBass); NB("fmTreble", a.fmTreble); NB("fmWarm", a.fmWarm); NB("fmAudioBw", a.fmAudioBw); NB("audioRate", a.audioRate);
        SW_PTA("recNamePattern", dect2::setRecordingNamePattern(plat::prefs().getS("recNamePattern", ""))); SW_PI("recSplitMb", 0); SW_PI("recMinFreeMb", 200);
        SW_NBA("recSigmf", a.recSigmf, a.engine.setRecordSigmf(a.recSigmf));
        NB("audioRecFmt", a.audioRecFmt); NB("audioRecGate", a.audioRecGate); NB("audioRecSplit", a.audioRecSplit); NB("audioRecKeep", a.audioRecKeep);
        SW_NBA("tvBufMb", a.tvBufMb, { a.engine.player().setBufferMB(a.tvBufMb); a.tvBufMb = a.engine.player().bufferMB(); });
        SW_PB("notifySystem", true); SW_PB("notifyBanner", true); SW_PI("notifyBannerSec", 10); SW_PB("notifySound", true); SW_PB("notifyQuiet", false); SW_PI("notifyQuietFrom", 1320); SW_PI("notifyQuietTo", 420);
        SW_PT("mapTileUrl"); SW_PI("mapCapMb", 500); SW_PB("mapKeepTiles", true); SW_PB("adsbMap", true);
        SW_NBA("compute", a.computeMode, a.engine.setComputeMode(a.computeMode));
        NB("verboseLog", a.verboseLog);
        m["uiPreviews"] = Bind{[](App&) -> double { return uiprev::enabled() ? 1.0 : 0.0; }, [](App&, double v) { uiprev::enabled() = v != 0; plat::prefs().setB("uiPreviews", v != 0); }, nullptr, nullptr};
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
    if (!strcmp(i.key, "gpuGlobal")) applyGpuPolicy(a);
    savePrefsSoon(a);
}

// the three custom rows that have a default to go back to
bool customChanged(const App& a, const Info& i) {
    if (!strcmp(i.key, "themeMode")) return a.lightUi || a.highContrast;
    if (!strcmp(i.key, "uiLang")) return !a.uiLang.empty();
    if (!strcmp(i.key, "uiTypography")) return plat::prefs().getI("uiTypography", 0) != 0;
    if (!strcmp(i.key, "startMode")) return plat::prefs().getI("startFamily", -1) >= 0;
    if (!strcmp(i.key, "audioDevDefault")) return !plat::prefs().getS("audioDevDefault", "").empty();
    if (!strcmp(i.key, "keysEditor")) return !inputmap::settings().isDefault();
    return false;
}
void customReset(App& a, const Info& i) {
    if (!strcmp(i.key, "themeMode")) { a.lightUi = false; a.highContrast = false; savePrefs(a); }
    else if (!strcmp(i.key, "uiLang")) setUiLanguage(a, "");
    else if (!strcmp(i.key, "uiTypography")) plat::prefs().setI("uiTypography", 0);
    else if (!strcmp(i.key, "startMode")) { plat::prefs().setI("startFamily", -1); plat::prefs().setS("startSub", ""); }
    else if (!strcmp(i.key, "audioDevDefault")) { plat::prefs().setS("audioDevDefault", ""); audioDefaultDeviceChanged(); }
    else if (!strcmp(i.key, "keysEditor")) { inputmap::settings().reset(); inputmap::save(); }
}
bool resettable(const Info& i) {
    if (!isCustom(i)) return true;
    for (const char* k : {"themeMode", "uiLang", "uiTypography", "startMode", "audioDevDefault", "keysEditor"}) if (!strcmp(i.key, k)) return true;
    return false;
}

bool changedNow(App& a, const Info& i) {
    if (isCustom(i)) return customChanged(a, i);
    return bindOf(i) && !isDefault(i, readVal(a, i));
}
void resetOne(App& a, const Info& i) {
    if (isCustom(i)) { customReset(a, i); return; }
    writeVal(a, i, defaultOf(i));
}

// the rows whose choices show a hover preview of the look (ui_preview.h); -1: none
int previewOpt(const char* k) {
    using O = uiprev::Opt;
    if (!strcmp(k, "uiTheme")) return (int)O::Theme;
    if (!strcmp(k, "uiVariant")) return (int)O::Variant;
    if (!strcmp(k, "layoutUnified")) return (int)O::Unified;
    if (!strcmp(k, "touchUi")) return (int)O::Touch;
    if (!strcmp(k, "wfMap3")) return (int)O::WfMap;
    return -1;
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
    char profName[64] = "";
    int profSel = -1;
    bool profDelAsk = false;
};
Ui U;
int catIndex(const char* name) {
    const std::vector<std::string> c = categories(table());
    for (size_t k = 0; k < c.size(); k++) if (c[k] == name) return (int)k;
    return 0;
}

// ---- profiles: <data folder>/profiles/<name>.onairprofile (settings_profile.h)
std::string profileDir() { return plat::dataDir() + "/profiles"; }
std::filesystem::path u8p(const std::string& p) { return std::filesystem::path(reinterpret_cast<const char8_t*>(p.c_str())); }   // a UTF-8 path
std::string utf8(const std::filesystem::path& p) { const auto u = p.u8string(); return std::string(u.begin(), u.end()); }
std::vector<std::string> profileNames() {
    std::vector<std::string> v;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(u8p(profileDir()), ec)) {
        const std::string n = profileNameOf(utf8(e.path().filename()));
        if (!n.empty()) v.push_back(n);
    }
    std::sort(v.begin(), v.end());
    return v;
}
std::string profileText(App& a) {
    const std::string e = exportAll(a);
    const size_t nl = e.find('\n');   // the "# OnAir settings" line: the profile has its own header
    return profileCompose(nl == std::string::npos ? e : e.substr(nl + 1), inputmap::exportText(inputmap::settings()));
}
bool writeFile(const std::string& path, const std::string& text) { std::ofstream f(dect2::u8file(path)); f << text; return (bool)f; }
bool readFile(const std::string& path, std::string& text) { std::ifstream f(dect2::u8file(path)); if (!f) return false; std::stringstream ss; ss << f.rdbuf(); text = ss.str(); return true; }
// apply a profile's text: the settings, then the keys when the file has them; the number of settings read (-1: not a profile)
int applyProfile(App& a, const std::string& text) {
    std::string st, keys;
    if (!profileSplit(text, st, keys)) return -1;
    const int n = importAll(a, st);
    if (!keys.empty()) { inputmap::Settings k; inputmap::importText(k, keys); inputmap::settings() = k; inputmap::save(); }
    return n;
}
void profilesRow(App& a) {
    ImGui::PushID("profiles");
    const std::vector<std::string> names = profileNames();
    const std::string cur = plat::prefs().getS("settingsProfile", "");
    ImGui::TextDisabled("%s %s", tr("Current profile:"), cur.empty() ? tr("(none)") : cur.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##prof", U.profSel >= 0 && U.profSel < (int)names.size() ? names[(size_t)U.profSel].c_str() : tr("Pick a saved profile"))) {
        for (int n = 0; n < (int)names.size(); n++) if (ImGui::Selectable(names[(size_t)n].c_str(), n == U.profSel)) { U.profSel = n; snprintf(U.profName, sizeof U.profName, "%s", names[(size_t)n].c_str()); U.profDelAsk = false; }
        ImGui::EndCombo();
    }
    const bool picked = U.profSel >= 0 && U.profSel < (int)names.size();
    ImGui::BeginDisabled(!picked);
    if (ImGui::Button(tr("Switch to it"))) {
        std::string text;
        if (readFile(profileDir() + "/" + names[(size_t)U.profSel] + kProfileExt, text)) {
            const int n = applyProfile(a, text);
            char b[160]; snprintf(b, sizeof b, tr("Profile %s: %d settings"), names[(size_t)U.profSel].c_str(), n);
            U.status = b;
            plat::prefs().setS("settingsProfile", names[(size_t)U.profSel]);
            savePrefs(a);
        } else U.status = tr("Could not read that profile");
    }
    ImGui::SameLine();
    if (!U.profDelAsk) { if (ImGui::Button(tr("Delete..."))) U.profDelAsk = true; }
    else {
        if (ImGui::Button(tr("Yes, delete it"))) {
            std::error_code ec;
            std::filesystem::remove(u8p(profileDir() + "/" + names[(size_t)U.profSel] + kProfileExt), ec);
            if (cur == names[(size_t)U.profSel]) plat::prefs().setS("settingsProfile", "");
            U.status = ec ? tr("Could not delete it") : tr("Profile deleted");
            U.profSel = -1; U.profDelAsk = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) U.profDelAsk = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Export..."))) {
        const std::string name = names[(size_t)U.profSel];
        std::string text;
        const std::string p = plat::saveFileDialog((name + kProfileExt).c_str());
        if (!p.empty() && readFile(profileDir() + "/" + name + kProfileExt, text)) U.status = writeFile(p, text) ? std::string(tr("Exported to ")) + p : std::string(tr("Could not write ")) + p;
    }
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(200 * gUi);
    ImGui::InputTextWithHint("##profname", tr("Name, e.g. Night"), U.profName, sizeof U.profName);
    ImGui::SameLine();
    const std::string file = profileFileName(U.profName);
    ImGui::BeginDisabled(file.empty());
    if (ImGui::Button(tr("Save the current settings as this profile"))) {
        std::error_code ec;
        std::filesystem::create_directories(u8p(profileDir()), ec);
        const std::string path = profileDir() + "/" + file;
        if (writeFile(path, profileText(a))) { U.status = std::string(tr("Saved ")) + path; plat::prefs().setS("settingsProfile", profileNameOf(file)); }
        else U.status = std::string(tr("Could not write ")) + path;
    }
    ImGui::EndDisabled();
    if (ImGui::Button(tr("Import a profile file..."))) {
        const std::string p = plat::openFileDialog();
        std::string text;
        if (p.empty()) {}
        else if (!readFile(p, text)) U.status = std::string(tr("No file at ")) + p;
        else {
            std::string st, keys;
            if (!profileSplit(text, st, keys)) U.status = tr("That file is not a settings profile");
            else {
                std::string base = utf8(u8p(p).stem());
                const std::string f = profileFileName(base.empty() ? "Imported" : base);
                std::error_code ec;
                std::filesystem::create_directories(u8p(profileDir()), ec);
                if (!f.empty() && writeFile(profileDir() + "/" + f, text)) { U.status = std::string(tr("Imported as profile ")) + profileNameOf(f) + tr(" (Switch to it to use it)"); snprintf(U.profName, sizeof U.profName, "%s", profileNameOf(f).c_str()); }
                else U.status = tr("Could not store the profile");
            }
        }
    }
    ImGui::TextDisabled("%s", profileDir().c_str());
    ImGui::PopID();
}

// the start-up mode: the one of the last run (default) or one picked here (prefs startFamily, startSub)
void startModeRow() {
    const int fam = (int)plat::prefs().getI("startFamily", -1);
    const std::string sub = plat::prefs().getS("startSub", "");
    const char* curName = tr("The mode of the last run");
    for (const auto& m : allModes()) if (m.family == fam && (m.subId ? sub == m.subId : sub.empty())) curName = m.name;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##startmode", curName, ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable(tr("The mode of the last run"), fam < 0)) { plat::prefs().setI("startFamily", -1); plat::prefs().setS("startSub", ""); }
        static char find[48] = "";
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##smfind", tr("Search modes"), find, sizeof find);
        for (const auto& m : allModes()) {
            if (find[0] && lower(m.name).find(lower(find)) == std::string::npos) continue;
            ImGui::PushID(&m);
            if (ImGui::Selectable(m.name, m.family == fam && (m.subId ? sub == m.subId : sub.empty()))) { plat::prefs().setI("startFamily", m.family); plat::prefs().setS("startSub", m.subId ? m.subId : ""); }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

// the output device for the modes without their own (prefs audioDevDefault; audio_control_ui.cpp uses it)
void audioDeviceRow() {
    static std::vector<std::string> devs;
    static bool listed = false;
    if (!listed) { listed = true; devs = dect2::AudioOut::devices(); }
    const std::string cur = plat::prefs().getS("audioDevDefault", "");
    ImGui::SetNextItemWidth(-FLT_MIN - 70 * gUi);
    if (ImGui::BeginCombo("##adev", cur.empty() ? tr("The system's default") : cur.c_str())) {
        if (ImGui::Selectable(tr("The system's default"), cur.empty())) { plat::prefs().setS("audioDevDefault", ""); audioDefaultDeviceChanged(); }
        for (const std::string& d : devs) if (ImGui::Selectable(d.c_str(), d == cur)) { plat::prefs().setS("audioDevDefault", d); audioDefaultDeviceChanged(); }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Refresh"))) devs = dect2::AudioOut::devices();
    if (!cur.empty() && std::find(devs.begin(), devs.end(), cur) == devs.end()) ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1), "%s", tr("That device is not connected: the system's default is used."));
}

void customRow(App& a, const Info& i) {
    const std::string k = i.key;
    if (k == "uiLang") languageButton(a);
    else if (k == "setupRun") { if (ImGui::Button(tr("Run setup again..."))) setupWizardOpen(a); }
    else if (k == "easyMode") {
        bool on = modepanel::easyModeOn();
        if (ImGui::Checkbox(tr("Easy mode"), &on)) modepanel::setEasyMode(on);
        uiprev::item(a, uiprev::Opt::Easy, on ? 0 : 1);
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
    else if (k == "gpuModes") gpuSettingsRow(a);
    else if (k == "modeVisLink") { if (ImGui::Button(tr("Open Modes shown..."))) a.modeVisOpen = true; }
    else if (k == "themeMode") ui2::themeChoice(a, false);
    else if (k == "accent") ui2::accentMenu();
    else if (k == "restoreView") { if (ImGui::Button(tr("Restore the default view"))) ui2::restoreDefaultView(a); }
    else if (k == "uiTypography") ui2::typographyChoice(false, &a);   // its own heading says it applies after a restart
    else if (k == "timeDisplay") ui2::timeDisplayChoice(false);
    else if (k == "startMode") startModeRow();
    else if (k == "audioDevDefault") audioDeviceRow();
    else if (k == "keysEditor") {   // the editor is drawn over the page's whole width (settingsWindow); in a search this row leads there
        if (ImGui::Button(tr("Open the key editor"))) { U.cat = catIndex(kKeys); U.query[0] = 0; }
    }
    else if (k == "profiles") profilesRow(a);
    else if (k == "recordingsFolder") {
        static char buf[1024]; static bool init = false;
        if (!init) { init = true; snprintf(buf, sizeof buf, "%s", chosenRecordingsDir().c_str()); }
        ImGui::PushID("recdirBlock");
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
        ImGui::PopID();
    }
    else if (k == "optLibs") appcodec::optionalLibrariesSection();
    else if (k == "ffmpegLibs") {
        static char buf[1024]; static bool init = false;
        if (!init) { init = true; snprintf(buf, sizeof buf, "%s", plat::prefs().getS("ffmpegDir", "").c_str()); }
        bool ext = plat::prefs().getB("ffmpegExternal", false);
        int sel = ext ? 1 : 0;
        ImGui::SetNextItemWidth(-1);
        bool changed = ImGui::Combo("##ffsel", &sel, (std::string(tr("Bundled (default)")) + '\0' + tr("External folder") + '\0').c_str());
        if (sel == 1) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##ffdir", tr("Folder with libavcodec and libavutil"), buf, sizeof buf);
            if (ImGui::Button(tr("Choose..."))) { const std::string d = plat::openFolderDialog(); if (!d.empty()) { snprintf(buf, sizeof buf, "%s", d.c_str()); changed = true; } }
            ImGui::SameLine();
            if (ImGui::Button(tr("Use typed path"))) changed = true;
        }
        if (changed) {
            plat::prefs().setB("ffmpegExternal", sel == 1);
            plat::prefs().setS("ffmpegDir", buf);
            applyFfmpegDirFromPrefs();
        }
        if (sel == 1 && buf[0]) ImGui::TextDisabled("%s", tr("Takes effect the next time an add-on looks for its library (restart if unsure)."));
    }
    else if (k == "sdrconnectLog") {
        bool on = plat::prefs().getB("sdrconnectLog", false);
        if (ImGui::Checkbox(tr("Log SDRconnect websocket traffic"), &on)) { plat::prefs().setB("sdrconnectLog", on); applyFfmpegDirFromPrefs(); }
        ImGui::TextDisabled("%s", (plat::dataDir() + "/sdrconnect-ws.log").c_str());
    }
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
            ImGui::TextWrapped("%s", tr("Every setting in this window, the keys included, goes back to its default. Continue?"));
            if (ImGui::Button(tr("Yes, reset all"))) { for (const Info& x : table()) resetOne(a, x); savePrefs(a); sure = false; U.status = tr("All settings reset"); }
            ImGui::SameLine();
            if (ImGui::Button(tr("Cancel"))) sure = false;
        }
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
        if (const int o = previewOpt(i.key); o >= 0) uiprev::item(a, (uiprev::Opt)o, v ? 0 : 1);   // the look a click gives
    } else if (i.kind == Kind::Choice) {
        const std::vector<std::string> items = split(i.choices ? i.choices : "", '|');
        const int sel = std::max(0, std::min((int)items.size() - 1, choiceIndex(i, cur.d)));
        if (ImGui::BeginCombo("##v2", items.empty() ? "" : tr(items[(size_t)sel].c_str()))) {
            for (int n = 0; n < (int)items.size(); n++)
            {
                if (ImGui::Selectable(tr(items[(size_t)n].c_str()), n == sel)) { cur.d = choiceValue(i, n); writeVal(a, i, cur); }
                if (const int o = previewOpt(i.key); o >= 0) uiprev::item(a, (uiprev::Opt)o, choiceValue(i, n));
            }
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

// a label with the words of the search marked (the matched part in the accent colour on a tinted background)
void highlighted(const char* text, const std::vector<std::pair<size_t, size_t>>& hl) {
    const ImVec4 acc = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::string t = text;
    size_t at = 0;
    bool first = true;
    auto piece = [&](size_t b, size_t e, bool mark) {
        if (e <= b) return;
        if (!first) ImGui::SameLine(0, 0);
        first = false;
        const std::string part = t.substr(b, e - b);
        if (mark) {
            const ImVec2 p = ImGui::GetCursorScreenPos(), sz = ImGui::CalcTextSize(part.c_str());
            dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), ImGui::GetColorU32(ImVec4(acc.x, acc.y, acc.z, 0.28f)), 2.f);
            ImGui::TextColored(acc, "%s", part.c_str());
        } else ImGui::TextUnformatted(part.c_str());
    };
    for (const auto& h : hl) { piece(at, std::min(h.first, t.size()), false); piece(std::min(h.first, t.size()), std::min(h.second, t.size()), true); at = std::min(h.second, t.size()); }
    piece(at, t.size(), false);
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
        const char* label = tr(i.label);
        const std::vector<std::pair<size_t, size_t>> hl = showCat ? highlightSpans(label, U.query) : std::vector<std::pair<size_t, size_t>>();
        if (hl.empty()) ImGui::TextWrapped("%s", label);
        else highlighted(label, hl);
        if (i.tip[0] && i.kind != Kind::Custom) { if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(i.tip)); }
        else if (i.tip[0] && showCat && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(i.tip));
        if (showCat) ImGui::TextDisabled("%s%s", tr(i.cat), i.advanced ? " / Advanced" : "");
        if (i.restart) ImGui::TextDisabled("%s", tr("restart needed"));
        ImGui::TableSetColumnIndex(1);
        control(a, i);
        ImGui::TableSetColumnIndex(2);
        if (ch && resettable(i))
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
    if (inputmap::pressed(inputmap::Action::OpenSettings)) settingsOpen() = !settingsOpen();   // Ctrl+, by default (Settings > Keyboard and mouse)
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
    int keyHits = 0;
    if (searching) for (int k = 0; k < inputmap::kActions; k++) if (inputmap::actionMatches(inputmap::settings(), k, U.query)) keyHits++;
    if (searching) {
        ImGui::TextDisabled("%zu %s", plain.size(), tr("matching settings"));
        if (keyHits) {
            char b[96]; snprintf(b, sizeof b, tr("%d keyboard actions match"), keyHits);
            ImGui::SameLine(); ImGui::TextDisabled("  %s", b); ImGui::SameLine();
            if (ImGui::SmallButton(tr("Show them"))) { inputmap::setFind(U.query); U.cat = catIndex(kKeys); U.query[0] = 0; }
        }
        if (plain.empty() && !keyHits) ImGui::TextWrapped("%s", tr("Nothing matches. Try another word, e.g. colour, update, audio."));
    } else {
        ImGui::TextUnformatted(tr(catName.c_str()));
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Reset this page"))) for (const Info& i : t) if (catName == i.cat) resetOne(a, i);
        ImGui::Separator();
    }
    if (!searching && catName == kKeys) {   // the key editor needs the page's whole width
        plain.erase(std::remove_if(plain.begin(), plain.end(), [&](size_t n) { return !strcmp(t[n].key, "keysEditor"); }), plain.end());
        ImGui::PushID("keys");
        inputmap::editor();
        ImGui::PopID();
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
