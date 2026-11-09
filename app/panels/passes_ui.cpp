// The satellite tracker (View > Passes, docs/SATTRACK-3.8.md): what is overhead now and the passes of the next two days over the receiver's position, for the
// satellites OnAir's modes receive (the weather and ISS lists here, the CubeSat table of the Smart decoder and the LoRa presets), with filters on the kind of satellite,
// peak elevation and what the radio and antenna can hear; a sky plot and a map with the track and footprint; a timeline; and one click to arm a pass:
// the planner (geo/pass_sched.h) settles conflicts by priority and the runner (geo/sat_plan.h) tunes, sets the mode, follows the Doppler shift, records and restores
// the receiver. TLEs: the built-in CubeSat snapshot, then Celestrak / Keplerpass downloads (newest epoch wins), pasted ones on top.
#include "dect2/common/thread_name.h"
#include "../app.h"
#include "../ui/typography.h"
#include "../ui/ease_logic.h"
#include "gallery.h"
#include "../adsb/adsb_map.h"
#include "dect2/geo/passes.h"
#include "dect2/geo/doppler_fit.h"
#include "dect2/lorasat/lorasat_presets.h"
#include "dect2/geo/pass_sched.h"
#include "dect2/geo/sat_plan.h"
#include "dect2/geo/maplayers.h"
#include "dect2/cubesat/cubesat_db.h"
#include "dect2/source/mount_track.h"
#include "dect2/automation/engine.h"
#include <algorithm>
#include <atomic>
#include <cctype>
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
void hfdigSetDemod(const App& a, int d);           // hfdig_ui.cpp
int hfdigDemod(const App& a);

