// DAB: the Map tab (the transmitters). The receiver names the transmitters of the network it hears by their TII codes (MainId / SubId, EN 300 401
// clause 14.8); with a list of where each transmitter of an ensemble stands they go on the map, with the distance and bearing from the
// antenna. The list comes from the crowd-sourced OnAir-channels repository (dab-tii/<country>.csv, downloaded and kept for offline use) and
// from the user's own file in OnAir's data folder (dab-transmitters.csv, the same columns; it wins where both name a transmitter):
//   eid,main,sub,lat,lon,site,power_kw,channel_mhz[,reports,first_seen,last_seen]      (eid in hex; a line starting with # is a comment)
// TII codes are unique within an ensemble, not worldwide: a transmitter is found by (ensemble id, MainId, SubId). A transmitter that is heard but
// not in the list can be submitted from here: the app fills in the ids, the user adds where it stands (the issue form of the repository).
// Another list (an export of heard transmitters with the positions filled in, a regional list) can be imported into the own file; the
// format is in docs/dab-transmitters.md, the parsing in dab_txlist.h. Without a list, the ensemble's own FIG 0/22 positions are used.
#include "dect2/common/thread_name.h"
#include "app.h"
#include "../adsb/adsb_map.h"
#include "dect2/channels/channel_db.h"
#include "dect2/engine/updater.h"
#include "dect2/dab/dab_fig.h"
#include "dab_txlist.h"
#include <algorithm>
#include <atomic>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>

