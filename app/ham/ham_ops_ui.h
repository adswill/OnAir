// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Ham view's operator tools around the operating screen (ham_ui.cpp): the logbook, awards, contest, map, spots, the station setup
// wizard and the guide. The logic is in core/include/dect2/ham/ham_log.h, ham_dxcc.h and ham_ops.h (tested); these files draw it.
// Nothing here transmits: the only transmitter is ham_ui.cpp's HamSlotTx behind the Arm button, the licence box and the band gate.
#pragma once
#include "../app.h"
#include "dect2/ham/ham.h"
#include "dect2/ham/ham_dxcc.h"
#include "dect2/ham/ham_log.h"
#include "dect2/ham/ham_ops.h"
#include <functional>
#include <string>
#include <vector>

enum HamTab { HamTabOperate, HamTabLog, HamTabAwards, HamTabMap, HamTabSpots, HamTabCluster, HamTabContest, HamTabActivate, HamTabGuide, HamTabCount };   // 4.0: Cluster (online spots), Activate (POTA / SOTA)

struct HamOps {
    bool loaded = false;
    dect2::ham::Logbook log;
    std::string logPath;
    dect2::ham::DxccDb dxcc;                 // the built-in table, or the user's cty.dat (prefs "ham.ctyPath")
    std::string ctyPath, ctyInfo;
    bool ctyInMsg = true;                    // country and zone beside decoded messages (prefs "ham.ctyInMsg")
    dect2::ham::WorkedIndex worked;
    bool workedDirty = true;                 // rebuilt from the log on the next frame
    dect2::ham::SpotList spots;
    // station (the setup wizard): prefs ham.region, ham.licClass, ham.power, ham.antenna, ham.setupDone
    int region = 0;                          // IARU region 1 2 3; 0 not chosen
    std::string licClass, power, antenna, myCall, myGrid;
    bool setupDone = false;
    // contest mode
    bool contestOn = false;
    dect2::ham::ContestSession contest;
    // the operator's macros (free-text messages and keyboard texts, prefs "ham.macros", one per line)
    std::vector<std::string> macros;
    // requests between the screens and the keyboard shortcuts
    int wantTab = -1;
    bool openWizard = false, focusSearch = false, openAdd = false;
    std::string selCall, selGrid;            // the station picked in the decode list / spots / log (shown on the map)
    std::string dxCall, dxGrid;              // the station of the running QSO
    std::string toast;                       // a short note at the top ("Logged K1ABC"), shown for a few seconds
    double toastAt = 0;
};
HamOps& hamOps();
void hamOpsLoad();                                           // once: the log, the country file, the settings
// a finished QSO into the logbook (DXCC filled in, contest exchange when contest mode is on), saved at once; false + err when it was not saved
bool hamLogQso(dect2::ham::LogQso q, std::string& err);
dect2::ham::LogQso hamFromRecord(const dect2::ham::QsoRecord& r);
std::string hamDxccKey(const std::string& call);            // the award key of a call's entity, "" when not known
std::string hamCountry(const std::string& call);            // the entity name, "" when not known
std::string hamCountryTag(const std::string& call);         // "Germany, CQ 14, ITU 28", "" when not known
bool hamCountryInMessages();                                 // the setting: show it beside decoded messages
bool hamCountryLoadFile(const std::string& path, std::string& err);   // cty.dat / wl_cty.dat; the old table stays when it fails
void hamCountryReset();                                      // back to the built-in table
void hamCountryUi();                                         // the "Country file" settings block (Load, Download on click, Reset)
void hamToast(const std::string& s);
void hamLogChanged();                                        // the book changed outside the Log tab: its filter and the worked marks are redone
void hamSaveStation();

void hamLogTab(App& a);
void hamAwardsTab(App& a);
void hamContestTab(App& a);
void hamMapTab(App& a);
void hamSpotsTab(App& a, const std::function<void(const std::string& call, const std::string& grid)>& callIt);
void hamGuideTab(App& a);
// the setup wizard (a modal popup, opened by hamOps().openWizard): the callsign and grid buffers and the licence flag belong to ham_ui.cpp
void hamSetupWizard(App& a, char* call, size_t callN, char* grid, size_t gridN, bool& licence, const std::function<void()>& rigWidgets,
                    const std::function<void()>& saveStation);
void hamShortcutsHelp();                                     // the shortcut table (guide and tooltip)
