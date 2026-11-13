// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB: the Map tab (the transmitters). The receiver names the transmitters of the network it hears by their TII codes (MainId / SubId, EN 300 401
// clause 14.8); with a list of where each transmitter of an ensemble stands they go on the map, with the distance and bearing from the
// antenna. Where a transmitter's position comes from, the first that has it winning:
//   1. the user's own file in OnAir's data folder (dab-transmitters.csv)
//   2. the crowd-sourced OnAir-channels repository (dab-tii/<country>.csv, CC0): the list of the country set below and of the country of every
//      ensemble heard (EId country nibble + ECC from FIG 0/9), downloaded once a day and kept for offline use
//   3. qt-dab's transmitter list, when the user has one and chose to use it (read in place: FMLIST data, personal use only, never copied,
//      exported or submitted by OnAir)
//   4. the ensemble itself: FIG 0/22 (EN 300 401 up to V1.4.1), the sub identifier's own position, else its main identifier's reference
//      point (shown as approximate)
//   eid,main,sub,lat,lon,site,power_kw,channel_mhz[,reports,first_seen,last_seen]      (eid in hex; a line starting with # is a comment)
// TII codes are unique within an ensemble, not worldwide: a transmitter is found by (ensemble id, MainId, SubId). A transmitter heard but not
// in any list is listed with the reason, and can optionally be shared (the issue form of the repository, the user types where it stands).
// Another list (an export of heard transmitters with the positions filled in, a regional list) can be imported into the own file; the
// format is in docs/dab-transmitters.md, the parsing in dab_txlist.h.
#include "dect2/common/thread_name.h"
#include "app.h"
#include "../adsb/adsb_map.h"
#include "dect2/channels/channel_db.h"
#include "dect2/engine/updater.h"
#include "dect2/dab/dab_fig.h"
#include "dect2/fm/fm_dx.h"
#include "dab_txlist.h"
#include "dect2/dab/dab_sfn.h"
#include <algorithm>
#include <atomic>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <thread>

namespace {

using dabtx::Src;
using TxSite = dabtx::Site;

struct Heard { int eid = 0, mainId = 0, subId = 0; std::string ens; double mhz = 0; float levelDb = 0; double seen = 0; };

// a shared list's state: 0 not tried, 1 here, 2 the repository has none for the country, 3 not reachable (no network), 4 downloading
struct CountryList { int state = 0, rows = 0; };

struct Download { std::atomic<int> state{0}; std::string country; };   // 0 working, then as CountryList::state

struct State {
    bool loaded = false;
    std::map<uint64_t, TxSite> sites;   // key: eid << 16 | main << 8 | sub
    std::map<int, int> rowsPerEid;      // transmitters with a position per ensemble, over every list
    std::string ownFile, loadMsg;
    char country[3] = {0, 0, 0};
    std::map<std::string, CountryList> lists;
    std::deque<std::string> queue;      // countries to download, one at a time
    std::shared_ptr<Download> dl;
    adsbmap::View map;
    bool fitted = false;
    double atLat = 1e9, atLon = 1e9;   // the antenna position the map was last centred on while nothing is placed
    double refLat = 0, refLon = 0;
    bool haveRef = false;
    // the positions the ensemble itself broadcasts (FIG 0/22), refreshed every couple of seconds: key as in sites, the lists win
    std::map<uint64_t, TxSite> air;
    std::set<int> airEids;                                           // ensembles that sent FIG 0/22
    std::map<int, std::vector<dect2::DabFicInfo::Region>> regions;   // FIG 0/11 per ensemble
    std::map<int, std::string> eidCountry;                           // ISO country of an ensemble (EId + ECC), "" when not known yet
    double airAt = -10, heardAt = -10;
    // every transmitter heard since the program started, over all channels (a band scan fills it): key as in sites
    std::map<uint64_t, Heard> heard;
    std::string exportMsg, importMsg;
    // qt-dab's list (FMLIST data): its path when in use, the one found in the home folder, what reading it said
    std::string extPath, extFound, extMsg;
    int extRows = 0;
    // the transmitter being shared: asked here (position and site), because the issue form of the site does not keep what is typed into it
    struct Pending { bool want = false; int eid = 0, mainId = 0, subId = 0; double mhz = 0; char lat[32] = "", lon[32] = "", site[96] = ""; } pend;
};
State S;

uint64_t key(int eid, int mainId, int subId) { return dabtx::key(eid, mainId, subId); }
std::filesystem::path u8(const std::string& p) { return std::filesystem::path(reinterpret_cast<const char8_t*>(p.c_str())); }

std::string repoUrl() {   // the raw files of the channel list (DECT2_CHANNELDB_URL for tests, as the Scan tab's shared data)
    const char* e = getenv("DECT2_CHANNELDB_URL");
    std::string b = e && *e ? e : "https://raw.githubusercontent.com/adswill/OnAir-channels/main";
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b;
}
std::string sharedFile(const std::string& cc) { return plat::cacheDir() + "/dab-tii-" + cc + ".csv"; }

std::string homeDir() {   // where qt-dab keeps its list (QDir::homePath(): the user profile on Windows)
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE");
    if (!h || !*h) h = std::getenv("HOME");
#else
    const char* h = std::getenv("HOME");
#endif
    return h && *h ? std::string(h) : std::string();
}

bool slurp(const std::string& p, std::string& out) {
    std::ifstream in(u8(p), std::ios::binary);
    if (!in) return false;
    std::ostringstream ss; ss << in.rdbuf(); out = ss.str(); return true;
}

void load();

void addSite(const dabtx::Row& r, Src src) {
    TxSite st;
    st.lat = r.lat; st.lon = r.lon; st.name = r.site; st.powerKw = r.powerKw; st.channelMhz = r.channelMhz; st.src = src;
    const uint64_t k = key(r.eid, r.mainId, r.subId);
    if (!S.sites.count(k)) S.rowsPerEid[r.eid]++;
    S.sites[k] = st;   // read in the order of the list above, the strongest last: it wins
}

// reads one list into S.sites; returns the transmitters read (lines not understood counted in `bad`), -1 when there is no such file
int readList(const std::string& path, Src src, int& bad) {
    std::ifstream in(u8(path));
    if (!in) return -1;
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        dabtx::Row r;
        const dabtx::Line t = dabtx::parse(line, r);
        if (t == dabtx::Line::Bad) { bad++; continue; }
        if (t != dabtx::Line::Ok) continue;   // a comment, the header, a row without a position (an export or a submission still to fill in)
        addSite(r, src);
        n++;
    }
    return n;
}

