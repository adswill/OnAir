// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Memory panel, the receiver side: what a Memory captures from a receiver (App) and how a recall applies it. The model, files and the recall order
// are in core (dect2/channels/memory_bank.h); this folder holds the App-specific parts and the panel.
#pragma once
#include "../app.h"
#include "dect2/channels/memory_bank.h"

// A mode option a memory keeps as key=value. A mode adds its own with MemoryOptionReg (at namespace scope in its own file, so nothing else changes):
//   static MemoryOptionReg reg{{"fmMono", [](const App& a) { return a.fmMode; }, [](const App& a) { return std::string(a.fmMono ? "1" : "0"); },
//                               [](App& a, const std::string& v) { a.fmMono = v == "1"; }}};
// applies(): the option exists in the receiver's current mode; get() reads it; set() puts it back the way the mode's own control would (and, for a
// running receiver, must reach it: most modes push their App fields every frame, a few need a restart call such as setDtmbBandwidth).
struct MemoryOption {
    const char* key;
    bool (*applies)(const App&);
    std::string (*get)(const App&);
    void (*set)(App&, const std::string&);
};
const std::vector<MemoryOption>& memoryOptions();
struct MemoryOptionReg { explicit MemoryOptionReg(MemoryOption o); };

// the mode key a memory stores (the Scanner's: protocol id, mode id or dvb / atsc / ... / fm) and its index in kModes (-1: not built in)
std::string memoryModeKey(const ModeDef& m);
int memoryModeIndex(const std::string& key);

dect2::Memory memoryCapture(const App& a);                      // the receiver as it is now (name left for the caller)
std::string memorySuggestName(const App& a);                    // the station name when the mode knows one, else the mode and frequency
// Applies a memory to a receiver. withMode false: the frequency only. A running receiver is retuned (and restarted when the mode or the sample rate
// changes); a stopped one is set up for its next start. false with note set when nothing was done (mode not built, a scan holds the radio ...).
bool memoryRecallApp(App& a, const dect2::Memory& m, bool withMode, std::string& note);

// the bank every panel shares, loaded on first use from <data folder>/memories.json; memoryChanged() saves it
dect2::MemoryBank& memoryBank();
void memoryChanged();
const std::string& memoryFilePath();
void memoryTestSetPath(const std::string& path);                // tests and --shot runs: another file, bank reloaded
