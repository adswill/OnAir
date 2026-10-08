// The shared channel database in the scan tabs: the question after a scan ("share it?", country, city, the exact text, then GitHub opens with
// the issue filled in) and the "Shared data" view (a country's channels by city, which of them the last scan found). Nothing is sent by the
// app; downloads happen only after a yes. The files live in github.com/adswill/OnAir-channels; DECT2_CHANNELDB_URL replaces the address
// (a folder or file:// address works too, for tests). Core part: core/src/channel_db.cpp.
#include "app.h"
#include "dect2/channel_db.h"
#include "dect2/updater.h"
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

using namespace dect2;
namespace fs = std::filesystem;

void scanDbHost(App& a, void (*inner)(App&));

namespace {

#include "scan_db_countries.inc"   // kCountries[]: {ISO code, name}, from GeoNames countryInfo (CC BY 4.0)

const char* kRepoName = "adswill/OnAir-channels";
const ImVec4 kGood(0.4f, 0.9f, 0.5f, 1), kWarn(0.95f, 0.6f, 0.25f, 1), kDimC(0.6f, 0.64f, 0.68f, 1);

const char* modeFolder(const App& a) {
    if (a.dabMode) return "dab";
    if (a.fmMode) return "fm";
    if (a.family == 7) return "dtmb";
    if (a.family >= 6) return "";            // the later modes have no scan
    if (a.atsc3Mode) return "atsc3";
    if (a.isdbtMode) return "isdb-t";
    if (a.atscMode) return "atsc";
    return "dvb-t";
}

std::string countryName(const std::string& iso) {
    for (const auto& c : kCountries) if (iso == c[0]) return c[1];
    return iso;
}

std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> v; std::string cur;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || (s[i] == ',' && i + 1 < s.size() && s[i + 1] == ' ')) { if (!cur.empty()) v.push_back(cur); cur.clear(); if (i < s.size()) i++; }
        else cur += s[i];
    }
    return v;
}
std::string stripTag(std::string s) { const size_t br = s.rfind(" ["); if (br != std::string::npos) s.resize(br); return s; }

// what the last scan found, in the shape that is shared: only frequency, width, standard, network, services, SNR
std::vector<chdb::Row> scanRows(App& a) {
    std::vector<chdb::Row> out;
    const std::string m = modeFolder(a);
    if (m.empty()) return out;
    if (m == "dab") {
        for (const auto& r : a.dabScan.results) if (r.found) {
            chdb::Row w; w.freqMhz = r.mhz; w.bwMhz = 1.536; w.standard = "DAB"; w.network = r.label; w.services = splitComma(r.stations); w.snrDb = r.snr; out.push_back(w);
        }
    } else if (m == "fm") {
        for (const auto& r : a.fmScan.results) if (r.found) {
            chdb::Row w; w.freqMhz = r.mhz; w.bwMhz = 0.2; w.standard = "FM"; w.network = r.name; w.snrDb = r.snr; out.push_back(w);
        }
    } else {
        for (const auto& r : a.scanner.results()) if (r.t2) {
            chdb::Row w; w.freqMhz = r.freqMHz; w.bwMhz = r.bwMhz; w.standard = r.standard; w.network = r.networkName; w.snrDb = r.snrDb;
            for (const auto& x : r.services) w.services.push_back(stripTag(x));
            out.push_back(w);
        }
    }
    return out;
}

bool scanRunning(App& a) {
    const std::string m = modeFolder(a);
    if (m == "dab") return a.dabScan.running;
    if (m == "fm") return a.fmScan.running;
    return a.scanner.progress().running;
}

// ---- files: the repo (or the override), cached; fetched on a thread so the window stays alive
struct Job { std::atomic<int> state{0}; std::string text, err; };   // 0 working, 1 done, 2 failed
using JobP = std::shared_ptr<Job>;

std::string baseUrl() {
    const char* e = getenv("DECT2_CHANNELDB_URL");
    std::string b = e && *e ? e : "https://raw.githubusercontent.com/adswill/OnAir-channels/main";
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b;
}