namespace {

namespace fs = std::filesystem;
fs::path fsPath(const std::string& p) { return fs::path(reinterpret_cast<const char8_t*>(p.c_str())); }

constexpr int kNoaaApt = 260, kNoaaHrpt = 261, kNoaaDsb = 263, kMeteorHrpt = 264, kArktikaGgak = 267, kAws = 268;   // families of the wxsat links
constexpr int kIssSstv = kPassFamilyIssSstv, kHfdig = 25;   // the ISS's SSTV events: the SSTV entry of the HF digital family on FM (AppHost)
constexpr int kSpecx = 41, kAprs = 24, kOrbcomm = 44, kMeteor = 59, kCubesat = 61, kGlobalstar = 62, kArgos = 152, kLoraSat = 201;   // mode families: Spectrum explorer (no decoder), APRS (AFSK), Orbcomm, Meteor-M LRPT, CubeSat telemetry, Argos, LoRa satellites

struct Source { const char* file; const char* url; };
const Source kSources[] = {
    {"weather.txt", "https://celestrak.org/NORAD/elements/gp.php?GROUP=weather&FORMAT=tle"},
    {"stations.txt", "https://celestrak.org/NORAD/elements/gp.php?GROUP=stations&FORMAT=tle"},
    {"orbcomm.txt", "https://celestrak.org/NORAD/elements/gp.php?GROUP=orbcomm&FORMAT=tle"},
    {"amateur.txt", "https://celestrak.org/NORAD/elements/gp.php?GROUP=amateur&FORMAT=tle"},
    {"globalstar.txt", "https://celestrak.org/NORAD/elements/gp.php?GROUP=globalstar&FORMAT=tle"},
    {"cubesat.txt", "https://celestrak.org/NORAD/elements/gp.php?GROUP=cubesat&FORMAT=tle"},
};
const char* const kPasted = "pasted.txt";
const char* const kDerived = "derived_doppler.tle";
const Source kKeplerpass = {"keplerpass.txt", "https://data.keplerpass.com/PU4ELT/tle.txt"};

enum Group { Weather, Iss, Orbcomm, Amateur, GlobalstarGrp, CubesatGrp, NumGroups };   // the first five keep their settings keys (passShow0..4, passPrio0..4); bit g of a filter is SatTag 1 << g
const char* const kGroupLabel[NumGroups] = {N_("Weather"), "ISS", "Orbcomm", N_("Amateur"), "Globalstar", N_("CubeSat")};

// needMsps: the sample rate the signal needs where that excludes small radios; retired: off the air for good (hidden unless asked for)
struct Known { const char* name; int norad; double mhz; const char* mode; int family; int group; double needMsps; bool retired; };
const Known kKnown[] = {
    // NOAA 15, 18 and 19 were decommissioned in 2025 (NOAA OSPO notices): kept for old recordings, hidden by default
    {"NOAA 15", 25338, 137.620, "APT", kNoaaApt, Weather, 0, true},
    {"NOAA 18", 28654, 137.9125, "APT", kNoaaApt, Weather, 0, true},
    {"NOAA 19", 33591, 137.100, "APT", kNoaaApt, Weather, 0, true},
    {"NOAA 15 DSB", 25338, 137.350, "DSB", kNoaaDsb, Weather, 0, true},
    {"NOAA 18 DSB", 28654, 137.350, "DSB", kNoaaDsb, Weather, 0, true},
    {"NOAA 19 DSB", 33591, 137.770, "DSB", kNoaaDsb, Weather, 0, true},
    {"NOAA 15 HRPT", 25338, 1702.5, "HRPT", kNoaaHrpt, Weather, 3, true},
    {"NOAA 18 HRPT", 28654, 1707.0, "HRPT", kNoaaHrpt, Weather, 3, true},
    {"NOAA 19 HRPT", 33591, 1698.0, "HRPT", kNoaaHrpt, Weather, 3, true},
    {"Meteor-M N2-3", 57166, 137.900, "LRPT", kMeteor, Weather, 0, false},   // SatDump's list: 137.9 MHz, may move between .1 and .9 and 72k / 80k
    {"Meteor-M N2-4", 59051, 137.900, "LRPT", kMeteor, Weather, 0, false},
    {"Meteor-M N2-3 HRPT", 57166, 1700.0, "HRPT", kMeteorHrpt, Weather, 3, false},
    {"Meteor-M N2-4 HRPT", 59051, 1700.0, "HRPT", kMeteorHrpt, Weather, 3, false},
    {"Arktika-M N1 GGAK", 47719, 1703.0, "GGAK", kArktikaGgak, Weather, 0, false},   // highly elliptical: SGP4 here is near-earth only, so "orbit not usable"
    {"Arktika-M N2 GGAK", 58584, 1703.0, "GGAK", kArktikaGgak, Weather, 0, false},
    {"Arctic Weather Satellite", 60543, 1707.0, "L-band", kAws, Weather, 6, false},
    {"MetOp-B Argos", 38771, 465.9875, "Argos downlink", kArgos, Weather, 0, false},   // A-DCS downlink (SatNOGS transmitter entry for MetOp-B)
    {"MetOp-C Argos", 43689, 465.9875, "Argos downlink", kArgos, Weather, 0, false},
    {"ISS APRS", 25544, 145.825, "APRS", kAprs, Iss, 0, false},
    {"ISS voice", 25544, 145.800, "FM voice", kSpecx, Iss, 0, false},
    {"ISS SSTV", 25544, 145.800, "SSTV events", kIssSstv, Iss, 0, false},   // only during announced events: never the default link (SatLink::eventOnly)
};

struct Cube { std::string name; int norad = 0; double mhz = 0; bool aprs = false; };

// What a pass of this link gives you
const char* yieldsOf(const dect2::SatLink& l) {
    switch (l.family) {
        case kNoaaApt: return N_("Picture (two channels)");
        case kNoaaDsb: return N_("Instrument data");
        case kNoaaHrpt: case kMeteorHrpt: return N_("High-resolution picture");
        case kMeteor: return N_("Picture (three channels)");
        case kArktikaGgak: return N_("Space-weather telemetry");
        case kAws: return N_("Microwave scans");
        case kArgos: return N_("Platform messages");
        case kIssSstv: return N_("Pictures, during announced events");
        case kAprs: return N_("APRS packets");
        case kOrbcomm: return N_("Decoded messages");
        case kGlobalstar: return N_("Activity monitor");
        case kLoraSat: return N_("LoRa frames");
        case kCubesat: return l.decoder ? N_("Telemetry frames") : N_("IQ recording only (framing not decoded)");
        default: return N_("IQ recording only");
    }
}

struct Sat {
    std::string name;
    int norad = 0, group = Weather;
    unsigned tags = 0;
    bool retired = false, onMap = false;     // onMap: on the live map (the short list, not the whole CubeSat table)
    std::vector<dect2::SatLink> links;
    int link = 0;                            // the link in use
    bool linkChosen = false;                 // by the user: bestLink() leaves it alone
    dect2::Sgp4 sgp;
    const dect2::SatLink& ln() const { return links[link]; }
    bool hasFamily(int f) const { for (const dect2::SatLink& l : links) if (l.family == f) return true; return false; }
};
struct Row { int sat; dect2::Pass p; };
struct Armed { int norad = 0; double hz = 0; dect2::Pass p; int family = 0; };   // the queue: passes the user armed

struct State {
    bool loaded = false;
    std::map<int, dect2::Tle> tles;     // by NORAD number: newest epoch wins, pasted and derived ones on top
    std::vector<Sat> sats;
    std::vector<std::string> missing;   // listed satellites without a usable TLE
    std::vector<Row> rows;              // every pass of every satellite, next two days, by AOS
    std::vector<int> visible;           // indices into rows that pass the filters
    bool usingSnapshot = false;         // no download yet: the built-in orbits
    bool dirty = true;
    uint64_t gen = 1;                   // of the satellite list: a prediction of an older list is thrown away
    double computedAt = 0, lat = 1e9, lon = 1e9, altM = 0;
    float minEl = 10;                   // the peak a pass must reach to be listed
    float horizon = 0;                  // AOS / LOS: the elevation of the horizon (a mask for hills)
    bool show[NumGroups] = {true, true, false, true, false, false};
    bool onlyHearable = true, showRetired = false;
    unsigned antenna = dect2::BandAny;
    char search[32] = "";
    double visAt = 0; bool visDirty = true;
    std::vector<Cube> cubes;
    std::vector<dect2::cubesat::TxEntry> userTx;   // the Smart decoder's own additions
    int selSat = -1; double selAos = 0;
    std::string selName;                // selSat again after the list is rebuilt
    bool advanced = false, showMap = false;
    adsbmap::View mapView;
    bool track = false, autoStart = false, autoRecord = false;   // Advanced: follow one satellite by hand
    int followNorad = 0;
    bool autoTle = true;                                     // download the TLEs by itself, at most once a day
    float notifyMin = 5;                                     // minutes before AOS of the next planned pass: banner and log line
    double notifiedAos = 0;                                  // the pass the log line was written for
    std::vector<Armed> queue;
    bool sched = false;                                      // take every listed pass by itself
    bool schedTakeOver = false;                              // a pass taken by itself may switch a running receiver (armed ones always do)
    int prio[NumGroups] = {3, 2, 0, 1, 0, 1};                // per group: higher wins an overlap
    bool resv = false; int resvFrom = 2, resvTo = 6;         // hours of the day the SDR is kept free for other jobs
    dect2::SchedPlan plan;
    double planAt = 0; bool planDirty = true;
    dect2::PassRunner run;
    bool rotatorOn = false;
    double cardAt = -1e18;                                   // a mode's next-pass card was on screen at this time
    int armFamily = -1;                                      // a mode's "follow the next pass": arm once the passes are known
    std::vector<std::string> history;                        // lines of passes_history.tsv
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

// One satellite per NORAD number with every downlink OnAir knows for it. The list is the one the modes use: this file's weather and ISS rows, the CubeSat
// transmitter table of the Smart decoder (cubesat_db), the LoRa presets and the user's additions.
void buildSats() {
    S.sats.clear(); S.missing.clear();
    S.gen++;
    std::map<int, int> at;   // NORAD to index in S.sats
    auto add = [&](const std::string& name, int norad, unsigned tag, int grp, bool retired, bool onMap, dect2::SatLink l) {
        auto it = S.tles.find(norad);
        if (it == S.tles.end()) { if (onMap) S.missing.push_back(name); return; }   // the CubeSat table is long: only the short list is reported
        auto f = at.find(norad);
        if (f == at.end()) {
            Sat s; s.name = name; s.norad = norad; s.group = grp; s.tags = tag; s.retired = retired; s.onMap = onMap;
            if (!s.sgp.init(it->second)) { if (onMap) S.missing.push_back(name + " (orbit not usable)"); return; }
            at[norad] = (int)S.sats.size();
            s.links.push_back(std::move(l));
            S.sats.push_back(std::move(s));
            return;
        }
        Sat& s = S.sats[f->second];
        s.tags |= tag; s.retired = s.retired && retired; s.onMap = s.onMap || onMap;
        for (const dect2::SatLink& o : s.links) if (o.family == l.family && std::fabs(o.hz - l.hz) < 1000) return;
        s.links.push_back(std::move(l));
    };
    auto link = [](double hz, int fam, const std::string& mode, double need = 0, bool dec = true) {
        dect2::SatLink l; l.hz = hz; l.family = fam; l.mode = mode; l.needRateHz = need; l.decoder = dec;
        l.radioFollowsDoppler = fam != kLoraSat;   // a retune in the middle of a frame would break it, and the decoder follows the shift
        l.eventOnly = fam == kIssSstv;
        return l;
    };
    std::map<int, bool> amateur;
    for (const dect2::Tle& t : dect2::parseTleText(readText(tleDir() + "/amateur.txt"))) amateur[t.norad] = true;
    for (const Known& k : kKnown)
        add(k.name, k.norad, k.group == Iss ? (unsigned)dect2::TagIss : (unsigned)dect2::TagWeather, k.group, k.retired, true, link(k.mhz * 1e6, k.family, k.mode, k.needMsps * 1e6, k.family != kSpecx));
    const ModeTuning* ot = modeTuning(46);
    for (const auto& [no, t] : S.tles)
        if (t.name.rfind("ORBCOMM", 0) == 0) add(t.name, no, dect2::TagOrbcomm, Orbcomm, false, true, link((ot ? ot->defMhz : 137.25) * 1e6, kOrbcomm, "Orbcomm"));
    for (const auto& [no, t] : S.tles)
        if (t.name.rfind("GLOBALSTAR", 0) == 0) add(t.name, no, dect2::TagGlobalstar, GlobalstarGrp, false, true, link(1618.25e6, kGlobalstar, "Globalstar"));
    for (const auto& [no, t] : S.tles)   // satellites that use LoRa (public presets, unverified): matched by the start of the name in the elements files
        for (const auto& p : dect2::lorasat::presets())
            if (p.tleName[0] && !p.uplinkOnly && t.name.rfind(p.tleName, 0) == 0) { add(t.name, no, dect2::TagAmateur, Amateur, false, true, link(p.mhz * 1e6, kLoraSat, "LoRa satellite")); break; }
    for (const Cube& c : S.cubes) add(c.name, c.norad, dect2::TagAmateur | dect2::TagCubesat, Amateur, false, true, link(c.mhz * 1e6, c.aprs ? kAprs : kCubesat, c.aprs ? "APRS" : "CubeSat telemetry"));
    auto addTx = [&](const dect2::cubesat::TxEntry& t) {
        const auto it = at.find(t.norad);
        if (it != at.end())   // a dedicated mode (Meteor's LRPT, ISS APRS) is what the user wants, not the SatNOGS row of the same downlink (listed as "FSK 40000")
            for (const dect2::SatLink& o : S.sats[it->second].links)
                if (o.family != kCubesat && std::fabs(o.hz - t.hz) < 2e6) return;
        const bool lora = t.mod == dect2::cubesat::ModLora, am = amateur.count(t.norad) != 0;
        std::string mode = t.mode.empty() ? std::string(dect2::cubesat::modName(t.mod)) : t.mode;
        if (t.baud > 0) mode += " " + std::to_string(t.baud);
        add(t.name, t.norad, dect2::TagCubesat | (am ? (unsigned)dect2::TagAmateur : 0u), am ? Amateur : CubesatGrp, false, false, link(t.hz, lora ? kLoraSat : kCubesat, mode, 0, dect2::cubesat::decodable(t)));
    };
    for (const dect2::cubesat::TxEntry& t : dect2::cubesat::txTable()) addTx(t);
    for (const dect2::cubesat::TxEntry& t : S.userTx) addTx(t);
    S.visDirty = true;
    S.selSat = -1;
    for (int i = 0; i < (int)S.sats.size(); i++) if (!S.selName.empty() && S.sats[i].name == S.selName) S.selSat = i;
    S.rows.clear();
    S.dirty = true;
    S.plan = dect2::SchedPlan(); S.planDirty = true;
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
    dect2::mergeNewestTle(S.tles, dect2::parseTleText(dect2::cubesat::tleSnapshotText()));   // offline, or before the first download: the built-in orbits
    S.usingSnapshot = true;
    auto addFile = [&](const std::string& file) {
        const std::vector<dect2::Tle> v = dect2::parseTleText(readText(tleDir() + "/" + file));
        if (!v.empty()) S.usingSnapshot = false;
        dect2::mergeNewestTle(S.tles, v);
    };
    for (const Source& s : kSources) addFile(s.file);
    addFile(kKeplerpass.file);
    for (const auto& src : S.customSources) addFile(src.first);
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
    S.horizon = (float)p.getD("passHorizon", 0);
    S.onlyHearable = p.getB("passOnlyHearable", true);
    S.showRetired = p.getB("passShowRetired", false);
    S.antenna = (unsigned)p.getD("passAntenna", dect2::BandAny);
    S.showMap = p.getB("passShowMap", false);
    S.mapView.zoom = 2;   // (centred on the receiver when first shown)
    S.schedTakeOver = p.getB("passSchedTakeOver", false);
    S.userTx = dect2::cubesat::txFromText(p.getS("csmUserTx", ""));
    if (std::string q = p.getS("passQueue", ""); !q.empty()) {   // armed passes survive a restart (those already over are dropped by refreshQueue)
        std::stringstream ss(q);
        for (std::string it; std::getline(ss, it, ';');) {
            Armed x; if (sscanf(it.c_str(), "%d|%lf|%lf|%lf|%lf|%lf|%d", &x.norad, &x.hz, &x.p.aos, &x.p.los, &x.p.tca, &x.p.maxElDeg, &x.family) >= 6) S.queue.push_back(x);
        }
    }
    { std::ifstream f(fsPath(plat::dataDir() + "/passes_history.tsv")); for (std::string l; std::getline(f, l);) S.history.push_back(l); }
    S.autoTle = p.getB("passTleAuto", true);
    D.measure = p.getB("passDopMeasure", false); D.useDerived = p.getB("passUseDerived", false);
    S.notifyMin = (float)p.getD("passNotifyMin", 5);
    S.sched = p.getB("passSched", false); S.resv = p.getB("passResv", false);
    S.resvFrom = (int)p.getD("passResvFrom", 2); S.resvTo = (int)p.getD("passResvTo", 6);
    for (int g = 0; g < NumGroups; g++) { char k[24]; snprintf(k, sizeof k, "passPrio%d", g); S.prio[g] = std::max(0, std::min(9, (int)p.getD(k, S.prio[g]))); }
    for (int g = 0; g < NumGroups; g++) { char k[24]; snprintf(k, sizeof k, "passShow%d", g); S.show[g] = p.getB(k, S.show[g]); }
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

// ---- the prediction, on its own thread: 48 hours of some 600 satellites takes seconds, and this used to stop the window
std::atomic<int> gCalc{0};   // 0 idle, 1 working, 2 finished
std::mutex gCalcMu;
struct CalcOut { std::vector<Row> rows; uint64_t gen = 0; } gCalcOut;

void saveQueue() {
    std::string q;
    for (const Armed& x : S.queue) { char b[180]; snprintf(b, sizeof b, "%d|%.1f|%.1f|%.1f|%.1f|%.2f|%d;", x.norad, x.hz, x.p.aos, x.p.los, x.p.tca, x.p.maxElDeg, x.family); q += b; }
    plat::prefs().setS("passQueue", q);
}

// New elements move a pass by seconds: each armed pass is matched to the new prediction by its culmination, so it is not lost
void refreshQueue(double now) {
    for (size_t i = 0; i < S.queue.size();) {
        Armed& q = S.queue[i];
        std::vector<dect2::Pass> c;
        for (const Row& r : S.rows) if (S.sats[r.sat].norad == q.norad) c.push_back(r.p);
        const int m = dect2::matchPass(q.p, c);
        if (m >= 0) q.p = c[m];
        if (q.p.los < now) { S.queue.erase(S.queue.begin() + (std::ptrdiff_t)i); continue; }
        ++i;
    }
    saveQueue();
}

void compute() {
    if (gCalc.load() == 2) {
        CalcOut out;
        { std::lock_guard<std::mutex> lk(gCalcMu); out = std::move(gCalcOut); }
        gCalc = 0;
        if (out.gen == S.gen) { S.rows = std::move(out.rows); refreshQueue(nowSec()); S.visDirty = true; S.planDirty = true; }
        else S.dirty = true;
    }
    if (gCalc.load() != 0) return;
    double lat, lon;
    receiverLocation(lat, lon);
    const double now = nowSec();
    const double alt = receiverAltitudeM();
    if (!S.dirty && lat == S.lat && lon == S.lon && alt == S.altM && now - S.computedAt < 600) return;
    S.dirty = false; S.lat = lat; S.lon = lon; S.altM = alt; S.computedAt = now;
    dect2::Observer o; o.latDeg = lat; o.lonDeg = lon; o.altKm = alt / 1000;
    std::vector<dect2::Sgp4> sgps;
    sgps.reserve(S.sats.size());
    for (const Sat& s : S.sats) sgps.push_back(s.sgp);
    gCalc = 1;
    dect2::namedThread("onair-passcalc", [sgps = std::move(sgps), o, now, horizon = (double)S.horizon, gen = S.gen] {
        CalcOut out;
        out.gen = gen;
        for (int i = 0; i < (int)sgps.size(); i++)
            for (const dect2::Pass& p : dect2::findPassesPeak(sgps[i], o, now - 1800, now + 2 * 86400, horizon, 0))
                if (p.los > now) out.rows.push_back({i, p});
        std::sort(out.rows.begin(), out.rows.end(), [](const Row& x, const Row& y) { return x.p.aos < y.p.aos; });
        { std::lock_guard<std::mutex> lk(gCalcMu); gCalcOut = std::move(out); }
        gCalc = 2;
    }).detach();
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

const Sat* satByNorad(int norad) {
    for (const Sat& s : S.sats) if (s.norad == norad) return &s;
    return nullptr;
}

void retuneTo(App& a, double mhz) {
    a.freqMhz = mhz;
    if (a.engine.running()) { a.tune.centerHz = a.freqMhz * 1e6; a.engine.retune(a.tune); }
}

dect2::RadioCaps radioCaps(App& a) {
    dect2::RadioCaps r;
    if (a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].isRadio()) {
        const auto& d = a.devices[a.devIdx];
        r.minHz = d.minFreqHz; r.maxHz = d.maxFreqHz; r.maxRateHz = d.maxRateHz;
    }
    return r;
}

dect2::PassFilter currentFilter(App& a) {
    dect2::PassFilter f;
    f.tagMask = 0;
    for (int g = 0; g < NumGroups; g++) if (S.show[g]) f.tagMask |= 1u << g;
    f.minPeakDeg = S.minEl;
    f.onlyHearable = S.onlyHearable;
    f.showRetired = S.showRetired;
    f.radio = radioCaps(a);
    f.antennaMask = S.antenna;
    return f;
}

void refreshVisible(App& a, double now) {
    if (!S.visDirty && now - S.visAt < 1.0) return;
    S.visDirty = false; S.visAt = now;
    const dect2::PassFilter f = currentFilter(a);
    for (Sat& s : S.sats) if (!s.linkChosen) { const int b = dect2::bestLink(s.links, f.radio, S.antenna); if (b >= 0) s.link = b; }
    std::string q(S.search);
    std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    S.visible.clear();
    for (int i = 0; i < (int)S.rows.size(); i++) {
        const Row& r = S.rows[i];
        if (r.p.los < now) continue;
        const Sat& s = S.sats[r.sat];
        if (!dect2::passVisible(s.tags, s.retired, s.links, r.p, f)) continue;
        if (!q.empty()) {
            std::string n = s.name;
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (n.find(q) == std::string::npos) continue;
        }
        S.visible.push_back(i);
    }
}

int armedIdx(int norad, const dect2::Pass& p) {
    for (int i = 0; i < (int)S.queue.size(); i++) if (S.queue[i].norad == norad && dect2::samePass(S.queue[i].p, p)) return i;
    return -1;
}

void armRow(const Row& r) {
    const Sat& s = S.sats[r.sat];
    if (armedIdx(s.norad, r.p) >= 0) return;
    Armed q; q.norad = s.norad; q.hz = s.ln().hz; q.p = r.p; q.family = s.ln().family;
    S.queue.push_back(q);
    saveQueue();
    S.planDirty = true;
}

std::string armNextOfFamily(int family) {
    const double now = nowSec();
    const Row* best = nullptr;
    for (const Row& r : S.rows) {
        const Sat& s = S.sats[r.sat];
        if (r.p.los < now || s.retired || r.p.maxElDeg < S.minEl || !s.hasFamily(family)) continue;
        if (!best || r.p.aos < best->p.aos) best = &r;
    }
    if (!best) return "no pass of this satellite is predicted: update the orbits in View > Passes";
    Sat& s = S.sats[best->sat];
    const int keepLink = s.link; const bool keepChosen = s.linkChosen;
    for (int i = 0; i < (int)s.links.size(); i++) if (s.links[i].family == family) { s.link = i; s.linkChosen = true; }
    armRow(*best);
    if (family == kIssSstv) { s.link = keepLink; s.linkChosen = keepChosen; }   // an event, not the ISS's usual downlink: the queue keeps the link for this pass
    return "armed " + s.name + " at " + timeText(best->p.aos) + ": the receiver starts, follows the Doppler shift and records by itself";
}

constexpr double kSchedLead = 20;   // seconds: the receiver is up this long before a slot

void rebuildPlan(App& a, double now) {
    std::vector<dect2::SchedPass> ps;
    const dect2::PassFilter f = currentFilter(a);
    for (const Row& r : S.rows) {
        const Sat& s = S.sats[r.sat];
        if (S.run.active() && s.norad == S.run.job().norad && dect2::samePass(r.p, S.run.job().pass)) continue;   // the pass under way is kept to its end
        const bool armed = armedIdx(s.norad, r.p) >= 0;
        const bool followed = S.track && S.followNorad == s.norad;
        const bool automatic = S.sched && dect2::passVisible(s.tags, s.retired, s.links, r.p, f);
        if (armed || followed || automatic) ps.push_back({r.sat, dect2::queuePriority(S.prio[s.group], armed), r.p});
    }
    std::vector<dect2::BusyWindow> busy;
    if (S.run.active()) busy.push_back({S.run.job().start - kSchedLead, S.run.job().end});
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
        const auto daily = dect2::dailyBusy(S.resvFrom * 60, S.resvTo * 60, now - 86400, now + 3 * 86400, asUtc(lt) - asUtc(gt));
        busy.insert(busy.end(), daily.begin(), daily.end());
    }
    dect2::SchedParams prm; prm.leadSec = kSchedLead;
    S.plan = dect2::planPasses(ps, busy, now, prm);
    S.planAt = now; S.planDirty = false;
}

// What the plan does with a listed pass (null: nothing)
const char* planStatus(const Row& r, std::string* why = nullptr) {
    const Sat& s = S.sats[r.sat];
    if (S.run.active() && s.norad == S.run.job().norad && dect2::samePass(r.p, S.run.job().pass))
        return S.run.phase() == dect2::PassRunner::Phase::Receiving ? N_("receiving") : N_("starting");
    auto otherAt = [&](double a0, double a1) -> std::string {   // who holds the receiver then
        for (const dect2::SchedSlot& o : S.plan.slots)
            if (o.sp.sat != r.sat && o.start < a1 && o.end > a0 && o.sp.sat >= 0 && o.sp.sat < (int)S.sats.size()) return S.sats[o.sp.sat].name;
        return {};
    };
    for (const dect2::SchedSlot& sl : S.plan.slots)
        if (sl.sp.sat == r.sat && dect2::samePass(sl.sp.pass, r.p)) {
            if (sl.trimmed && why) { const std::string o = otherAt(r.p.aos, r.p.los); if (!o.empty()) *why = o; }
            return sl.trimmed ? N_("trimmed") : N_("planned");
        }
    for (const dect2::SchedDropped& d : S.plan.dropped)
        if (d.sp.sat == r.sat && dect2::samePass(d.sp.pass, r.p)) {
            if (why) { const std::string o = otherAt(r.p.aos, r.p.los); if (!o.empty()) *why = o; }
            return d.why == dect2::DropReason::Busy ? N_("skipped: SDR reserved") : d.why == dect2::DropReason::TooShort ? N_("skipped: too short") : N_("skipped: higher priority");
        }
    return nullptr;
}

// The program as the runner sees it: the existing receiver, Doppler retune and IQ recording paths
struct IssSstvTaken { bool taken = false; int family = 0, sub = 0, demod = dect2::HfDemodNfm; } gIssSstv;

class AppHost : public dect2::PassHost {
    void setMode_(int sub) { ::setMode(a_, kHfdig, sub); }
public:
    explicit AppHost(App& a) : a_(a) {}
    bool running() const override { return a_.engine.running(); }
    bool transmitting() const override { return txActive(); }
    int family() const override {
        if (a_.family == kHfdig && a_.sub == subIndexOf("sstv") && std::fabs(a_.freqMhz - 145.8) < 0.1) return kIssSstv;   // already there: not switched, not restored
        return a_.family;
    }
    double tunedHz() const override { return a_.freqMhz * 1e6; }
    void setMode(int f) override {
        if (f == kIssSstv) {   // the SSTV entry on FM, and what to put back afterwards (the HF digital family is one family of many entries)
            if (!gIssSstv.taken) { gIssSstv.taken = true; gIssSstv.family = a_.family; gIssSstv.sub = a_.sub; }
            selectMode(a_, kHfdig, "sstv");
            gIssSstv.demod = hfdigDemod(a_);
            hfdigSetDemod(a_, dect2::HfDemodNfm);
            return;
        }
        if (gIssSstv.taken) {   // the pass is over
            gIssSstv.taken = false;
            hfdigSetDemod(a_, gIssSstv.demod);
            if (f == kHfdig && gIssSstv.family == kHfdig && a_.sub != gIssSstv.sub) { const bool on = a_.engine.running(); if (on) a_.engine.stop(); setMode_(gIssSstv.sub); if (on) startReceiver(a_); return; }
        }
        if (a_.family != f) selectMode(a_, f);
    }
    void tune(double hz) override { retuneTo(a_, hz / 1e6); }
    void start() override { startReceiver(a_); }
    void stop() override { a_.engine.stop(); }
    bool canRecord() const override { return a_.devIdx >= 0 && a_.devIdx < (int)a_.devices.size() && a_.devices[a_.devIdx].isRadio(); }
    bool recording() const override { return a_.engine.recordingStats().active; }
    bool startRecording(std::string& path, std::string& err) override {
        const FileFormat f = (FileFormat)recFileFormatOf(a_.recFormat);
        path = recordingsDir() + "/" + (a_.recFormat == 2 ? blueRecordingName(a_.engine.recordingName(f)) : a_.engine.recordingName(f));
        if (!a_.engine.startRecording(path, f, err)) return false;
        a_.recDonePath.clear();
        return true;
    }
    void stopRecording() override { a_.engine.stopRecording(); }
    double dopplerHz(const dect2::RunJob& j, double t) const override {
        const Sat* s = satByNorad(j.norad);
        dect2::Look l;
        if (!s || !lookNow(*s, t, l)) return 0;
        return dect2::dopplerHz(j.hz, l.rangeRateKmS);
    }
    void log(const std::string& s) override { a_.engine.log(s); }
    void onAos(const dect2::RunJob& j) override {
        gallery::setPassContext(j.name, j.pass.aos, j.pass.maxElDeg, j.end + 600);   // pictures of the pass are labelled (the decoders finish a picture late)
        if (dect2::automation::wants(dect2::automation::FSatellite)) {
            dect2::automation::Event ev("satellite.aos");
            ev.at(dect2::automation::wallNow(), "", j.hz);
            ev.str("name", j.name).num("max_el_deg", j.pass.maxElDeg);
            dect2::automation::emit(std::move(ev));
        }
        if (j.family == kLoraSat) a_.engine.lorasat().setTracked(j.norad, j.name);   // the decoder follows the Doppler shift itself
        if (j.family == kCubesat) a_.engine.cubesat().setTracked(j.norad, j.name);   // names the frames of the pass
    }
    void onLos(const dect2::RunJob& j, bool reached) override {
        if (reached && dect2::automation::wants(dect2::automation::FSatellite)) {
            dect2::automation::Event ev("satellite.los");
            ev.at(dect2::automation::wallNow(), "", j.hz);
            ev.str("name", j.name);
            dect2::automation::emit(std::move(ev));
        }
        a_.engine.cubesat().setTracked(0, "");
        a_.engine.lorasat().setTracked(0, "");
    }
private:
    App& a_;
};

bool jobFromSlot(const dect2::SchedSlot& sl, dect2::RunJob& j) {
    if (sl.sp.sat < 0 || sl.sp.sat >= (int)S.sats.size()) return false;
    const Sat& s = S.sats[sl.sp.sat];
    const int ai = armedIdx(s.norad, sl.sp.pass);
    const dect2::SatLink* l = &s.ln();
    if (ai >= 0) for (const dect2::SatLink& x : s.links) if (std::fabs(x.hz - S.queue[ai].hz) < 1 && (!S.queue[ai].family || x.family == S.queue[ai].family)) l = &x;   // the link it was armed with
    j.norad = s.norad; j.name = s.name; j.mode = l->mode; j.hz = l->hz; j.family = l->family;
    j.pass = sl.sp.pass; j.start = sl.start; j.end = sl.end;
    j.radioFollowsDoppler = l->radioFollowsDoppler; j.retuneTolHz = l->retuneTolHz;
    const bool byHand = ai < 0 && !S.sched;   // only followed (Advanced): its own switches
    j.startReceiver = byHand ? S.autoStart : true;
    j.record = byHand ? S.autoRecord : true;
    j.takeOver = ai >= 0 || (S.sched && S.schedTakeOver);
    return true;
}

const char* outcomeText(dect2::PassOutcome o) {
    switch (o) {
        case dect2::PassOutcome::Completed: return N_("received");
        case dect2::PassOutcome::Cancelled: return N_("cancelled");
        case dect2::PassOutcome::SkippedBusy: return N_("skipped: receiver in use");
        case dect2::PassOutcome::SkippedTx: return N_("skipped: transmitting");
        default: return N_("failed");
    }
}

void noteResults(std::vector<dect2::PassResult>& done) {
    for (const dect2::PassResult& r : done) {
        const int i = armedIdx(r.job.norad, r.job.pass);
        if (i >= 0) { S.queue.erase(S.queue.begin() + i); saveQueue(); }
        char b[900];
        const std::time_t tt = (std::time_t)r.startedAt;
        char ts[32]; std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", std::localtime(&tt));
        snprintf(b, sizeof b, "%s\t%s\t%s\t%.3f\t%.0f\t%s\t%s", ts, r.job.name.c_str(), r.job.mode.c_str(), r.job.hz / 1e6, r.job.pass.maxElDeg, outcomeText(r.outcome), r.iqPath.c_str());
        S.history.push_back(b);
        std::ofstream f(fsPath(plat::dataDir() + "/passes_history.tsv"), std::ios::app);
        f << b << "\n";
        S.planDirty = true;
    }
    done.clear();
}

// Once a frame, whether the window is open or not
void trackTick(App& a) {
    const double now = nowSec();
    if (S.planDirty || now - S.planAt > 20) rebuildPlan(a, now);
    const dect2::SchedSlot* next = nullptr;
    for (const dect2::SchedSlot& sl : S.plan.slots) if (sl.end > now) { next = &sl; break; }
    dect2::RunJob job;
    const dect2::RunJob* due = nullptr;
    if (S.run.active()) { job = S.run.job(); due = &job; }   // kept to its end (the plan works around it)
    else if (next && now >= next->start - kSchedLead && jobFromSlot(*next, job)) due = &job;
    AppHost host(a);
    std::vector<dect2::PassResult> done;
    S.run.tick(now, due, host, &done);
    noteResults(done);
    // the antenna mount: waits at the rise point of the next pass, follows the pass under way
    const Sat* mountSat = nullptr; dect2::Pass mountPass;
    if (S.run.active()) { mountSat = satByNorad(S.run.job().norad); mountPass = S.run.job().pass; }
    else if (next && next->sp.sat >= 0 && next->sp.sat < (int)S.sats.size()) { mountSat = &S.sats[next->sp.sat]; mountPass = next->sp.pass; }
    if (mountSat && rotatorPassDrive()) {
        dect2::Observer ob; ob.latDeg = S.lat; ob.lonDeg = S.lon; ob.altKm = S.altM / 1000;
        rotatorPassStep(dect2::planMountForPass(mountSat->sgp, ob, mountPass, now));
        S.rotatorOn = true;
    } else if (S.rotatorOn) { rotatorPassEnd(); S.rotatorOn = false; }
}

// A reminder in the last notifyMin minutes before the tracked satellite rises: one line over the top of the window (and one log line)
void aosBanner(App& a) {
    if (S.run.active() || S.notifyMin <= 0) return;
    const double now = nowSec();
    const dect2::SchedSlot* sl = nullptr;
    for (const dect2::SchedSlot& x : S.plan.slots) if (x.end > now && x.sp.sat >= 0 && x.sp.sat < (int)S.sats.size()) { sl = &x; break; }
    if (!sl) return;
    const double left = sl->start - now;
    if (left <= 0 || left > S.notifyMin * 60) return;
    const std::string& nm = S.sats[sl->sp.sat].name;
    if (S.notifiedAos != sl->start) {
        S.notifiedAos = sl->start;
        a.engine.log("pass of " + nm + " begins in " + std::to_string((int)(left / 60)) + " min");
    }
    const int in = (int)left;
    char b[200];
    snprintf(b, sizeof b, tr("Pass of %s in %d:%02d (max %.0f deg): the receiver starts by itself"), nm.c_str(), in / 60, in % 60, sl->sp.pass.maxElDeg);
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
    if (!D.measure || !S.run.following() || S.run.phase() != dect2::PassRunner::Phase::Receiving || !a.engine.running()) return;
    const Sat* sp = satByNorad(S.run.job().norad);
    if (!sp) return;
    const Sat& sat = *sp;
    if (a.spec.seq == D.lastSeq || a.spec.dbfs.empty()) return;
    const double now = (double)std::time(nullptr);
    D.lastSeq = a.spec.seq;
    if (D.norad != sat.norad || (!D.smp.empty() && now - D.smp.back().t > 900)) {   // another satellite or another pass: start over
        D.smp.clear(); D.fitted = false; D.norad = sat.norad; D.name = sat.name; D.nominalHz = S.run.job().hz; D.tle = sat.sgp.tle(); D.msg.clear();
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

// The next 24 hours as a strip: passes the plan takes on the top lane (amber: trimmed), those it skips below, the other listed passes faint at the bottom. A click selects a pass.
void timeline(double now) {
    const float w = std::max(200.f, ImGui::GetContentRegionAvail().x), h = 64 * gUi;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##timeline", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered(), clicked = ImGui::IsItemClicked();
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
    const float laneH = 13 * gUi;
    const ImVec2 mp = ImGui::GetIO().MousePos;
    std::string tip; const Row* hit = nullptr;
    auto bar = [&](double a0, double a1, int lane, ImU32 col, const Row* r, const std::string& label) {
        if (a1 <= now || a0 >= now + span) return;
        const float y0 = p0.y + 3 * gUi + lane * laneH;
        const ImVec2 a(xOf(a0), y0), b(std::max(xOf(a1), xOf(a0) + 2), y0 + laneH - 2);
        dl->AddRectFilled(a, b, col, 2.f);
        if (hovered && mp.x >= a.x && mp.x <= b.x && mp.y >= a.y && mp.y <= b.y) { tip = label; hit = r; }
    };
    auto rowFor = [&](int sat, const dect2::Pass& p) -> const Row* { for (const Row& r : S.rows) if (r.sat == sat && dect2::samePass(r.p, p)) return &r; return nullptr; };
    auto label = [&](int sat, const dect2::Pass& p) {
        char b[160]; snprintf(b, sizeof b, tr("%s - %s, max %.0f deg"), (S.sats[sat].name + "  " + timeText(p.aos)).c_str(), timeText(p.los).c_str(), p.maxElDeg);
        return std::string(b);
    };
    for (int i : S.visible) {   // bottom lane
        const Row& r = S.rows[i];
        if (r.p.aos > now + span) break;
        bar(r.p.aos, r.p.los, 2, (dim & 0x00FFFFFF) | 0x50000000, &r, label(r.sat, r.p));
    }
    for (const dect2::SchedDropped& d : S.plan.dropped) if (d.sp.sat >= 0 && d.sp.sat < (int)S.sats.size())
        bar(d.sp.pass.aos, d.sp.pass.los, 1, ImGui::GetColorU32(pal::badRed()), rowFor(d.sp.sat, d.sp.pass), label(d.sp.sat, d.sp.pass) + "  " + tr("skipped"));
    for (const dect2::SchedSlot& sl : S.plan.slots) if (sl.sp.sat >= 0 && sl.sp.sat < (int)S.sats.size())
        bar(sl.start, sl.end, 0, ImGui::GetColorU32(sl.trimmed ? pal::warnAmber() : pal::okGreen()), rowFor(sl.sp.sat, sl.sp.pass), label(sl.sp.sat, sl.sp.pass));
    if (S.run.active()) bar(S.run.job().start, S.run.job().end, 0, ImGui::GetColorU32(pal::accent()), nullptr, S.run.job().name);
    dl->AddLine(ImVec2(p0.x, p0.y), ImVec2(p0.x, p0.y + h), ImGui::GetColorU32(ImGuiCol_Text), 2.f);
    if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
    if (clicked && hit) { S.selSat = hit->sat; S.selAos = hit->p.aos; S.selName = S.sats[hit->sat].name; }
}

const char* compass(double az) {
    static const char* const names[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return names[((int)std::floor((az + 22.5) / 45.0)) & 7];
}

std::string startsText(const Row& r, double now) {
    char b[64];
    if (r.p.aos <= now) { snprintf(b, sizeof b, tr("now, %d min left"), (int)((r.p.los - now) / 60)); return b; }
    const double in = r.p.aos - now;
    if (in < 3600) { snprintf(b, sizeof b, tr("in %d min"), (int)(in / 60) + 1); return b; }
    return timeText(r.p.aos);
}

const Row* selectedRow() {
    if (S.selSat < 0 || S.selSat >= (int)S.sats.size()) return nullptr;
    const double now = nowSec();
    for (const Row& x : S.rows) if (x.sat == S.selSat && std::fabs(x.p.aos - S.selAos) < 1 && x.p.los >= now) return &x;
    return nullptr;
}

// Orbits: how old, from where; the button to update them now
void orbitStatus() {
    const double now = nowSec();
    const double ok = plat::prefs().getD("passTleOk", 0);
    char b[160];
    ImVec4 col = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    if (S.usingSnapshot) {
        double newest = 0;
        for (const auto& kv : S.tles) newest = std::max(newest, kv.second.epochJd);
        const double age = newest > 0 ? dect2::unixToJd(now) - newest : 0;
        snprintf(b, sizeof b, tr("Orbits: built-in set, %.0f days old"), age);
        col = pal::warnAmber();
    } else if (ok > 0) {
        const double h = (now - ok) / 3600;
        if (h < 48) snprintf(b, sizeof b, tr("Orbits: updated %.0f h ago"), std::max(0.0, h));
        else snprintf(b, sizeof b, tr("Orbits: updated %.0f days ago"), h / 24);
        if (h > 24 * 7) col = pal::warnAmber();
    } else snprintf(b, sizeof b, "%s", tr("Orbits: from files"));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col, "%s", b);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Satellite orbits age: older ones predict the times less well. They update by themselves once a day while this window is open or a pass is armed; without a network the saved and built-in ones are used."));
    ImGui::SameLine();
    ImGui::BeginDisabled(gDl.load() == 1);
    if (ImGui::SmallButton(tr(gDl.load() == 1 ? "Updating..." : "Update orbits"))) startDownload(false);
    ImGui::EndDisabled();
}

void filterBar(App& a) {
    plat::Prefs& pr = plat::prefs();
    bool ch = false;
    for (int g = 0; g < NumGroups; g++) {
        if (g) ImGui::SameLine();
        if (ImGui::Checkbox(tr(kGroupLabel[g]), &S.show[g])) { char k[24]; snprintf(k, sizeof k, "passShow%d", g); pr.setB(k, S.show[g]); ch = true; }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130 * gUi);
    if (ImGui::SliderFloat("##minel", &S.minEl, 0, 80, tr("peak over %.0f deg"))) { pr.setD("passMinEl", S.minEl); ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Lists a pass when it climbs at least this high. A listed pass is still received from horizon to horizon."));
    if (ImGui::Checkbox(tr("Only what my setup can hear"), &S.onlyHearable)) { pr.setB("passOnlyHearable", S.onlyHearable); ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Hides passes whose frequency is outside the selected radio or antenna. Nothing is blocked: untick it to arm anything."));
    ImGui::SameLine();
    static const struct { const char* name; unsigned mask; } kAnt[] = {
        {N_("Any antenna"), dect2::BandAny}, {N_("VHF (137-146 MHz)"), dect2::BandVhf}, {N_("UHF (400-470 MHz)"), dect2::BandUhf},
        {N_("VHF + UHF"), dect2::BandVhf | dect2::BandUhf}, {N_("L-band dish (1.4-1.8 GHz)"), dect2::BandL},
    };
    int cur = 0;
    for (int i = 0; i < 5; i++) if (kAnt[i].mask == S.antenna) cur = i;
    ImGui::SetNextItemWidth(170 * gUi);
    if (ImGui::BeginCombo("##ant", tr(kAnt[cur].name))) {
        for (int i = 0; i < 5; i++) if (ImGui::Selectable(tr(kAnt[i].name), i == cur)) { S.antenna = kAnt[i].mask; pr.setD("passAntenna", S.antenna); ch = true; }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130 * gUi);
    if (ImGui::InputTextWithHint("##satfind", tr("find a satellite"), S.search, sizeof S.search)) ch = true;
    if (ch) { S.visDirty = true; savePrefs(a); }
}

void overheadNow(double now) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Overhead now:"));
    int shown = 0, more = 0;
    for (int i : S.visible) {
        const Row& r = S.rows[i];
        if (r.p.aos > now) break;
        if (shown >= 8) { more++; continue; }
        const Sat& s = S.sats[r.sat];
        dect2::Look l;
        char b[96];
        snprintf(b, sizeof b, "%s %.0f%s##ov%d", s.name.c_str(), lookNow(s, now, l) ? l.elDeg : 0.0, now < r.p.tca ? " ^" : " v", i);
        ImGui::SameLine();
        if (ImGui::SmallButton(b)) { S.selSat = r.sat; S.selAos = r.p.aos; S.selName = s.name; }
        shown++;
    }
    if (more) { ImGui::SameLine(); ImGui::TextDisabled(tr("and %d more"), more); }
    if (!shown) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr(gCalc.load() != 0 && S.rows.empty() ? "working out the passes..." : "nothing above the horizon")); }
}

void passTable(double now) {
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##passes", 8, tf)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Satellite"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(tr("Starts"), ImGuiTableColumnFlags_WidthFixed, 100 * gUi);
    ImGui::TableSetupColumn(tr("Peak"), ImGuiTableColumnFlags_WidthFixed, 40 * gUi);
    ImGui::TableSetupColumn(tr("Length"), ImGuiTableColumnFlags_WidthFixed, 48 * gUi);
    ImGui::TableSetupColumn(tr("Path"), ImGuiTableColumnFlags_WidthFixed, 50 * gUi);
    ImGui::TableSetupColumn(tr("Signal"), ImGuiTableColumnFlags_WidthFixed, 118 * gUi);
    ImGui::TableSetupColumn(tr("You get"), ImGuiTableColumnFlags_WidthFixed, 122 * gUi);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 112 * gUi);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin((int)S.visible.size());
    while (clip.Step())
        for (int n = clip.DisplayStart; n < clip.DisplayEnd; n++) {
            const Row& r = S.rows[S.visible[n]];
            const Sat& s = S.sats[r.sat];
            const bool active = r.p.aos <= now && now <= r.p.los;
            ImGui::TableNextRow();
            ImGui::PushID(S.visible[n]);
            ImGui::TableNextColumn();
            const bool sel = S.selSat == r.sat && std::fabs(S.selAos - r.p.aos) < 1;
            if (active) ImGui::PushStyleColor(ImGuiCol_Text, pal::okGreen());
            if (ImGui::Selectable(s.name.c_str(), sel)) { S.selSat = r.sat; S.selAos = r.p.aos; S.selName = s.name; }
            if (active) ImGui::PopStyleColor();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(startsText(r, now).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0f\xC2\xB0", r.p.maxElDeg);
            ImGui::TableNextColumn(); { const int d = (int)std::lround(r.p.los - r.p.aos); ImGui::Text("%d:%02d", d / 60, d % 60); }
            ImGui::TableNextColumn(); ImGui::Text("%s > %s", compass(r.p.aosAzDeg), compass(r.p.losAzDeg));
            ImGui::TableNextColumn(); ImGui::Text("%s %.3f", s.ln().mode.c_str(), s.ln().hz / 1e6);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(tr(yieldsOf(s.ln())));
            ImGui::TableNextColumn();
            const int ai = armedIdx(s.norad, r.p);
            std::string why;
            const char* st = planStatus(r, &why);
            if (ai >= 0 || (st && !S.sched)) {
                if (ImGui::SmallButton(tr("Cancel"))) { if (ai >= 0) { S.queue.erase(S.queue.begin() + ai); saveQueue(); } S.planDirty = true; }
                if (st) { ImGui::SameLine(); ImGui::TextColored(strstr(st, "skipped") ? pal::badRed() : strcmp(st, "trimmed") == 0 ? pal::warnAmber() : pal::okGreen(), "%s", tr(st)); if (ImGui::IsItemHovered() && !why.empty()) { char b[120]; snprintf(b, sizeof b, tr("conflict with %s"), why.c_str()); ImGui::SetTooltip("%s", b); } }
            } else {
                if (ImGui::SmallButton(tr("Arm"))) armRow(r);
                if (st) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr(st)); }
            }
            ImGui::PopID();
        }
    ImGui::EndTable();
    if (S.visible.empty()) ImGui::TextDisabled("%s", tr(S.tles.empty() ? "No orbits: update them above." : gCalc.load() != 0 ? "working out the passes..." : "No passes match the filters in the next two days."));
}

// The pass on the map: its track, the footprint (at the satellite's place now during the pass, else at the highest point), the receiver
void passMap(const Row* r, float w, float h) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    { static bool centred = false; if (!centred) { centred = true; S.mapView.lat = S.lat; S.mapView.lon = S.lon; } }
    adsbmap::draw(S.mapView, ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + w, p0.y + h), true);
    auto poly = [&](const std::vector<std::pair<double, double>>& v, ImU32 c, float th) {
        for (size_t i = 1; i < v.size(); i++)
            if (std::fabs(v[i].second - v[i - 1].second) < 180) dl->AddLine(adsbmap::project(v[i - 1].first, v[i - 1].second), adsbmap::project(v[i].first, v[i].second), c, th);
    };
    if (r) {
        const Sat& s = S.sats[r->sat];
        const double now = nowSec(), at = now >= r->p.aos && now <= r->p.los ? now : r->p.tca;
        dect2::SatMapPoint pt;
        if (dect2::satelliteMapPoint(s.sgp, at, (at - r->p.aos) / 60 + 6, (r->p.los - at) / 60 + 6, 0.25, pt)) {
            const ImU32 acc = ImGui::GetColorU32(pal::accent());
            poly(pt.track, acc, 2.f * gUi);
            auto c = dect2::circlePoints(pt.lat, pt.lon, pt.footprintKm, 90);
            c.push_back(c[0]);
            poly(c, (acc & 0x00FFFFFF) | 0xC0000000, 1.5f * gUi);
            dl->AddCircleFilled(adsbmap::project(pt.lat, pt.lon), 4.f * gUi, ImGui::GetColorU32(pal::warnAmber()));
        }
    }
    dl->AddCircleFilled(adsbmap::project(S.lat, S.lon), 3.5f * gUi, ImGui::GetColorU32(pal::okGreen()));
    dl->PopClipRect();
}

