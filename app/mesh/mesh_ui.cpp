// Mesh (LoRa) screens: node table, APRS station table (LoRa APRS, MeshCom), map of node and station positions, chat per channel,
// packet log with the raw payload, settings (band plan, protocols, presets, channel keys); LoRaWAN device table and frame log with filters.
#include "app.h"
#include "../adsb/adsb_map.h"
#include "dect2/packet_aprs.h"
#include "dect2/mesh_proto.h"
#include "dect2/mesh_presets.h"
#include "results_export.h"
#include "dect2/mesh_report.h"
#include "dect2/u8file.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <thread>
#include <map>
#include <set>

namespace {

struct Key { int proto; std::string name, secret; };

struct State {
    bool loaded = false;
    int view = 0;                                // 0 nodes, 1 map, 2 chat, 3 packets, 4 settings, 5 APRS stations, 6 LoRaWAN devices, 7 LoRaWAN frames
    bool rawView = false;                        // packets: the raw payload instead of the detail line
    adsbmap::View map;
    bool mapFit = true;                          // centre on the nodes until the user drags the map
    std::string sel;                             // selected node: protocol digit + id
    int chatChannel = 0;                         // index into the channel list built each frame (0 = all)
    int region = 0, protocols = 15;              // region: index into kPlans
    bool allPresets = false;
    std::vector<Key> keys;
    bool keysPushed = false;
    int pushedRegion = -1, pushedProtocols = -1, pushedPresets = -1;
    // Meshtastic region / modem preset ("" = from the band plan) and the custom LoRa setting of each entry (prefs "meshCustom<mask>")
    std::string mtRegion, mtPreset = "LongFast", pushedMt = "-", pushedCustom = "-";
    struct Custom { bool on = false; double mhz = 869.525, bwHz = 125000; int sf = 9, cr = 5, sync = 0x2B, preamble = 16; };
    std::map<int, Custom> custom;                // by entry (subMask)
    int customEntry = -1;                        // the entry whose custom frequency was last applied
    double pushedTuned = 0;
    // MeshCore radio presets (meshcore.nz): the title chosen ("" = custom), the list in use, and the optional refresh from the internet
    std::string mcPreset;
    std::vector<dect2::MeshCorePresetEntry> mcList;
    bool mcFromFile = false, mcListed = false;
    std::atomic<int> mcFetch{0};                 // 0 idle, 1 working, 2 done, 3 failed
    std::thread mcThread;
    char keyName[40] = "", keySecret[96] = "";
    int keyProto = 0;
    std::string keyMsg;
    uint64_t lastSeq = 0;
    std::set<std::string> logged;                // packets whose position went to the log (from + id + time); filled without logging at start
    std::deque<std::string> loggedOrder;
    bool logPrimed = false;
    std::deque<float> snr;                       // SNR of the last good frame per report, for the history plot
    // LoRaWAN filters
    char lwFilter[48] = "";                      // DevAddr, EUI, network or vendor (any part, any case)
    std::string lwNet;                           // one network only ("" all)
    int lwType = 0;                              // frames: 0 all, 1 uplinks, 2 downlinks, 3 joins, 4 proprietary
    bool lwJoins = true;                         // devices: also the ones heard only by their join requests
    bool lwRaw = false;
};
State S;

bool live(const App& a) { return a.rx.standard == 21; }

ImVec4 protoColour(int proto) {
    switch (proto) {
    case 1: return ImVec4(0.46f, 0.84f, 0.70f, 1);
    case 2: return ImVec4(0.96f, 0.68f, 0.34f, 1);
    case 3: return ImVec4(0.55f, 0.70f, 0.98f, 1);
    case 5: return ImVec4(0.96f, 0.84f, 0.40f, 1);
    default: return ImVec4(0.86f, 0.58f, 0.92f, 1);
    }
}
const char* protoName(int proto) { return proto == 1 ? "Meshtastic" : proto == 2 ? "MeshCore" : proto == 3 ? "LoRa APRS" : proto == 4 ? "MeshCom" : "LoRaWAN"; }
const char* protoTag(int proto) { return proto == 1 ? "Mt" : proto == 2 ? "Mc" : proto == 3 ? "APRS" : proto == 4 ? "MCom" : "LWAN"; }

// band plans: the receiver's region (Meshtastic / MeshCore channel plan), the frequency to tune and the test signal's scene
struct Plan { const char* name; const char* shortName; double mhz; int rxRegion, synthRegion; };
const Plan kPlans[] = {
    {"EU 868  (Meshtastic LongFast 869.525 MHz)", "EU 868  869.525 MHz", 869.525, 0, 0},
    {"US 915  (Meshtastic LongFast 906.875 MHz)", "US 915  906.875 MHz", 906.875, 1, 1},
    {"LoRa APRS EU  433.775 MHz  SF12", "APRS EU  433.775 MHz", 433.775, 0, 2},
    {"LoRa APRS PL  434.855 MHz  SF9", "APRS PL  434.855 MHz", 434.855, 0, 2},
    {"LoRa APRS UK  439.9125 MHz  SF12", "APRS UK  439.9125 MHz", 439.9125, 0, 2},
    {"MeshCom EU  433.175 MHz (+ LoRa APRS 433.775)", "MeshCom EU  433.175 MHz", 433.175, 0, 2},
    {"LoRaWAN EU868  868.1 / 868.3 / 868.5 MHz (+ 867.x in a wide capture)", "LoRaWAN EU868  868.1 MHz", 868.1, 0, 3},
    {"LoRaWAN US915  903.9 MHz (sub-band 2)", "LoRaWAN US915  903.9 MHz", 903.9, 1, 4},
    {"ANZ 915  (Meshtastic LongFast 919.875 MHz)", "ANZ 915  919.875 MHz", 919.875, 2, 5},
    {"ANZ 915  (MeshCore Mid 915.075 MHz)", "ANZ 915  915.075 MHz", 915.075, 3, 5},   // last: the saved plan is an index
};
const char* planRegionName(int rxRegion) { return rxRegion == 1 ? "US" : rxRegion >= 2 ? "ANZ" : "EU_868"; }   // the plan's Meshtastic region
constexpr int kNumPlans = (int)(sizeof(kPlans) / sizeof(kPlans[0]));

std::string nodeKey(const dect2::MeshNode& n) { return std::to_string(n.protocol) + n.id; }

std::string nodeLabel(const dect2::MeshNode& n) {
    if (!n.longName.empty()) return n.longName;
    return n.id;
}

std::string clock(double s) {
    char b[24];
    const int t = (int)std::max(0.0, s);
    if (t >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}

std::string ago(double now, double then) {
    if (then < 0) return "-";
    const int d = (int)std::max(0.0, now - then);
    char b[24];
    if (d < 90) snprintf(b, sizeof b, "%d s", d);
    else if (d < 5400) snprintf(b, sizeof b, "%d min", (d + 30) / 60);
    else snprintf(b, sizeof b, "%.1f h", d / 3600.0);
    return b;
}

// the saved keys: one line each, "protocol<TAB>name<TAB>secret"
void saveKeys() {
    std::string s;
    for (const auto& k : S.keys) s += std::to_string(k.proto) + "\t" + k.name + "\t" + k.secret + "\n";
    plat::prefs().setS("meshKeys", s);
}

void pushKeys(App& a) {
    a.engine.mesh().clearUserChannels();
    for (const auto& k : S.keys) {
        if (k.proto == 1) a.engine.mesh().addMeshtasticChannel(k.name, k.secret);
        else a.engine.mesh().addMeshCoreChannel(k.name, k.secret);
    }
}

// Every position a node or station sends goes to the log once (Meshtastic POSITION_APP, MeshCom / LoRa APRS positions); the packets
// already in the table when the receiver starts are not logged again
void logPositions(App& a) {
    for (const auto& p : a.rx.mesh.packets) {
        if (p.type.find("POSITION") == std::string::npos && p.type.find("Position") == std::string::npos) continue;
        char k[96];
        snprintf(k, sizeof k, "%d %s %08x %.3f", p.protocol, p.from.c_str(), (unsigned)p.packetId, p.timeSec);
        if (!S.logged.insert(k).second) continue;
        S.loggedOrder.push_back(k);
        if (S.loggedOrder.size() > 400) { S.logged.erase(S.loggedOrder.front()); S.loggedOrder.pop_front(); }
        if (!S.logPrimed) continue;
        std::string who = p.from;
        for (const auto& n : a.rx.mesh.nodes) if (n.protocol == p.protocol && n.id == p.from && !n.longName.empty()) { who += " " + n.longName; break; }
        char b[64];
        snprintf(b, sizeof b, ", SNR %.1f dB, %.4f MHz", p.snrDb, p.freqHz / 1e6);
        a.engine.log("Mesh: position from " + who + ": " + (p.detail.empty() ? std::string(p.decrypted || p.protocol != 1 ? "?" : "encrypted") : p.detail) + b);
    }
    S.logPrimed = true;
}

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.map.zoom = (int)d.getI("meshZoom", 10);
    if (receiverLocationSet()) { S.map.lat = d.getD("adsbLat", S.map.lat); S.map.lon = d.getD("adsbLon", S.map.lon); }   // until a node sends its position
    S.view = (int)d.getI("meshView", 0);
    S.region = std::max(0, std::min(kNumPlans - 1, (int)d.getI("meshRegion", 0)));
    S.protocols = (int)d.getI("meshProtocols", 15);
    S.allPresets = d.getB("meshPresets", false);
    S.mtRegion = d.getS("meshMtRegion", "");
    S.mtPreset = d.getS("meshMtPreset", "LongFast");
    S.mcPreset = d.getS("meshCorePreset", "");
    const std::string s = d.getS("meshKeys", "");
    size_t pos = 0;
    while (pos < s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) e = s.size();
        const std::string line = s.substr(pos, e - pos);
        pos = e + 1;
        const size_t t1 = line.find('\t');
        const size_t t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        S.keys.push_back({atoi(line.c_str()), line.substr(t1 + 1, t2 - t1 - 1), line.substr(t2 + 1)});
    }
}

// a protocol entry (Meshtastic, MeshCore, LoRa APRS, MeshCom): its networks, and the band plans that carry it
int subMask(const App& a) { const SubMode* sm = curSub(a); return sm ? sm->decoders : 0; }
bool planFits(const App& a, int i) {
    const int m = subMask(a);
    return !m || ((m & 3) ? i <= 1 || i >= 8 : m == 4 ? (i >= 2 && i <= 4) : m == 8 ? i == 5 : (i == 6 || i == 7));
}
int effRegion(const App& a) {   // the selected plan, or the entry's first when the selected one does not carry it
    if (planFits(a, S.region)) return S.region;
    for (int i = 0; i < kNumPlans; i++) if (planFits(a, i)) return i;
    return 0;
}
// a node's or station's protocol (1 Meshtastic, 2 MeshCore, 3 LoRa APRS, 4 MeshCom, 5 LoRaWAN) shown in this entry: a protocol entry shows only its own
bool shows(const App& a, int proto) { const int m = subMask(a); return !m || (m & (1 << (proto - 1))); }
bool lorawanEntry(const App& a) { return subMask(a) == 16; }
bool stationEntry(const App& a) { const int m = subMask(a); return m == 4 || m == 8; }   // LoRa APRS, MeshCom: stations, not nodes
int effProtocols(const App& a) { const int m = subMask(a); return m ? m : S.protocols; }
bool planCombo(App& a, const char* id, int& r, bool shortNames) {   // the band plans this entry can use
    bool ch = false;
    if (ImGui::BeginCombo(id, shortNames ? kPlans[r].shortName : kPlans[r].name)) {
        for (int i = 0; i < kNumPlans; i++) if (planFits(a, i) && ImGui::Selectable(shortNames ? kPlans[i].shortName : kPlans[i].name, i == r)) { r = i; ch = true; }
        ImGui::EndCombo();
    }
    return ch;
}