// qt-dab's list, read in place (never copied into the own file: the data is for personal use only)
int readExternal(const std::string& path) {
    std::string bytes;
    if (!slurp(path, bytes)) { S.extMsg = std::string(tr("could not read")) + " " + path; return -1; }
    std::vector<dabtx::Row> rows;
    dabtx::QtDabStats st;
    if (!dabtx::readQtDab(bytes, rows, st)) { S.extMsg = path + ": " + tr("not a qt-dab transmitter list"); return -1; }
    for (const auto& r : rows) addSite(r, Src::External);
    char b[160];
    snprintf(b, sizeof b, tr("qt-dab's list: %d transmitters (FMLIST data, personal use, read in place)"), st.rows);
    S.extMsg = b;
    return st.rows;
}

// another list into the own file (a friend's export with the positions filled in, a regional list someone keeps): see dabtx::merge.
// qt-dab's list is not merged: it is used in place (see readExternal)
void importList(const std::string& path) {
    std::string imp, own;
    if (!slurp(path, imp)) { S.importMsg = "could not read " + path; return; }
    {
        std::vector<dabtx::Row> rows;
        dabtx::QtDabStats st;
        if (dabtx::readQtDab(imp, rows, st)) {
            S.extPath = path;
            plat::prefs().setS("dabTiiExternal", path);
            S.importMsg = std::string(tr("using qt-dab's transmitter list in place:")) + " " + path;
            load(); S.fitted = false;
            return;
        }
    }
    slurp(S.ownFile, own);   // none yet: a new file
    dabtx::MergeStats st;
    const std::string merged = dabtx::merge(own, imp, st);
    if (st.added + st.replaced == 0) {
        S.importMsg = "nothing to import from " + path + ": no line with ids and a position" +
                      (st.noPosition ? " (" + std::to_string(st.noPosition) + " without latitude/longitude)" : std::string()) +
                      (st.bad ? ", " + std::to_string(st.bad) + " line(s) not understood" : std::string());
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(u8(S.ownFile).parent_path(), ec);
    const std::string tmp = S.ownFile + ".part";
    { std::ofstream o(u8(tmp), std::ios::binary); o << merged; if (!o) { S.importMsg = "could not write " + S.ownFile; return; } }
    std::filesystem::rename(u8(tmp), u8(S.ownFile), ec);
    if (ec) { S.importMsg = "could not write " + S.ownFile + ": " + ec.message(); return; }
    char b[200];
    snprintf(b, sizeof b, "imported %d new and %d updated transmitter(s) into your own list", st.added, st.replaced);
    S.importMsg = b;
    if (st.noPosition) S.importMsg += ", " + std::to_string(st.noPosition) + " skipped without a position";
    if (st.bad) S.importMsg += ", " + std::to_string(st.bad) + " line(s) not understood";
    load(); S.fitted = false;
}

// the country of the shared channel list (the Scan tab), else the one the setup wizard stored, else none yet
void adoptKnownCountry() {
    if (S.country[0]) return;
    std::string c = plat::prefs().getS("dabTiiCountry", plat::prefs().getS("scanDbCountry", ""));
    if (c.size() != 2) c = plat::prefs().getS("locCountry", "");
    if (c.size() == 2) { S.country[0] = c[0]; S.country[1] = c[1]; }
}

bool isCountry(const std::string& c) { return c.size() == 2 && c[0] >= 'A' && c[0] <= 'Z' && c[1] >= 'A' && c[1] <= 'Z'; }

void load() {
    S.loaded = true;
    S.sites.clear();
    S.rowsPerEid.clear();
    adoptKnownCountry();
    if (S.extPath.empty()) S.extPath = plat::prefs().getS("dabTiiExternal", "");
    int bad = 0, shared = 0, files = 0;
    S.extRows = S.extPath.empty() ? 0 : readExternal(S.extPath);
    // every shared list kept from before (the country set below, the countries of the ensembles heard): the one set below last, so it wins
    std::error_code ec;
    std::vector<std::string> ccs;
    for (const auto& e : std::filesystem::directory_iterator(u8(plat::cacheDir()), ec)) {
        const std::string n = e.path().filename().string();
        if (n.size() == 14 && n.compare(0, 8, "dab-tii-") == 0 && n.compare(10, 4, ".csv") == 0 && isCountry(n.substr(8, 2))) ccs.push_back(n.substr(8, 2));
    }
    std::stable_sort(ccs.begin(), ccs.end(), [](const std::string& x, const std::string& y) { return (x == S.country) < (y == S.country); });
    for (const auto& cc : ccs) {
        const int n = readList(sharedFile(cc), Src::Shared, bad);
        if (n < 0) continue;
        files++; shared += n;
        auto& L = S.lists[cc];
        L.rows = n;
        if (L.state != 4) L.state = 1;
    }
    S.ownFile = plat::dataDir() + "/dab-transmitters.csv";
    const int own = readList(S.ownFile, Src::Own, bad);
    char b[240];
    snprintf(b, sizeof b, tr("%d transmitter position(s) known: %d from %d shared list(s), %d from your own file, %d from qt-dab's list%s"), (int)S.sites.size(), shared, files,
             own > 0 ? own : 0, S.extRows > 0 ? S.extRows : 0, bad ? ", some lines not understood" : "");
    S.loadMsg = (files == 0 && own < 0 && S.extRows <= 0) ? std::string(tr("no transmitter list yet")) : std::string(b);
}

// fetches dab-tii/<country>.csv from the repository into the cache, in the background (curl, as the updater and the map tiles). A failed
// fetch is either a country nobody has shared a transmitter for yet (404) or no network: curl says the same for both, so a file that is
// always there (the repository's README) tells them apart.
void startDownload(const std::string& cc) {
    if (!isCountry(cc) || (S.dl && S.dl->state == 0)) return;
    auto job = std::make_shared<Download>();
    job->country = cc;
    S.dl = job;
    S.lists[cc].state = 4;
    dect2::namedThread("onair-tii", [job] {
        const std::string tmp = sharedFile(job->country) + ".part";
        const bool ok = plat::fetchUrl(repoUrl() + "/dab-tii/" + job->country + ".csv", tmp, "OnAir/" ONAIR_VERSION);
        std::error_code ec;
        if (ok) { std::filesystem::rename(u8(tmp), u8(sharedFile(job->country)), ec); job->state = ec ? 3 : 1; return; }
        std::filesystem::remove(u8(tmp), ec);
        const std::string probe = plat::cacheDir() + "/dab-tii-probe.part";
        const bool reach = plat::fetchUrl(repoUrl() + "/README.md", probe, "OnAir/" ONAIR_VERSION);
        std::filesystem::remove(u8(probe), ec);
        job->state = reach ? 2 : 3;
    }).detach();
}

// a country's shared list wanted (the one set below, or an ensemble's): fetched when it is not here or older than a day, once per session
void want(const std::string& cc) {
    if (!isCountry(cc)) return;
    auto& L = S.lists[cc];
    static std::set<std::string> asked;
    if (!asked.insert(cc).second) return;
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(u8(sharedFile(cc)), ec);
    if (!ec && std::filesystem::file_time_type::clock::now() - t < std::chrono::hours(24)) { if (L.state == 0) L.state = 1; return; }
    if (std::find(S.queue.begin(), S.queue.end(), cc) == S.queue.end()) S.queue.push_back(cc);
}

// every frame: a download that ended is taken in, the next one started (only once the Map tab has been opened: no network before that)
void pump() {
    if (!S.loaded) return;
    if (S.dl && S.dl->state != 0) {
        S.lists[S.dl->country].state = S.dl->state;
        S.dl.reset();
        load(); S.fitted = false;
    }
    if (!S.dl && !S.queue.empty()) { const std::string cc = S.queue.front(); S.queue.pop_front(); startDownload(cc); }
}

// decimal degrees as people type them: a point or a comma ("48,80"), a leading + or -, no letters; false when it is not a number or out of range
bool parseDegrees(const char* t, double lim, double& v) {
    std::string x(t);
    for (auto& c : x) if (c == ',') c = '.';
    x.erase(0, x.find_first_not_of(" \t"));
    if (x.empty()) return false;
    char* end = nullptr;
    v = std::strtod(x.c_str(), &end);
    while (end && (*end == ' ' || *end == '\t')) end++;
    return end && !*end && std::isfinite(v) && std::fabs(v) <= lim;
}

std::string countryOf(int eid) {   // the ensemble's country, else the one set below
    auto it = S.eidCountry.find(eid);
    if (it != S.eidCountry.end() && !it->second.empty()) return it->second;
    return S.country[0] ? std::string(S.country) : std::string();
}

// the issue of the repository's submission form, filled in completely: the list refuses a transmitter without its position
void submitTransmitter(int eid, int mainId, int subId, double mhz, double lat, double lon, std::string site) {
    for (auto& c : site) if (c == ',' || c == '"' || c == '\n' || c == '\r' || c == '|' || (unsigned char)c < 32) c = ' ';
    char row[240];
    snprintf(row, sizeof row, "%04X,%d,%d,%.5f,%.5f,%s,,", eid & 0xFFFF, mainId, subId, lat, lon, site.c_str());
    std::string r = row;
    if (mhz > 0) { char m[24]; snprintf(m, sizeof m, "%.3f", mhz); r += m; }
    std::string cc = countryOf(eid);
    if (cc.empty()) cc = "XX";
    const std::string body = "mode: dab-tii\ncountry: " + cc + "\n\n```csv\neid,main,sub,lat,lon,site,power_kw,channel_mhz\n" + r + "\n```\n";
    const std::string title = "[submit] dab-tii " + cc;
    dect2::openUrl("https://github.com/adswill/OnAir-channels/issues/new?template=submit.yml&title=" + dect2::chdb::urlEncode(title) +
                   "&data=" + dect2::chdb::urlEncode(body));
}

// the window that asks where the transmitter stands; called every frame
void submitDialog() {
    auto& P = S.pend;
    if (P.want) { ImGui::OpenPopup("Submit a transmitter"); P.want = false; }
    if (!ImGui::BeginPopupModal("Submit a transmitter", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("Transmitter %04X, MainId %d / SubId %d", P.eid & 0xFFFF, P.mainId, P.subId);
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
    ImGui::TextDisabled("%s", tr("Optional: only if you know where this transmitter stands. It puts it on the map for everyone who hears it."));
    ImGui::TextDisabled("Where the TRANSMITTER stands (not where you are), in decimal degrees, e.g. 51.50134 and -0.14190. A map or the broadcaster's "
                        "site list gives it. Your own position is never sent.");
    ImGui::TextDisabled("%s", tr("Only positions you know yourself or from a source that allows it: not from qt-dab's or FMLIST's list."));
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10); ImGui::InputText("latitude", P.lat, sizeof P.lat);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10); ImGui::InputText("longitude", P.lon, sizeof P.lon);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18); ImGui::InputText("site name (optional)", P.site, sizeof P.site);
    double la = 0, lo = 0;
    const bool okLa = parseDegrees(P.lat, 90, la), okLo = parseDegrees(P.lon, 180, lo);
    const bool zero = okLa && okLo && std::fabs(la) < 1e-6 && std::fabs(lo) < 1e-6;
    if (P.lat[0] && !okLa) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "latitude: a number between -90 and 90");
    if (P.lon[0] && !okLo) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "longitude: a number between -180 and 180");
    if (zero) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "0, 0 is not a place where a transmitter stands");
    ImGui::TextDisabled("Your browser opens GitHub with this filled in; press Submit new issue there.\nGitHub shows your user name on the issue; the data becomes public domain (CC0).");
    adoptKnownCountry();   // the wizard may have run after the list was first loaded
    const bool noCountry = countryOf(P.eid).empty();
    if (noCountry) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "No country known yet: type it into \"country\" on this tab first.");
    ImGui::BeginDisabled(!(okLa && okLo) || zero || noCountry);
    if (ImGui::Button("  Open GitHub  ")) { submitTransmitter(P.eid, P.mainId, P.subId, P.mhz, la, lo, P.site); ImGui::CloseCurrentPopup(); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// great-circle distance (km) and initial bearing (degrees from north)
void distBearing(double la1, double lo1, double la2, double lo2, double& km, double& deg) {
    const double r = M_PI / 180.0, p1 = la1 * r, p2 = la2 * r, dl = (lo2 - lo1) * r;
    const double a = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    km = 6371.0 * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
    deg = std::fmod(std::atan2(std::sin(dl) * std::cos(p2), std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl)) / r + 360.0, 360.0);
}

