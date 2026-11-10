// Shared by the Mesh (LoRa) screens (mesh_*.cpp): the screen state, the band plans and the helpers more than one file uses.
// Everything is in namespace meshui; the mode entry is kMeshUi (declared in modeui.cpp).
// Included by mesh_ui.cpp, mesh_tables.cpp, mesh_lorawan_ui.cpp, mesh_views.cpp and mesh_settings_ui.cpp only.
#pragma once
#include "app.h"
#include "../adsb/adsb_map.h"
#include "dect2/packet/packet_aprs.h"
#include "dect2/mesh/mesh_proto.h"
#include "dect2/mesh/mesh_presets.h"
#include "state/results_export.h"
#include "dect2/mesh/mesh_report.h"
#include <atomic>
#include <deque>
#include <map>
#include <set>
#include <thread>


namespace meshui {

struct Key { int proto; std::string name, secret; };

struct State {
    bool loaded = false;
    int view = 0;                                // 0 nodes, 1 map, 2 chat, 3 packets, 4 settings, 5 APRS stations, 6 LoRaWAN devices, 7 LoRaWAN frames, 11 MeshCore device
    bool rawView = false;                        // packets: the raw payload instead of the detail line
    adsbmap::View map;
    bool mapFit = true;                          // centre on the nodes until the user drags the map
    std::string sel;                             // selected node: protocol digit + id
    int chatChannel = 0;                         // index into the channel list built each frame (0 = all)
    int region = 0, protocols = 15;              // region: index into kPlans
    bool allPresets = false;
    int tableScale = 1, pushedScale = -1;         // table sizes times 1 / 5 / 20 (pref meshTableScale)
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

// band plans: the receiver's region (Meshtastic / MeshCore channel plan), the frequency to tune and the test signal's scene
struct Plan { const char* name; const char* shortName; double mhz; int rxRegion, synthRegion; };

inline const Plan kPlans[] = {
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

constexpr int kNumPlans = (int)(sizeof(kPlans) / sizeof(kPlans[0]));


// mesh_ui.cpp
extern State S;
bool live(const App& a);
ImVec4 protoColour(int proto);
const char* protoName(int proto);
const char* protoTag(int proto);
const char* planRegionName(int rxRegion);
std::string nodeKey(const dect2::MeshNode& n);
std::string nodeLabel(const dect2::MeshNode& n);
std::string clock(double s);
std::string ago(double now, double then);
void loadState();
int subMask(const App& a);
bool shows(const App& a, int proto);
bool lorawanEntry(const App& a);
bool meshtasticShown(const App& a);
void synth(App& a, bool& changed);

// mesh_tables.cpp
std::vector<const dect2::MeshNode*> sortedNodes(const App& a);
float hdrW(const char* header, const char* sample);
void nodeTable(const App& a, bool compact);
void telemetryTable(const App& a);
void packetTable(const App& a);
std::string aprsKey(const dect2::MeshAprsStation& s);
std::string symbolText(const dect2::PacketStation& s);
std::vector<const dect2::MeshAprsStation*> sortedStations(const App& a);
void aprsTable(const App& a, bool compact = false);

// mesh_lorawan_ui.cpp
void lorawanPrivacyNote();
void lorawanDevices(const App& a, bool compact);
void lorawanFrames(const App& a);

// mesh_views.cpp
void mapView(App& a, ImVec2 size);
void nodeCard(const App& a);
void chatView(const App& a);

// meshcore_ui.cpp: the MeshCore device (send and receive as a MeshCore companion node)
void meshcoreTick(App& a);
void meshcoreView(App& a);

// mesh_settings_ui.cpp
void pushKeys(App& a);
int effRegion(const App& a);
bool planCombo(App& a, const char* id, int& r, bool shortNames);
void applyRegion(App& a, int region);
State::Custom& customOf(const App& a);
void settingsView(App& a);

}  // namespace meshui