void applyRegion(App& a, int region) {
    S.region = region;
    plat::prefs().setI("meshRegion", region);
    a.freqMhz = kPlans[region].mhz;                    // the plan's channel (Meshtastic LongFast, LoRa APRS, MeshCom)
    if (subMask(a) == 2) a.freqMhz = region == 1 ? 910.525 : region == 8 ? 915.8 : region == 9 ? 915.075 : 869.618;   // MeshCore's own channel (AU or Mid in ANZ)
    a.tune.synth.modeOpt[1] = kPlans[region].synthRegion;
}

// ---------------------------------------------------------------- custom LoRa setting, Meshtastic region

// the standard LoRa bandwidths (SX126x), then the 2.4 GHz ones (SX128x, Meshtastic LORA_24)
const double kBws[] = {7812.5, 10416.667, 15625, 20833.333, 31250, 41666.667, 62500, 125000, 250000, 500000, 812500, 1625000};
const char* kBwNames[] = {"7.8 kHz", "10.4 kHz", "15.6 kHz", "20.8 kHz", "31.25 kHz", "41.7 kHz", "62.5 kHz", "125 kHz", "250 kHz", "500 kHz",
                          "812.5 kHz (2.4 GHz)", "1625 kHz (2.4 GHz)"};
constexpr int kNumBws = (int)(sizeof(kBws) / sizeof(kBws[0]));

State::Custom& customOf(const App& a) {
    const int m = subMask(a);
    auto it = S.custom.find(m);
    if (it != S.custom.end()) return it->second;
    State::Custom c;
    const std::string v = plat::prefs().getS(("meshCustom" + std::to_string(m)).c_str(), "");
    int on = 0;
    if (!v.empty() && sscanf(v.c_str(), "%d %lf %lf %d %d %d %d", &on, &c.mhz, &c.bwHz, &c.sf, &c.cr, &c.sync, &c.preamble) == 7) c.on = on != 0;
    else {   // the entry's usual network as the starting point
        if (m == 2) { c.mhz = 869.618; c.bwHz = 62500; c.sf = 8; c.cr = 8; c.sync = 0x12; c.preamble = 32; }
        else if (m == 4) { c.mhz = 433.775; c.bwHz = 125000; c.sf = 12; c.cr = 5; c.sync = 0x12; c.preamble = 8; }
        else if (m == 8) { c.mhz = 433.175; c.bwHz = 250000; c.sf = 11; c.cr = 6; c.sync = 0x2B; c.preamble = 8; }
    }
    c.sf = std::max(5, std::min(12, c.sf)); c.cr = std::max(5, std::min(8, c.cr)); c.sync &= 0xFF; c.preamble = std::max(6, std::min(65535, c.preamble));
    return S.custom[m] = c;
}
void saveCustom(const App& a) {
    const State::Custom& c = customOf(a);
    char b[128];
    snprintf(b, sizeof b, "%d %.6f %.3f %d %d %d %d", c.on ? 1 : 0, c.mhz, c.bwHz, c.sf, c.cr, c.sync, c.preamble);
    plat::prefs().setS(("meshCustom" + std::to_string(subMask(a))).c_str(), b);
}
bool customActive(const App& a) { return !lorawanEntry(a) && customOf(a).on; }
bool meshtasticShown(const App& a) { const int m = subMask(a); return !m || m == 1; }

// ---------------------------------------------------------------- MeshCore radio presets

std::string mcFile() { return plat::dataDir() + "/meshcore-presets.json"; }
void mcLoad() { S.mcList = dect2::meshCorePresets(mcFile(), &S.mcFromFile); S.mcListed = true; }

// the refresh runs on its own thread, only after a click; the file replaces the saved one only when it parses
void mcRefresh() {
    if (S.mcFetch == 1) return;
    if (S.mcThread.joinable()) S.mcThread.join();
    S.mcFetch = 1;
    S.mcThread = std::thread([] {
        const std::string tmp = mcFile() + ".part";
        std::vector<dect2::MeshCorePresetEntry> v;
        bool ok = plat::fetchUrl("https://api.meshcore.nz/api/v1/config", tmp, std::string("OnAir/") + ONAIR_VERSION) && dect2::loadMeshCorePresetFile(tmp, v);
        if (ok) { std::remove(mcFile().c_str()); ok = std::rename(tmp.c_str(), mcFile().c_str()) == 0; }
        if (!ok) std::remove(tmp.c_str());
        S.mcFetch = ok ? 2 : 3;
    });
}

// a preset sets the custom LoRa setting (MeshCore sync word and preamble) and the tuning frequency
void mcApply(App& a, const dect2::MeshCorePresetEntry& p) {
    State::Custom& c = customOf(a);
    const dect2::MeshLoraSettings l = dect2::meshCorePresetToLora(p);
    c.on = true; c.mhz = p.mhz; c.bwHz = l.bwHz; c.sf = l.sf; c.cr = l.cr; c.sync = l.syncWord; c.preamble = l.preamble;
    a.freqMhz = p.mhz;
    saveCustom(a);
}

} // namespace

double meshSampleRate(App& a) {
    double bw = 0;
    if (customActive(a)) bw = customOf(a).bwHz;
    else if (meshtasticShown(a) && !S.mtRegion.empty()) {
        dect2::MeshLoraSettings s;
        if (dect2::meshtasticRegionPreset(S.mtRegion, S.mtPreset, s)) bw = s.bwHz;
    }
    return bw > 500e3 ? 4e6 : 2e6;
}