namespace {

struct TxSite { double lat = 0, lon = 0, powerKw = 0, channelMhz = 0; std::string name; bool own = false; };

struct Heard { int eid = 0, mainId = 0, subId = 0; std::string ens; double mhz = 0; float levelDb = 0; double seen = 0; };

struct Download { std::atomic<int> state{0}; std::string country; };   // 0 working, 1 done, 2 failed

struct State {
    bool loaded = false;
    std::map<uint64_t, TxSite> sites;   // key: eid << 16 | main << 8 | sub
    std::string ownFile, loadMsg;
    char country[3] = {0, 0, 0};
    std::shared_ptr<Download> dl;
    std::string dlMsg;
    adsbmap::View map;
    bool fitted = false;
    double refLat = 0, refLon = 0;
    bool haveRef = false;
    // the positions the ensemble itself broadcasts (FIG 0/22), refreshed every couple of seconds: key as in sites, own/shared lists win
    std::map<uint64_t, TxSite> air;
    double airAt = -10, heardAt = -10;
    // every transmitter heard since the program started, over all channels (a band scan fills it): key as in sites
    std::map<uint64_t, Heard> heard;
    std::string exportMsg, importMsg;
    // the transmitter being submitted: asked here (position and site), because the issue form of the site does not keep what is typed into it
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

void load();

// reads one list into S.sites; returns the transmitters read (lines not understood counted in `bad`), -1 when there is no such file
int readList(const std::string& path, bool own, int& bad) {
    std::ifstream in(u8(path));
    if (!in) return -1;
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        dabtx::Row r;
        const dabtx::Line t = dabtx::parse(line, r);
        if (t == dabtx::Line::Bad) { bad++; continue; }
        if (t != dabtx::Line::Ok) continue;   // a comment, the header, a row without a position (an export or a submission still to fill in)
        TxSite st;
        st.lat = r.lat; st.lon = r.lon; st.name = r.site; st.powerKw = r.powerKw; st.channelMhz = r.channelMhz; st.own = own;
        S.sites[key(r.eid, r.mainId, r.subId)] = st;   // the own file is read last: it wins
        n++;
    }
    return n;
}

// another list into the own file (a friend's export with the positions filled in, a regional list someone keeps): see dabtx::merge
void importList(const std::string& path) {
    auto slurp = [](const std::string& p, std::string& out) {
        std::ifstream in(u8(p), std::ios::binary);
        if (!in) return false;
        std::ostringstream ss; ss << in.rdbuf(); out = ss.str(); return true;
    };
    std::string imp, own;
    if (!slurp(path, imp)) { S.importMsg = "could not read " + path; return; }
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

void load() {
    S.loaded = true;
    S.sites.clear();
    adoptKnownCountry();
    int bad = 0, shared = -1;
    if (S.country[0]) shared = readList(sharedFile(S.country), false, bad);
    S.ownFile = plat::dataDir() + "/dab-transmitters.csv";
    const int own = readList(S.ownFile, true, bad);
    char b[200];
    snprintf(b, sizeof b, "%s%d transmitter(s)%s%s", shared >= 0 ? "shared list " : "", (int)S.sites.size(), own > 0 ? " (with your own file)" : "",
             bad ? ", some lines not understood" : "");
    S.loadMsg = (shared < 0 && own < 0) ? std::string("no transmitter list yet") : std::string(b);
}

// fetches dab-tii/<country>.csv from the repository into the cache, in the background (curl, as the updater and the map tiles)
void startDownload() {
    if (!S.country[0] || (S.dl && S.dl->state == 0)) return;
    auto job = std::make_shared<Download>();
    job->country = S.country;
    S.dl = job;
    S.dlMsg = "downloading the shared list...";
    dect2::namedThread("onair-tii", [job] {
        const std::string tmp = sharedFile(job->country) + ".part";
        const bool ok = plat::fetchUrl(repoUrl() + "/dab-tii/" + job->country + ".csv", tmp, "OnAir/" ONAIR_VERSION);
        std::error_code ec;
        if (ok) std::filesystem::rename(u8(tmp), u8(sharedFile(job->country)), ec);
        else std::filesystem::remove(u8(tmp), ec);
        job->state = ok && !ec ? 1 : 2;
    }).detach();
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

// the issue of the repository's submission form, filled in completely: the list refuses a transmitter without its position
void submitTransmitter(int eid, int mainId, int subId, double mhz, double lat, double lon, std::string site) {
    for (auto& c : site) if (c == ',' || c == '"' || c == '\n' || c == '\r' || c == '|' || (unsigned char)c < 32) c = ' ';
    char row[240];
    snprintf(row, sizeof row, "%04X,%d,%d,%.5f,%.5f,%s,,", eid & 0xFFFF, mainId, subId, lat, lon, site.c_str());
    std::string r = row;
    if (mhz > 0) { char m[24]; snprintf(m, sizeof m, "%.3f", mhz); r += m; }
    const std::string cc = S.country[0] ? std::string(S.country) : std::string("XX");
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
    ImGui::TextDisabled("Where the TRANSMITTER stands (not where you are), in decimal degrees, e.g. 51.50134 and -0.14190. A map or the broadcaster's "
                        "site list gives it. Your own position is never sent.");
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
    const bool noCountry = !S.country[0];
    if (noCountry) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "No country chosen yet: pick one on the Scan tab (Shared data) first.");
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

const TxSite* siteOf(const Heard& h) {   // the list first, then what the ensemble itself broadcasts (the sub identifier's own position, else the main one's)
    auto it = S.sites.find(key(h.eid, h.mainId, h.subId));
    if (it != S.sites.end()) return &it->second;
    auto b = S.air.find(key(h.eid, h.mainId, h.subId));
    if (b == S.air.end()) b = S.air.find(key(h.eid, h.mainId, 0));
    return b != S.air.end() ? &b->second : nullptr;
}

} // namespace

// Called every frame (also while the Map tab is not shown, so a band scan fills the list): notes the transmitters of the ensemble on the air.
void dabTiiCollect(App& a) {
    const double now = ImGui::GetTime();
    if (now - S.heardAt < 0.5) return;
    S.heardAt = now;
    const DabTelemetry& d = a.rx.dab;
    if (!(a.engine.running() && a.rx.standard == 3 && d.state == 2) || d.tii.empty()) return;
    const DabEnsemble ens = a.engine.dabEnsemble();
    if (!ens.valid) return;
    for (const auto& t : d.tii) {
        Heard& h = S.heard[key(ens.eid, t.mainId, t.subId)];
        h.eid = ens.eid; h.mainId = t.mainId; h.subId = t.subId; h.ens = ens.label; h.mhz = a.freqMhz; h.levelDb = t.levelDb; h.seen = now;
    }
    if (now - S.airAt > 2.0) {   // the positions the broadcast itself gives (FIG 0/22: main identifier, and sub identifiers as offsets from it)
        S.airAt = now;
        const DabFicInfo fi = dabFicInfo(a.engine.dabFic());
        for (const auto& m : fi.transmitters) { TxSite s; s.lat = m.lat; s.lon = m.lng; s.name = "broadcast position"; S.air[key(ens.eid, m.mainId, 0)] = s; }
        for (const auto& t : fi.subTransmitters) if (t.placed) { TxSite s; s.lat = t.lat; s.lon = t.lng; s.name = "broadcast position"; S.air[key(ens.eid, t.mainId, t.subId)] = s; }
    }
}

// the heard transmitters as a file in OnAir's data folder: the list's columns (so it can go straight into dab-transmitters.csv or a submission) plus the ensemble and level
static void exportHeard() {
    const std::string path = plat::dataDir() + "/dab-heard-" + std::to_string((long long)time(nullptr)) + ".csv";
    std::ofstream o(u8(path));
    if (!o) { S.exportMsg = "could not write " + path; return; }
    o << "eid,main,sub,lat,lon,site,power_kw,channel_mhz,ensemble,level_db\n";
    for (const auto& kv : S.heard) {
        const Heard& h = kv.second;
        const TxSite* st = siteOf(h);
        char b[256];
        snprintf(b, sizeof b, "%04X,%d,%d,", h.eid & 0xFFFF, h.mainId, h.subId);
        o << b;
        if (st) { snprintf(b, sizeof b, "%.5f,%.5f,", st->lat, st->lon); o << b; } else o << ",,";
        o << '"' << (st && st->name != "broadcast position" ? st->name : "") << '"' << ',';
        if (st && st->powerKw > 0) o << st->powerKw;
        snprintf(b, sizeof b, ",%.3f,\"%s\",%.1f\n", h.mhz, h.ens.c_str(), h.levelDb);
        o << b;
    }
    S.exportMsg = "saved " + std::to_string(S.heard.size()) + " transmitter(s) to " + path + " (CSV: eid,main,sub,lat,lon,site,power_kw,channel_mhz,ensemble,level_db)";
}

void dabTransmittersTab(App& a) {
    if (!S.loaded) {
        load();
        // the shared list: fetched when it is not here yet or older than a day (it changes as people submit)
        if (S.country[0]) {
            std::error_code ec;
            const auto t = std::filesystem::last_write_time(u8(sharedFile(S.country)), ec);
            if (ec || std::filesystem::file_time_type::clock::now() - t > std::chrono::hours(24)) startDownload();
        }
    }
    if (S.dl && S.dl->state != 0) {   // a download ended
        // a country nobody has submitted for yet has no file (404), which curl reports like no network: one message for both
        S.dlMsg = S.dl->state == 1 ? "" : "no shared list for " + S.dl->country + " yet (or no network): submit the first transmitter below";
        S.dl.reset();
        load(); S.fitted = false;
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

    // the full-screen view: the map takes the whole height, the table and the controls stand beside it; otherwise the map is above them
    const bool full = adsbmap::fullActive();
    const ImVec2 av = ImGui::GetContentRegionAvail();
    const float sideW = full ? std::max(340.f * gUi, av.x * 0.36f) : av.x;
    if (located > 0 || S.haveRef) {
        const ImVec2 size(full ? av.x - sideW - 8 * gUi : av.x, full ? av.y : std::max(160.f * gUi, av.y * 0.55f));
        if (!S.fitted && located > 0) {   // centre on what was found, once
            double la = 0, lo = 0; int n = 0;
            for (const auto& r : rows) if (r.site) { la += r.site->lat; lo += r.site->lon; n++; }
            if (S.haveRef) { la += S.refLat; lo += S.refLon; n++; }
            S.map.lat = la / n; S.map.lon = lo / n; S.map.zoom = 8; S.fitted = true;
        } else if (!S.fitted && S.haveRef) { S.map.lat = S.refLat; S.map.lon = S.refLon; S.map.zoom = 8; }
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
            if (S.haveRef && r.live) dl->AddLine(me, q, IM_COL32(120, 200, 255, (int)(60 + 120 * rel)), 1.5f);
            dl->AddCircleFilled(q, (4.f + 4.f * rel) * gUi, r.live ? IM_COL32(255, 170, 60, (int)(140 + 115 * rel)) : IM_COL32(150, 170, 200, (int)(120 + 100 * rel)));
            char lab[160];
            snprintf(lab, sizeof lab, "%s  %d/%d  %.3f", r.site->name.empty() || r.site->name == "broadcast position" ? r.h->ens.c_str() : r.site->name.c_str(), r.h->mainId, r.h->subId, r.h->mhz);
            dl->AddText(ImVec2(q.x + 8 * gUi, q.y - 7 * gUi), IM_COL32(255, 255, 255, 230), lab);
            const float dd = std::hypot(mouse.x - q.x, mouse.y - q.y);
            if (inMap && dd < hitD) { hitD = dd; hit = &r; }
        }
        if (S.haveRef) {   // the antenna: a cross
            const float c = 6.f * gUi;
            dl->AddLine(ImVec2(me.x - c, me.y), ImVec2(me.x + c, me.y), IM_COL32(120, 200, 255, 255), 2.f);
            dl->AddLine(ImVec2(me.x, me.y - c), ImVec2(me.x, me.y + c), IM_COL32(120, 200, 255, 255), 2.f);
        }
        dl->PopClipRect();
        if (hit) {
            char dist[32] = "";
            if (hit->km >= 0) snprintf(dist, sizeof dist, ", %.0f km", hit->km);
            ImGui::SetTooltip("%s  %.3f MHz\nTII %d / %d, %.0f dB above the noise%s\nclick to tune this ensemble", hit->h->ens.c_str(), hit->h->mhz, hit->h->mainId, hit->h->subId,
                              hit->h->levelDb, dist);
            if (ImGui::IsMouseReleased(0) && ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 16.f) tuneTo(*hit->h);
        }
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 6 * gUi, p0.y + 6 * gUi));
        if (ImGui::Button("+", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in"));
        ImGui::SameLine(0, 4 * gUi);
        if (ImGui::Button("-", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out"));
        ImGui::SameLine(0, 4 * gUi);
        if (ImGui::Button("fit")) S.fitted = false;
        ImGui::SameLine(0, 8 * gUi);
        adsbmap::tileControls("never the position itself");
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y - ImGui::GetTextLineHeightWithSpacing()));
        if (rows.empty()) adsbmap::legend(size.x, on ? "listening for transmitter identification..." : "start the receiver on a DAB channel");
        else if (located == 0) adsbmap::legend(size.x, "none of the transmitters heard has a known position yet (see the list)");
        if (full) { ImGui::SetCursorScreenPos(ImVec2(p0.x + size.x + 8 * gUi, p0.y)); ImGui::BeginChild("##tiiside", ImVec2(sideW, av.y), false); }
        else ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y + 4 * gUi));
    } else if (full) ImGui::BeginChild("##tiiside", ImVec2(sideW, av.y), false);

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
            if (r.site) { ImGui::TextUnformatted(r.site->name.empty() ? "?" : r.site->name.c_str());
                if (ImGui::IsItemHovered()) { if (r.site->own) ImGui::SetTooltip("from your own file"); else if (r.site->name == "broadcast position") ImGui::SetTooltip("%.4f, %.4f\nsent by the ensemble itself (FIG 0/22), no site name", r.site->lat, r.site->lon); } }
            else {
                // the built-in test signal's transmitters are made up: they must not end up in the public list
                const bool demo = a.devices[a.devIdx].kind == DeviceInfo::Synthetic || a.devices[a.devIdx].kind == DeviceInfo::File;
                ImGui::BeginDisabled(demo);
                if (ImGui::SmallButton("submit")) { auto& P = S.pend; P = State::Pending(); P.want = true; P.eid = h.eid; P.mainId = h.mainId; P.subId = h.subId; P.mhz = h.mhz; }
                ImGui::EndDisabled();
                if (demo && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Only transmitters received with a radio can be submitted (the test signal's and recordings' are not real ones).");
                else if (ImGui::IsItemHovered()) ImGui::SetTooltip("Position not known yet. Add it to the shared list: OnAir asks where it stands (latitude, longitude),\n"
                                                              "then your browser opens GitHub with everything filled in; press Submit there.\n"
                                                              "GitHub shows your user name on the issue; the data becomes public domain (CC0).");
            }
            ImGui::PopID();
            ImGui::TableNextColumn(); if (r.km >= 0) ImGui::Text("%.0f km", r.km); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (r.km >= 0) ImGui::Text("%.0f°", r.deg); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (r.site && r.site->powerKw > 0) ImGui::Text("%.1f kW", r.site->powerKw); else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }

