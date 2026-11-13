// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The keyboard-mode half of the Ham view (PSK31 / PSK63, RTTY, CW, Olivia, Contestia, MFSK, THOR, DominoEX, Hellschreiber): received text, the type-ahead
// transmit buffer, macros, callsign pick, QSO log. The logic is in core/include/dect2/ham/ham_kbd.h (tested); the station, the licence box, Arm and the
// transmitter belong to ham_ui.cpp and are handed in here, so there is one gate and one Arm for every Ham mode.
#pragma once
#include "../app.h"
#include "dect2/ham/ham.h"
#include "dect2/ham/ham_kbd.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct HamKbdCtx {
    App& a;
    const char* call;                 // my callsign and grid, already validated by the caller
    const char* grid;
    bool stationOk, licence;
    bool& armed;
    float level;
    ham::HamSlotTx& tx;
    std::thread& worker;
    std::atomic<int>& txState;        // 0 none, 1 running, 2 sent, 3 failed (shared with the slot modes: one transmitter)
    std::mutex& mu;
    std::string& txErr;
    std::string& status;
    ham::WorkedLog& worked;
    std::function<void()> rigWidgets; // PTT rig, audio output and level
    std::function<void(const ham::QsoRecord&)> logQso;
};

// preset: index into ham::kbdPresets(). Called every frame while a keyboard mode is chosen.
void hamKbdPanel(HamKbdCtx& c, int preset);
// The mode was just chosen: tune the receiver for it.
void hamKbdSelect(App& a, int preset);