fs::path u8(const std::string& p) { return fs::path(reinterpret_cast<const char8_t*>(p.c_str())); }   // a UTF-8 path

JobP fetchText(const std::string& rel, bool refresh) {
    auto j = std::make_shared<Job>();
    const std::string base = baseUrl();
    std::string cache;
    const bool local = base.find("://") == std::string::npos || base.compare(0, 7, "file://") == 0;
    if (!local) {
        std::string dir = plat::cacheDir();
        if (!dir.empty()) {   // UTF-8 strings throughout (fs::path(std::string) would read them in the ANSI code page on Windows)
            dir += "/channels";
            std::error_code ec; fs::create_directories(u8(dir), ec);
            std::string flat = rel; for (auto& c : flat) if (c == '/') c = '_';
            cache = dir + "/" + flat;
        }
    }
    std::thread([j, rel, base, cache, local, refresh]() {
        auto readAll = [](const std::string& p, std::string& out) {
            std::ifstream f(u8(p), std::ios::binary); if (!f) return false;
            std::stringstream ss; ss << f.rdbuf(); out = ss.str(); return true;
        };
        bool ok = false;
        if (local) {
            std::string root = base.compare(0, 7, "file://") == 0 ? base.substr(7) : base;
            ok = readAll(root + "/" + rel, j->text);
            if (!ok) j->err = "not in " + root;
        } else {
            std::error_code ec;
            const bool fresh = !refresh && !cache.empty() && fs::exists(u8(cache), ec) &&
                std::chrono::file_clock::now() - fs::last_write_time(u8(cache), ec) < std::chrono::hours(24);
            if (fresh) ok = readAll(cache, j->text);
            if (!ok) {
                const std::string tmp = cache.empty() ? std::string() : cache + ".part";
                if (!tmp.empty() && plat::fetchUrl(base + "/" + rel, tmp, "OnAir") && readAll(tmp, j->text)) {
                    fs::rename(u8(tmp), u8(cache), ec); ok = true;
                } else { j->err = "could not download it"; if (!tmp.empty()) fs::remove(u8(tmp), ec); }
            }
        }
        j->state = ok ? 1 : 2;
    }).detach();
    return j;
}

// ---- a combo with a filter box on top (thousands of cities)
bool filterCombo(const char* id, const char* cur, const std::vector<std::string>& items, int& sel, char* filter, size_t fsz, float w) {
    bool changed = false;
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo(id, cur)) {
        if (ImGui::IsWindowAppearing()) { ImGui::SetKeyboardFocusHere(); filter[0] = 0; }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##flt", "type to filter", filter, fsz);
        std::string f = filter; for (auto& c : f) c = (char)tolower((unsigned char)c);
        ImGui::BeginChild("##lst", ImVec2(0, 240 * gUi));
        int shown = 0;
        for (int i = 0; i < (int)items.size(); i++) {
            if (!f.empty()) {
                std::string l = items[(size_t)i]; for (auto& c : l) c = (char)tolower((unsigned char)c);
                if (l.find(f) == std::string::npos) continue;
            }
            if (shown++ > 400) { ImGui::TextDisabled("... keep typing to narrow the list"); break; }
            if (ImGui::Selectable(items[(size_t)i].c_str(), i == sel)) { sel = i; changed = true; ImGui::CloseCurrentPopup(); }
        }
        if (!shown) ImGui::TextDisabled("nothing matches");
        ImGui::EndChild();
        ImGui::EndCombo();
    }
    return changed;
}

std::vector<std::string> countryItems() {
    std::vector<std::string> v;
    for (const auto& c : kCountries) v.push_back(std::string(c[1]) + " (" + c[0] + ")");
    return v;
}
int countryIndex(const std::string& iso) {
    int i = 0;
    for (const auto& c : kCountries) { if (iso == c[0]) return i; i++; }
    return -1;
}

