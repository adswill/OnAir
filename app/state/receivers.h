// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Several receivers at once (docs/multi-sdr.md): RX 1 is the App main() makes, RX 2, 3 ... are Apps made on demand, each with its own engine,
// radio, mode, frequency, gains and results. One receiver's tab is drawn; all of them keep receiving and ticking.
#pragma once
#include "../app.h"

constexpr int kMaxReceivers = 4;

void receiversInit(App& first);                   // RX 1 is `first`; also restores the receivers that were open at the last exit
App* receiverById(int id);                        // nullptr when closed
App* receiverHeard();                             // the receiver whose sound plays (Hear this receiver); never null while RX 1 is open
void receiverHear(int id);                        // solo: this receiver's sound plays, the others are muted
std::vector<App*> receiversAll();                 // open receivers, RX 1 first
App* addReceiver();                               // a new receiver (nullptr: no more room); it becomes the selected tab
void closeReceiver(int id);                       // stops it and lets go of its radio (done at the start of the next frame)
void receiversFrame(App& first, ImVec2 disp);     // every frame: ticks all receivers, draws the selected one and the tab strip
void receiversShutdown(App& first);
bool receiverModeBusy(const App& a, std::string* why);   // another receiver runs this mode: its screen state is not separated yet
extern float gCpuPct;                             // the CPU meter of the status bar (process, 100 = one core), 0 until measured
