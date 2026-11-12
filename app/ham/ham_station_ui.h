// The 4.0 station layer of the Ham view (ham_station_ui.cpp): the big frequency and S-meter, the band bar with the frequency memory per band
// and mode, every other amateur mode in one list, online spots (DX cluster, RBN, POTA, SOTA, PSK Reporter: each off until switched on),
// callsign lookup (cache, earlier QSOs, QRZ / HamQTH only with the user's own account), needed and alert marks, POTA / SOTA / WWFF activation,
// the band-hop schedule, and the logbook tools (Cabrillo import, QSL confirmations, backups, LoTW through the user's own TQSL).
// The logic is in core/include/dect2/ham/ham_station.h, ham_spots.h, ham_net.h, ham_callbook.h, ham_needed.h, ham_logtools.h (tested).
// Nothing here transmits.
#pragma once
#include "../app.h"
#include "dect2/ham/ham_bandhop.h"
#include "dect2/ham/ham_callbook.h"
#include "dect2/ham/ham_logtools.h"
#include "dect2/ham/ham_needed.h"
#include "dect2/ham/ham_net.h"
#include "dect2/ham/ham_spots.h"
#include "dect2/ham/ham_station.h"
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

struct HamStation {
    bool loaded = false;
    std::string country;                       // ISO code, prefs ham.country
    std::vector<std::string> antennas;         // prefs ham.antennas (one per line)
    dect2::ham::FreqMemory mem;                // prefs ham.freqMem
    // spots
    dect2::ham::SpotFeedConfig feedCfg;        // prefs ham.feed.*
    dect2::ham::SpotFeeds feeds;
    dect2::ham::SpotBook book;
    unsigned srcMask = 0x3F;
    bool bandOnly = true, neededOnly = false;
    // callsign lookup
    dect2::ham::CallbookClient cb;
    dect2::ham::CallbookConfig cbCfg;          // prefs ham.cb.*
    std::mutex cbMu;
    std::thread cbWorker;
    std::atomic<bool> cbBusy{false};
    std::string cbCall, cbErr;                 // the call shown and the last online error
    dect2::ham::CallbookEntry cbEntry;
    // needed and alerts
    dect2::ham::NeedIndex need;
    bool needDirty = true;
    dect2::ham::NeedAlerter alerter;           // prefs ham.alert.*
    bool alertsOn = true;
    std::string lastAlert;
    // activation and hunting
    dect2::ham::Activation act;                // prefs ham.act.*
    std::map<std::string, std::pair<dect2::ham::Program, std::string>> hunted;   // call -> the reference of the spot it was called from
    // band hopping
    bool hopOn = false;                        // prefs ham.hop.on
    dect2::ham::HopSchedule hop;               // prefs ham.hop.text
    dect2::HamBandHop hopper;
    std::string hopText;
    long lastHopSlot = -1;
    // CQ modifier (CQ DX, CQ POTA ...) for the slot modes, prefs ham.cqMod
    std::string cqMod;
    // FT8 / FT4 contest exchange (WSJT-X): 0 none, 1 ARRL Field Day, 2 ARRL RTTY Roundup; prefs ham.ftxContest / ham.ftxExch
    int ftxContest = 0;
    std::string ftxExch;
    // CW speed matching (the keyboard panel), prefs ham.cwMatch
    bool cwMatch = false;
    dect2::ham::CwSpeedMatch cwm;
    int cwFocusId = -1;
    // split (from a spot comment)
    double splitTxHz = 0;
    std::string splitFor;
    // backups and LoTW
    bool backupChecked = false;
    std::string tqslPath, tqslLocation;        // prefs ham.tqsl.path / ham.tqsl.location
    std::atomic<int> tqslState{0};             // 0 idle, 1 running, 2 done, 3 failed
    std::thread tqslWorker;
    std::string tqslMsg;
    ~HamStation();
};
HamStation& hamStation();
void hamStationLoad();
void hamStationSave();

// The big frequency, the S-meter (rxDb: dBFS of the receive audio, uncalibrated) and the band plan hint for the mode class at the TX frequency.
void hamStationHeader(App& a, double txAudioHz, const std::string& modeClass, double rxDb);
// One button per band: a click stores the frequency of the band and mode left and goes to the one remembered for the new band (else the usual
// dial of the mode). modeId: the catalog id ("ft8", "psk31"). Returns true when it tuned.
bool hamBandBar(App& a, const std::string& modeId, int slotMode);
// The mode left: remember where it was (call before switching to another mode)
void hamRememberFreq(App& a, const std::string& modeId);
// The amateur modes that run in their own screens (JS8Call, SSTV, digital voice, packet ...): picking one leaves the Ham view for that screen.
void hamMoreModesCombo(App& a);
// The Cluster tab: the feeds (each opt-in), the spot list with needed marks; a click tunes there and puts the call into the QSO panel.
void hamClusterTab(App& a, const std::function<void(const std::string& call, const std::string& grid)>& pick);
// POTA / SOTA / WWFF / IOTA activation and hunting.
void hamActivateTab(App& a);
// Under the Log tab: Cabrillo import, LoTW / eQSL / card confirmations, backups, TQSL.
void hamLogToolsUi();
// In the QSO panel: name, QTH, state, grid of the call from the cache, the log and (if switched on) the online callbook.
void hamCallbookPanel(const std::string& call);
// The callbook account (wizard and the QSO panel's settings).
void hamCallbookSettings();
// Station extras for the setup wizard: country (region from it), the grid from the receiver position, the antennas.
void hamStationWizardExtras(char* grid, size_t gridN);
// Every QSO before it is logged: the activation reference, a hunted reference, the callbook fields. Called by hamLogQso.
void hamStationOnLog(dect2::ham::LogQso& q);
// The needed level of a heard station (and an alert toast when the rules ask for one).
dect2::ham::NeedResult hamNeedOf(const std::string& call, const std::string& grid, const std::string& band, const std::string& mode, bool alert);
// Band hopping by schedule and by the spots: the band for the slot starting at slotUtc, "" to stay. Called by the Ham view when nothing is running.
std::string hamHopBand(double slotUtc, double slotSec, const std::string& curBand);
void hamHopUi();
// A spot from outside heard on the station's own receiver: feeds the hopper.
void hamHopSpot(const std::string& band, double utc);
