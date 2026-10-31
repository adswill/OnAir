// The satellite pass planner (View > Passes): the next 24 hours of passes of the satellites OnAir's modes receive (weather satellites
// on 137 MHz, the ISS, Orbcomm, a short editable list of amateur satellites) over the receiver's position, a polar sky plot of the
// current or chosen pass, and optional tracking: Doppler-corrected retuning about once a second, the right mode started at AOS and
// an IQ recording (the same as Record IQ) until LOS. TLEs are downloaded from Celestrak or Keplerpass on request, or pasted.
#include "dect2/common/thread_name.h"
#include "../app.h"
#include "../ui/typography.h"
#include "gallery.h"
#include "../adsb/adsb_map.h"
#include "dect2/geo/passes.h"
#include "dect2/geo/doppler_fit.h"
#include "dect2/lorasat/lorasat_presets.h"
#include "dect2/geo/pass_sched.h"
#include "dect2/source/mount_track.h"
#include "dect2/automation/engine.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

void rotatorPassStep(const dect2::MountPlan& m);   // rotator_ui.cpp

namespace {

namespace fs = std::filesystem;
fs::path fsPath(const std::string& p) { return fs::path(reinterpret_cast<const char8_t*>(p.c_str())); }

constexpr int kSpecx = 41, kAprs = 24, kOrbcomm = 44, kMeteor = 59, kCubesat = 61, kGlobalstar = 62, kArgos = 152, kLoraSat = 201;   // mode families: Spectrum explorer (no decoder), APRS (AFSK), Orbcomm, Meteor-M LRPT, CubeSat telemetry, Argos, LoRa satellites

struct Source { const char* file; const char* url; };
const Source kSources[] = {
    {"weather.txt", "https://celestrak.org/NORAD/elements/gp.php?GROUP=weather&FORMAT=tle"},
    {"stations.txt", "https://celestrak.org/NORAD/elements/gp.php?GROUP=stations&FORMAT=tle"},
    {"orbcomm.txt", "https://celestrak.org/NORAD/elements/gp.php?GROUP=orbcomm&FORMAT=tle"},
    {"amateur.txt", "https://celestrak.org/NORAD/elements/gp.php?GROUP=amateur&FORMAT=tle"},
    {"globalstar.txt", "https://celestrak.org/NORAD/elements/gp.php?GROUP=globalstar&FORMAT=tle"},
};
const char* const kPasted = "pasted.txt";
const char* const kDerived = "derived_doppler.tle";
const Source kKeplerpass = {"keplerpass.txt", "https://data.keplerpass.com/PU4ELT/tle.txt"};

enum Group { Weather, Iss, Orbcomm, Amateur, GlobalstarGrp, NumGroups };
const char* const kGroupLabel[NumGroups] = {N_("Weather (137 MHz)"), "ISS", "Orbcomm", N_("Amateur"), "Globalstar"};

struct Known { const char* name; int norad; double mhz; const char* mode; int family; int group; };
const Known kKnown[] = {
    {"NOAA 15", 25338, 137.620, "APT", kSpecx, Weather},
    {"NOAA 18", 28654, 137.9125, "APT", kSpecx, Weather},
    {"NOAA 19", 33591, 137.100, "APT", kSpecx, Weather},
    {"Meteor-M N2-3", 57166, 137.100, "LRPT", kMeteor, Weather},
    {"Meteor-M N2-4", 59051, 137.900, "LRPT", kMeteor, Weather},
    {"MetOp-B Argos", 38771, 465.9875, "Argos downlink", kArgos, Weather},   // A-DCS downlink (SatNOGS transmitter entry for MetOp-B)
    {"MetOp-C Argos", 43689, 465.9875, "Argos downlink", kArgos, Weather},
    {"ISS APRS", 25544, 145.825, "APRS", kAprs, Iss},
    {"ISS voice", 25544, 145.800, "FM voice", kSpecx, Iss},
};

struct Cube { std::string name; int norad = 0; double mhz = 0; bool aprs = false; };

struct Sat {
    std::string name, mode;
    int norad = 0, family = kSpecx, group = Weather;
    double mhz = 0;
    dect2::Sgp4 sgp;
};
struct Row { int sat; dect2::Pass p; };

struct State {
    bool loaded = false;
    std::map<int, dect2::Tle> tles;     // by NORAD number; pasted ones win
    std::vector<Sat> sats;
    std::vector<std::string> missing;   // listed satellites without a TLE
    std::vector<Row> rows;
    bool dirty = true;
    double computedAt = 0, lat = 1e9, lon = 1e9, altM = 0;   // altM: the observer height (metres) the passes were computed for
    float minEl = 10;
    bool show[NumGroups] = {true, true, true, true, true};
    std::vector<Cube> cubes;
    // the chosen pass (a row): its satellite is the one tracked
    int selSat = -1; double selAos = 0;
    std::string selName;                // selSat again after the list is rebuilt
    bool track = false, autoStart = false, autoRecord = false;
    bool once = false, prevStart = false, prevRec = false;   // "Record this pass": armed for the chosen pass only (the settings it overrode)
    bool autoTle = true;                                     // download the TLEs by itself, at most once a day
    float notifyMin = 5;                                     // minutes before AOS of the tracked pass: banner and log line
    double notifiedAos = 0;                                  // the pass the log line was written for
    // tracking
    bool inPass = false, weStarted = false, weRecord = false, recTried = false;
    std::string passName;               // the satellite of the pass under way
    // the scheduler (Schedule passes by itself): one SDR, the passes of the ticked groups taken one at a time by priority
    bool sched = false;
    int prio[NumGroups] = {3, 2, 0, 1, 0};                  // per group: higher wins an overlap
    bool resv = false; int resvFrom = 2, resvTo = 6;        // hours of the day the SDR is kept free for other jobs
    dect2::SchedPlan plan;
    double planAt = 0, planRowsAt = 0; bool planDirty = true;
    bool hasSlot = false; Row slotRow{}; dect2::Pass slotPass;   // the slot being waited for or received (slotRow: the part taken, lead included)
    double passLos = 0, lastTune = 0, nominalMhz = 0;
    // TLE data
    bool showPaste = false;
    bool showSources = false;
    char newSourceUrl[512] = "";
    std::vector<std::pair<std::string,std::string>> customSources;
    std::vector<char> paste = std::vector<char>(32768, 0);
    std::string msg;
    char cubeName[40] = ""; int cubeNorad = 0; double cubeMhz = 145.9; bool cubeAprs = false;
};
State S;

struct DopState {
    bool measure = false;               // follow the carrier in the spectrum during a tracked pass
    std::vector<dect2::DopplerSample> smp;
    int norad = 0; std::string name; double nominalHz = 0; dect2::Tle tle;   // what the samples are of
    double lastT = 0; uint64_t lastSeq = 0;
    dect2::DopplerFit fit; bool fitted = false;
    std::string msg;
    bool useDerived = false;
    float gateKhz = 4;
};
DopState D;

// the download runs on its own thread: these outlive it
std::atomic<int> gDl{0};   // 0 idle, 1 working, 2 finished
std::atomic<bool> gDlOk{false};   // the finished download got at least one list
std::mutex gDlMu;
std::string gDlMsg;

std::string tleDir() { return plat::dataDir() + "/tle"; }

double nowSec() { return (double)std::time(nullptr); }

std::string readText(const std::string& path) {
    std::ifstream f(fsPath(path), std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

void saveCubes() {
    std::string s;
    for (const Cube& c : S.cubes) { char b[160]; snprintf(b, sizeof b, "%s|%d|%.4f|%d;", c.name.c_str(), c.norad, c.mhz, c.aprs ? 1 : 0); s += b; }
    plat::prefs().setS("passCubes", s);
}

void loadCubes() {
    const std::string def = "AO-73 (FUNcube-1)|39417|145.935|0;SO-50|27607|436.795|0;RS-44|44909|435.640|0;";
    const std::string s = plat::prefs().getS("passCubes", def);
    S.cubes.clear();
    std::stringstream ss(s);
    for (std::string item; std::getline(ss, item, ';');) {
        std::stringstream is(item);
        std::string n, no, mhz, ap;
        if (!std::getline(is, n, '|') || !std::getline(is, no, '|') || !std::getline(is, mhz, '|')) continue;
        std::getline(is, ap, '|');
        Cube c; c.name = n; c.norad = atoi(no.c_str()); c.mhz = atof(mhz.c_str()); c.aprs = ap == "1";
        if (c.norad > 0 && c.mhz > 0) S.cubes.push_back(c);
    }
}

void buildSats() {
    S.sats.clear(); S.missing.clear();
    auto add = [&](const std::string& name, int norad, double mhz, const std::string& mode, int fam, int grp) {
        auto it = S.tles.find(norad);
        if (it == S.tles.end()) { S.missing.push_back(name); return; }
        Sat s; s.name = name; s.norad = norad; s.mhz = mhz; s.mode = mode; s.family = fam; s.group = grp;
        if (!s.sgp.init(it->second)) { S.missing.push_back(name + " (orbit not usable)"); return; }
        S.sats.push_back(std::move(s));
    };
    for (const Known& k : kKnown) add(k.name, k.norad, k.mhz, k.mode, k.family, k.group);
    const ModeTuning* ot = modeTuning(46);
    for (const auto& [no, t] : S.tles)
        if (t.name.rfind("ORBCOMM", 0) == 0) add(t.name, no, ot ? ot->defMhz : 137.25, "Orbcomm", kOrbcomm, Orbcomm);
    for (const auto& [no, t] : S.tles)
        if (t.name.rfind("GLOBALSTAR", 0) == 0) add(t.name, no, 1618.25, "Globalstar", kGlobalstar, GlobalstarGrp);
    for (const auto& [no, t] : S.tles)   // satellites that use LoRa (public presets, unverified): matched by the start of the name in the elements files
        for (const auto& p : dect2::lorasat::presets())
            if (p.tleName[0] && !p.uplinkOnly && t.name.rfind(p.tleName, 0) == 0) { add(t.name, no, p.mhz, "LoRa satellite", kLoraSat, Amateur); break; }
    for (const Cube& c : S.cubes) add(c.name, c.norad, c.mhz, c.aprs ? "APRS" : "CubeSat telemetry", c.aprs ? kAprs : kCubesat, Amateur);
    S.selSat = -1;
    for (int i = 0; i < (int)S.sats.size(); i++) if (!S.selName.empty() && S.sats[i].name == S.selName) S.selSat = i;
    S.rows.clear();
    S.dirty = true;
    S.plan = dect2::SchedPlan(); S.planDirty = true; S.hasSlot = false;
}

// Custom source URLs are saved separately from the downloaded TLE files.
// Use generated local filenames, never remote URL fragments as paths.
// Local Raspberry Pi TLE feeds may use HTTP; public servers must use HTTPS.
bool validTleUrl(const std::string& url) {
    if (url.rfind("https://", 0) == 0) return true;
    if (url.rfind("http://", 0) != 0) return false;
    const size_t end = url.find_first_of("/?#", 7);
    const std::string hostPort = url.substr(7, end == std::string::npos ? end : end - 7);
    const std::string host = hostPort.substr(0, hostPort.find(':'));
    if (host == "localhost") return true;
    unsigned a=0,b=0,c=0,d=0;
    char extra=0;
    if (std::sscanf(host.c_str(), "%u.%u.%u.%u%c", &a,&b,&c,&d,&extra) != 4
        || a>255 || b>255 || c>255 || d>255) return false;
    return a==10 || a==127 || (a==192 && b==168) || (a==172 && b>=16 && b<=31);
}

void loadCustomSources() {
    S.customSources.clear();
    std::istringstream in(readText(tleDir() + "/custom_sources.txt"));
    std::string name, url;
    while (std::getline(in, name, '\t') && std::getline(in, url)) {
        if (name.rfind("user_", 0) == 0 && name.size() < 64 &&
            name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
            validTleUrl(url))
            S.customSources.emplace_back(name, url);
    }
}
void saveCustomSources() {
    std::error_code ec;
    fs::create_directories(fsPath(tleDir()), ec);
    std::ofstream out(fsPath(tleDir() + "/custom_sources.txt"), std::ios::trunc);
    for (const auto& item : S.customSources) out << item.first << '\t' << item.second << '\n';
}

void loadTles() {
    S.tles.clear();
    for (const Source& s : kSources) for (const dect2::Tle& t : dect2::parseTleText(readText(tleDir() + "/" + s.file))) S.tles[t.norad] = t;
    for (const dect2::Tle& t : dect2::parseTleText(readText(tleDir() + "/" + kKeplerpass.file))) S.tles[t.norad] = t;
    for (const auto& src : S.customSources) for (const dect2::Tle& t : dect2::parseTleText(readText(tleDir() + "/" + src.first))) S.tles[t.norad] = t;
    for (const dect2::Tle& t : dect2::parseTleText(readText(tleDir() + "/" + kPasted))) S.tles[t.norad] = t;
    if (plat::prefs().getB("passUseDerived", false)) {   // the elements fitted to a measured Doppler curve: the last one saved for each satellite wins
        for (const dect2::Tle& t : dect2::parseTleText(readText(tleDir() + "/" + kDerived))) S.tles[t.norad] = t;
    }
    buildSats();
}

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& p = plat::prefs();
    S.minEl = (float)p.getD("passMinEl", 10);
    S.autoTle = p.getB("passTleAuto", true);
    D.measure = p.getB("passDopMeasure", false); D.useDerived = p.getB("passUseDerived", false);
    S.notifyMin = (float)p.getD("passNotifyMin", 5);
    S.sched = p.getB("passSched", false); S.resv = p.getB("passResv", false);
    S.resvFrom = (int)p.getD("passResvFrom", 2); S.resvTo = (int)p.getD("passResvTo", 6);
    for (int g = 0; g < NumGroups; g++) { char k[24]; snprintf(k, sizeof k, "passPrio%d", g); S.prio[g] = std::max(0, std::min(9, (int)p.getD(k, S.prio[g]))); }
    for (int g = 0; g < NumGroups; g++) { char k[24]; snprintf(k, sizeof k, "passShow%d", g); S.show[g] = p.getB(k, true); }
    loadCubes();
    loadCustomSources();
    loadTles();
}

void startDownload(bool keplerpass = false, std::string customFile = {}, std::string customUrl = {}) {
    if (gDl.load() == 1) return;
    gDl = 1;
    gDlOk = false;
    plat::prefs().setD("passTleTry", nowSec());
    { std::lock_guard<std::mutex> lk(gDlMu); gDlMsg = tr("downloading..."); }
    const std::string dir = tleDir();
    dect2::namedThread("onair-passes", [dir, keplerpass, customFile, customUrl] {
        std::error_code ec;
        fs::create_directories(fsPath(dir), ec);
        int ok = 0, n = 0;
        const bool custom = !customUrl.empty();
        const size_t sourceCount = (keplerpass || custom) ? 1 : sizeof kSources / sizeof kSources[0];
        for (size_t i = 0; i < sourceCount; ++i) {
            const Source& source = keplerpass ? kKeplerpass : kSources[i];
            const std::string filename = custom ? customFile : source.file;
            const std::string url = custom ? customUrl : source.url;
            const std::string path = dir + "/" + filename, part = path + ".part";
            if (plat::fetchUrl(url, part, "OnAir/" ONAIR_VERSION " (passes)")) {
                const size_t got = dect2::parseTleText(readText(part)).size();
                if (got > 0) { fs::rename(fsPath(part), fsPath(path), ec); if (!ec) { ok++; n += (int)got; } }
            }
            fs::remove(fsPath(part), ec);
        }
        std::string m = !ok ? (custom ? "Custom TLE download failed (network or invalid TLE)" : keplerpass ? "Keplerpass download failed (check network or TLE format)" : std::string(tr("the download failed (no network, or Celestrak refused)")))
                            : std::string(custom ? "Custom source: downloaded " : keplerpass ? "Keplerpass: downloaded " : (std::string(tr("downloaded")) + " ")) + std::to_string(n) + " TLEs";
        if (ok && ok < (int)sourceCount) m += "; some lists failed";
        { std::lock_guard<std::mutex> lk(gDlMu); gDlMsg = m; }
        gDlOk = ok > 0;
        gDl = 2;
    }).detach();
}

// the end of a download, noticed on the main thread whether or not the window is open: the new lists are loaded, the day is noted
void pollDownload() {
    if (gDl.load() == 2) {
        gDl = 0;
        std::lock_guard<std::mutex> lk(gDlMu);
        S.msg = gDlMsg;
        if (gDlOk.load()) plat::prefs().setD("passTleOk", nowSec());
        loadTles();
    } else if (gDl.load() == 1) { std::lock_guard<std::mutex> lk(gDlMu); S.msg = gDlMsg; }
}

// Automatic update: once a day while the planner is open or a pass is tracked (cached lists are used meanwhile and when offline)
void autoUpdateTle(bool wanted) {
    if (!S.autoTle || !wanted || gDl.load() != 0) return;
    plat::Prefs& p = plat::prefs();
    const double ok = S.tles.empty() ? 0.0 : p.getD("passTleOk", 0), tried = p.getD("passTleTry", 0);
    if (dect2::tleUpdateDue(nowSec(), ok, tried)) startDownload();
}

void compute(App& a) {
    double lat, lon;
    receiverLocation(lat, lon);
    const double now = nowSec();
    const double alt = receiverAltitudeM();
    if (!S.dirty && lat == S.lat && lon == S.lon && alt == S.altM && now - S.computedAt < 600) return;
    S.dirty = false; S.lat = lat; S.lon = lon; S.altM = alt; S.computedAt = now;
    dect2::Observer o; o.latDeg = lat; o.lonDeg = lon; o.altKm = alt / 1000;
    S.rows.clear();
    for (int i = 0; i < (int)S.sats.size(); i++)
        for (const dect2::Pass& p : dect2::findPasses(S.sats[i].sgp, o, now - 1800, now + 86400, S.minEl))
            if (p.los > now) S.rows.push_back({i, p});
    std::sort(S.rows.begin(), S.rows.end(), [](const Row& x, const Row& y) { return x.p.aos < y.p.aos; });
    (void)a;
}

std::string timeText(double t) {
    const std::time_t tt = (std::time_t)t, nw = std::time(nullptr);
    std::tm lt = *std::localtime(&tt);
    const std::tm today = *std::localtime(&nw);
    char b[32];
    if (lt.tm_yday != today.tm_yday || lt.tm_year != today.tm_year) std::strftime(b, sizeof b, "%a %H:%M:%S", &lt);
    else std::strftime(b, sizeof b, "%H:%M:%S", &lt);
    return b;
}

bool lookNow(const Sat& s, double t, dect2::Look& l) {
    dect2::Observer o; o.latDeg = S.lat; o.lonDeg = S.lon; o.altKm = S.altM / 1000;
    return dect2::lookAt(s.sgp, o, t, l);
}

const Row* activeOrNext(int sat) {   // the pass under way, else the next one (of one satellite, or of any shown)
    const double now = nowSec();
    for (const Row& r : S.rows) {
        if (r.p.los < now) continue;
        if (sat >= 0 ? r.sat == sat : S.show[S.sats[r.sat].group]) return &r;
    }
    return nullptr;
}

void retuneTo(App& a, double mhz) {
    a.freqMhz = mhz;
    if (a.engine.running()) { a.tune.centerHz = a.freqMhz * 1e6; a.engine.retune(a.tune); }
}

// "Record this pass" is over (or cancelled): the tracking settings it overrode come back
void disarmOnce() {
    if (!S.once) return;
    S.once = false;
    S.track = false; S.autoStart = S.prevStart; S.autoRecord = S.prevRec;
}

void endPass(App& a, bool los) {
    S.inPass = false;
    rotatorPassEnd();   // the mount parks (or stops)
    a.engine.log("pass of " + S.passName + (los ? " ends (LOS)" : ": tracking stopped"));
    if (los && dect2::automation::wants(dect2::automation::FSatellite)) {   // the automations (satellite.los)
        dect2::automation::Event ev("satellite.los");
        ev.at(dect2::automation::wallNow(), "", S.nominalMhz * 1e6);
        ev.str("name", S.passName);
        dect2::automation::emit(std::move(ev));
    }
    if (S.weRecord) { a.engine.stopRecording(); S.weRecord = false; }
    if (S.weStarted && a.engine.running()) a.engine.stop();
    S.weStarted = false;
    a.engine.cubesat().setTracked(0, "");
    a.engine.lorasat().setTracked(0, "");
    retuneTo(a, S.nominalMhz);
    if (los) disarmOnce();
}

constexpr double kSchedLead = 20;   // seconds: the receiver is up this long before a scheduled slot

void replan(double now) {
    std::vector<dect2::SchedPass> ps;
    for (const Row& r : S.rows) {
        const Sat& s = S.sats[r.sat];
        if (S.show[s.group]) ps.push_back({r.sat, S.prio[s.group], r.p});
    }
    std::vector<dect2::BusyWindow> busy;
    if (S.resv) {
        const std::time_t tt = (std::time_t)now;
        std::tm lt = *std::localtime(&tt);
        std::tm gt = *std::gmtime(&tt);
        auto asUtc = [](std::tm t) {   // days-from-civil, no timegm needed
            const long y = t.tm_year + 1900 - (t.tm_mon < 2), m = t.tm_mon + 1;
            const long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
            const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.tm_mday - 1;
            const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return (double)((era * 146097 + doe - 719468) * 86400L + t.tm_hour * 3600L + t.tm_min * 60L + t.tm_sec);
        };
        busy = dect2::dailyBusy(S.resvFrom * 60, S.resvTo * 60, now - 86400, now + 2 * 86400, asUtc(lt) - asUtc(gt));
    }
    dect2::SchedParams prm; prm.leadSec = kSchedLead;
    S.plan = dect2::planPasses(ps, busy, now, prm);
    S.planAt = now; S.planRowsAt = S.computedAt; S.planDirty = false;
}

// The status of a listed pass in the plan (null when the scheduler is off or the pass is not in the plan)
const char* planStatus(int sat, double aos) {
    for (const dect2::SchedSlot& sl : S.plan.slots)
        if (sl.sp.sat == sat && std::fabs(sl.sp.pass.aos - aos) < 1) return sl.trimmed ? N_("trimmed") : N_("planned");
    for (const dect2::SchedDropped& d : S.plan.dropped)
        if (d.sp.sat == sat && std::fabs(d.sp.pass.aos - aos) < 1)
            return d.why == dect2::DropReason::Busy ? N_("skipped: SDR reserved") : d.why == dect2::DropReason::TooShort ? N_("skipped: too short") : N_("skipped: higher priority");
    return nullptr;
}

// The scheduler picks the next slot (the one under way is kept to its end) and aims the tracking below at it
void schedSelect(double now) {
    if (S.inPass) return;
    if (S.planDirty || now - S.planAt > 20 || S.planRowsAt != S.computedAt) replan(now);
    S.hasSlot = false;
    for (const dect2::SchedSlot& sl : S.plan.slots) {
        if (sl.end <= now || sl.sp.sat < 0 || sl.sp.sat >= (int)S.sats.size()) continue;
        S.hasSlot = true;
        S.slotPass = sl.sp.pass;
        S.slotRow.sat = sl.sp.sat; S.slotRow.p = sl.sp.pass;
        S.slotRow.p.aos = sl.start - kSchedLead; S.slotRow.p.los = sl.end;
        S.selSat = sl.sp.sat; S.selAos = sl.sp.pass.aos; S.selName = S.sats[sl.sp.sat].name;
        break;
    }
}

// once a frame, open or not: follow the tracked satellite's pass
void trackTick(App& a) {
    const double now = nowSec();
    const bool sch = S.sched && !S.sats.empty();
    if (sch) schedSelect(now);
    const bool have = S.selSat >= 0 && S.selSat < (int)S.sats.size();
    const bool track = S.track || sch, autoStart = S.autoStart || sch, autoRecord = S.autoRecord || sch;
    if (S.once && !S.track) { S.once = false; S.autoStart = S.prevStart; S.autoRecord = S.prevRec; }   // Track was switched off by hand
    if (S.inPass && (!have || !track || S.sats[S.selSat].name != S.passName)) { endPass(a, false); return; }
    if (S.inPass && now > S.passLos) { endPass(a, true); return; }
    if (!have || !track) { rotatorPassEnd(); return; }
    const Sat& sat = S.sats[S.selSat];
    const Row* r = nullptr;
    if (sch) {
        if (S.hasSlot && S.slotRow.sat == S.selSat) r = &S.slotRow;
        else if (!S.inPass) return;
    } else if (S.once) {   // armed for the chosen pass only, not for the satellite's next one
        for (const Row& x : S.rows) if (x.sat == S.selSat && std::fabs(x.p.aos - S.selAos) < 1 && x.p.los >= now) r = &x;
        if (!r && !S.inPass) { disarmOnce(); return; }   // it has gone (the list was rebuilt without it)
    } else r = activeOrNext(S.selSat);
    const bool active = r && r->p.aos <= now && now <= r->p.los;
    if (r && rotatorPassDrive()) {   // antenna mount: wait at the rise point, follow the pass (rotator_ui.cpp)
        dect2::Observer ob; ob.latDeg = S.lat; ob.lonDeg = S.lon; ob.altKm = S.altM / 1000;
        rotatorPassStep(dect2::planMountForPass(sat.sgp, ob, r->p, now));
    }
    if (active && !S.inPass) {   // AOS
        S.inPass = true; S.passLos = r->p.los; S.passName = sat.name; S.weStarted = false; S.weRecord = false; S.recTried = false; S.nominalMhz = sat.mhz; S.lastTune = 0;
        a.engine.log("pass of " + sat.name + " begins (AOS)");
        {   // pictures taken now are labelled with the pass (kept for ten minutes after LOS: the decoders finish a picture late)
            const dect2::Pass& pp = sch ? S.slotPass : r->p;
            gallery::setPassContext(sat.name, pp.aos, pp.maxElDeg, r->p.los + 600);
        }
        if (dect2::automation::wants(dect2::automation::FSatellite)) {   // the automations (satellite.aos)
            dect2::automation::Event ev("satellite.aos");
            ev.at(dect2::automation::wallNow(), "", sat.mhz * 1e6);
            ev.str("name", sat.name).num("max_el_deg", r->p.maxElDeg);
            dect2::automation::emit(std::move(ev));
        }
        if (sat.family == kLoraSat) a.engine.lorasat().setTracked(sat.norad, sat.name);   // the decoder follows the Doppler shift itself
        if (sat.family == kCubesat) a.engine.cubesat().setTracked(sat.norad, sat.name);   // names the frames of the pass
        if (autoStart && !a.engine.running()) {
            if (a.family != sat.family) selectMode(a, sat.family);
            a.freqMhz = sat.mhz;
            startReceiver(a);
            S.weStarted = a.engine.running();
        } else if (autoStart && a.family != sat.family) a.engine.log("the receiver runs another mode: the pass is tracked, the mode not changed");
    }
    if (!S.inPass) return;
    if (autoRecord && !S.recTried && a.engine.running() && !a.engine.recordingStats().active && a.devices[a.devIdx].isRadio()) {
        S.recTried = true;
        const FileFormat f = a.recFormat >= 1 ? FileFormat::CF32 : FileFormat::CS8;
        const std::string path = plat::dataDir() + "/recordings/" + (a.recFormat == 2 ? blueRecordingName(a.engine.recordingName(f)) : a.engine.recordingName(f));
        std::string err;
        if (a.engine.startRecording(path, f, err)) { S.weRecord = true; a.recDonePath.clear(); a.engine.log("pass of " + sat.name + ": recording IQ to " + path); }
        else a.engine.log("recording failed: " + err);
    }
    if (sat.family != kLoraSat && now - S.lastTune >= 1.0) {   // Doppler: about once a second (not for LoRa: a retune in the middle of a frame would break it, and the decoder follows the shift)
        S.lastTune = now;
        dect2::Look l;
        if (lookNow(sat, now, l)) retuneTo(a, (S.nominalMhz * 1e6 + dect2::dopplerHz(S.nominalMhz * 1e6, l.rangeRateKmS)) / 1e6);
    }
}

// A reminder in the last notifyMin minutes before the tracked satellite rises: one line over the top of the window (and one log line)
void aosBanner(App& a) {
    if (!S.track || S.inPass || S.selSat < 0 || S.selSat >= (int)S.sats.size() || S.notifyMin <= 0) return;
    const double now = nowSec();
    const Row* r = nullptr;
    for (const Row& x : S.rows) {
        if (x.sat != S.selSat || x.p.los < now) continue;
        if (S.once && std::fabs(x.p.aos - S.selAos) >= 1) continue;
        if (!r || x.p.aos < r->p.aos) r = &x;
    }
    if (!r) return;
    const double left = r->p.aos - now;
    if (left <= 0 || left > S.notifyMin * 60) return;
    const std::string& nm = S.sats[S.selSat].name;
    if (S.notifiedAos != r->p.aos) {
        S.notifiedAos = r->p.aos;
        a.engine.log("pass of " + nm + " begins in " + std::to_string((int)(left / 60)) + " min");
    }
    const int in = (int)left;
    char b[200];
    snprintf(b, sizeof b, tr(S.autoStart || S.autoRecord ? "Pass of %s in %d:%02d (max %.0f deg): the receiver starts by itself" : "Pass of %s in %d:%02d (max %.0f deg)"), nm.c_str(), in / 60, in % 60, r->p.maxElDeg);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(b), disp = ImGui::GetIO().DisplaySize;
    const float pad = 8 * gUi, x = (disp.x - ts.x) * 0.5f, y = 36 * gUi;
    dl->AddRectFilled(ImVec2(x - pad, y - pad * 0.5f), ImVec2(x + ts.x + pad, y + ts.y + pad * 0.5f), IM_COL32(20, 22, 24, 230), 4.f);
    dl->AddRect(ImVec2(x - pad, y - pad * 0.5f), ImVec2(x + ts.x + pad, y + ts.y + pad * 0.5f), ImGui::GetColorU32(pal::warnAmber()), 4.f);
    dl->AddText(ImVec2(x, y), ImGui::GetColorU32(pal::warnAmber()), b);
}

void skyPlot(const Row* r, float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    const float R = size * 0.5f - ImGui::GetFontSize() * 1.1f;
    const ImVec2 c(p0.x + size * 0.5f, p0.y + size * 0.5f);
    const ImU32 grid = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.5f), text = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    for (int k = 1; k <= 3; k++) dl->AddCircle(c, R * k / 3.f, grid, 64, 1.f);
    dl->AddLine(ImVec2(c.x - R, c.y), ImVec2(c.x + R, c.y), grid);
    dl->AddLine(ImVec2(c.x, c.y - R), ImVec2(c.x, c.y + R), grid);
    const char* nesw[4] = {"N", "E", "S", "W"};
    for (int k = 0; k < 4; k++) {
        const float a = k * 1.5707963f, d = R + ImGui::GetFontSize() * 0.55f;
        const ImVec2 ts = ImGui::CalcTextSize(nesw[k]);
        dl->AddText(ImVec2(c.x + d * std::sin(a) - ts.x * 0.5f, c.y - d * std::cos(a) - ts.y * 0.5f), text, nesw[k]);
    }
    dl->AddText(ImVec2(c.x + 2, c.y - R / 3.f * 2 - ImGui::GetFontSize()), text, "60");
    dl->AddText(ImVec2(c.x + 2, c.y - R / 3.f - ImGui::GetFontSize()), text, "30");
    if (!r) return;
    const Sat& s = S.sats[r->sat];
    auto at = [&](const dect2::Look& l) {
        const float rr = R * (float)(90 - std::max(0.0, l.elDeg)) / 90.f, az = (float)(l.azDeg * 3.14159265 / 180);
        return ImVec2(c.x + rr * std::sin(az), c.y - rr * std::cos(az));
    };
    std::vector<ImVec2> pts;
    const double step = std::max(5.0, (r->p.los - r->p.aos) / 120);
    for (double t = r->p.aos; t <= r->p.los + 0.1; t += step) { dect2::Look l; if (lookNow(s, std::min(t, r->p.los), l)) pts.push_back(at(l)); }
    const ImU32 acc = ImGui::GetColorU32(pal::accent());
    if (pts.size() > 1) dl->AddPolyline(pts.data(), (int)pts.size(), acc, 0, 2.f);
    if (!pts.empty()) {
        dl->AddCircleFilled(pts.front(), 3.5f, ImGui::GetColorU32(pal::okGreen()));
        dl->AddCircleFilled(pts.back(), 3.5f, ImGui::GetColorU32(pal::badRed()));
    }
    const double now = nowSec();
    if (now >= r->p.aos && now <= r->p.los) {
        dect2::Look l;
        if (lookNow(s, now, l)) { const ImVec2 q = at(l); dl->AddCircleFilled(q, 5.f, ImGui::GetColorU32(pal::warnAmber())); dl->AddCircle(q, 8.f, ImGui::GetColorU32(pal::warnAmber()), 24, 1.5f); }
    }
}

void tleSection(App& a) {
    if (!typo::collapsingHeader(trw("TLE data"), S.tles.empty() ? ImGuiTreeNodeFlags_DefaultOpen : 0)) return;
    ImGui::BeginDisabled(gDl.load() == 1);
    if (ImGui::Button(tr(gDl.load() == 1 ? "Downloading..." : "Download TLEs from Celestrak"))) startDownload(false);
    ImGui::SameLine();
    if (ImGui::Button(tr("Download TLEs from Keplerpass"))) startDownload(true);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        std::string tip = tr(S.autoTle ? "Downloads when you click, and by itself once a day. Fetches:" : "Downloads only when you click. Fetches:");
        for (const Source& s : kSources) tip += std::string("\n") + s.url;
        tip += "\nSaved in " + tleDir();
        ImGui::SetTooltip("%s", tip.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Automatic update"), &S.autoTle)) plat::prefs().setB("passTleAuto", S.autoTle);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Downloads the TLEs again once a day, only while this window is open or a pass is tracked. Without a network the saved lists are used."));
    ImGui::SameLine();
    if (ImGui::Button(tr(S.showPaste ? "Hide paste box" : "Paste TLEs..."))) S.showPaste = !S.showPaste;
    if (!S.msg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.msg.c_str()); }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("From: Celestrak, Keplerpass, and any TLE servers you add"));
    if (ImGui::Button(S.showSources ? tr("Hide TLE servers") : tr("Manage TLE servers"))) S.showSources = !S.showSources;
    if (S.showSources) {
        ImGui::TextDisabled("%s", tr("Built-in: Celestrak and Keplerpass (always available)"));
        ImGui::SetNextItemWidth(-140.f);
        ImGui::InputTextWithHint("##addTleUrl", "http://192.168.1.64/tle/active.tle", S.newSourceUrl, sizeof S.newSourceUrl);
        ImGui::SameLine();
        ImGui::BeginDisabled(gDl.load() == 1);
        if (ImGui::Button(tr("Add server"))) {
            const std::string url = S.newSourceUrl;
            const bool exists = std::any_of(S.customSources.begin(), S.customSources.end(),
                [&](const auto& src) { return src.second == url; });
            if (!validTleUrl(url) || url.find_first_of(" \t\r\n") != std::string::npos)
                S.msg = "Use HTTPS or HTTP for a local network server";
            else if (exists) S.msg = "Server already listed";
            else {
                unsigned id = 1;
                std::string file;
                do {
                    file = "user_" + std::to_string(id++) + ".tle";
                } while (std::any_of(S.customSources.begin(), S.customSources.end(),
                    [&](const auto& src) { return src.first == file; }) ||
                    fs::exists(fsPath(tleDir() + "/" + file)));
                S.customSources.emplace_back(file, url);
                saveCustomSources();
                S.newSourceUrl[0] = '\0';
                S.msg = "TLE server saved";
            }
        }
        for (size_t i = 0; i < S.customSources.size();) {
            ImGui::PushID((int)i);
            const auto item = S.customSources[i];
            ImGui::TextWrapped("%s", item.second.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Download"))) startDownload(false, item.first, item.second);
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Delete"))) {
                std::error_code ec;
                fs::remove(fsPath(tleDir() + "/" + item.first), ec);
                S.customSources.erase(S.customSources.begin() + (std::ptrdiff_t)i);
                saveCustomSources();
                loadTles();
                S.msg = "TLE server deleted";
                ImGui::PopID();
                continue;
            }
            ImGui::PopID();
            ++i;
        }
        ImGui::EndDisabled();
    }
    ImGui::PopTextWrapPos();
    double oldest = 0;
    for (const Sat& s : S.sats) oldest = std::max(oldest, nowSec() / 86400.0 + 2440587.5 - s.sgp.tle().epochJd);
    {
        const double ok = plat::prefs().getD("passTleOk", 0);
        if (ok > 0 && !S.tles.empty()) ImGui::TextDisabled(tr("last downloaded %s"), timeText(ok).c_str());
    }
    if (S.tles.empty()) ImGui::TextColored(pal::warnAmber(), "%s", tr("No TLEs yet: download them or paste some."));
    else {
        ImGui::Text(tr("%d TLEs, %d satellites listed"), (int)S.tles.size(), (int)S.sats.size());
        if (oldest > 14) { ImGui::SameLine(); ImGui::TextColored(pal::warnAmber(), tr("the oldest is %.0f days old: download again"), oldest); }
    }
    if (!S.missing.empty() && !S.tles.empty()) {
        std::string m = std::string(tr("No TLE for:")) + " ";
        for (size_t i = 0; i < S.missing.size(); i++) m += (i ? ", " : "") + S.missing[i];
        ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", m.c_str()); ImGui::PopTextWrapPos();
    }
    if (S.showPaste) {
        ImGui::InputTextMultiline("##tlepaste", S.paste.data(), S.paste.size(), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8));
        if (ImGui::Button(tr("Use pasted TLEs"))) {
            const std::string txt(S.paste.data());
            const std::vector<dect2::Tle> v = dect2::parseTleText(txt);
            if (v.empty()) S.msg = tr("no valid TLE in the pasted text (two lines per satellite, checksums must match)");
            else {
                std::error_code ec;
                fs::create_directories(fsPath(tleDir()), ec);
                std::string old = readText(tleDir() + "/" + kPasted);
                std::ofstream f(fsPath(tleDir() + "/" + kPasted), std::ios::binary);
                f << old << (old.empty() || old.back() == '\n' ? "" : "\n") << txt << "\n";
                f.close();
                S.msg = std::string(tr("pasted TLEs added:")) + " " + std::to_string(v.size());
                S.paste[0] = 0;
                loadTles();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Forget pasted TLEs"))) { std::error_code ec; fs::remove(fsPath(tleDir() + "/" + kPasted), ec); loadTles(); S.msg = "pasted TLEs removed"; }
    }
    if (ImGui::TreeNode(trw("Amateur satellites"))) {
        int del = -1;
        for (int i = 0; i < (int)S.cubes.size(); i++) {
            ImGui::PushID(i);
            ImGui::Text(tr("%s  #%d  %.3f MHz%s"), S.cubes[i].name.c_str(), S.cubes[i].norad, S.cubes[i].mhz, S.cubes[i].aprs ? "  APRS" : "");
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Remove"))) del = i;
            ImGui::PopID();
        }
        if (del >= 0) { S.cubes.erase(S.cubes.begin() + del); saveCubes(); savePrefs(a); buildSats(); }
        ImGui::SetNextItemWidth(130 * gUi); ImGui::InputTextWithHint("##cname", tr("name"), S.cubeName, sizeof S.cubeName);
        ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi); ImGui::InputInt("##cno", &S.cubeNorad, 0, 0);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("NORAD catalogue number"));
        ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ImGui::InputDouble("##cmhz", &S.cubeMhz, 0, 0, "%.3f MHz");
        ImGui::SameLine(); ImGui::Checkbox(tr("APRS"), &S.cubeAprs);
        ImGui::SameLine();
        ImGui::BeginDisabled(!S.cubeName[0] || S.cubeNorad <= 0 || S.cubeMhz <= 0);
        if (ImGui::Button(tr("Add"))) {
            Cube c; c.name = S.cubeName; c.norad = S.cubeNorad; c.mhz = S.cubeMhz; c.aprs = S.cubeAprs;
            for (auto& ch : c.name) if (ch == '|' || ch == ';') ch = ' ';
            S.cubes.push_back(c); saveCubes(); savePrefs(a); buildSats();
            S.cubeName[0] = 0; S.cubeNorad = 0;
        }
        ImGui::EndDisabled();
        ImGui::TreePop();
    }
}