    submitDialog();
    // ---- the list and the antenna position
    ImGui::Spacing();
    if (!S.dlMsg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.dlMsg.c_str()); ImGui::PopTextWrapPos(); }
    ImGui::TextDisabled("%s", S.loadMsg.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The shared list: github.com/adswill/OnAir-channels (dab-tii/<country>.csv), crowd-sourced.\n"
                                                  "Your own additions: %s\n(columns eid,main,sub,lat,lon,site,power_kw,channel_mhz; eid in hex; # starts a comment)", S.ownFile.c_str());
    flowNext(10 * gUi);   // the row wraps in a narrow window
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("country"); ImGui::SameLine(0, 4 * gUi);
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("WWW").x + 2 * ImGui::GetStyle().FramePadding.x);
    if (ImGui::InputText("##dabtiicc", S.country, sizeof S.country, ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_EnterReturnsTrue) && S.country[0] && S.country[1]) {
        plat::prefs().setS("dabTiiCountry", S.country); load(); startDownload();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The country of the shared transmitter list (ISO code, e.g. GB, DE, NO); Enter downloads it");
    flowNext(6 * gUi);
    ImGui::BeginDisabled(!S.country[0] || (S.dl && S.dl->state == 0));
    if (ImGui::SmallButton("download")) startDownload();
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
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add the transmitters of a CSV file to your own list (%s):\n"
                                                  "an export of heard transmitters with the positions filled in, or a list someone shares.\n"
                                                  "Columns eid,main,sub,lat,lon,site,power_kw,channel_mhz (eid in hex); rows without a position are skipped,\n"
                                                  "a transmitter already in your list is updated.", S.ownFile.c_str());
    flowNext(6 * gUi);
    if (ImGui::SmallButton("clear heard")) S.heard.clear();
    flowEnd();
    if (!S.exportMsg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.exportMsg.c_str()); ImGui::PopTextWrapPos(); }
    if (!S.importMsg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.importMsg.c_str()); ImGui::PopTextWrapPos(); }
    if (!S.haveRef) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Distances and bearings: set the antenna position in the ADS-B tuner."); ImGui::PopTextWrapPos(); }
    if (full) ImGui::EndChild();
}