// The chosen pass: what it is, the link, the one button
void detailPanel(App& a, const Row* r, double now, float w) {
    if (!r) { ImGui::TextDisabled("%s", tr("Choose a pass from the list.")); return; }
    Sat& s = S.sats[r->sat];
    ImGui::TextUnformatted(s.name.c_str());
    ImGui::TextDisabled(tr("%s - %s, max %.0f deg"), timeText(r->p.aos).c_str(), timeText(r->p.los).c_str(), r->p.maxElDeg);
    if (s.links.size() > 1) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        char cur[120]; snprintf(cur, sizeof cur, "%s  %.3f MHz", s.ln().mode.c_str(), s.ln().hz / 1e6);
        if (ImGui::BeginCombo("##satlink", cur)) {
            for (int i = 0; i < (int)s.links.size(); i++) {
                char b[120]; snprintf(b, sizeof b, "%s  %.3f MHz##%d", s.links[i].mode.c_str(), s.links[i].hz / 1e6, i);
                if (ImGui::Selectable(b, i == s.link)) { s.link = i; s.linkChosen = true; S.visDirty = true; }
            }
            ImGui::EndCombo();
        }
    } else ImGui::Text("%s  %.3f MHz", s.ln().mode.c_str(), s.ln().hz / 1e6);
    ImGui::TextDisabled("%s", tr(yieldsOf(s.ln())));
    {   // the orbit's age for this pass
        const double age = dect2::tleAgeDays(s.sgp.tle(), r->p.aos);
        const dect2::TleFresh f = dect2::tleFreshness(age);
        char b[160]; snprintf(b, sizeof b, tr("Orbit %.1f days old at this pass"), age);
        if (f == dect2::TleFresh::Fresh) ImGui::TextDisabled("%s", b);
        else {
            ImGui::TextColored(pal::warnAmber(), "%s", b);
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(pal::warnAmber(), tr("The times could be off by about %.0f s. Update the orbits."), dect2::tleTimingErrorSec(age));
            ImGui::PopTextWrapPos();
        }
    }
    if (now >= r->p.aos && now <= r->p.los) {
        dect2::Look l;
        if (lookNow(s, now, l)) ImGui::Text(tr("az %.0f  el %.0f  %.0f km\nDoppler %+.0f Hz"), l.azDeg, l.elDeg, l.rangeKm, dect2::dopplerHz(s.ln().hz, l.rangeRateKmS));
    } else {
        const int in = (int)std::max(0.0, r->p.aos - now);
        ImGui::TextDisabled(tr("AOS in %d:%02d:%02d"), in / 3600, in / 60 % 60, in % 60);
    }
    {
        const float bw = ImGui::GetContentRegionAvail().x;
        const int ai = armedIdx(s.norad, r->p);
        const bool thisRuns = S.run.active() && s.norad == S.run.job().norad && dect2::samePass(r->p, S.run.job().pass);
        if (thisRuns) {
            if (ImGui::Button(tr("Stop this pass"), ImVec2(bw, 0))) { AppHost h(a); std::vector<dect2::PassResult> d; S.run.cancel(now, h, &d); noteResults(d); }
            ImGui::TextColored(pal::okGreen(), tr("tracking%s"), S.run.recordingIq() ? tr(", recording") : "");
        } else if (ai >= 0) {
            if (ImGui::Button(tr("Cancel this pass"), ImVec2(bw, 0))) { S.queue.erase(S.queue.begin() + ai); saveQueue(); S.planDirty = true; }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.55f));
            if (ImGui::Button(tr("Arm this pass"), ImVec2(bw, 0))) armRow(*r);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("At AOS the receiver tunes, sets the mode, follows the Doppler shift, records and decodes, then saves. A receiver in another mode is switched for the pass and put back afterwards."));
        }
    }
    if (rotatorPassDrive()) ImGui::TextDisabled("%s", tr("The antenna mount follows the pass."));
    ImGui::Separator();
    if (ImGui::RadioButton(tr("Sky"), !S.showMap)) { S.showMap = false; plat::prefs().setB("passShowMap", false); }
    ImGui::SameLine();
    if (ImGui::RadioButton(tr("Map"), S.showMap)) { S.showMap = true; plat::prefs().setB("passShowMap", true); }
    const float size = std::min(w, std::max(120.f, ImGui::GetContentRegionAvail().y));
    if (S.showMap) passMap(r, w, size);
    else skyPlot(r, size);
}