const TxSite* siteOf(const Heard& h) { return dabtx::resolve(S.sites, S.air, h.eid, h.mainId, h.subId); }

// why a transmitter heard has no position: one sentence per cause, from the lists' states and what the ensemble sends
std::string whyUnknown(const Heard& h) {
    std::string s;
    const std::string cc = countryOf(h.eid);
    auto ec = S.eidCountry.find(h.eid);
    const bool ccFromAir = ec != S.eidCountry.end() && !ec->second.empty();
    char b[300];
    if (cc.empty()) s += tr("The ensemble's country is not known yet (it comes with FIG 0/9) and none is set below, so no shared list is used.");
    else {
        const auto it = S.lists.find(cc);
        const int st = it == S.lists.end() ? 0 : it->second.state;
        if (st == 2) snprintf(b, sizeof b, tr("Nobody has shared a transmitter for %s yet: the OnAir-channels repository has no list for it."), cc.c_str());
        else if (st == 3) snprintf(b, sizeof b, tr("The shared list for %s could not be downloaded (no network?)."), cc.c_str());
        else if (st == 4 || st == 0) snprintf(b, sizeof b, tr("The shared list for %s is being fetched."), cc.c_str());
        else snprintf(b, sizeof b, tr("The shared list for %s has %d transmitter(s)."), cc.c_str(), it->second.rows);
        s += b;
        if (!ccFromAir) { s += " "; s += tr("(Country from the setting below: the ensemble has not sent its own yet.)"); }
    }
    const auto ne = S.rowsPerEid.find(h.eid);
    s += "\n";
    if (ne == S.rowsPerEid.end()) snprintf(b, sizeof b, tr("No list in use knows any transmitter of this ensemble (EId %04X)."), h.eid & 0xFFFF);
    else snprintf(b, sizeof b, tr("The lists know %d transmitter(s) of this ensemble (EId %04X), but not TII %d/%d."), ne->second, h.eid & 0xFFFF, h.mainId, h.subId);
    s += b;
    s += "\n";
    s += S.airEids.count(h.eid) ? tr("The ensemble sends transmitter positions (FIG 0/22), but none for this transmitter or its MainId.")
                                : tr("The ensemble does not send transmitter positions (FIG 0/22): few networks do, and later editions of the standard dropped it.");
    auto rg = S.regions.find(h.eid);
    if (rg != S.regions.end()) {
        for (const auto& g : rg->second) {
            bool in = false;
            for (const auto& t : g.tii) if (t.first == h.mainId && t.second == h.subId) in = true;
            if (!in) continue;
            const std::string nm = g.label.empty() ? std::to_string(g.id) : g.label;
            if (g.hasBox) snprintf(b, sizeof b, tr("\nThe ensemble names this transmitter's region (FIG 0/11): %s, about %.2f..%.2f N, %.2f..%.2f E."), nm.c_str(),
                                   g.lat, g.lat + g.extLat, g.lng, g.lng + g.extLng);
            else snprintf(b, sizeof b, tr("\nThe ensemble names this transmitter's region (FIG 0/11): %s."), nm.c_str());
            s += b;
            break;
        }
    }
    return s;
}