// ---- the question after a scan
struct Share {
    bool pending = false, open = false;
    int stage = 0;                       // 0 question, 1 country and city, 2 the text
    std::string mode;
    std::vector<chdb::Row> rows;
    int country = -1, city = -1;
    char cflt[64] = {}, tflt[64] = {};
    std::string loadedCountry;
    JobP cityJob;
    std::vector<chdb::City> cities;
    std::vector<std::string> cityNames;
    std::vector<std::string> countries = countryItems();
    size_t sent = 0;
    bool wasRunning = false;
} sh;

void loadCities() {
    if (sh.country < 0) return;
    const std::string iso = kCountries[sh.country][0];
    if (iso == sh.loadedCountry) return;
    sh.loadedCountry = iso; sh.cities.clear(); sh.cityNames.clear(); sh.city = -1;
    sh.cityJob = fetchText("cities/" + iso + ".csv", false);
}

void pollCities() {
    if (!sh.cityJob || sh.cityJob->state == 0 || sh.cityJob->state >= 3) return;
    if (sh.cityJob->state == 1) {
        sh.cities = chdb::parseCities(sh.cityJob->text);
        // the biggest first: most people pick a large city
        std::stable_sort(sh.cities.begin(), sh.cities.end(), [](const chdb::City& x, const chdb::City& y) { return x.population > y.population; });
        for (const auto& c : sh.cities) sh.cityNames.push_back(c.admin1.empty() ? c.name : c.name + ", " + c.admin1);
        const std::string last = plat::prefs().getS("scanDbCity", "");
        for (size_t i = 0; i < sh.cities.size(); i++) if (sh.cities[i].name == last && sh.loadedCountry == plat::prefs().getS("scanDbCountry", "")) sh.city = (int)i;
    }
    sh.cityJob->state = sh.cityJob->state == 1 ? 4 : 3;   // 4 loaded, 3 failed (the message stays); not parsed again
}

chdb::Submission buildSubmission() {
    chdb::Submission s; s.mode = sh.mode; s.country = kCountries[sh.country][0];
    s.city = sh.city >= 0 ? sh.cities[(size_t)sh.city].name : ""; s.rows = sh.rows;
    return s;
}