// ---- Orbit from Doppler: measure the carrier of the satellite being tracked and fit the elements to it (core/src/geo/doppler_fit.cpp)

void dopplerCollect(App& a) {
    if (!D.measure || !S.inPass || !a.engine.running() || S.selSat < 0 || S.selSat >= (int)S.sats.size()) return;
    const Sat& sat = S.sats[S.selSat];
    if (a.spec.seq == D.lastSeq || a.spec.dbfs.empty()) return;
    const double now = (double)std::time(nullptr);
    D.lastSeq = a.spec.seq;
    if (D.norad != sat.norad || (!D.smp.empty() && now - D.smp.back().t > 900)) {   // another satellite or another pass: start over
        D.smp.clear(); D.fitted = false; D.norad = sat.norad; D.name = sat.name; D.nominalHz = S.nominalMhz * 1e6; D.tle = sat.sgp.tle(); D.msg.clear();
    }
    if (now - D.lastT < 1.0) return;
    D.lastT = now;
    dect2::Look l;
    if (!lookNow(sat, now, l) || l.elDeg < 3) return;
    const double fs = a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate;
    dect2::DopplerSample x;
    const double pred = D.nominalHz + dect2::dopplerHz(D.nominalHz, l.rangeRateKmS);
    static const dect2::DopplerTracker trk;
    if (trk.feed(now, a.spec.dbfs.data(), a.spec.dbfs.size(), a.freqMhz * 1e6, fs, D.nominalHz, pred, D.gateKhz * 1000.0, x)) D.smp.push_back(x);
}

