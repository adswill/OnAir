// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tools > Propagation, shared between prop_ui.cpp (window, worker, map, Simple mode, settings) and prop_adv.cpp (Advanced mode panels).
#pragma once
#include "../app.h"
#include "../adsb/adsb_map.h"
#include "dect2/prop/prop.h"
#include "dect2/prop/prop_scenario.h"
#include "dect2/prop/prop_terrain.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace propui {
namespace pr = dect2::prop;

// one finished computation (the worker builds it, the window only reads it)
struct Result {
    pr::Params p[2];
    std::shared_ptr<const pr::Terrain> terrain;                // keeps p[i].terrain alive
    std::shared_ptr<const pr::AntennaPattern> pattern[2];      // keeps p[i].txPattern alive
    bool compare = false;
    pr::Range range[2];
    double bestKm[2] = {0, 0};                                 // with terrain: the farthest of the bearings tried
    pr::Grid grid[2];
    bool sweeps = false;                                       // Advanced: the charts below
    double sweepBearing = 0, sweepKm = 0;
    pr::Series sigVsDist[2], rangeVsPower[2], rangeVsFreq[2];
    pr::HfDay day[2];
    // 4.0
    double tgtLat = 0, tgtLon = 0;                             // the point the profile and the HF chart are computed toward
    std::vector<pr::Series> lossByModel;                       // scenario A: path loss against distance, model by model
    pr::ProfileView profile;                                   // scenario A, 30 MHz and up
    pr::hf::Chart hfChart;                                     // scenario A, below 30 MHz: hour x band reliability
    bool haveChart = false;
};

// Satellite tab (4.0): an earth-space link from the transmitter position (your location) to a satellite
struct SatUi {
    int kind = 0;                            // 0 geostationary at a longitude, 1 low orbit at an altitude and elevation
    double satLon = 19.2, leoAltKm = 820, leoElevDeg = 30;
    double fGHz = 11.7, eirpDbw = 52, rxGainDbi = 37.6, nfDb = 1, feederDb = 0, dishM = 0.8, eff = 0.6;
    double r001 = 30, availPct = 99.9, bwHz = 36e6, reqSnrDb = 3, rho0 = 7.5, nWet = 42;
    int pol = 2;                             // 0 horizontal, 1 vertical, 2 circular
    int mode = -1;                           // index into modeThresholds() for the required SNR, -1 typed
};

struct State {
    bool loaded = false;
    bool advanced = false;
    bool compare = false;
    int editing = 0;                         // Advanced + compare: which scenario the inputs edit (0 A, 1 B)
    int show = 0;                            // which scenario the map colours
    pr::Scenario sc[2];
    std::shared_ptr<const pr::AntennaPattern> pattern[2];
    std::string patternMsg[2];
    std::shared_ptr<const pr::Terrain> terrain;
    std::string terrainMsg;
    std::vector<std::string> terrainFiles;
    int resolution = 1;                      // Advanced: 0 coarse 40, 1 medium 80, 2 fine 140 cells per side
    double hfChartKm = 1000;                 // the HF time-of-day chart's path length when no point is chosen
    double budgetKm = 10;                    // the link budget's distance (north of the transmitter) when no point is chosen
    adsbmap::View map;
    bool havePoint = false; double pLat = 0, pLon = 0;   // the receiver / probe point (a map click)
    bool placingTx = false;                  // the next map click moves the transmitter
    char coordBuf[96] = "";
    std::string msg;
    std::string csvMsg;
    float labelCol = 0;                      // numField / enumCombo: x of the input after the label (0: right after it)
    SatUi sat;
    // terrain download (opt-in, Advanced > Terrain)
    std::thread dlTh;
    std::atomic<bool> dlBusy{false};
    std::mutex dlMu;
    std::string dlMsg;                       // under dlMu
    std::vector<std::string> dlDone;         // files the download finished, picked up by the UI thread (under dlMu)
    double dlRadiusKm = 60;
    // worker
    std::mutex mu;
    std::thread th;
    std::atomic<bool> busy{false}, cancel{false};
    std::shared_ptr<const Result> result;    // under mu
    std::string lastKey;
    ~State() { cancel = true; if (th.joinable()) th.join(); if (dlTh.joinable()) dlTh.join(); }   // never leave a joinable thread at exit
};
State& st();

pr::Scenario& cur();                         // the scenario the inputs edit
bool numField(const char* label, double* v, const char* unit, const char* help, float w = 0, const char* fmt = "%.4g");
void helpMark(const char* text);
void enumCombo(const char* label, int* v, const char* items, const char* help, float w = 0);
std::shared_ptr<const Result> snapshot();
bool receiverPoint(double& lat, double& lon, std::string& label);   // the clicked point, else your location when it is set
void mapView(App& a, float reserveBelow);    // the shared map with the coverage, rings and markers; click = receiver point
void issuesUi(const pr::Scenario& s);        // the input checks of a scenario, red / amber / grey
void loadPatternFor(int i);                  // (re)read scenario i's antenna pattern file
void reloadTerrain();                        // (re)read S.terrainFiles
void placeRowUi();                           // the transmitter position row (my location, place on the map, typed position)

// prop_adv.cpp
void advancedInputs(App& a);
void advancedResults(App& a, const std::shared_ptr<const Result>& r);
}