void shareDialog(App& a) {
    if (sh.pending) { sh.pending = false; sh.open = true; sh.stage = 0; ImGui::OpenPopup("Share your scan"); }
    ImGui::SetNextWindowSize(ImVec2(620 * gUi, 0));
    if (!ImGui::BeginPopupModal("Share your scan", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (!sh.open) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    ImGui::PushTextWrapPos(600 * gUi);
    if (sh.stage == 0) {
        ImGui::Text("The scan found %zu channel%s. Do you want to share the result?", sh.rows.size(), sh.rows.size() == 1 ? "" : "s");
        ImGui::TextDisabled("It goes into a public list at github.com/%s that shows others which channels can be received where. You see exactly what is sent before anything leaves this computer, and nothing is sent without your click.", kRepoName);
        ImGui::Spacing();
        if (ImGui::Button("  Yes, share  ")) {
            sh.stage = 1;
            const std::string c = plat::prefs().getS("scanDbCountry", "");
            if (sh.country < 0) sh.country = countryIndex(c);
        }
        ImGui::SameLine(); if (ImGui::Button("  No  ")) { sh.open = false; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(); if (ImGui::Button("  No, and do not ask again  ")) { plat::prefs().setB("scanDbAsk", false); sh.open = false; ImGui::CloseCurrentPopup(); }
        ImGui::TextDisabled("You can turn the question back on, or share later, in the Scan tab under Shared data.");
    } else if (sh.stage == 1) {
        ImGui::TextUnformatted("Where did you scan?");
        if (filterCombo("country", sh.country >= 0 ? sh.countries[(size_t)sh.country].c_str() : "choose a country", sh.countries, sh.country, sh.cflt, sizeof sh.cflt, 360 * gUi)) { sh.city = -1; }
        loadCities();
        pollCities();
        if (sh.country >= 0) {
            if (sh.cityJob && sh.cityJob->state == 0) ImGui::TextDisabled("loading the list of cities from github.com/%s ...", kRepoName);
            else if (sh.cities.empty()) ImGui::TextColored(kWarn, "The list of cities could not be loaded (%s). Check the connection and try again.", sh.cityJob ? sh.cityJob->err.c_str() : "");
            else filterCombo("city", sh.city >= 0 ? sh.cityNames[(size_t)sh.city].c_str() : "choose the nearest city", sh.cityNames, sh.city, sh.tflt, sizeof sh.tflt, 360 * gUi);
        }
        ImGui::TextDisabled("The city is only used to group channels; no position is sent.");
        ImGui::Spacing();
        ImGui::BeginDisabled(sh.city < 0);
        if (ImGui::Button("  Submit  ")) sh.stage = 2;
        ImGui::EndDisabled();
        ImGui::SameLine(); if (ImGui::Button("  Cancel  ")) { sh.open = false; ImGui::CloseCurrentPopup(); }
    } else {
        const chdb::Submission s = buildSubmission();
        bool withSv = true;
        const auto parts = chdb::submissionParts(s, withSv);
        ImGui::TextUnformatted("This is exactly the text that will be submitted:");
        std::string text;
        for (size_t i = 0; i < parts.size(); i++) { if (i) text += "\n----- next issue -----\n"; text += chdb::issueBody(parts[i], withSv); }
        ImGui::PushFont(a.mono, 0);
        ImGui::InputTextMultiline("##txt", text.data(), text.size() + 1, ImVec2(-1, 200 * gUi), ImGuiInputTextFlags_ReadOnly);
        ImGui::PopFont();
        if (!withSv) ImGui::TextDisabled("The service lists are left out because the text is too long for one link.");
        if (parts.size() > 1) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("The text is too long for one issue: it goes as %zu issues, one after the other.", parts.size()); ImGui::PopTextWrapPos(); }
        { ImGui::PushTextWrapPos(0); ImGui::TextColored(kWarn, "GitHub shows your GitHub user name on the submission."); ImGui::PopTextWrapPos(); }
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("The button opens your browser on GitHub with this text filled in; you press \"Submit new issue\" there. You need a free GitHub account."); ImGui::PopTextWrapPos(); }
        ImGui::Spacing();
        if (sh.sent >= parts.size()) sh.sent = 0;
        char lbl[64]; snprintf(lbl, sizeof lbl, parts.size() > 1 ? "  Open GitHub (issue %zu of %zu)  " : "  Open GitHub  ", sh.sent + 1, parts.size());
        if (ImGui::Button(lbl)) {
            openUrl(chdb::submissionUrl(parts[sh.sent], withSv));
            plat::prefs().setS("scanDbCountry", s.country); plat::prefs().setS("scanDbCity", s.city); plat::prefs().flush();
            if (++sh.sent >= parts.size()) { sh.sent = 0; sh.open = false; ImGui::CloseCurrentPopup(); }
        }
        ImGui::SameLine(); if (ImGui::Button("  Back  ")) sh.stage = 1;
        ImGui::SameLine(); if (ImGui::Button("  Cancel  ")) { sh.open = false; sh.sent = 0; ImGui::CloseCurrentPopup(); }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
}

// ---- the Shared data view
struct View {
    int country = -1;
    bool consent = false, asked = false;
    JobP job;
    std::vector<chdb::Row> rows;
    std::string err, shownFor;
    char flt[64] = {};
    bool devInit = false;
    int sub = 0;
} vw;

void sharedView(App& a) {
    const std::string mode = modeFolder(a);
    if (mode.empty()) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("There is no shared data for this mode."); ImGui::PopTextWrapPos(); } return; }
    if (!vw.devInit) {   // dev: DECT2_CHANNELDB_COUNTRY=DE opens that country at once (screenshots)
        vw.devInit = true;
        const char* c = getenv("DECT2_CHANNELDB_COUNTRY");
        if (c && *c) { vw.country = countryIndex(c); vw.consent = vw.country >= 0; }
        else vw.country = countryIndex(plat::prefs().getS("scanDbCountry", ""));
    }
    static std::vector<std::string> countries = countryItems();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Channels other OnAir users received, by country and city (%s). Public list at github.com/%s.", mode.c_str(), kRepoName); ImGui::PopTextWrapPos(); }
    if (filterCombo("country##sd", vw.country >= 0 ? countries[(size_t)vw.country].c_str() : "choose a country", countries, vw.country, vw.flt, sizeof vw.flt, 320 * gUi)) { vw.consent = false; vw.job.reset(); vw.rows.clear(); vw.shownFor.clear(); }
    ImGui::SameLine();
    bool ask = plat::prefs().getB("scanDbAsk", true);
    if (ImGui::Checkbox("ask to share after a scan", &ask)) plat::prefs().setB("scanDbAsk", ask);
    const std::vector<chdb::Row> mine = scanRows(a);
    ImGui::BeginDisabled(mine.empty());
    if (ImGui::Button("Share my last scan...")) { sh.mode = mode; sh.rows = mine; sh.pending = true; }
    ImGui::EndDisabled();
    if (vw.country < 0) return;
    const std::string iso = kCountries[vw.country][0], cn = kCountries[vw.country][1];
    const std::string key = mode + "/" + iso;
    if (!vw.consent) {
        ImGui::Spacing();
        ImGui::TextWrapped("Load the shared data for %s? This downloads it from github.com/%s", cn.c_str(), kRepoName);
        if (ImGui::Button("  Yes, load  ")) { vw.consent = true; vw.job.reset(); }
        return;
    }
    if (!vw.job && vw.shownFor != key) { vw.job = fetchText(mode + "/" + iso + ".csv", false); vw.err.clear(); }
    ImGui::SameLine();
    if (ImGui::Button("refresh")) { vw.job = fetchText(mode + "/" + iso + ".csv", true); vw.shownFor.clear(); }
    if (vw.job && vw.job->state != 0) {
        if (vw.job->state == 1) { vw.rows = chdb::parseChannels(vw.job->text); vw.err.clear(); }
        else { vw.rows.clear(); vw.err = "Nothing shared for " + cn + " in this mode yet, or GitHub could not be reached (" + vw.job->err + ")."; }
        vw.shownFor = key; vw.job.reset();
    }
    if (vw.job) { ImGui::TextDisabled("loading ..."); return; }
    if (!vw.err.empty()) { ImGui::TextColored(kWarn, "%s", vw.err.c_str()); return; }
    std::stable_sort(vw.rows.begin(), vw.rows.end(), [](const chdb::Row& x, const chdb::Row& y) { return x.city != y.city ? x.city < y.city : x.freqMhz < y.freqMhz; });
    int found = 0;
    auto seen = [&](const chdb::Row& r) { for (const auto& m : mine) if (std::fabs(m.freqMhz - r.freqMhz) <= 0.05) return true; return false; };
    if (!mine.empty()) for (const auto& r : vw.rows) if (seen(r)) found++;
    if (!mine.empty()) { ImGui::PushTextWrapPos(0); ImGui::Text("%d of %zu channels listed here were found in your last scan.", found, vw.rows.size()); ImGui::PopTextWrapPos(); }
    else { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%zu channels. Scan first to see which of them you receive.", vw.rows.size()); ImGui::PopTextWrapPos(); }
    ImGui::Spacing();
    const ImGuiTableFlags fl = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("shared", 10, fl)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Country", ImGuiTableColumnFlags_WidthFixed, 130 * gUi);
    ImGui::TableSetupColumn("City", ImGuiTableColumnFlags_WidthFixed, 130 * gUi);
    ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
    ImGui::TableSetupColumn("Standard", ImGuiTableColumnFlags_WidthFixed, 80 * gUi);
    ImGui::TableSetupColumn("Network / ensemble", ImGuiTableColumnFlags_WidthFixed, 150 * gUi);
    ImGui::TableSetupColumn("Services", ImGuiTableColumnFlags_WidthFixed, 280 * gUi);
    ImGui::TableSetupColumn("Best SNR", ImGuiTableColumnFlags_WidthFixed, 65 * gUi);
    ImGui::TableSetupColumn("Reports", ImGuiTableColumnFlags_WidthFixed, 60 * gUi);
    ImGui::TableSetupColumn("Last seen", ImGuiTableColumnFlags_WidthFixed, 80 * gUi);
    ImGui::TableSetupColumn("Your last scan", ImGuiTableColumnFlags_WidthFixed, 150 * gUi);
    ImGui::TableHeadersRow();
    std::string prevCity; bool first = true;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < vw.rows.size(); i++) {
        const auto& r = vw.rows[i];
        const bool newCity = first || r.city != prevCity;
        ImGui::TableNextRow();
        const float y = ImGui::GetCursorScreenPos().y;
        ImGui::TableSetColumnIndex(0);
        if (first) ImGui::TextUnformatted(cn.c_str());   // one country per view: drawn once, its cell looks like it spans every row
        ImGui::TableSetColumnIndex(1);
        if (newCity && !first) {   // a line between the cities, so each city's cell reads as one block
            const float x = ImGui::GetCursorScreenPos().x - ImGui::GetStyle().CellPadding.x;
            dl->AddLine(ImVec2(x, y), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), y), IM_COL32(120, 130, 140, 160));
        }
        if (newCity) ImGui::TextUnformatted(r.city.c_str());
        ImGui::TableSetColumnIndex(2);
        char lbl[48]; snprintf(lbl, sizeof lbl, "%.3f##%zu", r.freqMhz, i);
        if (ImGui::Selectable(lbl) && !scanRunning(a)) {
            if (mode == "dab") { a.freqMhz = r.freqMhz; a.tune.centerHz = r.freqMhz * 1e6; if (a.engine.running()) { a.engine.retuneReset(a.tune); a.peak.clear(); } }
            else if (mode == "fm") fmTune(a, r.freqMhz);
            else {
                if (mode == "dvb-t") for (int k = 0; k < (int)(sizeof(kBw) / sizeof(kBw[0])); k++) if (kBw[k].mhz == r.bwMhz) { a.bwIdx = k; applyBandwidth(a); }
                tuneFreq(a, r.freqMhz);
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to tune the receiver to this channel");
        ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(r.standard.c_str());
        ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(r.network.c_str());
        ImGui::TableSetColumnIndex(5);
        std::string sv; for (const auto& x : r.services) { if (!sv.empty()) sv += ", "; sv += x; }
        ImGui::TextUnformatted(sv.c_str());
        ImGui::TableSetColumnIndex(6); ImGui::Text("%.1f dB", r.snrDb);
        ImGui::TableSetColumnIndex(7); ImGui::Text("%d", r.reports);
        ImGui::TableSetColumnIndex(8); ImGui::TextUnformatted(r.lastSeen.c_str());
        ImGui::TableSetColumnIndex(9);
        if (!mine.empty()) { if (seen(r)) { ImGui::PushTextWrapPos(0); ImGui::TextColored(kGood, "seen in your last scan"); ImGui::PopTextWrapPos(); } else { ImGui::PushTextWrapPos(0); ImGui::TextColored(kWarn, "not found in your last scan"); ImGui::PopTextWrapPos(); } }
        prevCity = r.city; first = false;
    }
    ImGui::EndTable();
}

} // namespace

// A scan has just ended with channels found: ask once (unless switched off). Runs every frame, also when another tab is open.
void scanDbTick(App& a) {
    const bool run = scanRunning(a);
    if (sh.wasRunning && !run && plat::prefs().getB("scanDbAsk", true)) {
        auto rows = scanRows(a);
        if (!rows.empty()) { sh.mode = modeFolder(a); sh.rows = rows; sh.pending = true; }
    }
    sh.wasRunning = run;
}

// The Scan tab of every mode: the scan itself, or the shared data; the dialog sits on top of both.
void scanDbHost(App& a, void (*inner)(App&)) {
    static bool devOnce = false;
    if (!devOnce) { devOnce = true; if (getenv("DECT2_CHANNELDB_COUNTRY")) vw.sub = 1; }   // dev: open on the Shared data view
    if (modeFolder(a)[0]) {
        subNav("scdb", vw.sub, {"Scan", "Shared data"});
        shareDialog(a);
    }
    if (vw.sub == 1 && modeFolder(a)[0]) sharedView(a); else inner(a);
}