void dopplerPlot(float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    const ImU32 grid = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.4f), text = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), grid);
    if (D.smp.size() < 2) { dl->AddText(ImVec2(p0.x + 6, p0.y + 4), text, tr("no measurements yet")); return; }
    const double t0 = D.smp.front().t, t1 = D.smp.back().t;
    double lo = 1e18, hi = -1e18;
    for (const auto& x : D.smp) { lo = std::min(lo, x.hz); hi = std::max(hi, x.hz); }
    if (hi - lo < 100) { lo -= 50; hi += 50; }
    const double pad = (hi - lo) * 0.08; lo -= pad; hi += pad;
    const float resH = D.fitted ? h * 0.28f : 0.f, mainH = h - resH - (D.fitted ? 6.f : 0.f);
    auto X = [&](double t) { return p0.x + float((t - t0) / std::max(1.0, t1 - t0)) * w; };
    auto Y = [&](double hz) { return p0.y + mainH - float((hz - lo) / (hi - lo)) * mainH; };
    dect2::Sgp4 orig;
    const bool haveOrig = orig.init(D.tle);
    // predicted by the unrefined elements: the curve (a faint line) plus the receiver offset it cannot know about
    if (haveOrig) {
        std::vector<ImVec2> pts;
        double lat = S.lat, lon = S.lon; dect2::Observer o; o.latDeg = lat; o.lonDeg = lon; o.altKm = S.altM / 1000;
        for (int k = 0; k <= 100; k++) { const double t = t0 + (t1 - t0) * k / 100; dect2::Look l; if (dect2::lookAt(orig, o, t, l)) pts.push_back(ImVec2(X(t), Y(dect2::dopplerHz(D.nominalHz, l.rangeRateKmS)))); }
        if (pts.size() > 1) dl->AddPolyline(pts.data(), (int)pts.size(), ImGui::GetColorU32(ImGuiCol_TextDisabled), 0, 1.5f);
    }
    if (D.fitted) {
        std::vector<ImVec2> pts;
        for (size_t i = 0; i < D.smp.size() && i < D.fit.predictedHz.size(); i++) pts.push_back(ImVec2(X(D.smp[i].t), Y(D.fit.predictedHz[i])));
        if (pts.size() > 1) dl->AddPolyline(pts.data(), (int)pts.size(), ImGui::GetColorU32(pal::accent()), 0, 2.f);
    }
    for (const auto& x : D.smp) dl->AddCircleFilled(ImVec2(X(x.t), Y(x.hz)), 2.2f, ImGui::GetColorU32(pal::okGreen()));
    char b[48];
    snprintf(b, sizeof b, "%+.0f Hz", hi); dl->AddText(ImVec2(p0.x + 3, p0.y + 1), text, b);
    snprintf(b, sizeof b, "%+.0f Hz", lo); dl->AddText(ImVec2(p0.x + 3, p0.y + mainH - ImGui::GetFontSize() - 1), text, b);
    if (D.fitted) {   // the residuals of the refined curve
        const float y0 = p0.y + mainH + 6.f;
        dl->AddRect(ImVec2(p0.x, y0), ImVec2(p0.x + w, y0 + resH), grid);
        double m = 10;
        for (double r : D.fit.residualHz) m = std::max(m, std::fabs(r));
        dl->AddLine(ImVec2(p0.x, y0 + resH * 0.5f), ImVec2(p0.x + w, y0 + resH * 0.5f), grid);
        for (size_t i = 0; i < D.smp.size() && i < D.fit.residualHz.size(); i++) dl->AddCircleFilled(ImVec2(X(D.smp[i].t), y0 + resH * 0.5f - float(D.fit.residualHz[i] / m) * resH * 0.5f), 1.8f, ImGui::GetColorU32(pal::warnAmber()));
        snprintf(b, sizeof b, "+-%.0f Hz", m); dl->AddText(ImVec2(p0.x + 3, y0 + 1), text, b);
    }
}

