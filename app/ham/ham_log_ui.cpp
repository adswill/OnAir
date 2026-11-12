// The Ham view's logbook, awards and contest screens: see ham_ops_ui.h.
#include "ham_ops_ui.h"
#include "ham_station_ui.h"
#include "../platform/platform.h"
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <sstream>

using namespace dect2;

namespace {
std::string readFile(const std::string& p, bool& ok) {
    std::ifstream f(p, std::ios::binary);
    ok = (bool)f;
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
bool writeFile(const std::string& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
    return (bool)f;
}
std::string dateTime(double utc) {
    if (utc <= 0) return "";
    const std::string d = ham::adifDate(utc), t = ham::adifTime(utc);
    return d.substr(0, 4) + "-" + d.substr(4, 2) + "-" + d.substr(6, 2) + " " + t.substr(0, 2) + ":" + t.substr(2, 2);
}
void saveLog() {
    HamOps& o = hamOps();
    std::string err;
    if (!o.log.save(o.logPath, err)) hamToast("Log not saved: " + err);
    o.workedDirty = true;
}

// ---- the QSO edit / add dialog ----
struct EditBuf {
    long long id = 0;   // 0: a new QSO
    char call[20] = {}, grid[10] = {}, mode[12] = {}, submode[12] = {}, mhz[16] = {}, date[10] = {}, timeOn[8] = {}, timeOff[8] = {};
    char rstS[8] = {}, rstR[8] = {}, name[40] = {}, qth[48] = {}, comment[128] = {}, state[8] = {}, stx[24] = {}, srx[24] = {}, pwr[8] = {};
    bool qslS = false, qslR = false, lotw = false;
    ham::LogQso base;   // the fields the dialog does not show are kept from here
};
EditBuf E;
bool editOpen = false;
template <size_t N> void put(char (&b)[N], const std::string& s) { snprintf(b, N, "%s", s.c_str()); }
void beginEdit(const ham::LogQso& q) {
    E = EditBuf();
    E.id = q.id; E.base = q;
    put(E.call, q.call); put(E.grid, q.grid); put(E.mode, q.mode); put(E.submode, q.submode);
    if (q.freqHz > 0) snprintf(E.mhz, sizeof E.mhz, "%.6f", q.freqHz / 1e6);
    if (q.startUtc > 0) { put(E.date, ham::adifDate(q.startUtc)); put(E.timeOn, ham::adifTime(q.startUtc)); }
    if (q.endUtc > 0) put(E.timeOff, ham::adifTime(q.endUtc));
    put(E.rstS, q.rstSent); put(E.rstR, q.rstRcvd); put(E.name, q.name); put(E.qth, q.qth); put(E.comment, q.comment); put(E.state, q.state);
    put(E.stx, q.stx); put(E.srx, q.srx); put(E.pwr, q.txPwr);
    E.qslS = q.qslSent; E.qslR = q.qslRcvd; E.lotw = q.lotwRcvd;
    editOpen = true;
}
ham::LogQso fromEdit() {
    ham::LogQso q = E.base;
    q.id = E.id; q.call = E.call; q.grid = E.grid; q.mode = E.mode; q.submode = E.submode;
    const double f = std::atof(E.mhz) * 1e6;
    if (f != q.freqHz) { q.freqHz = f; q.band = ham::bandNameFor(f); }
    if (q.band.empty() && f > 0) q.band = ham::bandNameFor(f);
    q.startUtc = ham::adifUtc(E.date, E.timeOn);
    q.endUtc = E.timeOff[0] ? ham::adifUtc(E.date, E.timeOff) : q.startUtc;
    if (q.endUtc < q.startUtc) q.endUtc += 86400;
    q.rstSent = E.rstS; q.rstRcvd = E.rstR; q.name = E.name; q.qth = E.qth; q.comment = E.comment; q.state = E.state;
    q.stx = E.stx; q.srx = E.srx; q.txPwr = E.pwr;
    q.qslSent = E.qslS; q.qslRcvd = E.qslR; q.lotwRcvd = E.lotw;
    if (E.id == 0 || q.call != E.base.call) { q.dxcc = 0; q.country.clear(); q.cont.clear(); q.cqz = q.ituz = 0; }   // a new call: looked up again
    return q;
}
void editDialog() {
    HamOps& o = hamOps();
    if (editOpen) { ImGui::OpenPopup("##hamqso"); editOpen = false; }
    ImGui::SetNextWindowSize(ImVec2(560 * gUi, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("##hamqso", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(E.id ? tr("Edit QSO") : tr("Log a QSO"));
    ImGui::Separator();
    const float w = 120 * gUi;
    auto in = [&](const char* label, char* b, size_t n, ImGuiInputTextFlags fl = 0) { ImGui::SetNextItemWidth(w); return ImGui::InputText(label, b, n, fl); };
    const ImGuiInputTextFlags U = ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    in(tr("Call"), E.call, sizeof E.call, U); ImGui::SameLine(); in(tr("Grid"), E.grid, sizeof E.grid, U);
    in(tr("Mode"), E.mode, sizeof E.mode, U); ImGui::SameLine(); in(tr("Submode"), E.submode, sizeof E.submode, U);
    in(tr("MHz"), E.mhz, sizeof E.mhz, ImGuiInputTextFlags_CharsDecimal); ImGui::SameLine(); in(tr("Power W"), E.pwr, sizeof E.pwr, ImGuiInputTextFlags_CharsDecimal);
    in(tr("Date (YYYYMMDD)"), E.date, sizeof E.date, ImGuiInputTextFlags_CharsDecimal); ImGui::SameLine(); in(tr("On (HHMMSS)"), E.timeOn, sizeof E.timeOn, ImGuiInputTextFlags_CharsDecimal);
    in(tr("Off (HHMMSS)"), E.timeOff, sizeof E.timeOff, ImGuiInputTextFlags_CharsDecimal);
    if (ImGui::SmallButton(tr("Now"))) { const double t = (double)time(nullptr); put(E.date, ham::adifDate(t)); put(E.timeOn, ham::adifTime(t)); put(E.timeOff, ham::adifTime(t)); }
    in(tr("RST sent"), E.rstS, sizeof E.rstS); ImGui::SameLine(); in(tr("RST rcvd"), E.rstR, sizeof E.rstR);
    in(tr("Name"), E.name, sizeof E.name); ImGui::SameLine(); in(tr("QTH"), E.qth, sizeof E.qth);
    in(tr("State"), E.state, sizeof E.state, U); ImGui::SameLine();
    ImGui::Checkbox(tr("QSL sent"), &E.qslS); ImGui::SameLine(); ImGui::Checkbox(tr("QSL rcvd"), &E.qslR); ImGui::SameLine(); ImGui::Checkbox("LoTW", &E.lotw);
    if (o.contestOn || E.stx[0] || E.srx[0]) { in(tr("Exch sent"), E.stx, sizeof E.stx); ImGui::SameLine(); in(tr("Exch rcvd"), E.srx, sizeof E.srx); }
    ImGui::SetNextItemWidth(w * 3); ImGui::InputText(tr("Comment"), E.comment, sizeof E.comment);
    // what the call is, and whether it is a dupe
    const ham::LogQso q = fromEdit();
    if (q.call.size() >= 3) {
        const ham::DxccHit h = o.dxcc.lookup(q.call);
        if (h.e) ImGui::TextDisabled("%s, %s, CQ %d, ITU %d", h.e->name.c_str(), h.e->cont.c_str(), h.cqz, h.ituz);
        ham::PathInfo pi;
        if (!q.grid.empty() && ham::pathBetween(o.myGrid, q.grid, pi)) { ImGui::SameLine(); ImGui::TextDisabled("  %.0f km, %.0f deg", pi.km, pi.bearing); }
        const std::string band = q.band.empty() ? ham::bandNameFor(q.freqHz) : q.band;
        const int d = o.log.findDupe(q.call, band, q.mode, true, true, 0, 0, E.id);
        const bool cd = o.contestOn && o.contest.dupe(o.log, q.call, band, q.mode) && E.id == 0;
        if (cd) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", tr("DUPE in this contest"));
        else if (d >= 0) ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "%s %s %s, %s", tr("Worked before on"), o.log.qsos()[(size_t)d].band.c_str(),
                                            o.log.qsos()[(size_t)d].mode.c_str(), dateTime(o.log.qsos()[(size_t)d].startUtc).c_str());
    }
    const bool ok = q.call.size() >= 3 && q.startUtc > 0;
    ImGui::BeginDisabled(!ok);
    if (ImGui::Button(E.id ? tr("Save") : tr("Log it"), ImVec2(100 * gUi, 0)) || (ok && ImGui::IsKeyPressed(ImGuiKey_Enter) && ImGui::GetIO().KeyCtrl)) {
        std::string err;
        if (E.id) { o.log.update(q); saveLog(); hamToast("Edited " + q.call); }
        else if (hamLogQso(q, err)) hamToast("Logged " + q.call); else hamToast(err);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"), ImVec2(100 * gUi, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::SameLine(); ImGui::TextDisabled("%s", tr("Ctrl+Enter saves"));
    ImGui::EndPopup();
}

struct LogUi { char search[96] = {}; long long sel = 0; std::vector<size_t> rows; std::string lastQuery = "\x01"; size_t lastSize = (size_t)-1; size_t lastGen = 0; };
LogUi L;
size_t gGen = 0;   // grows with every change of the book: the filter is redone
}  // namespace

HamOps& hamOps() { static HamOps o; return o; }
void hamLogChanged() { gGen++; hamOps().workedDirty = true; }

// ---- the country file (cty.dat / wl_cty.dat): load, reset, download, the settings block ----
namespace {
constexpr size_t kCtyMax = 16u << 20;   // a real cty.dat is about 0.5 MB; more than 16 MB is not one
std::string readCapped(const std::string& p, bool& ok, std::string& err) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(std::filesystem::u8path(p), ec);
    if (ec) { ok = false; err = "cannot read it"; return ""; }
    if (sz > kCtyMax) { ok = false; err = "the file is too large for a country file"; return ""; }
    return readFile(p, ok);
}
// reads path into hamOps().dxcc (the table is replaced only when the file parses); sets ctyInfo (the date / version line)
bool ctyApply(const std::string& path, std::string& err) {
    HamOps& o = hamOps();
    bool ok;
    const std::string t = readCapped(path, ok, err);
    if (!ok) return false;
    ham::DxccDb u;
    if (!u.loadCty(t, err)) return false;
    o.dxcc = u; o.ctyPath = path;
    const std::string& v = o.dxcc.version();
    if (v.size() == 8) o.ctyInfo = "version " + v.substr(0, 4) + "-" + v.substr(4, 2) + "-" + v.substr(6, 2);
    else {
        std::error_code ec;
        const auto ft = std::filesystem::last_write_time(std::filesystem::u8path(path), ec);
        if (ec) o.ctyInfo.clear();
        else {
            const std::time_t tt = std::chrono::system_clock::to_time_t(std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()));
            char b[32];
            std::strftime(b, sizeof b, "file dated %Y-%m-%d", std::gmtime(&tt));
            o.ctyInfo = b;
        }
    }
    return true;
}
struct CtyDl { std::mutex m; bool running = false, done = false, ok = false; std::string msg, file; };
std::shared_ptr<CtyDl>& ctyDl() { static std::shared_ptr<CtyDl> d = std::make_shared<CtyDl>(); return d; }
}  // namespace

bool hamCountryLoadFile(const std::string& path, std::string& err) {
    HamOps& o = hamOps();
    if (!ctyApply(path, err)) return false;
    plat::basePrefs().setS("ham.ctyPath", path); plat::basePrefs().flush();
    std::string e2;
    o.log.load(o.logPath, e2);   // fills in the countries the log did not have yet
    gGen++; o.workedDirty = true;
    return true;
}
void hamCountryReset() {
    HamOps& o = hamOps();
    o.dxcc = ham::DxccDb::builtin(); o.ctyPath.clear(); o.ctyInfo.clear();
    plat::basePrefs().setS("ham.ctyPath", ""); plat::basePrefs().flush();
    gGen++; o.workedDirty = true;
}
// "Germany, CQ 14, ITU 28" for a callsign: the country and zones shown next to a decoded message; "" when the call is not known
std::string hamCountryTag(const std::string& call) {
    if (call.empty()) return "";
    const ham::DxccHit h = hamOps().dxcc.lookup(call);
    if (!h.e) return "";
    return h.e->name + ", CQ " + std::to_string(h.cqz) + ", ITU " + std::to_string(h.ituz);
}
bool hamCountryInMessages() { return hamOps().ctyInMsg; }

void hamCountryUi() {
    HamOps& o = hamOps();
    ImGui::TextUnformatted(o.ctyPath.empty() ? tr("Built-in country table (about 200 entities)") : o.ctyPath.c_str());
    ImGui::TextDisabled("%zu %s, %zu %s%s%s", o.dxcc.entities().size(), tr("entities"), o.dxcc.prefixCount(), tr("prefixes"), o.ctyInfo.empty() ? "" : ", ", o.ctyPath.empty() ? "" : o.ctyInfo.c_str());
    ImGui::TextWrapped("%s", tr("A country file (cty.dat, wl_cty.dat) from country-files.com gives every DXCC entity with its CQ and ITU zone. Thanks to Jim Reisert AD1C and the country-files.com team. OnAir does not bundle it: load one you have, or press Download (only on your click)."));
    if (ImGui::Checkbox(tr("Show country and zone in decoded messages"), &o.ctyInMsg)) { plat::basePrefs().setB("ham.ctyInMsg", o.ctyInMsg); plat::basePrefs().flush(); }
    std::string err;
    if (ImGui::Button(tr("Load file..."))) {
        const std::string p = plat::openFileDialog();
        if (!p.empty()) {
            if (hamCountryLoadFile(p, err)) hamToast("Country file loaded: " + std::to_string(o.dxcc.entities().size()) + " entities");
            else hamToast("Not a country file: " + err);
        }
    }
    ImGui::SameLine();
    auto dl = ctyDl();
    bool running, done = false, dok = false;
    std::string msg;
    { std::lock_guard<std::mutex> lk(dl->m); running = dl->running; done = dl->done; dok = dl->ok; msg = dl->msg; }
    ImGui::BeginDisabled(running);
    if (ImGui::Button(running ? tr("Downloading...") : tr("Download from country-files.com"))) {
        const std::string file = plat::dataDir() + "/cty.dat";
        { std::lock_guard<std::mutex> lk(dl->m); dl->running = true; dl->done = false; dl->msg.clear(); dl->file = file; }
        std::thread([dl, file] {
            const std::string tmp = file + ".part";
            bool ok = plat::fetchUrl("https://www.country-files.com/cty/cty.dat", tmp, "OnAir");
            std::string m;
            if (ok) {
                std::error_code ec;
                const auto sz = std::filesystem::file_size(std::filesystem::u8path(tmp), ec);
                if (ec || sz > kCtyMax || sz < 1000) { ok = false; m = "the download is not a country file (size)"; }
                else {
                    bool rd; std::string err;
                    const std::string t = readCapped(tmp, rd, err);
                    ham::DxccDb probe;
                    if (!rd || !probe.loadCty(t, m)) { ok = false; if (m.empty()) m = "the download is not a country file"; }
                    else { std::filesystem::rename(std::filesystem::u8path(tmp), std::filesystem::u8path(file), ec); if (ec) { ok = false; m = "cannot save the file"; } }
                }
            } else m = "the download did not work (no network?)";
            if (!ok) { std::error_code ec; std::filesystem::remove(std::filesystem::u8path(tmp), ec); }
            std::lock_guard<std::mutex> lk(dl->m);
            dl->running = false; dl->done = true; dl->ok = ok; dl->msg = m;
        }).detach();
    }
    ImGui::EndDisabled();
    if (done) {   // the download finished: apply it on this (UI) thread
        std::string file;
        { std::lock_guard<std::mutex> lk(dl->m); dl->done = false; file = dl->file; }
        if (dok && hamCountryLoadFile(file, err)) hamToast("Country file downloaded: " + std::to_string(o.dxcc.entities().size()) + " entities");
        else hamToast("Country file: " + (dok ? err : msg));
    }
    if (!o.ctyPath.empty()) { ImGui::SameLine(); if (ImGui::Button(tr("Reset to built-in"))) hamCountryReset(); }
    if (!o.ctyInfo.empty() && o.ctyInfo.rfind("could not", 0) == 0) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", o.ctyInfo.c_str());
}


void hamToast(const std::string& s) { hamOps().toast = s; hamOps().toastAt = (double)time(nullptr); }

std::string hamCountry(const std::string& call) { const ham::DxccHit h = hamOps().dxcc.lookup(call); return h.e ? h.e->name : ""; }
std::string hamDxccKey(const std::string& call) { return hamOps().dxcc.lookup(call).key(); }

void hamSaveStation() {
    HamOps& o = hamOps();
    plat::Prefs& p = plat::basePrefs();
    p.setI("ham.region", o.region); p.setS("ham.licClass", o.licClass); p.setS("ham.power", o.power); p.setS("ham.antenna", o.antenna);
    p.setB("ham.setupDone", o.setupDone); p.setB("ham.contestOn", o.contestOn); p.setI("ham.contest", o.contest.def);
    p.setS("ham.contestExch", o.contest.myExch);
    std::string m;
    for (const auto& s : o.macros) m += s + "\n";
    p.setS("ham.macros", m);
    p.flush();
}

void hamOpsLoad() {
    HamOps& o = hamOps();
    if (o.loaded) return;
    o.loaded = true;
    plat::Prefs& p = plat::basePrefs();
    o.region = (int)p.getI("ham.region", 0); o.licClass = p.getS("ham.licClass", ""); o.power = p.getS("ham.power", ""); o.antenna = p.getS("ham.antenna", "");
    o.setupDone = p.getB("ham.setupDone", false); o.contestOn = p.getB("ham.contestOn", false);
    o.contest.def = std::max(0, std::min((int)p.getI("ham.contest", 0), (int)ham::contestDefs().size() - 1));
    o.contest.myExch = p.getS("ham.contestExch", "");
    {
        std::istringstream is(p.getS("ham.macros", "CQ {MYCALL} {MYGRID}\n{CALL} {MYCALL} {GRID}\nTNX 73 GL\n"));
        std::string l;
        while (std::getline(is, l)) if (!l.empty()) o.macros.push_back(l);
    }
    o.dxcc = ham::DxccDb::builtin();
    o.ctyPath = p.getS("ham.ctyPath", "");
    o.ctyInMsg = p.getB("ham.ctyInMsg", true);
    if (!o.ctyPath.empty()) { std::string err; if (!ctyApply(o.ctyPath, err)) o.ctyInfo = "could not read " + o.ctyPath + (err.empty() ? "" : ": " + err); }
    o.log.enrich = [](ham::LogQso& q) {
        if (q.dxcc > 0 && !q.country.empty()) return;
        const ham::DxccHit h = hamOps().dxcc.lookup(q.call);
        if (!h.e) return;
        if (q.dxcc == 0) q.dxcc = h.e->adif;
        if (q.country.empty()) q.country = h.e->name;
        if (q.cont.empty()) q.cont = h.e->cont;
        if (q.cqz == 0) q.cqz = h.cqz;
        if (q.ituz == 0) q.ituz = h.ituz;
    };
    o.logPath = plat::dataDir() + "/onair_ham.adi";
    std::string err;
    if (!o.log.load(o.logPath, err)) hamToast(err);
    o.contest.resume(o.log);
    o.workedDirty = true;
}

ham::LogQso hamFromRecord(const ham::QsoRecord& r) {
    ham::LogQso q;
    q.call = r.call; q.grid = r.grid; q.mode = r.mode; q.submode = r.submode; q.rstSent = r.rstSent; q.rstRcvd = r.rstRcvd;
    q.myCall = r.myCall; q.myGrid = r.myGrid; q.freqHz = r.freqHz; q.startUtc = r.startUtc; q.endUtc = r.endUtc;
    q.txPwr = hamOps().power;
    return q;
}

bool hamLogQso(ham::LogQso q, std::string& err) {
    HamOps& o = hamOps();
    if (o.contestOn) {
        q.contestId = ham::contestDefs()[(size_t)o.contest.def].id;
        if (q.stx.empty()) {
            std::string s = o.contest.sent("");
            while (!s.empty() && s[0] == ' ') s.erase(0, 1);
            q.stx = s;
        }
        o.contest.taken();
    }
    if (q.txPwr.empty()) q.txPwr = o.power;
    hamStationOnLog(q);   // 4.0: activation and hunted references, callbook fields (ham_station_ui.cpp)
    o.log.add(q);
    gGen++;
    if (!o.log.save(o.logPath, err)) return false;
    o.workedDirty = true;
    return true;
}

// ---- the logbook screen ----
void hamLogTab(App& a) {
    (void)a;
    HamOps& o = hamOps();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextItemWidth(260 * gUi);
    if (o.focusSearch) { ImGui::SetKeyboardFocusHere(); o.focusSearch = false; }
    ImGui::InputTextWithHint("##logsearch", tr("Search: call, grid, country, band:20m, mode:FT8, qsl:n ..."), L.search, sizeof L.search);
    ImGui::SameLine();
    if (ImGui::Button(tr("Add QSO")) || o.openAdd) {
        o.openAdd = false;
        ham::LogQso q;
        q.startUtc = q.endUtc = (double)time(nullptr);
        q.freqHz = a.freqMhz * 1e6;
        q.mode = "SSB";
        q.rstSent = q.rstRcvd = "59";
        q.myCall = o.myCall; q.myGrid = o.myGrid;
        if (o.contestOn) q.stx = o.contest.sent("");
        beginEdit(q);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!o.log.canUndo());
    if (ImGui::Button(tr("Undo"))) { o.log.undo(); saveLog(); gGen++; }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s (Ctrl+Z)", o.log.undoLabel().c_str());
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!o.log.canRedo());
    if (ImGui::Button(tr("Redo"))) { o.log.redo(); saveLog(); gGen++; }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s (Ctrl+Y)", o.log.redoLabel().c_str());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Import ADIF"))) {
        const std::string p = plat::openFileDialog();
        if (!p.empty()) {
            bool ok;
            const std::string t = readFile(p, ok);
            if (!ok) hamToast("Cannot read " + p);
            else {
                const auto r = o.log.importAdif(t);
                saveLog(); gGen++;
                char b[160]; snprintf(b, sizeof b, "Imported %d QSOs, %d dupes skipped, %d unreadable", r.added, r.dupes, r.bad);
                hamToast(b);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Export ADIF"))) {
        const std::string p = plat::saveFileDialog("onair_log.adi");
        if (!p.empty()) {
            const bool all = L.search[0] == 0;
            hamToast(writeFile(p, all ? o.log.exportAdif() : o.log.exportAdif(L.rows)) ? "Exported " + std::to_string(all ? o.log.size() : L.rows.size()) + " QSOs to " + p : "Cannot write " + p);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Writes the QSOs shown (all of them when the search is empty) as an ADIF file, e.g. to sign with TQSL for LoTW."));
    ImGui::SameLine();
    if (ImGui::Button(tr("Country file"))) ImGui::OpenPopup("##cty");
    if (ImGui::BeginPopup("##cty")) {
        hamCountryUi();
        ImGui::EndPopup();
    }
    hamLogToolsUi();   // 4.0: Cabrillo import, QSL confirmations, backups, LoTW through TQSL (ham_station_ui.cpp)

    // the filter, redone when the query or the book changed
    if (L.lastQuery != L.search || L.lastSize != o.log.size() || L.lastGen != gGen) {
        L.rows = o.log.search(L.search);
        L.lastQuery = L.search; L.lastSize = o.log.size(); L.lastGen = gGen;
    }
    ImGui::TextDisabled("%zu / %zu QSOs", L.rows.size(), o.log.size());

    const bool typing = io.WantTextInput;
    if (!typing && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (L.sel && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
            const ham::LogQso* q = o.log.find(L.sel);
            if (q) { hamToast("Deleted " + q->call + " (Ctrl+Z brings it back)"); o.log.remove(L.sel); saveLog(); gGen++; L.sel = 0; }
        }
        if (L.sel && ImGui::IsKeyPressed(ImGuiKey_Enter)) { const ham::LogQso* q = o.log.find(L.sel); if (q) beginEdit(*q); }
    }

    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit |
                               ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable;
    if (ImGui::BeginTable("##hamlog", 13, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(1, 1);
        const char* cols[] = {"UTC", "Call", "Band", "Mode", "MHz", "Sent", "Rcvd", "Grid", "Country", "Name", "QSL", "Exch", "Comment"};
        for (int i = 0; i < 13; i++) ImGui::TableSetupColumn(cols[i], i == 12 ? ImGuiTableColumnFlags_WidthStretch : 0);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)L.rows.size());
        long long editId = 0, delId = 0;
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                const ham::LogQso& q = o.log.qsos()[L.rows[(size_t)r]];
                ImGui::TableNextRow();
                ImGui::PushID((int)q.id);
                ImGui::TableNextColumn();
                if (ImGui::Selectable(dateTime(q.startUtc).c_str(), L.sel == q.id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    L.sel = q.id; o.selCall = q.call; o.selGrid = q.grid;
                    if (ImGui::IsMouseDoubleClicked(0)) editId = q.id;
                }
                if (ImGui::BeginPopupContextItem("##ctx")) {
                    L.sel = q.id;
                    if (ImGui::MenuItem(tr("Edit"), "Enter")) editId = q.id;
                    if (ImGui::MenuItem(tr("Delete"), "Del")) delId = q.id;
                    if (ImGui::MenuItem(q.confirmed() ? tr("Mark QSL not received") : tr("Mark QSL received"))) {
                        ham::LogQso c = q; c.qslRcvd = !q.confirmed(); if (!c.qslRcvd) c.lotwRcvd = false;
                        o.log.update(c); saveLog(); gGen++;
                    }
                    if (ImGui::MenuItem(tr("Show on the map"))) { o.selCall = q.call; o.selGrid = q.grid; o.wantTab = HamTabMap; }
                    if (ImGui::MenuItem(tr("Show all QSOs with this call"))) snprintf(L.search, sizeof L.search, "call:%s", ham::baseCall(q.call).c_str());
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn(); ImGui::TextUnformatted(q.call.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(q.band.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(q.submode.empty() ? q.mode.c_str() : q.submode.c_str());
                ImGui::TableNextColumn(); if (q.freqHz > 0) ImGui::Text("%.4f", q.freqHz / 1e6);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(q.rstSent.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(q.rstRcvd.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(q.grid.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(q.country.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(q.name.c_str());
                ImGui::TableNextColumn();
                if (q.confirmed()) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "%s", q.lotwRcvd ? "LoTW" : "QSL");
                else if (q.qslSent) ImGui::TextDisabled("sent");
                ImGui::TableNextColumn(); if (!q.stx.empty() || !q.srx.empty()) ImGui::Text("%s / %s", q.stx.c_str(), q.srx.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(q.comment.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
        if (editId) { const ham::LogQso* q = o.log.find(editId); if (q) beginEdit(*q); }
        if (delId) { const ham::LogQso* q = o.log.find(delId); if (q) hamToast("Deleted " + q->call + " (Ctrl+Z brings it back)"); o.log.remove(delId); saveLog(); gGen++; L.sel = 0; }
    }
    editDialog();
}

// ---- awards and statistics ----
void hamAwardsTab(App& a) {
    (void)a;
    HamOps& o = hamOps();
    static int award = 0, modeSel = 0, bandSel = 0;
    static const char* kModes[] = {"All modes", "CW", "PHONE", "DIGI", "FT8", "FT4", "RTTY", "PSK31"};
    ImGui::SetNextItemWidth(120 * gUi);
    ImGui::Combo("##award", &award, "DXCC\0WAS\0Grids\0CQ zones\0Statistics\0");
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
    ImGui::Combo("##amode", &modeSel, kModes, IM_ARRAYSIZE(kModes));
    std::vector<std::string> bands = {"All bands"};
    for (const auto& b : ham::bandDials()) bands.push_back(b.band);
    ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
    if (ImGui::BeginCombo("##aband", bands[(size_t)bandSel].c_str())) {
        for (int i = 0; i < (int)bands.size(); i++) if (ImGui::Selectable(bands[(size_t)i].c_str(), i == bandSel)) bandSel = i;
        ImGui::EndCombo();
    }
    const std::string mf = modeSel ? kModes[modeSel] : "", bf = bandSel ? bands[(size_t)bandSel] : "";
    if (award == 4) {   // statistics
        std::vector<ham::LogQso> sub;
        for (const auto& q : o.log.qsos()) if ((mf.empty() || q.mode == mf || ham::modeClass(q.mode) == mf) && (bf.empty() || q.band == bf)) sub.push_back(q);
        const ham::LogStats s = ham::logStats(sub, o.myGrid);
        ImGui::Text("%s %d   %s %d   DXCC %d   %s %d   %s %d", tr("QSOs"), s.qsos, tr("calls"), s.calls, s.dxcc, tr("grids"), s.grids, tr("confirmed"), s.confirmed);
        if (s.qsos) ImGui::TextDisabled("%s %s  ..  %s", tr("From"), dateTime(s.firstUtc).c_str(), dateTime(s.lastUtc).c_str());
        if (!s.bestCall.empty()) ImGui::Text("%s: %s, %.0f km", tr("Longest distance"), s.bestCall.c_str(), s.bestKm);
        ImGui::Text("%s: %.0f", tr("QSOs in the last hour of operating"), s.qsoPerHourLast);
        auto bars = [&](const char* title, const std::map<std::string, int>& m, bool bandSort) {
            ImGui::SeparatorText(title);
            std::vector<std::pair<std::string, int>> v(m.begin(), m.end());
            if (bandSort) std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return ham::bandOrder(x.first) < ham::bandOrder(y.first); });
            else std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
            for (const auto& kv : v) {
                ImGui::TextUnformatted(kv.first.c_str()); ImGui::SameLine(70 * gUi);
                char b[16]; snprintf(b, sizeof b, "%d", kv.second);
                ImGui::ProgressBar(s.qsos ? (float)kv.second / (float)s.qsos : 0.f, ImVec2(240 * gUi, 0), b);
            }
        };
        bars(tr("Per band"), s.perBand, true);
        bars(tr("Per mode"), s.perMode, false);
        bars(tr("Per continent"), s.perCont, false);
        std::map<std::string, int> hrs;
        for (const auto& kv : s.perHour) { char b[8]; snprintf(b, sizeof b, "%02d UTC", kv.first); hrs[b] = kv.second; }
        ImGui::SeparatorText(tr("Per hour of the day"));
        for (const auto& kv : hrs) { ImGui::TextUnformatted(kv.first.c_str()); ImGui::SameLine(70 * gUi); char b[16]; snprintf(b, sizeof b, "%d", kv.second); ImGui::ProgressBar(s.qsos ? (float)kv.second / (float)s.qsos : 0.f, ImVec2(240 * gUi, 0), b); }
        return;
    }
    const ham::Award aw = (ham::Award)award;
    const ham::AwardTable t = ham::awardTable(o.log.qsos(), aw, mf, bf);
    ImGui::SameLine();
    if (t.total) ImGui::Text("  %s: %s %d / %d, %s %d", ham::awardName(aw), tr("worked"), t.worked, t.total, tr("confirmed"), t.confirmed);
    else ImGui::Text("  %s: %s %d, %s %d", ham::awardName(aw), tr("worked"), t.worked, tr("confirmed"), t.confirmed);
    ImGui::TextDisabled("%s", tr("W worked, C confirmed (QSL card or LoTW). Mark a QSL in the log with a right click on the QSO."));
    if (aw == ham::Award::Was) {   // the states still needed
        std::string need;
        for (const auto& s : ham::usStates()) { bool got = false; for (const auto& r : t.rows) got |= r.key == s; if (!got) need += s + " "; }
        if (!need.empty()) ImGui::TextWrapped("%s: %s", tr("Still needed"), need.c_str());
    }
    const int nb = (int)t.bands.size();
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginTable("##award", 5 + nb, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(1, 2);
        ImGui::TableSetupColumn(aw == ham::Award::Dxcc ? "Entity" : aw == ham::Award::Was ? "State" : aw == ham::Award::Grid ? "Grid" : "Zone");
        ImGui::TableSetupColumn("Any");
        for (const auto& b : t.bands) ImGui::TableSetupColumn(b.c_str());
        ImGui::TableSetupColumn("CW"); ImGui::TableSetupColumn("PHONE"); ImGui::TableSetupColumn("DIGI");
        ImGui::TableHeadersRow();
        // the totals per band
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", tr("total W / C"));
        ImGui::TableNextColumn(); ImGui::TextDisabled("%d/%d", t.worked, t.confirmed);
        for (const auto& b : t.bands) { ImGui::TableNextColumn(); auto it = t.perBand.find(b); if (it != t.perBand.end()) ImGui::TextDisabled("%d/%d", it->second.first, it->second.second); }
        ImGui::TableNextColumn(); ImGui::TableNextColumn(); ImGui::TableNextColumn();
        auto cell = [](char c) {
            if (c == 'C') ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.4f, 1), "C");
            else if (c == 'W') ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.3f, 1), "W");
        };
        ImGuiListClipper clip;
        clip.Begin((int)t.rows.size());
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const ham::AwardRow& r = t.rows[(size_t)i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.label.c_str());
                ImGui::TableNextColumn(); cell(r.any);
                for (const auto& b : t.bands) { ImGui::TableNextColumn(); auto it = r.byBand.find(b); if (it != r.byBand.end()) cell(it->second); }
                for (const char* mc : {"CW", "PHONE", "DIGI"}) { ImGui::TableNextColumn(); auto it = r.byClass.find(mc); if (it != r.byClass.end()) cell(it->second); }
            }
        }
        ImGui::EndTable();
    }
}

// ---- contest mode ----
void hamContestTab(App& a) {
    HamOps& o = hamOps();
    bool ch = ImGui::Checkbox(tr("Contest mode"), &o.contestOn);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Every QSO logged gets the contest id and the exchange; the serial number counts up; dupes are marked in the decode list."));
    ImGui::SameLine(); ImGui::SetNextItemWidth(320 * gUi);
    const auto& defs = ham::contestDefs();
    if (ImGui::BeginCombo("##contest", defs[(size_t)o.contest.def].name)) {
        for (int i = 0; i < (int)defs.size(); i++) if (ImGui::Selectable(defs[(size_t)i].name, i == o.contest.def)) { o.contest.def = i; o.contest.resume(o.log); ch = true; }
        ImGui::EndCombo();
    }
    const ham::ContestDef& d = defs[(size_t)o.contest.def];
    if (d.exch != ham::Exch::Serial) {
        static char ex[24] = {};
        static bool init = false;
        if (!init) { init = true; snprintf(ex, sizeof ex, "%s", o.contest.myExch.c_str()); }
        ImGui::SetNextItemWidth(100 * gUi);
        const char* lbl = d.exch == ham::Exch::CqZone ? tr("My CQ zone") : d.exch == ham::Exch::ItuZone ? tr("My ITU zone") : d.exch == ham::Exch::Grid ? tr("My grid") : tr("My exchange");
        if (ImGui::InputText(lbl, ex, sizeof ex, ImGuiInputTextFlags_CharsUppercase)) { o.contest.myExch = ex; ch = true; }
        if (ex[0] == 0 && (d.exch == ham::Exch::CqZone || d.exch == ham::Exch::ItuZone || d.exch == ham::Exch::Grid)) {
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("From my station"))) {
                const ham::DxccHit h = o.dxcc.lookup(o.myCall);
                std::string v = d.exch == ham::Exch::Grid ? o.myGrid.substr(0, std::min<size_t>(4, o.myGrid.size())) : h.e ? std::to_string(d.exch == ham::Exch::CqZone ? h.cqz : h.ituz) : "";
                snprintf(ex, sizeof ex, "%s", v.c_str()); o.contest.myExch = v; ch = true;
            }
        }
    }
    ImGui::SetNextItemWidth(100 * gUi);
    if (ImGui::InputInt(tr("Next serial"), &o.contest.nextSerial)) o.contest.nextSerial = std::max(1, o.contest.nextSerial);
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Go on from the log"))) o.contest.resume(o.log);
    ImGui::Text("%s: %s", tr("Sent exchange"), o.contest.sent(d.exch == ham::Exch::Grid ? "" : "599").c_str());
    if (ch) hamSaveStation();

    // the dupe sheet
    const auto sheet = o.contest.sheet(o.log);
    int n = 0;
    for (const auto& kv : sheet) n += (int)kv.second.size();
    ImGui::SeparatorText(tr("Dupe sheet"));
    ImGui::TextDisabled("%d %s %s", n, tr("QSOs in"), d.id);
    if (!sheet.empty() && ImGui::BeginTable("##dupes", (int)sheet.size(), ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 180 * gUi))) {
        size_t rows = 0;
        std::vector<const std::pair<const std::string, std::vector<std::string>>*> cols;
        for (const auto& kv : sheet) { cols.push_back(&kv); rows = std::max(rows, kv.second.size()); }
        std::sort(cols.begin(), cols.end(), [](auto* x, auto* y) { return ham::bandOrder(x->first) < ham::bandOrder(y->first); });
        for (auto* c : cols) ImGui::TableSetupColumn(c->first.c_str());
        ImGui::TableHeadersRow();
        for (size_t r = 0; r < rows; r++) {
            ImGui::TableNextRow();
            for (auto* c : cols) { ImGui::TableNextColumn(); if (r < c->second.size()) ImGui::TextUnformatted(c->second[r].c_str()); }
        }
        ImGui::EndTable();
    }

    // Cabrillo
    ImGui::SeparatorText(tr("Cabrillo export"));
    static char cat[48] = "SINGLE-OP ALL LOW", loc[16] = {}, ops[48] = {}, club[48] = {}, nm[48] = {};
    ImGui::SetNextItemWidth(220 * gUi); ImGui::InputText(tr("Category"), cat, sizeof cat, ImGuiInputTextFlags_CharsUppercase);
    ImGui::SetNextItemWidth(100 * gUi); ImGui::InputText(tr("Location"), loc, sizeof loc, ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine(); ImGui::SetNextItemWidth(160 * gUi); ImGui::InputText(tr("Operators"), ops, sizeof ops, ImGuiInputTextFlags_CharsUppercase);
    ImGui::SetNextItemWidth(160 * gUi); ImGui::InputText(tr("Name"), nm, sizeof nm);
    ImGui::SameLine(); ImGui::SetNextItemWidth(160 * gUi); ImGui::InputText(tr("Club"), club, sizeof club);
    if (ImGui::Button(tr("Export Cabrillo..."))) {
        ham::CabrilloHeader h;
        h.call = o.myCall; h.contest = d.id; h.category = cat; h.location = loc; h.operators = ops; h.club = club; h.name = nm; h.grid = o.myGrid;
        const std::string p = plat::saveFileDialog((std::string(d.id) + ".log").c_str());
        if (!p.empty()) hamToast(writeFile(p, ham::cabrilloExport(o.log.qsos(), h)) ? "Cabrillo log written: " + p : "Cannot write " + p);
    }
    (void)a;
}