void helpPopup() {
    if (!ImGui::BeginPopup("##dabtiihelp")) return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34);
    ImGui::TextUnformatted(tr("Where transmitter positions come from"));
    ImGui::Separator();
    ImGui::TextUnformatted(tr(
        "A DAB transmitter sends only its own TII code (MainId / SubId), not where it stands. Some networks also send a table of positions (FIG 0/22), "
        "most do not: then a list is needed that says where each code of the ensemble stands.\n\n"
        "1. The shared list (OnAir-channels, public domain): filled by users. It is downloaded for the country set below and for the country of "
        "every ensemble heard. A country nobody has shared a transmitter for has no list yet.\n\n"
        "2. qt-dab's list: if you use qt-dab, its file .txdata.tii in your home folder (or helpers/tiiFile.zip from the qt-dab repository, unzipped) "
        "can be used with \"import list...\" or the button OnAir shows when it finds the file. It holds FMLIST's data, which is for personal use: "
        "OnAir reads it where it is, does not copy, export or share it, and does not ship it.\n\n"
        "3. Your own file (dab-transmitters.csv, \"open folder\"): CSV, columns eid,main,sub,lat,lon,site,power_kw,channel_mhz. \"export heard\" "
        "writes every transmitter heard in these columns: fill in the positions you know and \"import list...\" it back.\n\n"
        "OnAir does not ship a list for Germany or other countries where no list with a licence that allows it exists (FMLIST, ukwtv.de and "
        "wohnort.org do not allow copying; the Bundesnetzagentur's data has no TII codes). Sharing a transmitter you know (\"share...\") is "
        "optional and makes it work for everyone. See docs/dab-transmitters.md."));
    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
}

} // namespace

