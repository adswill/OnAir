// The Frequencies panel: the built-in frequency database (core/include/dect2/channels/freq_db.h, data/frequencies) and the user's own entries.
// A window (View > Frequencies, Ctrl+Shift+F), an optional tab of the main screen, and the "Known frequencies" drop-down of the Tuner panel.
// Tuning goes through the Memory panel's recall (memory_options.h), so the mode is switched and its tuning applied exactly as a recalled memory does.
#include "airband/airport_ui.h"
#include "../memory/memory_options.h"
#include "../ui/ui_common.h"
#include "dect2/channels/freq_db.h"
#include "dect2/common/u8file.h"
#include "dect2/qo100/qo100_dsp.h"
#include "dect2/specan/lnb_cal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "../ui/input_map.h"

namespace {

using dect2::FreqEntry;
using dect2::FreqQuery;

struct U {
    char query[64] = "";
    std::string mode, region, category;
    bool favOnly = false, userOnly = false, loaded = false, tabLoaded = false;
    std::vector<FreqEntry> all;          // built in, then the user's
    size_t nBuiltin = 0;
    std::vector<FreqEntry> user;
    std::set<std::string> fav;
    std::string sel;                     // freqKey of the selected row
    std::string pendingDel;              // a delete asked for inside a row waits until the table is done
    std::string msg; double msgT = 0; bool msgBad = false;
    // the "Add entry" dialog
    bool addRequest = false;
    char aName[96] = "", aRegion[24] = "", aCat[32] = "", aNotes[200] = "";
    double aMhz = 0, aBwKhz = 0;
    std::string aMode;
    std::string aErr;
} u;

void say(const std::string& s, bool bad = false) { u.msg = s; u.msgBad = bad; u.msgT = ImGui::GetTime(); }

std::string userFile() { return plat::dataDir() + "/frequencies.json"; }

void rebuild() {
    u.all = dect2::builtinFrequencies();
    u.nBuiltin = u.all.size();
    for (const FreqEntry& e : u.user) u.all.push_back(e);
}

void saveUser() {
    std::error_code ec;
    const std::string path = userFile(), tmp = path + ".tmp";
    {
        std::ofstream f(dect2::u8file(tmp), std::ios::binary);
        f << dect2::freqExportJson(u.user);
        if (!f) { say(std::string(tr("Could not save ")) + path, true); return; }
    }
    std::filesystem::rename(dect2::u8file(tmp), dect2::u8file(path), ec);
    if (ec) say(std::string(tr("Could not save ")) + path, true);
}

void saveFav() {
    std::string s;
    for (const std::string& k : u.fav) s += (s.empty() ? "" : ";") + k;
    plat::prefs().setS("freqFav", s);
}

void load() {
    if (u.loaded) return;
    u.loaded = true;
    {
        std::ifstream f(dect2::u8file(userFile()), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        if (!ss.str().empty()) {
            dect2::FreqParseReport r = dect2::freqImportText(ss.str(), u.user);
            for (const std::string& l : r.log) fprintf(stderr, "frequencies: %s\n", l.c_str());
        }
    }
    const std::string fv = plat::prefs().getS("freqFav", "");
    size_t p = 0;
    while (p < fv.size()) {
        size_t q = fv.find(';', p);
        if (q == std::string::npos) q = fv.size();
        if (q > p) u.fav.insert(fv.substr(p, q - p));
        p = q + 1;
    }
    rebuild();
}

// the mode key of the receiver on screen (the Scanner's / Memory's)
std::string currentModeKey(const App& a) {
    for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) return memoryModeKey(kModes[i]);
    return std::string();
}

// tunes a (the receiver on screen) to an entry: through the Memory recall, so the mode is set and its tuning applied
bool tuneTo(App& a, const FreqEntry& e, bool withMode) {
    dect2::Memory m = dect2::freqToMemory(e);
    m.bandwidthHz = 0; m.group.clear(); m.tags.clear(); m.notes.clear();
    // the radio sees the first IF behind the LNB (the oscillator chosen on the QO-100 screen). A QO-100 preset of another mode (FT8, SSTV, PSK31 ...) is
    // tuned the same way: the transponder does not invert, so with an LNB whose oscillator is below the downlink (9750 MHz) the sideband stays upper
    const bool qoRf = e.hz >= 10489000000ull && e.hz <= 10500000000ull && (e.mode == "qo100" || e.name.rfind("QO-100", 0) == 0);
    if (qoRf) {
        double lo = 9750e6;
        int n = 0; const dect2::qo100::LoPreset* pr = dect2::qo100::loPresets(n);
        const long sel = plat::prefs().getI("qo100LoPreset", 0);
        lo = sel >= 0 && sel < n ? pr[sel].loHz : plat::prefs().getD("qo100LoCustom", 9750) * 1e6;
        lo = dect2::lnbCorrectedLoHz(lo, plat::prefs().getD("qo100LnbPpm", 0));   // the calibrated LNB error (qo100_ui LNB calibration)
        m.freqHz = (uint64_t)std::llround(dect2::qo100::ifOf((double)e.hz, lo));
    }
    if (memoryModeIndex(m.mode) < 0) withMode = false;   // a mode this build lacks: the frequency alone
    std::string note;
    if (memoryRecallApp(a, m, withMode, note)) {
        char bf[200];
        snprintf(bf, sizeof bf, tr("Tuned: %s  %s MHz"), e.name.c_str(), dect2::memoryFreqMhzText(e.hz).c_str());
        say(std::string(bf) + (note.empty() ? "" : "  (" + note + ")"));
        return true;
    }
    say(note.empty() ? std::string(tr("Could not tune to this entry.")) : note, true);
    return false;
}

void addToMemory(const FreqEntry& e) {
    dect2::MemoryBank& b = memoryBank();
    dect2::Memory m = dect2::freqToMemory(e);
    m.scan = false;
    const dect2::MemoryAddResult r = b.add(m, dect2::DupPolicy::Skip);
    if (r.what == dect2::MemoryAdd::Added) { memoryChanged(); say(tr("Added to Memory.")); }
    else if (r.what == dect2::MemoryAdd::Duplicate) say(tr("Already in Memory."));
    else say(tr("The memory bank is full."), true);
}

void toggleFav(const FreqEntry& e) {
    const std::string k = dect2::freqKey(e);
    if (!u.fav.erase(k)) u.fav.insert(k);
    saveFav();
}

void deleteUser(const FreqEntry& e) {
    const std::string k = dect2::freqKey(e);
    u.user.erase(std::remove_if(u.user.begin(), u.user.end(), [&](const FreqEntry& x) { return dect2::freqKey(x) == k; }), u.user.end());
    u.fav.erase(k);
    saveUser(); saveFav(); rebuild();
}

std::string bwText(double hz) {
    if (hz <= 0) return "-";
    char b[32];
    if (hz >= 1e6) snprintf(b, sizeof b, "%.3g MHz", hz / 1e6); else snprintf(b, sizeof b, "%.4g kHz", hz / 1e3);
    return b;
}
std::string readout(uint64_t hz) {
    std::string d = std::to_string((unsigned long long)hz), o;
    for (size_t i = 0; i < d.size(); i++) { if (i && (d.size() - i) % 3 == 0) o += '.'; o += d[i]; }
    return o;
}

// a drop-down of the distinct values of a field; "" is "all"
void filterCombo(const char* id, const char* allLabel, std::string& cur, const std::vector<std::string>& values, bool modeNames, float w) {
    ImGui::SetNextItemWidth(w);
    const std::string shown = cur.empty() ? std::string(tr(allLabel)) : modeNames ? dect2::freqModeName(cur) : cur;
    if (ImGui::BeginCombo(id, shown.c_str())) {
        if (ImGui::Selectable(tr(allLabel), cur.empty())) cur.clear();
        for (const std::string& v : values) {
            const std::string l = modeNames ? dect2::freqModeName(v) : v;
            if (ImGui::Selectable(l.c_str(), cur == v)) cur = v;
        }
        ImGui::EndCombo();
    }
}

void row(App& a, const FreqEntry& e) {
    const std::string key = dect2::freqKey(e);
    ImGui::PushID(key.c_str());
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    {
        const bool fav = u.fav.count(key) > 0;
        if (iconFlat(Ic::Star, tr(fav ? "Favourite: kept at the top. Click to remove" : "Click to make this a favourite"), fav)) toggleFav(e);
    }
    ImGui::TableNextColumn();
    ImGui::PushFont(a.mono, 0);
    const bool sel = u.sel == key;
    if (ImGui::Selectable(readout(e.hz).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
        u.sel = key;
        if (ImGui::IsMouseDoubleClicked(0)) tuneTo(a, e, true);
    }
    ImGui::PopFont();
    if (ImGui::IsItemHovered() && !e.notes.empty()) wrapTooltip(380 * gUi, e.notes.c_str());
    if (ImGui::BeginPopupContextItem("##ctx")) {
        u.sel = key;
        if (ImGui::MenuItem(tr("Tune"))) tuneTo(a, e, true);
        if (ImGui::MenuItem(tr("Frequency only"))) tuneTo(a, e, false);
        if (ImGui::MenuItem(tr("Add to Memory"))) addToMemory(e);
        if (ImGui::MenuItem(tr("Favourite"), nullptr, u.fav.count(key) > 0)) toggleFav(e);
        if (!e.source.empty() && ImGui::MenuItem(tr("Source..."))) say(e.source);
        if (e.user && ImGui::MenuItem(tr("Delete"))) u.pendingDel = key;
        ImGui::EndPopup();
    }
    ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::freqModeName(e.mode).c_str());
    ImGui::TableNextColumn(); ImGui::TextUnformatted(e.name.c_str());
    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", bwText(e.bwHz).c_str());
    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", e.region.c_str());
    ImGui::TableNextColumn(); ImGui::TextDisabled("%s%s", e.category.c_str(), e.user ? (e.category.empty() ? "mine" : "  mine") : "");
    ImGui::PopID();
}

// An airport code in the search (EGLL, LHR): its airband channels are listed first, as entries of the airband mode
const std::vector<FreqEntry>& airportRows() {
    static std::string last = "\x01";
    static std::vector<FreqEntry> rows;
    if (last == u.query) return rows;
    last = u.query;
    rows.clear();
    std::string t;
    for (char c : last) if (c != ' ') t += c;
    if (t.size() < 3 || t.size() > 4 || t.size() != last.size()) return rows;
    const dect2::Airport* ap = airportDb().find(t);
    if (!ap) return rows;
    for (const auto& c : dect2::airportAirbandChannels(*ap)) {
        FreqEntry e;
        e.hz = (uint64_t)std::llround(c.freqHz);
        e.mode = "airband";
        e.name = dect2::airportRowName(*ap, c.label, t);
        e.region = ap->country;
        e.category = "airport";
        e.notes = ap->name + (ap->city.empty() ? "" : ", " + ap->city) + (c.is833 ? ". 8.33 kHz channel." : ". 25 kHz channel.");
        e.source = "OurAirports (public domain)";
        rows.push_back(e);
    }
    return rows;
}

// The rows the table shows now (airport channels first, then the matches of the search and filters), for "Export results CSV..."
std::vector<FreqEntry> resultsNow() {
    FreqQuery q;
    q.text = u.query; q.mode = u.mode; q.region = u.region; q.category = u.category; q.favouritesOnly = u.favOnly; q.userOnly = u.userOnly;
    std::vector<FreqEntry> out;
    if ((u.mode.empty() || u.mode == "airband") && !u.favOnly && !u.userOnly) for (const FreqEntry& e : airportRows()) out.push_back(e);
    for (size_t i : dect2::freqSearch(u.all, q, &u.fav)) out.push_back(u.all[i]);
    return out;
}

void table(App& a, float h) {
    FreqQuery q;
    q.text = u.query; q.mode = u.mode; q.region = u.region; q.category = u.category; q.favouritesOnly = u.favOnly; q.userOnly = u.userOnly;
    std::vector<size_t> v = dect2::freqSearch(u.all, q, &u.fav);
    // the favourites first, and the mode on screen before the rest when no mode is chosen
    const std::string cur = u.mode.empty() ? currentModeKey(a) : std::string();
    std::stable_sort(v.begin(), v.end(), [&](size_t x, size_t y) {
        const int fx = u.fav.count(dect2::freqKey(u.all[x])) > 0, fy = u.fav.count(dect2::freqKey(u.all[y])) > 0;
        if (fx != fy) return fx > fy;
        const int cx = !cur.empty() && u.all[x].mode == cur, cy = !cur.empty() && u.all[y].mode == cur;
        return cx > cy;
    });
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Hideable;
    if (ImGui::BeginTable("##freqtable", 7, tf, ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("*", ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoHide, 24 * gUi);
        ImGui::TableSetupColumn(tr("Frequency"), ImGuiTableColumnFlags_WidthFixed, 112 * gUi);
        ImGui::TableSetupColumn(tr("Mode"), ImGuiTableColumnFlags_WidthFixed, 110 * gUi);
        ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch, 3.f);
        ImGui::TableSetupColumn(tr("Bandwidth"), ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 76 * gUi);
        ImGui::TableSetupColumn(tr("Region"), ImGuiTableColumnFlags_WidthFixed, 64 * gUi);
        ImGui::TableSetupColumn(tr("Category"), ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableHeadersRow();
        if ((u.mode.empty() || u.mode == "airband") && !u.favOnly && !u.userOnly) for (const FreqEntry& e : airportRows()) row(a, e);
        ImGuiListClipper clip;
        clip.Begin((int)v.size());
        while (clip.Step()) for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) row(a, u.all[v[(size_t)i]]);
        ImGui::EndTable();
    }
    if (v.empty() && airportRows().empty()) ImGui::TextDisabled("%s", tr("Nothing matches. Clear the search or the filters."));
}

const FreqEntry* selected() {
    if (u.sel.empty()) return nullptr;
    for (const FreqEntry& e : u.all) if (dect2::freqKey(e) == u.sel) return &e;
    return nullptr;
}

std::string slurp(const std::string& p) {
    std::ifstream f(dect2::u8file(p), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void addDialog() {
    if (u.addRequest) { ImGui::OpenPopup(trw("Add frequency")); u.addRequest = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(trw("Add frequency"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const float w = 300 * gUi;
    auto label = [&](const char* t) { ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", t); ImGui::SameLine(130 * gUi); ImGui::SetNextItemWidth(w); };
    label(tr("Name")); ImGui::InputText("##fname", u.aName, sizeof u.aName);
    label(tr("Frequency (MHz)")); ImGui::InputDouble("##ffreq", &u.aMhz, 0, 0, "%.6f");
    label(tr("Mode"));
    const int cur = memoryModeIndex(u.aMode);
    if (ImGui::BeginCombo("##fmode", u.aMode.empty() ? tr("(frequency only)") : cur >= 0 ? kModes[cur].name : u.aMode.c_str())) {
        if (ImGui::Selectable(tr("(frequency only)"), u.aMode.empty())) u.aMode.clear();
        const char* qs = modeSearchBox();
        for (int i = 0; i < kNumModes; i++) {
            if (!modeMatches(kModes[i], qs)) continue;
            if (ImGui::Selectable(kModes[i].name, i == cur)) u.aMode = memoryModeKey(kModes[i]);
        }
        ImGui::EndCombo();
    }
    label(tr("Bandwidth (kHz)")); ImGui::InputDouble("##fbw", &u.aBwKhz, 0, 0, "%.3f");
    label(tr("Region")); ImGui::InputText("##freg", u.aRegion, sizeof u.aRegion);
    label(tr("Category")); ImGui::InputText("##fcat", u.aCat, sizeof u.aCat);
    label(tr("Notes")); ImGui::InputText("##fnotes", u.aNotes, sizeof u.aNotes);
    if (!u.aErr.empty()) ImGui::TextColored(pal::badRed(), "%s", u.aErr.c_str());
    ImGui::Spacing();
    if (ImGui::Button(tr("Save"), fitButton(tr("Save"), 90 * gUi))) {
        FreqEntry e;
        e.name = u.aName; e.hz = (uint64_t)std::max(0.0, u.aMhz * 1e6 + 0.5); e.mode = u.aMode.empty() ? "general" : u.aMode;
        e.bwHz = std::max(0.0, u.aBwKhz * 1e3); e.region = u.aRegion; e.category = u.aCat; e.notes = u.aNotes; e.user = true;
        if (e.hz == 0) u.aErr = tr("Enter a frequency.");
        else {
            if (e.name.empty()) e.name = dect2::memoryFreqMhzText(e.hz) + " MHz";
            const std::string k = dect2::freqKey(e);
            bool dup = false;
            for (const FreqEntry& x : u.all) dup |= dect2::freqKey(x) == k;
            if (dup) u.aErr = tr("An entry with this frequency and mode exists already.");
            else { u.user.push_back(e); saveUser(); rebuild(); u.sel = k; ImGui::CloseCurrentPopup(); }
        }
    }
    ImGui::SameLine(0, 8 * gUi);
    if (ImGui::Button(tr("Cancel"), fitButton(tr("Cancel"), 90 * gUi)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void panelBody(App& a) {
    load();
    // search and filters
    ImGui::SetNextItemWidth(std::max(120.f * gUi, ImGui::GetContentRegionAvail().x - 540 * gUi));
    ImGui::InputTextWithHint("##freqq", tr("Search name, notes, mode, frequency (162.4)"), u.query, sizeof u.query);
    ImGui::SameLine(); filterCombo("##fmode", "All modes", u.mode, dect2::freqDistinct(u.all, 0), true, 130 * gUi);
    ImGui::SameLine(); filterCombo("##freg", "All regions", u.region, dect2::freqDistinct(u.all, 1), false, 100 * gUi);
    ImGui::SameLine(); filterCombo("##fcat", "All categories", u.category, dect2::freqDistinct(u.all, 2), false, 120 * gUi);
    ImGui::SameLine(); ImGui::Checkbox(tr("Stars only"), &u.favOnly);
    ImGui::SameLine(); ImGui::Checkbox(tr("Mine"), &u.userOnly);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Only the entries you added or imported"));
    const float footer = ImGui::GetFrameHeightWithSpacing() * 3 + 6 * gUi;
    table(a, std::max(80.f * gUi, ImGui::GetContentRegionAvail().y - footer));
    // buttons
    if (!u.pendingDel.empty()) {
        for (const FreqEntry& e : u.user) if (dect2::freqKey(e) == u.pendingDel) { const FreqEntry c = e; deleteUser(c); break; }
        u.pendingDel.clear();
    }
    const FreqEntry* s = selected();
    ImGui::BeginDisabled(!s);
    if (ImGui::Button(tr("Tune"))) tuneTo(a, *s, true);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) wrapTooltip(380 * gUi, tr("Switches to the entry's mode, applies the mode's tuning and tunes to the frequency. A double-click on a row does the same. A running receiver restarts when the mode changes."));
    ImGui::SameLine();
    if (ImGui::Button(tr("Frequency only"))) tuneTo(a, *s, false);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Tunes to the frequency and keeps the mode as it is"));
    ImGui::SameLine();
    if (ImGui::Button(tr("Add to Memory"))) addToMemory(*s);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Keeps this entry in the Memory panel"));
    ImGui::EndDisabled();
    ImGui::SameLine(0, 16 * gUi);
    if (ImGui::Button(tr("Add entry..."))) {
        u.aMhz = 0; u.aBwKhz = 0; u.aName[0] = u.aRegion[0] = u.aCat[0] = u.aNotes[0] = 0; u.aErr.clear();
        u.aMode = currentModeKey(a);
        if (const FreqEntry* sel = selected()) { u.aMhz = (double)sel->hz / 1e6; u.aMode = sel->mode; }
        u.addRequest = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Import..."))) {
        const std::string p = openFileDialog();
        if (!p.empty()) {
            const dect2::FreqParseReport r = dect2::freqImportText(slurp(p), u.user);
            if (r.loaded == 0 && r.skipped == 0) say(tr("Nothing to import in this file."), true);
            else {
                char bf[160]; snprintf(bf, sizeof bf, tr("Imported %zu entries, %zu skipped (duplicates or lines without a frequency)."), r.loaded, r.skipped);
                say(bf);
                if (r.loaded) { saveUser(); rebuild(); }
            }
            for (const std::string& l : r.log) fprintf(stderr, "frequencies import: %s\n", l.c_str());
        }
    }
    if (ImGui::IsItemHovered()) wrapTooltip(380 * gUi, tr("Reads a CSV or JSON file written by Export (or by the Memory panel), or a plain list of 'frequency,name' lines (MHz). Entries on a frequency and mode that exist already are skipped."));
    ImGui::SameLine();
    ImGui::BeginDisabled(u.user.empty());
    if (ImGui::Button(tr("Export CSV..."))) {
        const std::string p = saveFileDialog("onair-frequencies.csv");
        if (!p.empty()) { std::ofstream f(dect2::u8file(p), std::ios::binary); f << dect2::freqExportCsv(u.user); say(f ? std::string(tr("Exported to ")) + p : std::string(tr("Could not save ")) + p, !f); }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Export JSON..."))) {
        const std::string p = saveFileDialog("onair-frequencies.json");
        if (!p.empty()) { std::ofstream f(dect2::u8file(p), std::ios::binary); f << dect2::freqExportJson(u.user); say(f ? std::string(tr("Exported to ")) + p : std::string(tr("Could not save ")) + p, !f); }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Exports your own entries (not the built-in ones)"));
    ImGui::SameLine();
    if (ImGui::Button(tr("Export results CSV..."))) {
        const std::vector<FreqEntry> rows = resultsNow();
        const std::string p = rows.empty() ? std::string() : saveFileDialog("onair-search-results.csv");
        if (rows.empty()) say(tr("Nothing to export: no row matches the search."), true);
        else if (!p.empty()) { std::ofstream f(dect2::u8file(p), std::ios::binary); f << dect2::freqExportCsv(rows); say(f ? std::string(tr("Exported to ")) + p : std::string(tr("Could not save ")) + p, !f); }
    }
    if (ImGui::IsItemHovered()) wrapTooltip(380 * gUi, tr("Saves the rows now in the table (with the search and the filters applied, built-in and yours) as a CSV file. Import and the Memory panel read it back."));
    ImGui::SameLine(0, 12 * gUi); ImGui::TextDisabled(tr("%zu built in, %zu yours"), u.nBuiltin, u.user.size());
    if (!u.msg.empty() && ImGui::GetTime() - u.msgT < 12) { ImGui::PushTextWrapPos(0); ImGui::TextColored(u.msgBad ? pal::badRed() : pal::okGreen(), "%s", u.msg.c_str()); ImGui::PopTextWrapPos(); }
    addDialog();
}

}  // namespace

void freqdbWindow(App& a) {
    ImGuiIO& io = ImGui::GetIO();
    if (inputmap::pressed(inputmap::Action::FreqDbWin)) a.freqOpen = !a.freqOpen;   // Ctrl+Shift+F by default
    (void)io;
    if (!u.tabLoaded) { u.tabLoaded = true; a.freqTab = a.freqTab || plat::prefs().getB("freqTab", false); }
    if (!a.freqOpen) return;
    firstWindowSize(820 * gUi, 480 * gUi);
    if (!ImGui::Begin(trw("Frequencies"), &a.freqOpen)) { ImGui::End(); return; }
    panelBody(a);
    ImGui::End();
}

void freqdbTabItem(App& a) {
    if (!a.freqTab) return;
    if (tabItem(N_("Frequencies"), Ic::Doc)) { panelBody(a); ImGui::EndTabItem(); }
}

void freqdbTabToggle(App& a) {
    a.freqTab = !a.freqTab;
    plat::prefs().setB("freqTab", a.freqTab);
}

// The transmitter's band presets: the known frequencies of one mode (the list row `m`), favourites first. Returns true and sets mhz when one is picked.
bool freqdbPickMhz(const ModeDef& m, float width, double& mhz) {
    load();
    static std::string cachedMode;
    static std::vector<size_t> cache;
    static size_t cachedSize = 0;
    const std::string mode = memoryModeKey(m);
    if (mode != cachedMode || cachedSize != u.all.size()) {
        cachedMode = mode; cachedSize = u.all.size();
        cache = dect2::freqForMode(u.all, mode);
    }
    if (cache.empty()) return false;
    bool picked = false;
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##txfreqknown", tr("Presets"), ImGuiComboFlags_HeightLarge)) {
        std::vector<size_t> order = cache;
        std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return u.fav.count(dect2::freqKey(u.all[x])) > u.fav.count(dect2::freqKey(u.all[y])); });
        for (size_t i : order) {
            const FreqEntry& e = u.all[i];
            char l[200];
            snprintf(l, sizeof l, "%s%s MHz  %s###%zu", u.fav.count(dect2::freqKey(e)) ? "* " : "", dect2::memoryFreqMhzText(e.hz).c_str(), e.name.c_str(), i);
            if (ImGui::Selectable(l)) { mhz = (double)e.hz / 1e6; picked = true; }
            if (ImGui::IsItemHovered() && !e.notes.empty()) wrapTooltip(340 * gUi, e.notes.c_str());
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Well-known frequencies of this signal: pick one to set the transmit frequency"));
    return picked;
}

// The Tuner panel's drop-down: the known frequencies of the mode on screen (favourites first). Picking one tunes to it, the mode stays.
void freqdbKnownCombo(App& a, float width) {
    load();
    static std::string cachedMode;
    static std::vector<size_t> cache;
    static size_t cachedSize = 0;
    const std::string mode = currentModeKey(a);
    if (mode != cachedMode || cachedSize != u.all.size()) {
        cachedMode = mode; cachedSize = u.all.size();
        cache = dect2::freqForMode(u.all, mode);
    }
    if (cache.empty()) return;
    ImGui::SetNextItemWidth(width);
    char pv[96];
    snprintf(pv, sizeof pv, tr("Known frequencies (%zu)"), cache.size());
    if (ImGui::BeginCombo("##freqknown", pv, ImGuiComboFlags_HeightLarge)) {
        std::vector<size_t> order = cache;
        std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return u.fav.count(dect2::freqKey(u.all[x])) > u.fav.count(dect2::freqKey(u.all[y])); });
        for (size_t i : order) {
            const FreqEntry& e = u.all[i];
            char l[200];
            snprintf(l, sizeof l, "%s%s MHz  %s###%zu", u.fav.count(dect2::freqKey(e)) ? "* " : "", dect2::memoryFreqMhzText(e.hz).c_str(), e.name.c_str(), i);
            if (ImGui::Selectable(l)) tuneTo(a, e, false);
            if (ImGui::IsItemHovered() && !e.notes.empty()) wrapTooltip(340 * gUi, e.notes.c_str());
        }
        ImGui::Separator();
        if (ImGui::Selectable(tr("Open the Frequencies panel..."))) { a.freqOpen = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Well-known frequencies of this mode: pick one to tune to it"));
}

// the database (built in and the user's own) as labels on the spectrum; a click on one is handled in gfx/autolabel.cpp (the Memory recall)
void freqdbLabelSource(std::vector<autolabel::Label>& out) {
    load();
    for (const FreqEntry& e : u.all) {
        if (e.name.empty() || e.hz == 0 || e.hz > 12000000000ull) continue;
        autolabel::Label l;
        l.mhz = (double)e.hz / 1e6; l.text = e.name; l.mode = e.mode; l.src = autolabel::FreqDb; l.bwMhz = e.bwHz > 0 ? e.bwHz / 1e6 : 0;
        l.detail = e.category + (e.region.empty() ? "" : "  " + e.region);
        l.prio = e.user ? 0.5f : 0.f;
        out.push_back(std::move(l));
    }
}