namespace {

void tick(App& a) {
    loadState();
    dect2::MeshReceiver& r = a.engine.mesh();
    {
        const std::string mreg = meshtasticShown(a) ? S.mtRegion : "";
        const std::string k = mreg + "|" + S.mtPreset;
        if (k != S.pushedMt) { r.setMeshtasticPlan(mreg, S.mtPreset); S.pushedMt = k; }
        const State::Custom& c = customOf(a);
        const bool on = customActive(a);
        if (S.customEntry != subMask(a)) {   // an entry with its custom setting on starts on its custom frequency
            S.customEntry = subMask(a);
            if (on) a.freqMhz = c.mhz;
        }
        char b[128];
        snprintf(b, sizeof b, "%d %.3f %d %d %d %d", on ? 1 : 0, c.bwHz, c.sf, c.cr, c.sync, c.preamble);
        if (b != S.pushedCustom) {
            dect2::MeshLoraSettings s;
            s.name = "Custom"; s.freqHz = 0;          // the tuned frequency (the custom frequency is put in the frequency box)
            s.sf = c.sf; s.bwHz = c.bwHz; s.cr = c.cr; s.syncWord = (uint8_t)c.sync; s.preamble = c.preamble; s.ldro = dect2::meshLoraLdro(c.sf, c.bwHz);
            r.setCustom(on, s);
            S.pushedCustom = b;
        }
    }
    if (!S.keysPushed) { pushKeys(a); S.keysPushed = true; }
    if (S.pushedRegion != effRegion(a)) { S.pushedRegion = effRegion(a); r.setRegion(kPlans[S.pushedRegion].rxRegion); }
    if (S.pushedProtocols != effProtocols(a)) { S.pushedProtocols = effProtocols(a); r.setProtocols(S.pushedProtocols); }   // a protocol entry: only its own network
    if (S.pushedPresets != (int)S.allPresets) { r.setPresetSearch(S.allPresets); S.pushedPresets = (int)S.allPresets; }
    if (S.pushedTuned != a.freqMhz) { r.setTunedHz(a.freqMhz * 1e6); S.pushedTuned = a.freqMhz; }   // the engine does not pass the user's frequency
    if (!a.engine.running()) { S.lastSeq = 0; S.logPrimed = false; return; }
    if (live(a) && a.rx.seq != S.lastSeq) {
        S.lastSeq = a.rx.seq;
        logPositions(a);
        if (a.rx.mesh.dataValid) { S.snr.push_back(a.rx.mesh.snrDb); if (S.snr.size() > 240) S.snr.pop_front(); }
        if (S.mapFit) {
            double la = 0, lo = 0; int n = 0;
            for (const auto& nd : a.rx.mesh.nodes) if (nd.hasPosition) { la += nd.lat; lo += nd.lon; n++; }
            for (const auto& st : a.rx.mesh.aprsStations) if (st.st.hasPos) { la += st.st.lat; lo += st.st.lon; n++; }
            if (n) { S.map.lat = la / n; S.map.lon = lo / n; }
        }
    }
}

// ---------------------------------------------------------------- tables

std::vector<const dect2::MeshNode*> sortedNodes(const App& a) {
    std::vector<const dect2::MeshNode*> v;
    for (const auto& n : a.rx.mesh.nodes) if (shows(a, n.protocol)) v.push_back(&n);
    std::sort(v.begin(), v.end(), [](const dect2::MeshNode* x, const dect2::MeshNode* y) { return x->lastHeard > y->lastHeard; });
    return v;
}

// the width of a fixed column: its header or its widest usual value, whichever is wider (an auto-fit column was sized to its cells
// only, and cut its header short)
float hdrW(const char* header, const char* sample) { return std::max(ImGui::CalcTextSize(header).x, ImGui::CalcTextSize(sample).x); }

// the clock time of a signal time (seconds since the receiver started): the export's time column
double meshEpoch(const dect2::MeshTelemetry& t, double sec, double now) { return sec < 0 ? 0 : now - (t.timeSec - sec); }
std::string meshTime(double epoch) {
    if (epoch <= 0) return "";
    const std::time_t tt = (std::time_t)epoch;
    char b[24];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", std::localtime(&tt));
    return b;
}

void nodeTable(const App& a, bool compact) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    if (!compact) {
        static std::string note;
        resexp::Table x;
        x.cols = {"time", "frequency_hz", "mode", "name", "short_name", "id", "hardware", "role", "lat", "lon", "alt_m", "snr_db", "hops", "battery_pct",
                  "voltage", "battery_time", "temp_c", "humidity_pct", "pressure_hpa", "sensors_time", "heard_s_ago", "packets"};
        const double now = (double)std::time(nullptr);
        for (const dect2::MeshNode* n : sortedNodes(a)) {
            const double ep = meshEpoch(t, n->lastHeard, now);
            x.add({meshTime(ep), resexp::num(a.freqMhz * 1e6, 0), protoName(n->protocol), n->longName, n->shortName, n->id, n->hwModel, n->role,
                   n->hasPosition ? resexp::num(n->lat, 6) : "", n->hasPosition ? resexp::num(n->lon, 6) : "", n->hasPosition ? resexp::num(n->altM, 0) : "",
                   resexp::num(n->lastSnrDb, 1), n->hopsAway >= 0 ? std::to_string(n->hopsAway) : "", n->batteryPct >= 0 ? resexp::num(n->batteryPct, 0) : "",
                   n->voltage >= 0 ? resexp::num(n->voltage, 2) : "", meshTime(meshEpoch(t, n->devSec, now)), n->hasEnv ? resexp::num(n->tempC, 1) : "",
                   n->hasEnv ? resexp::num(n->humidity, 0) : "", n->hasEnv ? resexp::num(n->pressureHpa, 1) : "", meshTime(meshEpoch(t, n->envSec, now)), n->lastHeard >= 0 ? resexp::num(t.timeSec - n->lastHeard, 0) : "", std::to_string(n->packets)}, ep);
        }
        resexp::saveButton(x, "mesh-nodes", note, "svmn");
        ImGui::SameLine();
        if (ImGui::SmallButton("Report...")) {   // a short text summary: copied to the clipboard and saved as .txt
            char mhz[32];
            snprintf(mhz, sizeof mhz, "  %.3f MHz", a.freqMhz);
            const std::string rep = dect2::meshReport(t, std::string(kPlans[effRegion(a)].shortName) + (t.custom ? "  (custom LoRa setting)" : "") + mhz, now);
            ImGui::SetClipboardText(rep.c_str());
            const std::string path = saveFileDialog(("mesh-report-" + resexp::stamp() + ".txt").c_str());
            if (!path.empty()) {
                std::ofstream f(dect2::u8file(path), std::ios::binary);
                f << rep;
                note = f ? "report saved to " + path + " and copied" : "could not write " + path;
            } else note = "report copied to the clipboard";
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A short text report: period, frames and CRC errors, packet types, every node with its SNR range,\nhops, battery, sensors and position, and the text messages. Copied to the clipboard and saved as a .txt file.");
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
    const int cols = compact ? 3 : 11;
    if (!ImGui::BeginTable(compact ? "##mn_s" : "##mn_f", cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | (compact ? 0 : ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX),
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    if (compact) {
        ImGui::TableSetupColumn("Node", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Heard"); ImGui::TableSetupColumn("SNR");
    } else {
        ImGui::TableSetupColumn("Net"); ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 160 * gUi); ImGui::TableSetupColumn("Short");
        ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, 110 * gUi); ImGui::TableSetupColumn("Hardware", ImGuiTableColumnFlags_WidthFixed, 170 * gUi);
        ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed, 90 * gUi); ImGui::TableSetupColumn("Heard");
        ImGui::TableSetupColumn("SNR dB", ImGuiTableColumnFlags_WidthFixed, hdrW("SNR dB", "-12.5")); ImGui::TableSetupColumn("Hops", ImGuiTableColumnFlags_WidthFixed, hdrW("Hops", "3/7"));
        ImGui::TableSetupColumn("Battery", ImGuiTableColumnFlags_WidthFixed, hdrW("Battery", "100%")); ImGui::TableSetupColumn("Packets", ImGuiTableColumnFlags_WidthFixed, hdrW("Packets", "99999"));
    }
    ImGui::TableHeadersRow();
    if (on) {
        for (const dect2::MeshNode* n : sortedNodes(a)) {
            ImGui::TableNextRow();
            const bool sel = S.sel == nodeKey(*n);
            ImGui::TableNextColumn();
            ImGui::PushID(nodeKey(*n).c_str());
            if (compact) {
                if (ImGui::Selectable(nodeLabel(*n).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) S.sel = nodeKey(*n);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, n->lastHeard).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.1f", n->lastSnrDb);
            } else {
                if (ImGui::Selectable(protoTag(n->protocol), sel, ImGuiSelectableFlags_SpanAllColumns)) S.sel = nodeKey(*n);
                ImGui::TableNextColumn(); ImGui::TextColored(protoColour(n->protocol), "%s", n->longName.empty() ? "(no name yet)" : n->longName.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(n->shortName.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(n->id.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(n->hwModel.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(n->role.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, n->lastHeard).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.1f", n->lastSnrDb);
                ImGui::TableNextColumn(); if (n->hopsAway >= 0) ImGui::Text("%d", n->hopsAway); else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn();
                if (n->batteryPct >= 0) ImGui::Text("%.0f %%", n->batteryPct);
                else if (n->voltage > 0) ImGui::Text("%.2f V", n->voltage);
                else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn(); ImGui::Text("%u", n->packets);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    if (!on || sortedNodes(a).empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", !on ? "start the receiver to see nodes" : subMask(a) == 2 ? "no node heard yet: a MeshCore node advertises itself now and then" : "no node heard yet: a Meshtastic node announces itself every few minutes"); ImGui::PopTextWrapPos(); }
}

// every telemetry and position report, newest first: the history behind the battery, sensor and position values of the node table
void telemetryTable(const App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    std::map<std::string, std::string> names;
    for (const auto& n : t.nodes) names[std::to_string(n.protocol) + n.id] = nodeLabel(n);
    auto nameOf = [&](const dect2::MeshReading& r) { auto it = names.find(std::to_string(r.protocol) + r.id); return it == names.end() ? r.id : it->second; };
    auto opt = [](double v, int dec) { return v >= 0 ? resexp::num(v, dec) : std::string(); };
    {
        static std::string note;
        resexp::Table x;
        x.cols = {"time", "frequency_hz", "mode", "name", "id", "battery_pct", "voltage", "channel_util_pct", "air_util_tx_pct", "uptime_s", "temp_c", "humidity_pct",
                  "pressure_hpa", "lat", "lon", "alt_m"};
        const double now = (double)std::time(nullptr);
        for (const auto& r : t.readings) {
            const double ep = r.unixTime > 0 ? r.unixTime : meshEpoch(t, r.timeSec, now);
            x.add({meshTime(ep), resexp::num(a.freqMhz * 1e6, 0), protoName(r.protocol), nameOf(r), r.id, opt(r.batteryPct, 0), opt(r.voltage, 2), opt(r.channelUtilPct, 1),
                   opt(r.airUtilTxPct, 1), r.uptimeS >= 0 ? std::to_string(r.uptimeS) : "", r.hasEnv ? resexp::num(r.tempC, 1) : "", r.hasEnv ? resexp::num(r.humidity, 0) : "",
                   r.hasEnv ? resexp::num(r.pressureHpa, 1) : "", r.hasPosition ? resexp::num(r.lat, 6) : "", r.hasPosition ? resexp::num(r.lon, 6) : "",
                   r.hasPosition ? resexp::num(r.altM, 0) : ""}, ep);
        }
        resexp::saveButton(x, "mesh-telemetry", note, "svmt");
        ImGui::SameLine();
        ImGui::TextDisabled("%zu reports (the last %d kept); KML / GPX: one point per position", t.readings.size(), 2000);
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
    if (!ImGui::BeginTable("##mt", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Node", ImGuiTableColumnFlags_WidthFixed, 180 * gUi); ImGui::TableSetupColumn("Battery");
    ImGui::TableSetupColumn("Voltage"); ImGui::TableSetupColumn("Ch util"); ImGui::TableSetupColumn("Air tx"); ImGui::TableSetupColumn("Temp");
    ImGui::TableSetupColumn("Humidity"); ImGui::TableSetupColumn("Pressure"); ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    if (on) {
        ImGuiListClipper clip;   // up to 2000 rows: draw only the visible ones
        clip.Begin((int)t.readings.size());
        while (clip.Step())
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
                const dect2::MeshReading& r = t.readings[t.readings.size() - 1 - (size_t)row];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();   // clock time (this is a history), the date in the tooltip
                {
                    const std::string full = meshTime(r.unixTime > 0 ? r.unixTime : meshEpoch(t, r.timeSec, (double)std::time(nullptr)));
                    ImGui::TextUnformatted(full.size() > 11 ? full.c_str() + 11 : clock(r.timeSec).c_str());
                    if (!full.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%s after the start)", full.c_str(), clock(r.timeSec).c_str());
                }
                ImGui::TableNextColumn(); ImGui::TextColored(protoColour(r.protocol), "%s", nameOf(r).c_str());
                ImGui::TableNextColumn(); if (r.batteryPct > 100) ImGui::TextUnformatted("powered"); else if (r.batteryPct >= 0) ImGui::Text("%.0f %%", r.batteryPct);
                ImGui::TableNextColumn(); if (r.voltage >= 0) ImGui::Text("%.2f V", r.voltage);
                ImGui::TableNextColumn(); if (r.channelUtilPct >= 0) ImGui::Text("%.1f %%", r.channelUtilPct);
                ImGui::TableNextColumn(); if (r.airUtilTxPct >= 0) ImGui::Text("%.1f %%", r.airUtilTxPct);
                ImGui::TableNextColumn(); if (r.hasEnv) ImGui::Text("%.1f C", r.tempC);
                ImGui::TableNextColumn(); if (r.hasEnv) ImGui::Text("%.0f %%", r.humidity);
                ImGui::TableNextColumn(); if (r.hasEnv) ImGui::Text("%.1f hPa", r.pressureHpa);
                ImGui::TableNextColumn(); if (r.hasPosition) ImGui::Text("%.5f, %.5f  %.0f m", r.lat, r.lon, r.altM);
            }
    }
    ImGui::EndTable();
}

void packetTable(const App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    {
        static std::string note;
        resexp::Table x;
        x.cols = {"time", "frequency_hz", "mode", "type", "from", "to", "channel", "bytes", "decrypted", "hop_limit", "hop_start", "snr_db", "level_dbfs", "sf", "bw_hz",
                  "detail", "raw", "packet_id", "relay_node", "next_hop", "via_mqtt", "cfo_hz"};
        const double now = (double)std::time(nullptr);
        for (const dect2::MeshPacket& p : t.packets) {
            const double ep = meshEpoch(t, p.timeSec, now);
            // Meshtastic header: the packet id ties together the copies of one packet (flood) and the same packet seen by the node or MQTT;
            // relay_node is who sent this copy, next_hop who should send it on (last byte of the node number, 0 = none)
            const bool mt = p.protocol == 1 && p.parsed;
            char pid[12] = "", rel[4] = "", nxt[4] = "";
            if (mt) snprintf(pid, sizeof pid, "%08x", p.packetId);
            if (mt && p.relayNode >= 0) snprintf(rel, sizeof rel, "%02x", p.relayNode);
            if (mt && p.nextHop >= 0) snprintf(nxt, sizeof nxt, "%02x", p.nextHop);
            x.add({meshTime(ep), resexp::num(p.freqHz, 0), protoName(p.protocol), p.crcOk ? p.type : "CRC error", p.from, p.to, p.channel, std::to_string(p.size),
                   p.decrypted ? "yes" : "no", p.hopLimit >= 0 ? std::to_string(p.hopLimit) : "", p.hopStart >= 0 ? std::to_string(p.hopStart) : "",
                   resexp::num(p.snrDb, 1), resexp::num(p.levelDb, 1), std::to_string(p.sf), resexp::num(p.bwHz, 0), p.detail.empty() ? p.note : p.detail, p.raw,
                   pid, rel, nxt, mt ? (p.viaMqtt ? "yes" : "no") : "", resexp::num(p.cfoHz, 0)}, ep);
        }
        resexp::saveButton(x, "mesh-packets", note, "svmp");
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
    ImGui::Checkbox("raw payload", &S.rawView);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the frame's bytes as text (other bytes as \\xNN) in the last column, for LoRa APRS, MeshCom\nand frames that did not parse. Hover a row's last column to see it either way.");
    if (!ImGui::BeginTable("##mp", 11, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Net"); ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("From"); ImGui::TableSetupColumn("To");
    ImGui::TableSetupColumn("Bytes"); ImGui::TableSetupColumn("Content"); ImGui::TableSetupColumn("Hops"); ImGui::TableSetupColumn("SNR dB");
    ImGui::TableSetupColumn("SF / BW"); ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    if (on) {
        for (size_t i = t.packets.size(); i-- > 0;) {
            const dect2::MeshPacket& p = t.packets[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(p.timeSec).c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(protoColour(p.protocol), "%s", protoTag(p.protocol));
            ImGui::TableNextColumn();
            if (!p.crcOk) ImGui::TextColored(pal::badRed(), "CRC error");
            else ImGui::TextUnformatted(p.type.empty() ? "?" : p.type.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.from.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.to.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", p.size);
            ImGui::TableNextColumn();
            if (!p.crcOk) ImGui::TextDisabled("-");
            else if (p.decrypted) ImGui::TextColored(pal::okGreen(), "decrypted");
            else ImGui::TextColored(pal::warnAmber(), "%s", p.note.empty() ? "encrypted" : p.note.c_str());
            ImGui::TableNextColumn();
            if (p.hopStart >= 0 && p.hopLimit >= 0) ImGui::Text("%d", p.hopStart - p.hopLimit);
            else if (!p.path.empty() && p.protocol >= 3) ImGui::TextUnformatted(p.path.c_str());
            else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); ImGui::Text("%.1f", p.snrDb);
            ImGui::TableNextColumn(); ImGui::Text("%d / %.1f", p.sf, p.bwHz / 1e3);
            ImGui::TableNextColumn();
            if (S.rawView && !p.raw.empty()) { ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(p.raw.c_str()); ImGui::PopFont(); }
            else ImGui::TextUnformatted(p.detail.c_str());
            if (!p.raw.empty() && ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(560 * gUi); ImGui::TextUnformatted(p.raw.c_str()); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
        }
    }
    ImGui::EndTable();
    if (!on) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("start the receiver to see packets"); ImGui::PopTextWrapPos(); }
}

std::string aprsKey(const dect2::MeshAprsStation& s) { return "A" + std::to_string(s.protocol) + s.st.call; }

std::string symbolText(const dect2::PacketStation& s) {
    if (!s.symTable) return "-";
    char b[4] = {s.symTable, s.symCode, 0, 0};
    return b;
}

std::vector<const dect2::MeshAprsStation*> sortedStations(const App& a) {
    std::vector<const dect2::MeshAprsStation*> v;
    for (const auto& x : a.rx.mesh.aprsStations) if (shows(a, x.protocol)) v.push_back(&x);
    std::sort(v.begin(), v.end(), [](const dect2::MeshAprsStation* x, const dect2::MeshAprsStation* y) { return x->st.lastHeardSec > y->st.lastHeardSec; });
    return v;
}

void aprsTable(const App& a, bool compact = false) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    const char* none = !on ? "start the receiver to see stations"
                     : subMask(a) == 4 ? "no LoRa APRS station heard yet: tune 433.775 MHz"
                     : subMask(a) == 8 ? "no MeshCom station heard yet: tune 433.175 MHz"
                     : "no LoRa APRS or MeshCom station heard yet: pick a 70 cm plan (433.775 or 433.175 MHz) in the tuner";
    if (compact) {   // the list beside the main view: call, heard, SNR
        if (ImGui::BeginTable("##maprs_s", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Station", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Heard"); ImGui::TableSetupColumn("SNR");
            ImGui::TableHeadersRow();
            if (on) for (const auto* s : sortedStations(a)) {
                ImGui::TableNextRow();
                ImGui::PushID(aprsKey(*s).c_str());
                ImGui::TableNextColumn();
                if (ImGui::Selectable(s->st.call.c_str(), S.sel == aprsKey(*s), ImGuiSelectableFlags_SpanAllColumns)) S.sel = aprsKey(*s);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, s->st.lastHeardSec).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.1f", s->lastSnrDb);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (!on || sortedStations(a).empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", none); ImGui::PopTextWrapPos(); }
        return;
    }
    if (!ImGui::BeginTable("##maprs", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable,
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Net"); ImGui::TableSetupColumn("Call", ImGuiTableColumnFlags_WidthFixed, hdrW("Call", "A61ABC-15")); ImGui::TableSetupColumn("Sym");
    ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthFixed, hdrW("Position", "25.0805N 155.1403E")); ImGui::TableSetupColumn("Last");
    ImGui::TableSetupColumn("Heard"); ImGui::TableSetupColumn("SNR dB", ImGuiTableColumnFlags_WidthFixed, hdrW("SNR dB", "-12.5"));
    ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, hdrW("Frames", "9999")); ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthFixed, 150 * gUi);
    ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthFixed, 320 * gUi);
    ImGui::TableHeadersRow();
    if (on) {
        for (const auto* s : sortedStations(a)) {
            ImGui::TableNextRow();
            ImGui::PushID(aprsKey(*s).c_str());
            ImGui::TableNextColumn();
            if (ImGui::Selectable(protoTag(s->protocol), S.sel == aprsKey(*s), ImGuiSelectableFlags_SpanAllColumns)) S.sel = aprsKey(*s);
            ImGui::TableNextColumn(); ImGui::TextColored(protoColour(s->protocol), "%s%s", s->st.call.c_str(), s->st.isObject ? " (obj)" : "");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(symbolText(s->st).c_str());
            ImGui::TableNextColumn();
            if (s->st.hasPos) ImGui::Text("%s %s", dect2::aprs::fmtLat(s->st.lat).c_str(), dect2::aprs::fmtLon(s->st.lon).c_str()); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s->lastType.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, s->st.lastHeardSec).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f", s->lastSnrDb);
            ImGui::TableNextColumn(); ImGui::Text("%u", s->st.count);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s->st.isObject ? ("by " + s->st.via).c_str() : s->path.c_str());
            ImGui::TableNextColumn();
            if (s->batteryPct >= 0) { ImGui::Text("battery %d %%", s->batteryPct); ImGui::SameLine(0, 6 * gUi); }
            ImGui::TextUnformatted(s->st.comment.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    if (!on || sortedStations(a).empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", none); ImGui::PopTextWrapPos(); }
}

// ---------------------------------------------------------------- LoRaWAN

void lorawanPrivacyNote() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only: the frame headers every LoRaWAN gateway in range receives in the clear (message type, DevAddr, frame counter, "
                        "MAC commands, port, MIC; DevEUI and JoinEUI of join requests). The payloads are encrypted with the devices' own keys and "
                        "stay encrypted: OnAir never decrypts them, never checks a MIC and never transmits. These are other people's devices.");
    ImGui::PopTextWrapPos();
}

bool lwMatch(const std::string& hay, const char* needle) {
    if (!needle[0]) return true;
    std::string h = hay, n = needle;
    for (auto& c : h) c = (char)std::tolower((unsigned char)c);
    for (auto& c : n) c = (char)std::tolower((unsigned char)c);
    return h.find(n) != std::string::npos;
}

void lwNetworkCombo(const App& a, const char* id) {
    std::vector<std::string> nets;
    for (const auto& d : a.rx.mesh.lorawanDevices) if (!d.network.empty() && std::find(nets.begin(), nets.end(), d.network) == nets.end()) nets.push_back(d.network);
    std::sort(nets.begin(), nets.end());
    ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo(id, S.lwNet.empty() ? "all networks" : S.lwNet.c_str())) {
        if (ImGui::Selectable("all networks", S.lwNet.empty())) S.lwNet.clear();
        for (const auto& n : nets) if (ImGui::Selectable(n.c_str(), S.lwNet == n)) S.lwNet = n;
        ImGui::EndCombo();
    }
}

void lwFilters(const App& a, bool frames) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Filter"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##lwf", "DevAddr, EUI, network, vendor", S.lwFilter, sizeof S.lwFilter);
    flowNext(10 * gUi);
    lwNetworkCombo(a, "##lwn");
    flowNext(10 * gUi);
    if (frames) {
        static const char* types[] = {"all frames", "uplinks", "downlinks", "joins", "proprietary"};
        ImGui::SetNextItemWidth(std::min(130 * gUi, ImGui::GetContentRegionAvail().x));
        ImGui::Combo("##lwt", &S.lwType, types, 5);
        flowNext(10 * gUi);
        ImGui::Checkbox("hex", &S.lwRaw);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The frame's bytes in hex in the last column (hover a row to see them either way)");
    } else {
        ImGui::Checkbox("join-only devices", &S.lwJoins);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Also list devices heard only by their join requests (by DevEUI): after joining they use a DevAddr\nthe network assigns, which cannot be linked to the DevEUI without the network's keys.");
    }
    flowBreak();
}

bool lwDeviceShown(const dect2::MeshLorawanDevice& d) {
    if (!S.lwJoins && d.devAddr.empty()) return false;
    if (!S.lwNet.empty() && d.network != S.lwNet) return false;
    return lwMatch(d.id + " " + d.devEui + " " + d.joinEui + " " + d.network + " " + d.vendor, S.lwFilter);
}

std::vector<const dect2::MeshLorawanDevice*> sortedDevices(const App& a, bool filtered) {
    std::vector<const dect2::MeshLorawanDevice*> v;
    for (const auto& d : a.rx.mesh.lorawanDevices) if (!filtered || lwDeviceShown(d)) v.push_back(&d);
    std::sort(v.begin(), v.end(), [](const dect2::MeshLorawanDevice* x, const dect2::MeshLorawanDevice* y) { return x->lastHeard > y->lastHeard; });
    return v;
}

std::string lwBattery(const dect2::MeshLorawanDevice& d) {
    if (d.battery < 0) return "";
    char b[48];
    if (d.battery == 0) snprintf(b, sizeof b, "external power, margin %d dB", d.marginDb);
    else if (d.battery == 255) snprintf(b, sizeof b, "battery ?, margin %d dB", d.marginDb);
    else snprintf(b, sizeof b, "battery %.0f %%, margin %d dB", d.battery * 100.0 / 254, d.marginDb);
    return b;
}

void lorawanDevices(const App& a, bool compact) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    const char* none = !on ? "start the receiver to see devices" : "no LoRaWAN frame heard yet: devices send every few minutes to hours, on 868.1 / 868.3 / 868.5 MHz in Europe";
    if (compact) {
        if (ImGui::BeginTable("##lwd_s", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Heard"); ImGui::TableSetupColumn("SNR");
            ImGui::TableHeadersRow();
            if (on) for (const auto* d : sortedDevices(a, false)) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextColored(protoColour(5), "%s", d->id.c_str());
                if (ImGui::IsItemHovered() && !d->network.empty()) ImGui::SetTooltip("%s", d->network.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, d->lastHeard).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.1f", d->lastSnrDb);
            }
            ImGui::EndTable();
        }
        if (!on || t.lorawanDevices.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", none); ImGui::PopTextWrapPos(); }
        return;
    }
    lorawanPrivacyNote();
    lwFilters(a, false);
    const auto devs = sortedDevices(a, true);
    if (on) ImGui::TextDisabled("%zu of %zu devices", devs.size(), t.lorawanDevices.size());
    if (!ImGui::BeginTable("##lwd", 14, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable,
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("DevAddr / DevEUI", ImGuiTableColumnFlags_WidthFixed, hdrW("DevAddr / DevEUI", "DevEUI A84041000181C61A"));
    ImGui::TableSetupColumn("Network", ImGuiTableColumnFlags_WidthFixed, hdrW("Network", "The Things Network"));
    ImGui::TableSetupColumn("Vendor / JoinEUI", ImGuiTableColumnFlags_WidthFixed, hdrW("Vendor / JoinEUI", "A840410000000101"));
    ImGui::TableSetupColumn("Heard"); ImGui::TableSetupColumn("Level dBFS", ImGuiTableColumnFlags_WidthFixed, hdrW("Level dBFS", "-100.0"));
    ImGui::TableSetupColumn("SNR dB", ImGuiTableColumnFlags_WidthFixed, hdrW("SNR dB", "-12.5")); ImGui::TableSetupColumn("FCnt", ImGuiTableColumnFlags_WidthFixed, hdrW("FCnt", "65535"));
    ImGui::TableSetupColumn("Missed", ImGuiTableColumnFlags_WidthFixed, hdrW("Missed", "999")); ImGui::TableSetupColumn("SF"); ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed, hdrW("MHz", "868.500"));
    ImGui::TableSetupColumn("Packets", ImGuiTableColumnFlags_WidthFixed, hdrW("Packets", "999 / 99 / 9"));
    ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthFixed, hdrW("Last", "Unconfirmed down")); ImGui::TableSetupColumn("Port"); ImGui::TableSetupColumn("MAC / status", ImGuiTableColumnFlags_WidthFixed, 260 * gUi);
    ImGui::TableHeadersRow();
    if (on) {
        for (const auto* d : devs) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(protoColour(5), "%s", d->id.c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d->network.empty() ? "-" : d->network.c_str());
            ImGui::TableNextColumn();
            if (!d->vendor.empty()) ImGui::TextUnformatted(d->vendor.c_str()); else if (!d->joinEui.empty()) ImGui::TextUnformatted(d->joinEui.c_str()); else ImGui::TextDisabled("-");
            if (ImGui::IsItemHovered() && !d->joinEui.empty()) ImGui::SetTooltip("JoinEUI %s", d->joinEui.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, d->lastHeard).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f", d->lastLevelDb);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", d->lastSnrDb);
            ImGui::TableNextColumn(); if (d->fcnt >= 0) ImGui::Text("%ld", d->fcnt); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (d->missed) ImGui::TextColored(pal::warnAmber(), "%u", d->missed); else ImGui::TextDisabled("0");
            ImGui::TableNextColumn(); ImGui::Text("%d", d->sf);
            ImGui::TableNextColumn(); ImGui::Text("%.3f", d->freqHz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%u / %u / %u", d->uplinks, d->downlinks, d->joins);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("uplinks / downlinks / join requests (%u confirmed)", d->confirmed);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d->lastType.c_str());
            ImGui::TableNextColumn(); if (d->lastFport >= 0) ImGui::Text("%d", d->lastFport); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            const std::string st = lwBattery(*d);
            ImGui::TextUnformatted((d->lastMac + (d->lastMac.empty() || st.empty() ? "" : "; ") + st).c_str());
        }
    }
    ImGui::EndTable();
    if (!on || t.lorawanDevices.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", none); ImGui::PopTextWrapPos(); }
}

void lorawanFrames(const App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    lwFilters(a, true);
    if (!ImGui::BeginTable("##lwp", 14, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable,
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, hdrW("Type", "Unconfirmed down"));
    ImGui::TableSetupColumn("DevAddr / DevEUI", ImGuiTableColumnFlags_WidthFixed, hdrW("DevAddr / DevEUI", "A84041000181C61A"));
    ImGui::TableSetupColumn("Network", ImGuiTableColumnFlags_WidthFixed, hdrW("Network", "The Things Network"));
    ImGui::TableSetupColumn("FCnt", ImGuiTableColumnFlags_WidthFixed, hdrW("FCnt", "65535")); ImGui::TableSetupColumn("Port");
    ImGui::TableSetupColumn("Payload", ImGuiTableColumnFlags_WidthFixed, hdrW("Payload", "222 B encrypted")); ImGui::TableSetupColumn("FCtrl");
    ImGui::TableSetupColumn("MAC commands", ImGuiTableColumnFlags_WidthFixed, 180 * gUi); ImGui::TableSetupColumn("MIC", ImGuiTableColumnFlags_WidthFixed, hdrW("MIC", "DEADBEEF"));
    ImGui::TableSetupColumn("SF"); ImGui::TableSetupColumn("SNR dB", ImGuiTableColumnFlags_WidthFixed, hdrW("SNR dB", "-12.5"));
    ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed, hdrW("MHz", "868.500")); ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthFixed, 380 * gUi);
    ImGui::TableHeadersRow();
    size_t shown = 0;
    if (on) {
        for (size_t i = t.packets.size(); i-- > 0;) {
            const dect2::MeshPacket& p = t.packets[i];
            if (p.protocol != 5) continue;
            const int ty = !p.crcOk || !p.parsed ? -1 : p.mtype == 0 || p.mtype == 1 || p.mtype == 6 ? 3 : p.mtype == 7 ? 4 : (p.mtype == 2 || p.mtype == 4) ? 1 : 2;
            if (S.lwType && ty != S.lwType) continue;
            if (!S.lwNet.empty() && p.network != S.lwNet) continue;
            if (!lwMatch(p.from + " " + p.to + " " + p.network, S.lwFilter)) continue;
            shown++;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(p.timeSec).c_str());
            ImGui::TableNextColumn();
            if (!p.crcOk) ImGui::TextColored(pal::badRed(), "CRC error");
            else if (!p.parsed) ImGui::TextColored(pal::warnAmber(), "not LoRaWAN");
            else ImGui::TextUnformatted(p.type.c_str());
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(protoColour(5), "%s", p.from.c_str()); ImGui::PopFont();
            if (!p.to.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("JoinEUI %s", p.to.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.network.c_str());
            ImGui::TableNextColumn(); if (p.parsed && p.mtype >= 2 && p.mtype <= 5) ImGui::Text("%u", p.packetId); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (p.fport >= 0) ImGui::Text("%d", p.fport); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (p.frmLen >= 0) ImGui::TextColored(pal::warnAmber(), "%d B encrypted", p.frmLen); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.flags.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.mac.c_str());
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); if (p.parsed && p.mtype != 7) ImGui::Text("%08X", p.mic); else ImGui::TextDisabled("-"); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::Text("%d", p.sf);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", p.snrDb);
            ImGui::TableNextColumn(); ImGui::Text("%.3f", p.freqHz / 1e6);
            ImGui::TableNextColumn();
            if (S.lwRaw && !p.raw.empty()) { ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(p.raw.c_str()); ImGui::PopFont(); }
            else ImGui::TextUnformatted(p.parsed ? p.detail.c_str() : p.note.c_str());
            if (!p.raw.empty() && ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(560 * gUi); ImGui::TextUnformatted(p.raw.c_str()); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
        }
    }
    ImGui::EndTable();
    if (!on || !shown) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", !on ? "start the receiver to see frames" : "no LoRaWAN frame (that the filters let through) yet"); ImGui::PopTextWrapPos(); }
}

// ---------------------------------------------------------------- map

void mapView(App& a, ImVec2 size) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const double lat0 = S.map.lat, lon0 = S.map.lon;
    adsbmap::draw(S.map, size);
    if (S.map.lat != lat0 || S.map.lon != lon0) S.mapFit = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    ImVec2 click; const bool clicked = adsbmap::clickedAt(click);
    const dect2::MeshNode* tip = nullptr; float tipD = 14 * gUi;
    int shown = 0;
    if (on) {
        for (const auto& n : t.nodes) {
            if (!n.hasPosition) continue;
            shown++;
            const ImVec2 q = adsbmap::project(n.lat, n.lon);
            const bool sel = S.sel == nodeKey(n);
            const ImVec4 c = protoColour(n.protocol);
            const ImU32 col = ImGui::ColorConvertFloat4ToU32(c);
            dl->AddCircleFilled(q, 6 * gUi, col, 20);
            dl->AddCircle(q, 6 * gUi, sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 24, 26, 220), 20, sel ? 2.f : 1.2f);
            const std::string lb = !n.shortName.empty() ? n.shortName : !n.longName.empty() ? n.longName : n.id;
            const ImVec2 ts = ImGui::CalcTextSize(lb.c_str());
            dl->AddRectFilled(ImVec2(q.x + 8 * gUi, q.y - ts.y * 0.5f - 1), ImVec2(q.x + 12 * gUi + ts.x, q.y + ts.y * 0.5f + 1), IM_COL32(10, 12, 14, 190), 2.f);
            dl->AddText(ImVec2(q.x + 10 * gUi, q.y - ts.y * 0.5f), IM_COL32(230, 233, 236, 255), lb.c_str());
            const float d = std::hypot(q.x - mouse.x, q.y - mouse.y);
            if (d < tipD) { tipD = d; tip = &n; }
            if (clicked && std::hypot(q.x - click.x, q.y - click.y) < 14 * gUi) S.sel = nodeKey(n);
        }
    }
    const dect2::MeshAprsStation* tipA = nullptr;
    if (on) {
        for (const auto& s : t.aprsStations) {
            if (!s.st.hasPos) continue;
            shown++;
            const ImVec2 q = adsbmap::project(s.st.lat, s.st.lon);
            const bool sel = S.sel == aprsKey(s);
            const ImU32 col = ImGui::ColorConvertFloat4ToU32(protoColour(s.protocol));
            const float r = 6 * gUi;
            // a diamond: APRS stations and objects, apart from the round mesh nodes
            dl->AddQuadFilled(ImVec2(q.x, q.y - r), ImVec2(q.x + r, q.y), ImVec2(q.x, q.y + r), ImVec2(q.x - r, q.y), col);
            dl->AddQuad(ImVec2(q.x, q.y - r), ImVec2(q.x + r, q.y), ImVec2(q.x, q.y + r), ImVec2(q.x - r, q.y), sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 24, 26, 220), sel ? 2.f : 1.2f);
            const ImVec2 ts = ImGui::CalcTextSize(s.st.call.c_str());
            dl->AddRectFilled(ImVec2(q.x + 8 * gUi, q.y - ts.y * 0.5f - 1), ImVec2(q.x + 12 * gUi + ts.x, q.y + ts.y * 0.5f + 1), IM_COL32(10, 12, 14, 190), 2.f);
            dl->AddText(ImVec2(q.x + 10 * gUi, q.y - ts.y * 0.5f), IM_COL32(230, 233, 236, 255), s.st.call.c_str());
            const float d = std::hypot(q.x - mouse.x, q.y - mouse.y);
            if (d < tipD) { tipD = d; tipA = &s; tip = nullptr; }
            if (clicked && std::hypot(q.x - click.x, q.y - click.y) < 14 * gUi) S.sel = aprsKey(s);
        }
    }
    dl->PopClipRect();
    if (tipA && ImGui::IsWindowHovered()) {
        ImGui::SetTooltip("%s  %s  %s\n%.5f, %.5f\n%s\nSNR %.1f dB, heard %s ago", tipA->st.call.c_str(), protoName(tipA->protocol), symbolText(tipA->st).c_str(), tipA->st.lat, tipA->st.lon,
                          tipA->st.comment.c_str(), tipA->lastSnrDb, ago(t.timeSec, tipA->st.lastHeardSec).c_str());
    }
    if (tip && ImGui::IsWindowHovered()) {
        ImGui::SetTooltip("%s  %s\n%s  %s\n%.5f, %.5f  %.0f m\nSNR %.1f dB, heard %s ago", nodeLabel(*tip).c_str(), tip->id.c_str(), protoName(tip->protocol),
                          tip->hwModel.c_str(), tip->lat, tip->lon, tip->altM, tip->lastSnrDb, ago(t.timeSec, tip->lastHeard).c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1);
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1);
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("fit")) S.mapFit = true;
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never a node's position");
    plat::prefs().setI("meshZoom", S.map.zoom);
    if (!on || shown == 0) {
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
        adsbmap::legend(size.x, "%s", on ? "no node or station has sent its position yet" : "start the receiver");
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
}

void nodeCard(const App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const dect2::MeshNode* n = nullptr;
    if (live(a)) for (const auto& x : t.nodes) if (S.sel == nodeKey(x)) n = &x;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(96 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    const dect2::MeshAprsStation* as = nullptr;
    if (live(a)) for (const auto& x : t.aprsStations) if (S.sel == aprsKey(x)) as = &x;
    if (as) {
        ImGui::TextColored(protoColour(as->protocol), "%s", as->st.call.c_str());
        ImGui::TextDisabled("%s%s", protoName(as->protocol), as->st.isObject ? " object" : " station");
        ImGui::Spacing();
        kv("symbol", "%s", symbolText(as->st).c_str());
        if (as->st.isObject) kv("sent by", "%s", as->st.via.c_str());
        kv("heard", "%s ago, %u frames", ago(t.timeSec, as->st.lastHeardSec).c_str(), as->st.count);
        kv("last", "%s", as->lastType.c_str());
        kv("last SNR", "%.1f dB", as->lastSnrDb);
        if (!as->path.empty()) kv("path", "%s", as->path.c_str());
        if (as->st.hasPos) kv("position", "%.5f %.5f", as->st.lat, as->st.lon);
        if (as->st.hasAlt) kv("altitude", "%.0f m", as->st.altM);
        if (as->st.hasCourse || as->st.hasSpeed) kv("course", "%d deg, %.0f km/h", as->st.courseDeg, as->st.speedKnots * 1.852);
        if (as->batteryPct >= 0) kv("battery", "%d %%", as->batteryPct);
        if (!as->st.comment.empty()) kv("comment", "%s", as->st.comment.c_str());
        return;
    }
    if (!n) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Select a node or station in a table or on the map."); ImGui::PopTextWrapPos(); } return; }
    ImGui::TextColored(protoColour(n->protocol), "%s", nodeLabel(*n).c_str());
    ImGui::TextDisabled("%s", protoName(n->protocol));
    ImGui::Spacing();
    kv("id", "%s", n->id.c_str());
    if (!n->shortName.empty()) kv("short name", "%s", n->shortName.c_str());
    if (!n->hwModel.empty()) kv("hardware", "%s", n->hwModel.c_str());
    if (!n->role.empty()) kv("role", "%s", n->role.c_str());
    kv("heard", "%s ago, %u packets", ago(t.timeSec, n->lastHeard).c_str(), n->packets);
    kv("last SNR", "%.1f dB", n->lastSnrDb);
    if (n->hopsAway >= 0) kv("hops away", "%d", n->hopsAway);
    if (n->hasPosition) {
        kv("position", "%.5f %.5f", n->lat, n->lon);
        kv("altitude", "%.0f m", n->altM);
    }
    if (n->batteryPct >= 0) kv("battery", "%.0f %%", n->batteryPct);
    if (n->voltage > 0) kv("voltage", "%.2f V", n->voltage);
    if (n->channelUtilPct >= 0) kv("channel use", "%.1f %%", n->channelUtilPct);
    if (n->airUtilTxPct >= 0) kv("air time tx", "%.2f %%", n->airUtilTxPct);
    if (n->uptimeS >= 0) kv("uptime", "%s", clock((double)n->uptimeS).c_str());
    if (n->hasEnv) {
        kv("temperature", "%.1f C", n->tempC);
        kv("humidity", "%.0f %%", n->humidity);
        kv("pressure", "%.0f hPa", n->pressureHpa);
    }
}

// ---------------------------------------------------------------- chat

void chatView(const App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    std::vector<std::string> chans;      // "<protocol digit><channel>"
    if (on) {
        for (const auto& m : t.messages) {
            const std::string k = std::to_string(m.protocol) + m.channel;
            if (std::find(chans.begin(), chans.end(), k) == chans.end()) chans.push_back(k);
        }
        std::sort(chans.begin(), chans.end());
    }
    if (S.chatChannel > (int)chans.size()) S.chatChannel = 0;
    std::string cur = "All channels";
    if (S.chatChannel > 0) cur = std::string(protoName(chans[S.chatChannel - 1][0] - '0')) + ": " + chans[S.chatChannel - 1].substr(1);
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::BeginCombo("##mchan", cur.c_str())) {
        if (ImGui::Selectable("All channels", S.chatChannel == 0)) S.chatChannel = 0;
        for (size_t i = 0; i < chans.size(); i++) {
            const std::string l = std::string(protoName(chans[i][0] - '0')) + ": " + chans[i].substr(1);
            if (ImGui::Selectable(l.c_str(), S.chatChannel == (int)i + 1)) S.chatChannel = (int)i + 1;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("%zu messages", t.messages.size());
    ImGui::BeginChild("##mchat", ImVec2(0, 0), ImGuiChildFlags_Borders);
    bool any = false;
    if (on) {
        for (const auto& m : t.messages) {
            if (S.chatChannel > 0 && std::to_string(m.protocol) + m.channel != chans[S.chatChannel - 1]) continue;
            any = true;
            ImGui::TextDisabled("%s", clock(m.timeSec).c_str());
            ImGui::SameLine(0, 8 * gUi);
            ImGui::TextColored(protoColour(m.protocol), "%s", protoTag(m.protocol));
            ImGui::SameLine(0, 8 * gUi);
            if (S.chatChannel == 0) { ImGui::TextDisabled("[%s]", m.channel.c_str()); ImGui::SameLine(0, 6 * gUi); }
            const std::string who = m.fromName.empty() ? m.from : m.fromName;
            ImGui::TextColored(ImVec4(0.9f, 0.92f, 0.94f, 1), "%s", who.c_str());
            if ((m.protocol == 1 && m.to != "^all" && !m.to.empty()) || (m.protocol >= 3 && !m.to.empty() && m.to != "*")) { ImGui::SameLine(0, 6 * gUi); ImGui::TextDisabled("to %s", m.to.c_str()); }
            if (m.hops > 0) { ImGui::SameLine(0, 6 * gUi); ImGui::TextDisabled("(%d hop%s)", m.hops, m.hops == 1 ? "" : "s"); }
            ImGui::PushTextWrapPos(0);
            ImGui::Indent(18 * gUi);
            ImGui::TextUnformatted(m.text.c_str());
            ImGui::Unindent(18 * gUi);
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
        }
    }
    if (!any) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", !on ? "start the receiver to see messages" : "no text message yet"); ImGui::PopTextWrapPos(); }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
}

// ---------------------------------------------------------------- settings


// the LoRa setting: the band plan's presets (with the Meshtastic region and modem preset) or a custom one, remembered per entry
void loraSettings(App& a) {
    State::Custom& c = customOf(a);
    if (subMask(a) == 2) {   // MeshCore: the community's regional radio presets (meshcore.nz), or custom entry
        if (!S.mcListed) mcLoad();
        if (S.mcFetch == 2 || S.mcFetch == 3) { if (S.mcThread.joinable()) S.mcThread.join(); if (S.mcFetch == 2) mcLoad(); S.mcFetch = S.mcFetch == 2 ? 4 : 5; }
        const dect2::MeshCorePresetEntry* cur = nullptr;
        for (const auto& p : S.mcList) if (p.title == S.mcPreset) cur = &p;
        ImGui::TextDisabled("MeshCore radio preset");
        ImGui::SetNextItemWidth(340 * gUi);
        if (ImGui::BeginCombo("##mcpre", cur ? cur->title.c_str() : "Custom")) {
            if (ImGui::Selectable("Custom", !cur)) { S.mcPreset.clear(); plat::prefs().setS("meshCorePreset", ""); }
            for (const auto& reg : dect2::meshCoreRegions(S.mcList)) {
                ImGui::TextDisabled("%s", reg.c_str());
                for (const auto& p : S.mcList) {
                    if (p.region != reg) continue;
                    const std::string lab = p.title + "   " + dect2::meshCorePresetSummary(p) + "##mcp" + p.title;
                    if (ImGui::Selectable(lab.c_str(), cur && cur->title == p.title)) {
                        S.mcPreset = p.title; plat::prefs().setS("meshCorePreset", p.title);
                        mcApply(a, p);
                        if (a.engine.running()) startReceiver(a);
                    }
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The radio settings the MeshCore app suggests for each region (frequency, bandwidth, spreading factor, coding rate). A preset fills in the\ncustom LoRa setting and tunes to its frequency. Custom keeps the values you enter below. %s", S.mcFromFile ? "The list is the one you refreshed from meshcore.nz." : "The list is the one built into this version.");
        if (cur) ImGui::TextDisabled("%s", dect2::meshCorePresetSummary(*cur).c_str());
        ImGui::BeginDisabled(S.mcFetch == 1);
        if (ImGui::SmallButton("Refresh from meshcore.nz")) mcRefresh();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Contacts the internet (api.meshcore.nz), only when you click, and nothing about you is sent. Saves the list of presets and uses it instead of the built-in one.");
        if (S.mcFetch == 1) { ImGui::SameLine(); ImGui::TextDisabled("fetching..."); }
        else if (S.mcFetch == 4) { ImGui::SameLine(); ImGui::TextDisabled("%zu presets loaded", S.mcList.size()); }
        else if (S.mcFetch == 5) { ImGui::SameLine(); ImGui::TextDisabled("could not fetch (the previous list stays)"); }
        ImGui::Spacing();
    }
    ImGui::TextDisabled("LoRa setting");
    int mode = c.on ? 1 : 0;
    ImGui::SetNextItemWidth(220 * gUi);
    static const char* modes[] = {"Presets of the band plan", "Custom"};
    if (ImGui::Combo("##mlmode", &mode, modes, 2)) {
        c.on = mode == 1;
        if (!c.on) { S.mcPreset.clear(); plat::prefs().setS("meshCorePreset", ""); }
        if (c.on) a.freqMhz = c.mhz;
        else applyRegion(a, effRegion(a));
        saveCustom(a);
        if (a.engine.running()) startReceiver(a);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Custom: one LoRa setting of your own (bandwidth, spreading factor, coding rate, sync word, preamble) on the\nfrequency below, for networks that do not use the usual presets. Only that setting is searched. Kept for each mode entry.");
    if (!c.on) {
        if (!meshtasticShown(a)) return;
        // Meshtastic: any firmware region and modem preset; the frequency box goes to the preset's slot
        const auto regions = dect2::meshtasticRegions();
        const auto presets = dect2::meshtasticPresetNames();
        bool ch = false;
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::BeginCombo("Meshtastic region##mtreg", S.mtRegion.empty() ? "from the band plan" : S.mtRegion.c_str())) {
            if (ImGui::Selectable("from the band plan", S.mtRegion.empty())) { S.mtRegion.clear(); ch = true; }
            for (const auto& r : regions) if (ImGui::Selectable(r.c_str(), r == S.mtRegion)) { S.mtRegion = r; ch = true; }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The region set in the Meshtastic app (Config > LoRa > Region). The channel slot follows from the region,\nthe modem preset and the channel name, as the firmware computes it. LORA_24 is 2.4 GHz LoRa (812.5 kHz and wider).");
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::BeginCombo("Modem preset##mtpre", S.mtPreset.c_str())) {
            for (const auto& p : presets) if (ImGui::Selectable(p.c_str(), p == S.mtPreset)) { S.mtPreset = p; ch = true; }
            ImGui::EndCombo();
        }
        if (ch) {
            plat::prefs().setS("meshMtRegion", S.mtRegion);
            plat::prefs().setS("meshMtPreset", S.mtPreset);
            dect2::MeshLoraSettings s;
            if (!S.mtRegion.empty() && dect2::meshtasticRegionPreset(S.mtRegion, S.mtPreset, s)) a.freqMhz = s.freqHz / 1e6;
            else if (S.mtRegion.empty()) applyRegion(a, effRegion(a));
            if (a.engine.running()) startReceiver(a);
        }
        dect2::MeshLoraSettings s;
        const std::string reg = !S.mtRegion.empty() ? S.mtRegion : planRegionName(kPlans[effRegion(a)].rxRegion);
        if (dect2::meshtasticRegionPreset(reg, S.mtPreset, s))
            ImGui::TextDisabled("%s %s: %.4f MHz, SF%d, %g kHz, 4/%d", reg.c_str(), S.mtPreset.c_str(), s.freqHz / 1e6, s.sf, s.bwHz / 1e3, s.cr);
        else ImGui::TextDisabled("%s has no slot for %s (the region is narrower than the preset)", reg.c_str(), S.mtPreset.c_str());
        return;
    }
    bool ch = false, retune = false;
    ImGui::SetNextItemWidth(220 * gUi);
    double mhz = a.freqMhz;
    if (ImGui::InputDouble("Frequency (MHz)##mcf", &mhz, 0.025, 1.0, "%.4f", ImGuiInputTextFlags_EnterReturnsTrue) && mhz > 0) {   // any frequency: the radio is the limit
        a.freqMhz = mhz; retune = true;
    }
    if (S.customEntry == subMask(a) && std::fabs(c.mhz - a.freqMhz) > 1e-7) { c.mhz = a.freqMhz; ch = true; }   // the frequency box is the custom frequency
    int bi = 7;
    for (int i = 0; i < kNumBws; i++) if (std::fabs(kBws[i] - c.bwHz) < 1) bi = i;
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::Combo("Bandwidth##mcb", &bi, kBwNames, kNumBws)) {
        const bool rateChange = (kBws[bi] > 500e3) != (c.bwHz > 500e3);
        c.bwHz = kBws[bi]; ch = true; retune |= rateChange;
    }
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::SliderInt("Spreading factor##mcs", &c.sf, 5, 12, "SF%d")) { c.sf = std::max(5, std::min(12, c.sf)); ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("SF5 and SF6 follow the SX126x frame layout (two more symbols after the sync word); they are not checked against a real radio yet.");
    int ci = c.cr - 5;
    static const char* crs[] = {"4/5", "4/6", "4/7", "4/8"};
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::Combo("Coding rate##mcc", &ci, crs, 4)) { c.cr = ci + 5; ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Each frame's header carries its coding rate: frames at any rate decode.");
    char sw[8];
    snprintf(sw, sizeof sw, "%02X", c.sync);
    ImGui::SetNextItemWidth(60 * gUi);
    if (ImGui::InputText("##mcw", sw, sizeof sw, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) { c.sync = (int)strtol(sw, nullptr, 16) & 0xFF; ch = true; }
    ImGui::SameLine(0, 6 * gUi); ImGui::TextUnformatted("Sync word (hex)");
    if (ImGui::SmallButton("0x2B Meshtastic / MeshCom")) { c.sync = 0x2B; ch = true; }
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::SmallButton("0x12 MeshCore / LoRa APRS")) { c.sync = 0x12; ch = true; }
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::SmallButton("0x34 LoRaWAN")) { c.sync = 0x34; ch = true; }
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::InputInt("Preamble (symbols)##mcp", &c.preamble, 1, 4)) { c.preamble = std::max(6, std::min(65535, c.preamble)); ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Meshtastic 16, MeshCore 32 (SF8 and below) or 16, LoRa APRS and LoRaWAN 8.");
    ImGui::TextDisabled("Low data rate optimisation: %s (on when a symbol lasts 16 ms or more)", dect2::meshLoraLdro(c.sf, c.bwHz) ? "on" : "off");
    if (ch) { saveCustom(a); if (!S.mcPreset.empty()) { S.mcPreset.clear(); plat::prefs().setS("meshCorePreset", ""); } }
    if (retune && a.engine.running()) startReceiver(a);
}

void settingsView(App& a) {
    loadState();
    ImGui::TextDisabled("Band plan");
    ImGui::SetNextItemWidth(340 * gUi);
    int r = effRegion(a);
    if (planCombo(a, "##mreg", r, false)) {
        applyRegion(a, r);
        if (a.engine.running()) startReceiver(a);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sets the tuning frequency (and the Meshtastic / MeshCore channel plan). Meshtastic and MeshCore use different\nfrequencies in each region; the US pair is 3.65 MHz apart and needs a radio rate of about 8 Msps or more (ANZ: 4.1 MHz).\nLoRa APRS and MeshCom are on 70 cm: every known LoRa APRS and MeshCom channel inside the captured band is searched.");
    ImGui::Spacing();
    if (lorawanEntry(a)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("EU868: 868.1, 868.3 and 868.5 MHz (every network), and 867.1 - 867.9 MHz when the capture is wide enough, at SF7 - SF12, 125 kHz, "
                            "and SF7 250 kHz on 868.3 MHz. US915: the 125 kHz uplink channels (902.3 + 0.2 n MHz, SF7 - SF10) and the 500 kHz ones inside the capture. "
                            "Sync word 0x34 (public networks). Every spreading factor of each channel is searched at once. Downlinks (inverted chirps) are not received.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        lorawanPrivacyNote();
        return;
    }
    if (!subMask(a)) {
    ImGui::TextDisabled("Networks");
    bool mt = (S.protocols & 1) != 0, mc = (S.protocols & 2) != 0, la = (S.protocols & 4) != 0, mm = (S.protocols & 8) != 0, lw = (S.protocols & 16) != 0, ch = false;
    if (ImGui::Checkbox("Meshtastic", &mt)) ch = true;
    ImGui::SameLine(0, 14 * gUi);
    if (ImGui::Checkbox("MeshCore", &mc)) ch = true;
    ImGui::SameLine(0, 14 * gUi);
    if (ImGui::Checkbox("LoRa APRS", &la)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("LoRa APRS (lora-aprs / CA2RXU trackers and iGates): 125 kHz, sync word 0x12;\n433.775 MHz SF12 4/5 (EU), 434.855 MHz SF9 4/7 (PL), 439.9125 MHz SF12 (UK), 915 MHz SF12.");
    ImGui::SameLine(0, 14 * gUi);
    if (ImGui::Checkbox("MeshCom", &mm)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("MeshCom 4 (icssw.org): sync word 0x2B, 4/6; 433.175 MHz SF11 250 kHz (EU) and the firmware's other country profiles.");
    ImGui::SameLine(0, 14 * gUi);
    if (ImGui::Checkbox("LoRaWAN", &lw)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("LoRaWAN headers (EU868 / US915, SF7 - SF12): one demodulator per channel and spreading factor, so it uses much more CPU.\nOnly the clear headers are shown; payloads stay encrypted.");
    if (ch) { S.protocols = (mt ? 1 : 0) | (mc ? 2 : 0) | (la ? 4 : 0) | (mm ? 8 : 0) | (lw ? 16 : 0); plat::prefs().setI("meshProtocols", S.protocols); }
    }
    if (!subMask(a) || subMask(a) == 1)    if (ImGui::Checkbox("Search every Meshtastic preset of the region", &S.allPresets)) plat::prefs().setB("meshPresets", S.allPresets);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: LongFast only (the default of every new node). On: also MediumFast, MediumSlow, ShortFast, ShortSlow, LongModerate\nand LongSlow, each on its own channel slot. Uses more CPU, and only the slots inside the captured band are searched.");
    ImGui::Spacing();
    loraSettings(a);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Channel keys");
    ImGui::TextWrapped("The default keys are built in (Meshtastic default channel, MeshCore Public). Add a key for a private channel: Meshtastic takes the channel name and the key in base64; MeshCore takes the channel name and the secret in hex or base64.");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(120 * gUi);
    static const char* protos[] = {"Meshtastic", "MeshCore"};
    ImGui::Combo("##mkp", &S.keyProto, protos, 2);
    ImGui::SameLine(0, 6 * gUi); ImGui::SetNextItemWidth(150 * gUi);
    ImGui::InputTextWithHint("##mkn", "channel name", S.keyName, sizeof S.keyName);
    ImGui::SameLine(0, 6 * gUi); ImGui::SetNextItemWidth(260 * gUi);
    ImGui::InputTextWithHint("##mks", "key (base64 or hex)", S.keySecret, sizeof S.keySecret, ImGuiInputTextFlags_Password);
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("Add")) {
        const int proto = S.keyProto == 0 ? 1 : 2;
        const bool ok = proto == 1 ? a.engine.mesh().addMeshtasticChannel(S.keyName, S.keySecret) : a.engine.mesh().addMeshCoreChannel(S.keyName, S.keySecret);
        if (ok) {
            S.keys.push_back({proto, S.keyName, S.keySecret});
            saveKeys();
            S.keyName[0] = 0; S.keySecret[0] = 0;
            S.keyMsg = "key added";
        } else {
            S.keyMsg = "this key is not valid";
        }
    }
    if (!S.keyMsg.empty()) { ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("%s", S.keyMsg.c_str()); }
    for (size_t i = 0; i < S.keys.size(); i++) {
        ImGui::PushID((int)i);
        ImGui::TextColored(protoColour(S.keys[i].proto), "%s", protoName(S.keys[i].proto));
        ImGui::SameLine(110 * gUi); ImGui::TextUnformatted(S.keys[i].name.c_str());
        ImGui::SameLine(300 * gUi);
        if (ImGui::SmallButton("remove")) {
            S.keys.erase(S.keys.begin() + (long)i);
            saveKeys(); pushKeys(a);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (S.keys.empty()) ImGui::TextDisabled("no key added");
    ImGui::Spacing();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Keys are kept in this computer's settings, never sent anywhere."); ImGui::PopTextWrapPos(); }
}

// ---------------------------------------------------------------- the hooks

void tab(App& a) {
    loadState();
    if (const int m = subMask(a)) {   // a protocol entry: only its own views (LoRa APRS: stations instead of nodes and chat; MeshCom: stations and chat; LoRaWAN: devices and frames)
        static const int aprsIds[] = {5, 1, 3, 4}, comIds[] = {5, 1, 2, 3, 4}, meshIds[] = {0, 1, 2, 3, 8, 4}, lwIds[] = {6, 7, 4};
        const bool ap = m == 4, com = m == 8, lw = m == 16;
        const int* ids = ap ? aprsIds : com ? comIds : lw ? lwIds : meshIds;
        const int n = ap ? 4 : lw ? 3 : com ? 5 : 6;
        int k = 0;
        for (int i = 0; i < n; i++) if (ids[i] == S.view) k = i;
        if (ap) subNav("meshva", k, {"Stations", "Map", "Packets", "Settings"});
        else if (com) subNav("meshvc", k, {"Stations", "Map", "Chat", "Packets", "Settings"});
        else if (lw) subNav("meshvl", k, {"Devices", "Frames", "Settings"});
        else subNav("meshvm", k, {"Nodes", "Map", "Chat", "Packets", "Telemetry", "Settings"});
        S.view = ids[k];
    } else {
        if (S.view == 7) S.view = 3;
        static const int ids[] = {0, 1, 2, 3, 8, 4, 5, 6};   // the views in the order of the tabs (7, the LoRaWAN frames, has no tab here)
        int k = 0;
        for (int i = 0; i < 8; i++) if (ids[i] == S.view) k = i;
        subNav("meshv", k, {"Nodes", "Map", "Chat", "Packets", "Telemetry", "Settings", "APRS", "LoRaWAN"});
        S.view = ids[k];
    }
    plat::prefs().setI("meshView", S.view);
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    if (S.view == 0) { nodeTable(a, false); return; }
    if (S.view == 2) { chatView(a); return; }
    if (S.view == 3) { packetTable(a); return; }
    if (S.view == 8) { telemetryTable(a); return; }
    if (S.view == 4) { settingsView(a); return; }
    if (S.view == 5) { aprsTable(a); return; }
    if (S.view == 6) { lorawanDevices(a, false); return; }
    if (S.view == 7) { lorawanFrames(a); return; }
    const float cw = std::min(300.f * gUi, W * 0.32f);
    ImGui::BeginChild("##mesh_l", ImVec2(W - cw - 8 * gUi, H));
    mapView(a, ImGui::GetContentRegionAvail());
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##mesh_r", ImVec2(0, H));
    nodeCard(a);
    ImGui::EndChild();
}

void list(App& a) {
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see nodes"); ImGui::PopTextWrapPos(); } return; }
    if (resultsClearButton("mesh")) a.engine.mesh().clearResults();
    const dect2::MeshTelemetry& t = a.rx.mesh;
    size_t msgs = 0;
    for (const auto& m : t.messages) msgs += shows(a, m.protocol) ? 1 : 0;
    ImGui::PushTextWrapPos(0);
    if (lorawanEntry(a)) ImGui::TextDisabled("%zu LoRaWAN devices", t.lorawanDevices.size());
    else if (stationEntry(a)) ImGui::TextDisabled("%zu stations, %zu messages", sortedStations(a).size(), msgs);
    else if (subMask(a)) ImGui::TextDisabled("%zu nodes, %zu messages", sortedNodes(a).size(), msgs);
    else ImGui::TextDisabled("%zu nodes, %zu APRS stations, %zu messages", t.nodes.size(), t.aprsStations.size(), t.messages.size());
    ImGui::PopTextWrapPos();
    if (lorawanEntry(a)) lorawanDevices(a, true);
    else if (stationEntry(a)) aprsTable(a, true);
    else nodeTable(a, true);
}

void receiver(App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    if (t.custom) kv("tuned", "%.4f MHz, %.3f Msps, custom LoRa setting", t.tunedHz / 1e6, t.inputRate / 1e6);
    else kv("tuned", "%.4f MHz, %.3f Msps, %s plan, Meshtastic %s", t.tunedHz / 1e6, t.inputRate / 1e6, t.region == 1 ? "US" : t.region == 2 ? "ANZ" : t.region == 3 ? "ANZ Mid" : "EU", t.mtRegion.c_str());
    kv("frames", "%llu good, %llu CRC errors, %llu bad headers", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.headerBad);
    kv("preambles", "%llu", (unsigned long long)t.preambles);
    kv("signal time", "%s", clock(t.timeSec).c_str());
    ImGui::Spacing();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("LoRa settings searched"); ImGui::PopTextWrapPos(); }
    if (ImGui::BeginTable("##mdec", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX, ImVec2(0, std::max(60.f * gUi, ImGui::GetContentRegionAvail().y * 0.5f)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthFixed, hdrW("Setting", "LoRa APRS EU")); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("SF"); ImGui::TableSetupColumn("BW kHz");
        ImGui::TableSetupColumn("CR"); ImGui::TableSetupColumn("Sync"); ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, hdrW("Frames", "out of band")); ImGui::TableSetupColumn("Bad", ImGuiTableColumnFlags_WidthFixed, hdrW("Bad", "99999"));
        ImGui::TableSetupColumn("Last SNR", ImGuiTableColumnFlags_WidthFixed, hdrW("Last SNR", "-12.5"));
        ImGui::TableHeadersRow();
        for (const auto& d : t.decoders) {
            if (!d.inBand && d.protocol >= 3) continue;    // the 70 cm list is long: only the channels in the capture
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(protoColour(d.protocol), "%s", ellipsize(d.preset, ImGui::GetContentRegionAvail().x).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.preset.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.3f", d.freqHz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%d", d.sf);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", d.bwHz / 1e3);
            ImGui::TableNextColumn(); ImGui::Text("4/%d", d.cr);
            ImGui::TableNextColumn(); ImGui::Text("0x%02X", d.syncWord);
            ImGui::TableNextColumn(); if (d.inBand) ImGui::Text("%llu", (unsigned long long)d.frames); else ImGui::TextDisabled("out of band");
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)(d.crcBad + d.headerBad + d.syncBad));
            ImGui::TableNextColumn(); if (d.frames) ImGui::Text("%.1f", d.lastSnrDb); else ImGui::TextUnformatted("-");
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Packets by type");
    // at least a few rows: in a short tab the pane scrolls to it instead (it was left a sliver of its header)
    if (ImGui::BeginTable("##mcnt", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY,
                          ImVec2(0, std::max(ImGui::GetContentRegionAvail().y, 5 * ImGui::GetTextLineHeightWithSpacing())))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Net"); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, hdrW("Count", "99999"));
        ImGui::TableHeadersRow();
        for (const auto& c : t.counts) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(protoColour(c.protocol), "%s", protoTag(c.protocol));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.type.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%u", c.count);
        }
        ImGui::EndTable();
    }
}

void panels(App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi, plotH = std::max(60.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    const float w1 = std::max(200.f, (W - 3 * gap) * 0.45f), w2 = std::max(120.f, (W - 3 * gap) * 0.25f), w3 = std::max(120.f, W - w1 - w2 - 3.5f * gap);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    ImGui::BeginGroup();
    captionFit(w1, "Last packets");
    if (ImGui::BeginTable("##mpl", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(w1, plotH))) {
        ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Net"); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("From"); ImGui::TableSetupColumn("SNR");
        if (on) {
            for (size_t i = t.packets.size(); i-- > 0;) {
                const dect2::MeshPacket& p = t.packets[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(p.timeSec).c_str());
                ImGui::TableNextColumn(); ImGui::TextColored(protoColour(p.protocol), "%s", protoTag(p.protocol));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ellipsize(p.crcOk ? (p.type.empty() ? "?" : p.type) : "CRC error", ImGui::GetContentRegionAvail().x).c_str());   // the stretched column of a narrow table
                ImGui::TableNextColumn(); ImGui::TextUnformatted(p.from.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.1f", p.snrDb);
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(w2, "Packets by type");
    if (ImGui::BeginTable("##mcn", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(w2, plotH))) {
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("n");
        if (on) {
            for (const auto& c : t.counts) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextColored(protoColour(c.protocol), "%s", c.type.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%u", c.count);
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(w3, "SNR of good frames (dB)");
    std::vector<float> v(S.snr.begin(), S.snr.end());
    if (v.empty()) v.push_back(0);
    ImGui::PlotLines("##msn", v.data(), (int)v.size(), 0, nullptr, -25.f, 30.f, ImVec2(w3, plotH));
    ImGui::EndGroup();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("LoRa", !on ? 0 : t.state >= 1 ? 1 : 2); flowNext(12 * gUi);
    lamp("Decoding", !on ? 0 : t.state == 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[64];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    ro("State", t.state == 2 ? "Decoding" : t.state == 1 ? "Signal" : "Searching");
    snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro("Frames ok / bad", b);
    int mt = 0, mc = 0;
    for (const auto& n : t.nodes) (n.protocol == 1 ? mt : mc)++;
    if (!lorawanEntry(a)) { snprintf(b, sizeof b, "%d / %d", mt, mc); ro("Nodes Mt / Mc", b); }
    if (!t.aprsStations.empty()) { snprintf(b, sizeof b, "%zu", t.aprsStations.size()); ro("APRS stations", b); }
    if (!t.lorawanDevices.empty() || lorawanEntry(a)) { snprintf(b, sizeof b, "%zu", t.lorawanDevices.size()); ro("LoRaWAN devices", b); }
    snprintf(b, sizeof b, "%zu", t.messages.size()); ro("Messages", b);
    snprintf(b, sizeof b, "%.1f dB", t.snrDb); ro("SNR", b);
}

void summary(const App& a, std::string& l1, std::string& l2) {
    const SubMode* sm = curSub(a);
    l1 = sm ? sm->name : "Mesh (LoRa)";
    if (live(a)) l2 = dect2::meshSummary(a.rx.mesh);
}

void tuner(App& a, bool& retune) {
    loadState();
    ImGui::TextDisabled("Band plan");
    ImGui::SetNextItemWidth(std::min(230 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    int r = effRegion(a);
    if (planCombo(a, "##mreg_t", r, true)) { applyRegion(a, r); retune = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("EU / US / ANZ: the Meshtastic LongFast channel; MeshCore sits 93 kHz above it in the EU (one capture holds both).\nAPRS / MeshCom: 70 cm LoRa APRS and MeshCom; MeshCom EU at 433.175 MHz also holds LoRa APRS at 433.775 MHz.\nThe frequency box can be changed for other slots: the receiver follows it.");
}

void decoder(App& a, bool&) {
    loadState();
    if (customActive(a)) {
        const State::Custom& c = customOf(a);
        ImGui::TextDisabled("custom: SF%d %g kHz 4/%d, sync 0x%02X", c.sf, c.bwHz / 1e3, c.cr, c.sync);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A custom LoRa setting is on (Settings tab of the mode).");
        return;
    }
    if (const int m = subMask(a)) {   // a protocol entry decodes only its own network
        if (m == 1) { if (ImGui::Checkbox("all presets", &S.allPresets)) plat::prefs().setB("meshPresets", S.allPresets); if (ImGui::IsItemHovered()) ImGui::SetTooltip("Also search MediumFast, MediumSlow, ShortFast, ShortSlow, LongModerate and LongSlow. Uses more CPU."); }
        else ImGui::TextDisabled("%s only", curSub(a)->name);
        if (m == 16 && ImGui::IsItemHovered())
            ImGui::SetTooltip("Every spreading factor (SF7 - SF12) of every channel in the capture at once.\nOnly the headers every gateway receives in the clear; payloads stay encrypted, nothing is decrypted.");
        return;
    }
    bool mt = (S.protocols & 1) != 0, mc = (S.protocols & 2) != 0, la = (S.protocols & 4) != 0, mm = (S.protocols & 8) != 0, ch = false;
    auto boxW = [](const char* l) { return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(l).x; };   // on the next line when the panel is narrow
    if (ImGui::Checkbox("Meshtastic", &mt)) ch = true;
    sameLineIf(boxW("MeshCore"), 8 * gUi);
    if (ImGui::Checkbox("MeshCore", &mc)) ch = true;
    sameLineIf(boxW("LoRa APRS"), 8 * gUi);
    if (ImGui::Checkbox("LoRa APRS", &la)) ch = true;
    sameLineIf(boxW("MeshCom"), 8 * gUi);
    if (ImGui::Checkbox("MeshCom", &mm)) ch = true;
    bool lw = (S.protocols & 16) != 0;
    sameLineIf(boxW("LoRaWAN"), 8 * gUi);
    if (ImGui::Checkbox("LoRaWAN", &lw)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("LoRaWAN headers: one demodulator per channel and spreading factor (much more CPU). Payloads stay encrypted.");
    if (ch) { S.protocols = (mt ? 1 : 0) | (mc ? 2 : 0) | (la ? 4 : 0) | (mm ? 8 : 0) | (lw ? 16 : 0); plat::prefs().setI("meshProtocols", S.protocols); }
    sameLineIf(boxW("all presets"), 8 * gUi);
    if (ImGui::Checkbox("all presets", &S.allPresets)) plat::prefs().setB("meshPresets", S.allPresets);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Also search MediumFast, MediumSlow, ShortFast, ShortSlow, LongModerate and LongSlow. Uses more CPU.");
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("simulated mesh");
    flowNext(); ImGui::SetNextItemWidth(100 * gUi);
    const bool p70 = sc.modeOpt[1] == 2;
    int prot = sc.modeOpt[0] > 0 ? sc.modeOpt[0] : 15;
    // the scene of the band: Meshtastic / MeshCore on 868 or 915, LoRa APRS / MeshCom on 70 cm
    static const char* names868[] = {"", "Meshtastic", "MeshCore", "both"};
    static const char* names70[] = {"", "", "", "", "LoRa APRS", "", "", "", "MeshCom", "", "", "", "both"};
    const int sel = p70 ? ((prot & 12) ? (prot & 12) : 12) : ((prot & 3) ? (prot & 3) : 3);
    if (subMask(a)) ImGui::TextDisabled("%s", curSub(a)->name);   // a protocol entry plays only its own network
    else if (ImGui::BeginCombo("##msyp", p70 ? names70[sel] : names868[sel])) {
        if (p70) { for (int i : {4, 8, 12}) if (ImGui::Selectable(names70[i], sel == i)) { sc.modeOpt[0] = i; changed = true; } }
        else for (int i = 1; i <= 3; i++) if (ImGui::Selectable(names868[i], sel == i)) { sc.modeOpt[0] = i; changed = true; }
        ImGui::EndCombo();
    }
    flowNext(); ImGui::TextDisabled("SNR"); ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
    float snr = (float)sc.snrDb;
    if (ImGui::SliderFloat("##msnr", &snr, -20.f, 30.f, "%.0f dB")) { sc.snrDb = snr; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("SNR of the strongest node. The weakest is 20 dB lower unless set otherwise.");
    flowNext(); ImGui::TextDisabled("CFO"); ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
    float cfo = (float)(sc.cfoHz / 1e3);
    if (ImGui::SliderFloat("##mcfo", &cfo, -25, 25, "%.1f kHz")) { sc.cfoHz = cfo * 1e3; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The radio's frequency error at 869 MHz (a 1 ppm TCXO is off by about 0.9 kHz).");
    if (subMask(a)) return;   // the band follows the entry
    flowNext();
    bool us = sc.modeOpt[1] == 1;
    if (ImGui::Checkbox("US plan", &us)) { applyRegion(a, us ? 1 : 0); sc.modeOpt[0] = 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("LongFast at 906.875 MHz and MeshCore at 910.525 MHz. MeshCore needs a rate of about 8 Msps to be inside the capture.");
    flowNext();
    bool anz = sc.modeOpt[1] == 5;
    if (ImGui::Checkbox("ANZ plan", &anz)) { applyRegion(a, anz ? 8 : 0); sc.modeOpt[0] = 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("LongFast at 919.875 MHz and MeshCore AU at 915.8 MHz. MeshCore needs a rate of about 10 Msps to be inside the capture.");
    flowNext();
    bool b70 = p70;
    if (ImGui::Checkbox("70 cm", &b70)) { applyRegion(a, b70 ? 5 : 0); sc.modeOpt[0] = 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("LoRa APRS stations on 433.775 MHz and MeshCom nodes on 433.175 MHz (tunes to MeshCom EU, which holds both).");
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    if (lorawanEntry(a)) out.push_back({"DEVICES", "%.0f", (double)t.lorawanDevices.size(), 0, 50, t.lorawanDevices.empty() ? 0 : 1});
    else out.push_back({"NODES", "%.0f", (double)t.nodes.size(), 0, 20, t.nodes.empty() ? 0 : 1});
    out.push_back({"FRAMES OK", "%.0f", (double)t.blocksOk, 0, 100, t.blocksOk ? 1 : 0});
    out.push_back({"SNR  dB", "%.1f", (double)t.snrDb, -20, 20, t.snrDb >= 0 ? 1 : t.snrDb >= -10 ? 2 : 3});
}

} // namespace

extern const ModeUi kMeshUi;
const ModeUi kMeshUi = {
    .sideTitle = "NODES",
    .tabName = "Mesh",
    .tabIcon = Ic::Link,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .decoder = decoder,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