// Called every frame (also while the Map tab is not shown, so a band scan fills the list): notes the transmitters of the ensemble on the air.
void dabTiiCollect(App& a) {
    pump();
    const double now = ImGui::GetTime();
    if (now - S.heardAt < 0.5) return;
    S.heardAt = now;
    const DabTelemetry& d = a.rx.dab;
    if (!(a.engine.running() && a.rx.standard == 3 && d.state == 2)) return;
    const DabEnsemble ens = a.engine.dabEnsemble();
    if (!ens.valid) return;
    for (const auto& t : d.tii) {
        Heard& h = S.heard[key(ens.eid, t.mainId, t.subId)];
        h.eid = ens.eid; h.mainId = t.mainId; h.subId = t.subId; h.ens = ens.label; h.mhz = a.freqMhz; h.levelDb = t.levelDb; h.seen = now;
    }
    if (now - S.airAt > 2.0) {   // what the broadcast itself gives: positions (FIG 0/22), regions (FIG 0/11), the country (EId + ECC of FIG 0/9)
        S.airAt = now;
        const dect2::DabFicInfo fi = dabFicInfo(a.engine.dabFic());
        for (const auto& m : fi.transmitters) {
            TxSite s; s.lat = m.lat; s.lon = m.lng; s.src = Src::AirMain; s.mainRef = m.mainId;
            S.air[key(ens.eid, m.mainId, 0)] = s;
        }
        for (const auto& t : fi.subTransmitters) if (t.placed) { TxSite s; s.lat = t.lat; s.lon = t.lng; s.src = Src::Air; S.air[key(ens.eid, t.mainId, t.subId)] = s; }
        if (!fi.transmitters.empty() || !fi.subTransmitters.empty()) S.airEids.insert(ens.eid);
        if (!fi.regions.empty()) S.regions[ens.eid] = fi.regions;
        if (fi.hasEcc) {
            const std::string cc = dect2::rdsCountry(ens.eid, fi.ecc);
            if (!cc.empty()) { S.eidCountry[ens.eid] = cc; if (S.loaded) want(cc); }
        }
    }
}

// the heard transmitters as a file in OnAir's data folder: the list's columns (so it can go straight into dab-transmitters.csv or a submission) plus
// the ensemble and level. Positions from qt-dab's list (not ours to pass on) and approximate ones (a main identifier's reference) are left empty.
static void exportHeard() {
    const std::string path = plat::dataDir() + "/dab-heard-" + std::to_string((long long)time(nullptr)) + ".csv";
    std::ofstream o(u8(path));
    if (!o) { S.exportMsg = "could not write " + path; return; }
    o << "eid,main,sub,lat,lon,site,power_kw,channel_mhz,ensemble,level_db\n";
    int left = 0;
    for (const auto& kv : S.heard) {
        const Heard& h = kv.second;
        const TxSite* st = siteOf(h);
        if (st && (st->src == Src::External || st->src == Src::AirMain)) { if (st->src == Src::External) left++; st = nullptr; }
        char b[256];
        snprintf(b, sizeof b, "%04X,%d,%d,", h.eid & 0xFFFF, h.mainId, h.subId);
        o << b;
        if (st) { snprintf(b, sizeof b, "%.5f,%.5f,", st->lat, st->lon); o << b; } else o << ",,";
        o << '"' << (st && st->src != Src::Air ? st->name : "") << '"' << ',';
        if (st && st->powerKw > 0) o << st->powerKw;
        snprintf(b, sizeof b, ",%.3f,\"%s\",%.1f\n", h.mhz, h.ens.c_str(), h.levelDb);
        o << b;
    }
    S.exportMsg = "saved " + std::to_string(S.heard.size()) + " transmitter(s) to " + path + " (CSV: eid,main,sub,lat,lon,site,power_kw,channel_mhz,ensemble,level_db)";
    if (left) S.exportMsg += "; " + std::to_string(left) + " " + tr("position(s) from qt-dab's list left out (FMLIST data, not for sharing)");
}