void advancedPage(App& a) {
    plat::Prefs& pr = plat::prefs();
    ImGui::BeginChild("##passadv", ImVec2(0, 0));
    tleSection(a);
    dopplerSection(a);
    if (typo::collapsingHeader(trw("Follow and schedule"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const Row* sel = selectedRow();
        if (S.track) ImGui::TextDisabled(tr("following #%d"), S.followNorad);
        ImGui::BeginDisabled(!sel && !S.track);
        bool t = S.track;
        if (ImGui::Checkbox(sel && !S.track ? (std::string(tr("Follow")) + " " + S.sats[sel->sat].name).c_str() : tr("Follow the satellite"), &t)) { S.track = t; S.followNorad = t && sel ? S.sats[sel->sat].norad : S.followNorad; S.planDirty = true; }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Every pass of this satellite, as long as this is ticked. To receive one pass, arm it instead."));
        ImGui::BeginDisabled(!S.track);
        ImGui::SameLine(); if (ImGui::Checkbox(tr("Auto start at AOS, stop at LOS"), &S.autoStart)) S.planDirty = true;
        ImGui::SameLine(); if (ImGui::Checkbox(tr("Auto record IQ"), &S.autoRecord)) S.planDirty = true;
        ImGui::EndDisabled();
        {
            bool drive = rotatorPassWanted();
            if (ImGui::Checkbox(tr("Point the antenna mount"), &drive)) rotatorPassSetDrive(drive);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Drives the rotator or telescope mount set up in View > Antenna rotator: waits at the rise point, follows the satellite, parks at LOS"));
        }
        ImGui::SetNextItemWidth(160 * gUi);
        if (ImGui::SliderFloat("##horizon", &S.horizon, 0, 25, tr("horizon %.0f deg"))) { pr.setD("passHorizon", S.horizon); S.dirty = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Raise it for hills or buildings: AOS and LOS are worked out for this elevation"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160 * gUi);
        if (ImGui::SliderFloat("##notmin", &S.notifyMin, 0, 30, tr("remind %.0f min before"))) pr.setD("passNotifyMin", S.notifyMin);
        if (ImGui::Checkbox(tr("Show retired satellites"), &S.showRetired)) { pr.setB("passShowRetired", S.showRetired); S.visDirty = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Satellites that have stopped transmitting for good (NOAA 15, 18 and 19, decommissioned in 2025)"));
        if (ImGui::Checkbox(tr("Take every listed pass by itself"), &S.sched)) { pr.setB("passSched", S.sched); S.planDirty = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Takes the passes the filters list, highest priority first. Overlapping passes are trimmed or skipped; a pass you armed always wins over these."));
        if (S.sched) {
            ImGui::SameLine();
            if (ImGui::Checkbox(tr("Also switch a running receiver"), &S.schedTakeOver)) { pr.setB("passSchedTakeOver", S.schedTakeOver); S.planDirty = true; }
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", tr("Priority (higher wins)"));
        for (int g = 0; g < NumGroups; g++) {
            ImGui::SameLine(); ImGui::TextUnformatted(tr(kGroupLabel[g]));
            ImGui::SameLine(); ImGui::SetNextItemWidth(55 * gUi);
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
    }
    if (typo::collapsingHeader(trw("Finished passes"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (S.history.empty()) ImGui::TextDisabled("%s", tr("None yet."));
        for (int i = (int)S.history.size() - 1, n = 0; i >= 0 && n < 30; i--, n++) {
            std::string l = S.history[i];
            std::replace(l.begin(), l.end(), '\t', ' ');
            ImGui::TextUnformatted(l.c_str());
        }
    }
    ImGui::EndChild();
}

void drawWindow(App& a) {
    static bool full = false;   // the window over the whole display (the shared full-screen button; Esc leaves)
    { static bool af = std::getenv("ONAIR_MAP_FULL") != nullptr; if (af) { af = false; full = true; } }   // dev: --shot enters full screen
    if (full && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) full = false;
    if (full) { ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize); }
    else firstWindowSize(1060 * gUi, 680 * gUi);
    if (!ImGui::Begin(trw("Passes"), &a.passesOpen, full ? ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse : 0)) { ImGui::End(); return; }
    const double now = nowSec();
    compute();
    refreshVisible(a, now);
    {   // where the receiver is, how old the orbits are
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", tr("Receiver position"));
        ImGui::SameLine();
        receiverPositionFields(a);
        if (!receiverLocationSet()) { ImGui::SameLine(); ImGui::TextColored(pal::warnAmber(), "%s", tr("not set: the times are for a placeholder position")); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The same position as the ADS-B antenna position"));
        ImGui::SameLine();
        orbitStatus();
        ImGui::SameLine();
        if (ImGui::SmallButton(tr(S.advanced ? "Back" : "Advanced"))) S.advanced = !S.advanced;
        ImGui::SameLine();
        if (adsbmap::fullButton(full, "screen")) full = !full;
    }
    if (S.advanced) { advancedPage(a); ImGui::End(); return; }
    filterBar(a);
    overheadNow(now);
    const float panelW = std::max(240 * gUi, ImGui::GetContentRegionAvail().x * 0.3f);
    const float tlH = 72 * gUi;
    const float mainH = std::max(160.f, ImGui::GetContentRegionAvail().y - tlH);
    ImGui::BeginChild("##passlist", ImVec2(std::max(200.f, ImGui::GetContentRegionAvail().x - panelW - 8 * gUi), mainH));
    passTable(now);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##passdetail", ImVec2(0, mainH));
    const Row* r = selectedRow();
    if (!r && !S.visible.empty()) r = &S.rows[S.visible[0]];   // the current or next pass until one is chosen
    detailPanel(a, r, now, std::max(120.f, ImGui::GetContentRegionAvail().x));
    ImGui::EndChild();
    timeline(now);
    ImGui::End();
}

}  // namespace

void passesDownloadTles() { loadState(); startDownload(); }

// dev: --tle <file> loads TLEs from a file as pasted ones would (tests without the network)
// One click from a mode's screen: arm the next pass of a satellite of this family (the passes may still be being worked out: then it is armed when they are known)
std::string passesArmFamily(App& a, int family) {
    (void)a;
    loadState();
    if (S.rows.empty()) { S.armFamily = family; return tr("working out the passes: the next one will be armed"); }
    return armNextOfFamily(family);
}

// A mode's own line about its satellites (app/ui/mode_hints.cpp): the next pass in plain words, and one click to follow it. While it is on screen the passes
// are worked out and the elements are kept up to date (once a day, the same limit as the planner, with a line saying so while it happens).
std::string passesFamilyLine(App& a, int family, bool* armed) {
    (void)a;
    loadState();
    S.cardAt = nowSec();
    if (armed) *armed = false;
    if (S.sats.empty()) return gDl.load() == 1 ? tr("Getting the satellite orbits from celestrak.org...") : tr("No orbits for this satellite yet: update them in View > Passes.");
    const double now = nowSec();
    const Row* best = nullptr;
    for (const Row& r : S.rows) {
        const Sat& s = S.sats[r.sat];
        if (r.p.los < now || s.retired || r.p.maxElDeg < S.minEl || !s.hasFamily(family)) continue;
        if (!best || r.p.aos < best->p.aos) best = &r;
    }
    if (!best) return gCalc.load() != 0 || S.rows.empty() ? tr("Working out the next pass...") : tr("No pass of this satellite in the next two days.");
    const Sat& s = S.sats[best->sat];
    if (armed) *armed = armedIdx(s.norad, best->p) >= 0;
    std::string line = ease::passLine(s.name, timeText(best->p.aos), best->p.maxElDeg, best->p.aos - now, best->p.los - now);
    if (gDl.load() == 1) line += std::string(" ") + tr("(updating the orbits from celestrak.org)");
    return line;
}

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
    for (const Sat& s : S.sats) if (s.onMap) out.push_back({s.name, s.norad, &s.sgp});
}

void passesWindow(App& a) {
    loadState();
    const bool cardShown = nowSec() - S.cardAt < 3;
    if (!S.sats.empty() && (S.track || S.sched || a.passesOpen || !S.queue.empty() || S.armFamily >= 0 || S.run.active() || cardShown)) compute();
    if (S.armFamily >= 0 && !S.rows.empty()) { a.engine.log(armNextOfFamily(S.armFamily)); S.armFamily = -1; }
    pollDownload();
    autoUpdateTle(a.passesOpen || S.track || S.sched || !S.queue.empty() || cardShown);
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
        if (!s.hasFamily(kArgos)) continue;
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
        if (!s.hasFamily(kGlobalstar)) continue;
        dect2::Look l;
        if (dect2::lookAt(s.sgp, o, unixSec, l) && l.elDeg > 0) out.push_back({s.name, dect2::dopplerHz(freqHz, l.rangeRateKmS)});
    }
    return out;
}