void dopplerSection(App& a) {
    if (!ImGui::CollapsingHeader(trw("Orbit from Doppler"))) return;
    ImGui::TextWrapped("%s", tr("Follows the carrier of the tracked satellite during a pass and fits its orbit to the Doppler curve. Needs a steady frequency reference (a GPS-disciplined or well calibrated receiver); an error of the receiver's clock is fitted as a constant offset."));
    if (ImGui::Checkbox(tr("Measure the carrier during tracked passes"), &D.measure)) plat::prefs().setB("passDopMeasure", D.measure);
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); ImGui::SliderFloat("##dopgate", &D.gateKhz, 1, 10, tr("gate +-%.0f kHz"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How far from the predicted carrier the peak is looked for"));
    ImGui::Text(tr("%d measurements of %s"), (int)D.smp.size(), D.name.empty() ? "-" : D.name.c_str());
    ImGui::BeginDisabled(D.smp.size() < 12);
    if (ImGui::Button(tr("Fit orbit"))) {
        dect2::Observer o; o.latDeg = S.lat; o.lonDeg = S.lon; o.altKm = S.altM / 1000;
        D.fitted = dect2::fitDopplerOrbit(D.tle, o, D.nominalHz, D.smp, dect2::DopplerFitOptions(), D.fit);
        D.msg = D.fitted ? "" : D.fit.why;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear"))) { D.smp.clear(); D.fitted = false; D.msg.clear(); }
    ImGui::SameLine();
    ImGui::BeginDisabled(!D.fitted);
    if (ImGui::Button(tr("Save refined TLE"))) {
        std::error_code ec;
        fs::create_directories(fsPath(tleDir()), ec);
        std::string old = readText(tleDir() + "/" + kDerived);
        std::ofstream f(fsPath(tleDir() + "/" + kDerived), std::ios::binary);
        f << old << dect2::derivedTleText(D.fit, D.nominalHz);
        { char mb[600]; snprintf(mb, sizeof mb, tr("saved in %s"), (tleDir() + "/" + kDerived).c_str()); D.msg = mb; }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Writes the refined elements to a separate file, marked as derived from this measurement; the downloaded TLEs are not touched"));
    if (ImGui::Checkbox(tr("Use the derived elements for predictions"), &D.useDerived)) { plat::prefs().setB("passUseDerived", D.useDerived); loadTles(); buildSats(); S.dirty = true; }
    if (!D.msg.empty()) ImGui::TextDisabled("%s", D.msg.c_str());
    dopplerPlot(std::max(200.f, ImGui::GetContentRegionAvail().x), 170 * gUi);
    if (D.fitted) {
        const dect2::DopplerFit& f = D.fit;
        ImGui::TextDisabled("%s", tr("grey: predicted by the downloaded elements, blue: refined, green: measured, amber: residual of the refined curve"));
        ImGui::Text(tr("residual %.1f Hz -> %.1f Hz (%d used, %d rejected); receiver offset %+.0f Hz"), f.rmsBeforeHz, f.rmsAfterHz, f.used, f.rejected, f.offsetHz);
        ImGui::Text(tr("mean anomaly %+.3f deg, mean motion %+.5f rev/day, inclination %+.4f deg, RAAN %+.4f deg"), f.dMeanAnomDeg, f.dMeanMotion, f.dInclDeg, f.dRaanDeg);
        ImGui::Text(tr("estimated timing error of the prediction: %.1f s before, %.1f s after (1 sigma)"), f.alongTrackSigmaBeforeSec, f.alongTrackSigmaSec);
    }
}

// The next 24 hours as a strip: the passes the scheduler takes on the top lane (amber: trimmed), the ones it skips below
void calendarStrip(double now) {
    const float w = std::max(200.f, ImGui::GetContentRegionAvail().x), h = 52 * gUi;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##cal", ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg), dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), bg, 3.f);
    const double span = 86400;
    auto xOf = [&](double t) { return p0.x + (float)((std::max(now, std::min(now + span, t)) - now) / span) * w; };
    {   // hour ticks in local time
        const std::time_t tt = (std::time_t)now;
        std::tm lt = *std::localtime(&tt);
        const double first = now + (3600 - (lt.tm_min * 60 + lt.tm_sec));
        for (int i = 0; i < 24; i++) {
            const double t = first + i * 3600.0;
            const std::time_t t2 = (std::time_t)t;
            std::tm l2 = *std::localtime(&t2);
            const float x = xOf(t);
            const bool lab = l2.tm_hour % 3 == 0;
            dl->AddLine(ImVec2(x, p0.y + h - (lab ? 12 : 5) * gUi), ImVec2(x, p0.y + h), dim);
            if (lab) { char b[8]; snprintf(b, sizeof b, "%d", l2.tm_hour); dl->AddText(ImVec2(x + 2, p0.y + h - 14 * gUi), dim, b); }
        }
    }
    const float laneH = 14 * gUi;
    const dect2::SchedSlot* hov = nullptr; const dect2::SchedDropped* hovD = nullptr;
    const ImVec2 mp = ImGui::GetIO().MousePos;
    for (const dect2::SchedDropped& d : S.plan.dropped) {
        if (d.sp.pass.los <= now || d.sp.pass.aos >= now + span) continue;
        const ImVec2 a(xOf(d.sp.pass.aos), p0.y + 3 * gUi + laneH + 2), b(std::max(xOf(d.sp.pass.los), xOf(d.sp.pass.aos) + 2), p0.y + 3 * gUi + 2 * laneH);
        dl->AddRectFilled(a, b, (dim & 0x00FFFFFF) | 0x80000000, 2.f);
        if (ImGui::IsItemHovered() && mp.x >= a.x && mp.x <= b.x && mp.y >= a.y && mp.y <= b.y) hovD = &d;
    }
    for (const dect2::SchedSlot& sl : S.plan.slots) {
        if (sl.end <= now || sl.start >= now + span) continue;
        const ImVec2 a(xOf(sl.start), p0.y + 3 * gUi), b(std::max(xOf(sl.end), xOf(sl.start) + 2), p0.y + 3 * gUi + laneH);
        dl->AddRectFilled(a, b, ImGui::GetColorU32(sl.trimmed ? pal::warnAmber() : pal::okGreen()), 2.f);
        if (ImGui::IsItemHovered() && mp.x >= a.x && mp.x <= b.x && mp.y >= a.y && mp.y <= b.y) hov = &sl;
    }
    dl->AddLine(ImVec2(p0.x, p0.y), ImVec2(p0.x, p0.y + h), ImGui::GetColorU32(ImGuiCol_Text), 2.f);
    const dect2::SchedPass* hp = hov ? &hov->sp : hovD ? &hovD->sp : nullptr;
    if (hp && hp->sat >= 0 && hp->sat < (int)S.sats.size()) {
        const double a0 = hov ? hov->start : hp->pass.aos, a1 = hov ? hov->end : hp->pass.los;
        ImGui::SetTooltip(tr("%s - %s, max %.0f deg"), (S.sats[hp->sat].name + "  " + timeText(a0)).c_str(), timeText(a1).c_str(), hp->pass.maxElDeg);
    }
}

void schedControls() {
    plat::Prefs& pr = plat::prefs();
    if (ImGui::Checkbox(tr("Schedule passes by itself"), &S.sched)) { pr.setB("passSched", S.sched); S.planDirty = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Takes the passes of the ticked groups one at a time, highest priority first, and starts, tunes and records each one without you. Overlapping passes are trimmed or skipped; a receiver you started yourself is left alone."));
    if (!S.sched) return;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Priority (higher wins)"));
    for (int g = 0; g < NumGroups; g++) {
        ImGui::SameLine();
        ImGui::TextUnformatted(tr(kGroupLabel[g]));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60 * gUi);
        char id[16]; snprintf(id, sizeof id, "##prio%d", g);
        if (ImGui::SliderInt(id, &S.prio[g], 0, 9)) { char k[24]; snprintf(k, sizeof k, "passPrio%d", g); pr.setD(k, S.prio[g]); S.planDirty = true; }
    }
    if (ImGui::Checkbox(tr("Keep the SDR free every day"), &S.resv)) { pr.setB("passResv", S.resv); S.planDirty = true; }
    if (S.resv) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
        if (ImGui::SliderInt("##rf", &S.resvFrom, 0, 23, tr("from %d:00"))) { pr.setD("passResvFrom", S.resvFrom); S.planDirty = true; }
        ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
        if (ImGui::SliderInt("##rt", &S.resvTo, 0, 24, tr("to %d:00"))) { pr.setD("passResvTo", S.resvTo); S.planDirty = true; }
    }
    if (S.plan.slots.empty()) ImGui::TextDisabled("%s", tr("Nothing planned."));
    else calendarStrip(nowSec());
}

void drawWindow(App& a) {
    static bool full = false;   // the window over the whole display (the shared full-screen button; Esc leaves)
    { static bool af = std::getenv("ONAIR_MAP_FULL") != nullptr; if (af) { af = false; full = true; } }   // dev: --shot enters full screen
    if (full && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) full = false;
    if (full) { ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize); }
    else firstWindowSize(820 * gUi, 560 * gUi);
    if (!ImGui::Begin(trw("Passes"), &a.passesOpen, full ? ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse : 0)) { ImGui::End(); return; }
    {   // where the receiver is
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", tr("Receiver position"));
        ImGui::SameLine();
        receiverPositionFields(a);
        if (!receiverLocationSet()) { ImGui::SameLine(); ImGui::TextColored(pal::warnAmber(), "%s", tr("not set: the times are for a placeholder position")); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The same position as the ADS-B antenna position"));
        ImGui::SameLine();
        if (adsbmap::fullButton(full, "screen")) full = !full;
    }
    tleSection(a);
    dopplerSection(a);
    {   // filters
        bool ch = false;
        for (int g = 0; g < NumGroups; g++) {
            if (g) ImGui::SameLine();
            if (ImGui::Checkbox(tr(kGroupLabel[g]), &S.show[g])) { char k[24]; snprintf(k, sizeof k, "passShow%d", g); plat::prefs().setB(k, S.show[g]); ch = true; }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::SliderFloat("##minel", &S.minEl, 0, 45, "min el %.0f deg")) { S.dirty = true; plat::prefs().setD("passMinEl", S.minEl); ch = true; }
        if (ch) savePrefs(a);
    }
    schedControls();
    compute(a);
    const float plotW = full ? std::max(250 * gUi, ImGui::GetContentRegionAvail().x * 0.42f) : 250 * gUi;
    const double now = nowSec();
    // the table of passes
    ImGui::BeginChild("##passlist", ImVec2(std::max(200.f, ImGui::GetContentRegionAvail().x - plotW - 8 * gUi), 0));
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##passes", S.sched ? 8 : 7, tf)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Satellite"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("AOS"));
        ImGui::TableSetupColumn(tr("LOS"));
        ImGui::TableSetupColumn(tr("Max el"));
        ImGui::TableSetupColumn(tr("Duration"));
        ImGui::TableSetupColumn(tr("Frequency"));
        ImGui::TableSetupColumn(tr("Mode"));
        if (S.sched) ImGui::TableSetupColumn(tr("Status"));
        ImGui::TableHeadersRow();
        int shown = 0;
        for (const Row& r : S.rows) {
            const Sat& s = S.sats[r.sat];
            if (!S.show[s.group] || r.p.los < now) continue;
            shown++;
            const bool active = r.p.aos <= now && now <= r.p.los;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((int)(r.sat * 100003 + (int64_t)r.p.aos % 100000));
            const bool sel = S.selSat == r.sat && std::fabs(S.selAos - r.p.aos) < 1;
            if (active) ImGui::PushStyleColor(ImGuiCol_Text, pal::okGreen());
            if (ImGui::Selectable(s.name.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) { S.selSat = r.sat; S.selAos = r.p.aos; S.selName = s.name; }
            if (active) ImGui::PopStyleColor();
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(active ? tr("now") : timeText(r.p.aos).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(timeText(r.p.los).c_str());
            ImGui::TableNextColumn(); ImGui::Text(tr("%.0f deg"), r.p.maxElDeg);
            ImGui::TableNextColumn(); { const int d = (int)std::lround(r.p.los - r.p.aos); ImGui::Text("%d:%02d", d / 60, d % 60); }
            ImGui::TableNextColumn(); ImGui::Text("%.3f", s.mhz);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.mode.c_str());
            if (ImGui::IsItemHovered() && s.family == kSpecx) ImGui::SetTooltip("%s", tr("OnAir has no decoder for this: auto start opens the Spectrum explorer, and Auto record keeps the IQ"));
            if (S.sched) { ImGui::TableNextColumn(); const char* st = planStatus(r.sat, r.p.aos); ImGui::TextUnformatted(st ? tr(st) : ""); }
        }
        ImGui::EndTable();
        if (!shown) ImGui::TextDisabled("%s", tr(S.tles.empty() ? "No TLEs: open TLE data above." : "No passes in the next 24 hours."));
    }
    ImGui::EndChild();
    ImGui::SameLine();
    // the sky plot and tracking of the chosen pass (or the current / next one)
    ImGui::BeginChild("##passsky", ImVec2(0, 0));
    const Row* r = nullptr;
    if (S.selSat >= 0 && S.selSat < (int)S.sats.size()) {
        for (const Row& x : S.rows) if (x.sat == S.selSat && std::fabs(x.p.aos - S.selAos) < 1 && x.p.los >= now) r = &x;
        if (!r) r = activeOrNext(S.selSat);
    }
    if (!r) r = activeOrNext(-1);
    if (r) {
        const Sat& s = S.sats[r->sat];
        ImGui::TextUnformatted(s.name.c_str());
        ImGui::TextDisabled(tr("%s - %s, max %.0f deg"), timeText(r->p.aos).c_str(), timeText(r->p.los).c_str(), r->p.maxElDeg);
    } else ImGui::TextDisabled("%s", tr("no pass"));
    skyPlot(r, std::min(ImGui::GetContentRegionAvail().x, plotW));
    if (r && now >= r->p.aos && now <= r->p.los) {
        dect2::Look l;
        if (lookNow(S.sats[r->sat], now, l))
            ImGui::Text(tr("az %.0f  el %.0f  %.0f km\nDoppler %+.0f Hz"), l.azDeg, l.elDeg, l.rangeKm, dect2::dopplerHz(S.sats[r->sat].mhz * 1e6, l.rangeRateKmS));
    } else if (r) {
        const int in = (int)std::max(0.0, r->p.aos - now);
        ImGui::TextDisabled(tr("AOS in %d:%02d:%02d"), in / 3600, in / 60 % 60, in % 60);
    }
    ImGui::Separator();
    const bool haveSel = S.selSat >= 0 && S.selSat < (int)S.sats.size();
    ImGui::BeginDisabled(!haveSel || S.sched);
    std::string tl = haveSel ? std::string(tr("Track")) + " " + S.sats[S.selSat].name : std::string(tr("Track (choose a pass)"));
    ImGui::Checkbox(tl.c_str(), &S.track);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("During each pass of this satellite, retune about once a second for the Doppler shift"));
    ImGui::BeginDisabled(!S.track);
    ImGui::Checkbox(tr("Auto start at AOS, stop at LOS"), &S.autoStart);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Switches to the satellite's mode and starts the receiver at AOS (when it is stopped), and stops it at LOS"));
    {
        bool drive = rotatorPassWanted();
        if (ImGui::Checkbox(tr("Point the antenna mount"), &drive)) rotatorPassSetDrive(drive);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Drives the rotator or telescope mount set up in View > Antenna rotator: waits at the rise point, follows the satellite, parks at LOS"));
    }
    ImGui::Checkbox(tr("Auto record IQ"), &S.autoRecord);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(tr("Records the radio's IQ during the pass, as Record IQ does (format from the Record IQ setting), into %s/recordings"), plat::dataDir().c_str());
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::SliderFloat("##notmin", &S.notifyMin, 0, 30, tr("remind %.0f min before"))) plat::prefs().setD("passNotifyMin", S.notifyMin);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("A line at the top of the window and a log entry before the tracked pass rises (0: off)"));
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    {   // one pass, everything armed
        const bool canArm = r && r->p.los > now && !S.inPass;
        ImGui::BeginDisabled((!canArm && !S.once) || S.sched);
        const bool armed = S.once;
        if (ImGui::Button(tr(armed ? "Cancel recording this pass" : "Record this pass"))) {
            if (armed) { disarmOnce(); }
            else {
                S.selSat = r->sat; S.selAos = r->p.aos; S.selName = S.sats[r->sat].name;
                S.prevStart = S.autoStart; S.prevRec = S.autoRecord;
                S.once = true; S.track = true; S.autoStart = true; S.autoRecord = true;
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Arms this pass only: at AOS the receiver starts in the satellite's mode and frequency (when it is stopped), Doppler is followed and the IQ is recorded; at LOS it stops and tracking switches off again"));
        if (S.once) ImGui::TextColored(pal::okGreen(), tr("armed: %s at %s"), S.selName.c_str(), timeText(S.selAos).c_str());
    }
    if (S.inPass) ImGui::TextColored(pal::okGreen(), tr("tracking%s"), S.weRecord ? tr(", recording") : "");
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace

// dev: --tle <file> loads TLEs from a file as pasted ones would (tests without the network)
void passesDevTle(const std::string& path) {
    loadState();
    for (const dect2::Tle& t : dect2::parseTleText(readText(path))) S.tles[t.norad] = t;
    buildSats();
}

const dect2::Sgp4* passesFindSat(const std::string& part, double unixSec, std::string* name) {
    loadState();
    double lat, lon;
    receiverLocation(lat, lon);
    dect2::Observer o; o.latDeg = lat; o.lonDeg = lon; o.altKm = receiverAltitudeM() / 1000;
    const Sat* best = nullptr;
    double bestEl = -1e9;
    for (const Sat& s : S.sats) {
        if (s.name.find(part) == std::string::npos) continue;
        dect2::Look l;
        const double el = dect2::lookAt(s.sgp, o, unixSec, l) ? l.elDeg : -90;
        if (!best || el > bestEl) { best = &s; bestEl = el; }
    }
    if (!best) return nullptr;
    if (name) *name = best->name;
    return &best->sgp;
}

void passesMapSatellites(std::vector<MapSatRef>& out) {
    loadState();
    for (const Sat& s : S.sats) out.push_back({s.name, s.norad, &s.sgp});
}

void passesWindow(App& a) {
    loadState();
    if (!S.sats.empty() && (S.track || S.sched || a.passesOpen)) compute(a);
    pollDownload();
    autoUpdateTle(a.passesOpen || S.track || S.sched);
    trackTick(a);
    dopplerCollect(a);
    aosBanner(a);
    if (a.passesOpen) drawWindow(a);
}

// The Argos mode's overlay: the MetOp satellites (Argos downlink, kArgos rows of kKnown) above the horizon at this time and the Doppler of a
// carrier at freqHz for the receiver's position (empty without TLEs or a position).
std::vector<std::pair<std::string, double>> passesArgosDoppler(double unixSec, double freqHz) {
    loadState();
    std::vector<std::pair<std::string, double>> out;
    if (!receiverLocationSet()) return out;
    double lat, lon;
    receiverLocation(lat, lon);
    dect2::Observer o; o.latDeg = lat; o.lonDeg = lon; o.altKm = receiverAltitudeM() / 1000;
    for (const Sat& s : S.sats) {
        if (s.family != kArgos) continue;
        dect2::Look l;
        if (dect2::lookAt(s.sgp, o, unixSec, l) && l.elDeg > 0) out.push_back({s.name, dect2::dopplerHz(freqHz, l.rangeRateKmS)});
    }
    return out;
}

// The Globalstar activity monitor's overlay: every Globalstar satellite above the horizon at this time with the Doppler shift a link to it would have
// at freqHz for the receiver's position (names and Doppler, empty without TLEs).
std::vector<std::pair<std::string, double>> passesGlobalstarDoppler(double unixSec, double freqHz) {
    loadState();
    std::vector<std::pair<std::string, double>> out;
    if (!receiverLocationSet()) return out;
    double lat, lon;
    receiverLocation(lat, lon);
    dect2::Observer o; o.latDeg = lat; o.lonDeg = lon; o.altKm = receiverAltitudeM() / 1000;
    for (const Sat& s : S.sats) {
        if (s.family != kGlobalstar) continue;
        dect2::Look l;
        if (dect2::lookAt(s.sgp, o, unixSec, l) && l.elDeg > 0) out.push_back({s.name, dect2::dopplerHz(freqHz, l.rangeRateKmS)});
    }
    return out;
}