void dabTransmittersTab(App& a) {
    if (!S.loaded) {
        load();
        if (S.extPath.empty()) {   // qt-dab's list in the home folder: offered, never used without a click
            const std::string h = homeDir();
            std::error_code ec;
            for (const char* n : {"/.txdata.tii", "/.txdata.ti"})
                if (!h.empty() && std::filesystem::is_regular_file(u8(h + n), ec)) { S.extFound = h + n; break; }
        }
        if (S.country[0]) want(S.country);
        for (const auto& kv : S.eidCountry) want(kv.second);
        pump();
    }
    // the antenna position: the one the map modes share (set in the ADS-B tuner, or below)
    if (!S.haveRef) {
        S.refLat = plat::prefs().getD("adsbLat", 0); S.refLon = plat::prefs().getD("adsbLon", 0);
        S.haveRef = S.refLat != 0 || S.refLon != 0;
    }
    dabTiiCollect(a);
    const DabTelemetry& d = a.rx.dab;
    const bool on = a.engine.running() && d.state == 2;
    const double now = ImGui::GetTime();

    { ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("Every transmitter of a DAB network sends its own identification (TII: MainId / SubId) in the null symbol. OnAir lists every one it hears, on every channel "
                          "(run a band scan to fill the list), and puts them on the map where a transmitter list or the ensemble itself (FIG 0/22) gives the position. Click a marker or a row to tune that ensemble.");
      ImGui::PopTextWrapPos(); }

    struct Row { const Heard* h; const TxSite* site; double km = -1, deg = 0; bool live; };
    std::vector<Row> rows;
    for (const auto& kv : S.heard) {
        Row r{&kv.second, siteOf(kv.second), -1, 0, now - kv.second.seen < 3.0};
        if (r.site && S.haveRef) distBearing(S.refLat, S.refLon, r.site->lat, r.site->lon, r.km, r.deg);
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.h->mhz != y.h->mhz ? x.h->mhz < y.h->mhz : x.h->levelDb > y.h->levelDb; });
    int located = 0;
    for (const auto& r : rows) if (r.site) located++;
    auto tuneTo = [&](const Heard& h) { dabTuneMhz(a, h.mhz); };
    auto siteName = [&](const Row& r) -> std::string {
        if (r.site->src == Src::AirMain) { char b[96]; snprintf(b, sizeof b, tr("near MainId %d's reference point"), r.site->mainRef); return b; }
        if (r.site->src == Src::Air) return tr("position sent by the ensemble");
        return r.site->name.empty() ? std::string("?") : r.site->name;
    };

    // the full-screen view: the map takes the whole height, the table and the controls stand beside it; otherwise the map is above them
    const bool full = adsbmap::fullActive();
    const ImVec2 av = ImGui::GetContentRegionAvail();
    const float sideW = full ? std::max(340.f * gUi, av.x * 0.36f) : av.x;
    if (located > 0 || S.haveRef) {
        const ImVec2 size(full ? av.x - sideW - 8 * gUi : av.x, full ? av.y : std::max(160.f * gUi, av.y * 0.55f));
        // centre once: on what was found, else on the antenna until something is found. It must not be set again every frame, or the
        // + / - buttons and the wheel do nothing (they did while no transmitter had a position)
        if (!S.fitted && located > 0) {
            double la = 0, lo = 0; int n = 0;
            for (const auto& r : rows) if (r.site) { la += r.site->lat; lo += r.site->lon; n++; }
            if (S.haveRef) { la += S.refLat; lo += S.refLon; n++; }
            S.map.lat = la / n; S.map.lon = lo / n; S.map.zoom = 8; S.fitted = true;
        } else if (!S.fitted && S.haveRef) {
            if (S.atLat != S.refLat || S.atLon != S.refLon) { S.map.lat = S.refLat; S.map.lon = S.refLon; S.map.zoom = 8; S.atLat = S.refLat; S.atLon = S.refLon; }
        }
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        adsbmap::draw(S.map, size);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
        const ImVec2 me = adsbmap::project(S.refLat, S.refLon);
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool inMap = ImGui::IsMouseHoveringRect(p0, ImVec2(p0.x + size.x, p0.y + size.y));
        const Row* hit = nullptr;
        float hitD = 12.f * gUi;
        for (const auto& r : rows) {
            if (!r.site) continue;
            const ImVec2 q = adsbmap::project(r.site->lat, r.site->lon);
            const float rel = std::max(0.f, std::min(1.f, r.h->levelDb / 40.f));   // brighter and larger for a stronger transmitter
            const bool approx = r.site->src == Src::AirMain;
            if (S.haveRef && r.live && !approx) dl->AddLine(me, q, IM_COL32(120, 200, 255, (int)(60 + 120 * rel)), 1.5f);
            const ImU32 col = r.live ? IM_COL32(255, 170, 60, (int)(140 + 115 * rel)) : IM_COL32(150, 170, 200, (int)(120 + 100 * rel));
            if (approx) dl->AddCircle(q, (6.f + 4.f * rel) * gUi, col, 0, 1.5f * gUi);   // a ring: somewhere around here
            else dl->AddCircleFilled(q, (4.f + 4.f * rel) * gUi, col);
            char lab[160];
            snprintf(lab, sizeof lab, "%s  %d/%d  %.3f", r.site->src == Src::Air || approx || r.site->name.empty() ? r.h->ens.c_str() : r.site->name.c_str(), r.h->mainId, r.h->subId, r.h->mhz);
            dl->AddText(ImVec2(q.x + 8 * gUi, q.y - 7 * gUi), IM_COL32(255, 255, 255, 230), lab);
            const float dd = std::hypot(mouse.x - q.x, mouse.y - q.y);
            if (inMap && dd < hitD) { hitD = dd; hit = &r; }
        }
        dl->PopClipRect();
        if (hit) {
            char dist[32] = "";
            if (hit->km >= 0) snprintf(dist, sizeof dist, ", %s%.0f km", hit->site->src == Src::AirMain ? "~" : "", hit->km);
            ImGui::SetTooltip("%s  %.3f MHz\nTII %d / %d, %.0f dB above the noise%s\n%s\nclick to tune this ensemble", hit->h->ens.c_str(), hit->h->mhz, hit->h->mainId, hit->h->subId,
                              hit->h->levelDb, dist, siteName(*hit).c_str());
            if (ImGui::IsMouseReleased(0) && ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 16.f) tuneTo(*hit->h);
        }
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 6 * gUi, p0.y + 6 * gUi));
        if (ImGui::Button("+", ImVec2(26 * gUi, 0))) { S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); S.fitted = true; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in"));
        ImGui::SameLine(0, 4 * gUi);
        if (ImGui::Button("-", ImVec2(26 * gUi, 0))) { S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); S.fitted = true; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out"));
        ImGui::SameLine(0, 4 * gUi);
        if (ImGui::Button("fit")) { S.fitted = false; S.atLat = 1e9; }
        ImGui::SameLine(0, 8 * gUi);
        adsbmap::tileControls("never the position itself");
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y - ImGui::GetTextLineHeightWithSpacing()));
        if (rows.empty()) adsbmap::legend(size.x, on ? "listening for transmitter identification..." : "start the receiver on a DAB channel");
        else if (located == 0) adsbmap::legend(size.x, "%s", tr("no position known yet for the transmitters heard: the reasons are below the map"));
        if (full) { ImGui::SetCursorScreenPos(ImVec2(p0.x + size.x + 8 * gUi, p0.y)); ImGui::BeginChild("##tiiside", ImVec2(sideW, av.y), false); }
        else ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y + 4 * gUi));
    } else if (full) ImGui::BeginChild("##tiiside", ImVec2(sideW, av.y), false);

    // ---- the transmitters without a position: why, and what can be done (each ensemble once)
    if (located < (int)rows.size()) {
        std::map<int, const Heard*> ensembles;
        for (const auto& r : rows) if (!r.site && !ensembles.count(r.h->eid)) ensembles[r.h->eid] = r.h;
        ImGui::PushTextWrapPos(0);
        char hb[160];
        snprintf(hb, sizeof hb, tr("%d of %d transmitter(s) heard have no known position yet."), (int)rows.size() - located, (int)rows.size());
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", hb);
        ImGui::SameLine();
        if (ImGui::SmallButton("?##dabtiiq")) ImGui::OpenPopup("##dabtiihelp");
        helpPopup();
        int shown = 0;
        for (const auto& kv : ensembles) {
            if (shown++ == 3) { ImGui::TextDisabled("..."); break; }
            const Heard& h = *kv.second;
            const std::string cc = countryOf(h.eid);
            ImGui::BulletText("%s (EId %04X%s%s)", h.ens.c_str(), h.eid & 0xFFFF, cc.empty() ? "" : ", ", cc.c_str());
            ImGui::Indent();
            ImGui::TextDisabled("%s", whyUnknown(h).c_str());
            ImGui::Unindent();
        }
        ImGui::TextUnformatted(tr("What you can do:"));
        if (!S.extFound.empty() && S.extPath.empty()) {
            ImGui::BulletText("%s", tr("qt-dab's transmitter list was found on this computer:"));
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("use it"))) { S.extPath = S.extFound; plat::prefs().setS("dabTiiExternal", S.extPath); load(); S.fitted = false; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", S.extFound.c_str(), tr("FMLIST's data, for personal use: OnAir reads it where it is and never copies, exports or shares it."));
        } else if (S.extPath.empty()) ImGui::BulletText("%s", tr("Have a transmitter list (qt-dab's .txdata.tii, or a CSV)? \"import list...\" below."));
        ImGui::BulletText("%s", tr("Know where a transmitter stands? Put it in your own list (\"export heard\", fill in, \"import list...\"), or share it with everyone: \"share...\" on its row (optional)."));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
    }

    // ---- the table: every transmitter heard (ids, ensemble, channel, level), with the position where one is known
    if (rows.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", !a.engine.running() ? "start the receiver on a DAB channel, or run a band scan on the Scan tab" :
                            d.tiiFrames < 30 ? "listening for transmitter identification..." : "no transmitter identification heard (yet): TII needs a decent signal and is not sent by every network");
        ImGui::PopTextWrapPos();
    } else if (ImGui::BeginTable("##dabtii", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX,
                               ImVec2(0, std::max(5 * ImGui::GetTextLineHeightWithSpacing(), ImGui::GetContentRegionAvail().y - 4 * ImGui::GetTextLineHeightWithSpacing())))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("TII"); ImGui::TableSetupColumn("Ensemble"); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("Level");
        ImGui::TableSetupColumn("Transmitter", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Distance"); ImGui::TableSetupColumn("Bearing"); ImGui::TableSetupColumn("Power");
        ImGui::TableHeadersRow();
        for (const auto& r : rows) {
            const Heard& h = *r.h;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((int)key(h.eid, h.mainId, h.subId));
            char id[24];
            snprintf(id, sizeof id, "%02d / %02d", h.mainId, h.subId);
            ImGui::PushFont(a.mono, 0);
            if (ImGui::Selectable(id, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) tuneTo(h);
            ImGui::PopFont();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("MainId %d (pattern), SubId %d (comb)\n%.1f dB above the noise of the null symbol\nclick to tune %s", h.mainId, h.subId, h.levelDb, h.ens.c_str());
            ImGui::TableNextColumn(); if (r.live) ImGui::TextUnformatted(h.ens.c_str()); else ImGui::TextDisabled("%s", h.ens.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.3f", h.mhz);
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", h.levelDb);
            ImGui::TableNextColumn();
            if (r.site) {
                ImGui::TextUnformatted(siteName(r).c_str());
                if (ImGui::IsItemHovered()) {
                    if (r.site->src == Src::Own) ImGui::SetTooltip("%s", tr("from your own file"));
                    else if (r.site->src == Src::External) ImGui::SetTooltip("%s", tr("from qt-dab's list (FMLIST data, personal use)"));
                    else if (r.site->src == Src::Shared) ImGui::SetTooltip("%s", tr("from the shared list (OnAir-channels, public domain)"));
                    else if (r.site->src == Src::Air) ImGui::SetTooltip("%.4f, %.4f\n%s", r.site->lat, r.site->lon, tr("sent by the ensemble itself (FIG 0/22), no site name"));
                    else ImGui::SetTooltip("%.4f, %.4f\n%s", r.site->lat, r.site->lon,
                                           tr("Approximate: the ensemble sends the reference point of this MainId (FIG 0/22) but not this SubId's own position.\nThe distance is to that point."));
                }
            } else {
                ImGui::TextDisabled("%s", tr("position unknown"));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", whyUnknown(h).c_str());
                ImGui::SameLine();
                // the built-in test signal's transmitters are made up: they must not end up in the public list
                const bool demo = a.devices[a.devIdx].kind == DeviceInfo::Synthetic || a.devices[a.devIdx].kind == DeviceInfo::File;
                ImGui::BeginDisabled(demo);
                if (ImGui::SmallButton(tr("share..."))) { auto& P = S.pend; P = State::Pending(); P.want = true; P.eid = h.eid; P.mainId = h.mainId; P.subId = h.subId; P.mhz = h.mhz; }
                ImGui::EndDisabled();
                if (demo && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Only transmitters received with a radio can be submitted (the test signal's and recordings' are not real ones).");
                else if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Optional. If you know where this transmitter stands, share it: OnAir asks for its latitude and longitude,\n"
                                                                            "then your browser opens GitHub with everything filled in; press Submit there.\n"
                                                                            "GitHub shows your user name on the issue; the data becomes public domain (CC0)."));
            }
            ImGui::PopID();
            const bool approx = r.site && r.site->src == Src::AirMain;
            ImGui::TableNextColumn(); if (r.km >= 0) ImGui::Text("%s%.0f km", approx ? "~" : "", r.km); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (r.km >= 0) ImGui::Text("%s%.0f°", approx ? "~" : "", r.deg); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (r.site && r.site->powerKw > 0) ImGui::Text("%.1f kW", r.site->powerKw); else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }

    {   // the network's paths and which transmitter each is: needs the antenna position and the transmitters' positions
        const DabTelemetry& d = a.rx.dab;
        const auto paths = dect2::dabCirPaths(d.cir);
        if (a.rx.standard == 3 && d.state == 2 && paths.size() >= 2 && ImGui::TreeNode(trw("Transmitter delays"))) {
            std::vector<dect2::DabSfnTx> txs;
            for (const auto& r : rows) {
                dect2::DabSfnTx t; t.mainId = r.h->mainId; t.subId = r.h->subId; t.km = r.site && r.site->src != Src::AirMain ? r.km : -1; t.bearingDeg = r.deg; t.tiiDb = r.h->levelDb;
                txs.push_back(t);
            }
            const auto s = dect2::dabSfnSolve(paths, txs);
            for (const auto& p : paths) ImGui::TextDisabled("%+.1f us  %.0f dB  (%.1f km)", p.delayUs, p.levelDb, std::fabs(p.delayUs) * 0.2998);
            if (s.outsideGuard) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.3f, 1), "%s", tr("A path arrives beyond the guard interval: it interferes. A directional antenna away from it helps."));
            for (const auto& r : s.rows) if (r.matched) ImGui::Text("%02d/%02d  %.0f km  %.0f deg  %s %+.1f us", r.tx.mainId, r.tx.subId, r.tx.km, r.tx.bearingDeg, tr("path"), r.path.delayUs);
            if (s.refTx >= 0) ImGui::TextDisabled("%s %.0f deg", tr("Strongest path comes from bearing"), s.rows[(size_t)s.refTx].tx.bearingDeg);
            ImGui::TreePop();
        }
    }
    submitDialog();
    // ---- the lists and the antenna position
    ImGui::Spacing();
    {
        std::string st;
        for (const auto& kv : S.lists) {
            const char* w = kv.second.state == 1 ? nullptr : kv.second.state == 2 ? tr("none yet") : kv.second.state == 3 ? tr("not reachable") : kv.second.state == 4 ? tr("downloading") : nullptr;
            if (!w && kv.second.state != 1) continue;
            st += (st.empty() ? "" : ", ") + kv.first + ": " + (w ? std::string(w) : std::to_string(kv.second.rows));
        }
        if (!st.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s %s", tr("shared lists:"), st.c_str()); ImGui::PopTextWrapPos(); }
    }
    ImGui::TextDisabled("%s", S.loadMsg.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The shared list: github.com/adswill/OnAir-channels (dab-tii/<country>.csv), crowd-sourced.\n"
                                                  "Your own additions: %s\n(columns eid,main,sub,lat,lon,site,power_kw,channel_mhz; eid in hex; # starts a comment)", S.ownFile.c_str());
    if (!S.extPath.empty()) {
        ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.extMsg.c_str()); ImGui::PopTextWrapPos();
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("stop using it"))) { S.extPath.clear(); S.extMsg.clear(); plat::prefs().setS("dabTiiExternal", ""); load(); S.fitted = false; }
    }
    flowNext(10 * gUi);   // the row wraps in a narrow window
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("country"); ImGui::SameLine(0, 4 * gUi);
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("WWW").x + 2 * ImGui::GetStyle().FramePadding.x);
    if (ImGui::InputText("##dabtiicc", S.country, sizeof S.country, ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_EnterReturnsTrue) && S.country[0] && S.country[1]) {
        plat::prefs().setS("dabTiiCountry", S.country); load(); want(S.country); pump();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The country of the shared transmitter list (ISO code, e.g. GB, DE, NO); Enter downloads it.\n"
                                                           "The lists of the countries of the ensembles heard are fetched as well."));
    flowNext(6 * gUi);
    ImGui::BeginDisabled(!S.country[0] || (S.dl && S.dl->state == 0));
    if (ImGui::SmallButton("download")) { S.queue.push_front(S.country); pump(); }
    ImGui::EndDisabled();
    flowNext(6 * gUi);
    if (ImGui::SmallButton("reload")) { load(); S.fitted = false; }
    flowNext(6 * gUi);
    if (ImGui::SmallButton("open folder")) dect2::openUrl(plat::dataDir());
    flowNext(6 * gUi);
    ImGui::BeginDisabled(S.heard.empty());
    if (ImGui::SmallButton("export heard")) exportHeard();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Save every transmitter heard (all channels) as a CSV file in OnAir's data folder, to share or to add to the list");
    flowNext(6 * gUi);
    if (ImGui::SmallButton("import list...")) { const std::string p = plat::openFileDialog(); if (!p.empty()) importList(p); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", tr("Add the transmitters of a CSV file to your own list (columns eid,main,sub,lat,lon,site,power_kw,channel_mhz, eid in hex):\n"
                                                              "an export of heard transmitters with the positions filled in, or a list someone shares; rows without a position are skipped.\n"
                                                              "qt-dab's .txdata.tii is used where it is instead (FMLIST data, personal use)."), S.ownFile.c_str());
    flowNext(6 * gUi);
    if (ImGui::SmallButton("clear heard")) S.heard.clear();
    flowNext(6 * gUi);
    if (ImGui::SmallButton("?##dabtiiq2")) ImGui::OpenPopup("##dabtiihelp");
    helpPopup();
    flowEnd();
    if (!S.exportMsg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.exportMsg.c_str()); ImGui::PopTextWrapPos(); }
    if (!S.importMsg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.importMsg.c_str()); ImGui::PopTextWrapPos(); }
    if (!S.haveRef) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Distances and bearings: set the antenna position in the ADS-B tuner."); ImGui::PopTextWrapPos(); }
    if (full) ImGui::EndChild();
}
