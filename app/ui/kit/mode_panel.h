// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Per-mode Simple / Advanced view (docs/SIMPLE-ADVANCED.md). Simple: the spectrum and ONE merged "Command" view under it (a summary strip,
// the meters, the mode's key controls and its live data side by side). Advanced: the mode's tabs as before. The choice is saved per mode.
#pragma once
#include <functional>
#include <initializer_list>
struct App;
struct ModeUi;

namespace modepanel {

void startup(bool freshProfile);        // main_start.cpp, once: a profile with no settings yet gets Simple as its default, an existing one Advanced
// Easy mode: every mode opens in its Simple view (the spectrum and one Command view with the Start button). On: the default of the profile and every
// mode choice saved so far become Simple; off: Advanced. Used by the setup and by Settings > Easy mode. easyModeOn() is the profile default.
void setEasyMode(bool on);
bool easyModeOn();
bool available(const App& a);           // the mode on screen has a Simple view (a mode with its own screens, not marked noSimple)
bool simple(const App& a);              // available and the user chose Simple for this mode
void setSimple(App& a, bool simple);    // saves the choice for this mode
int commandPane();                      // while a Command view pane draws the mode's tab: 0 its Live view, 1 its Map view (kit::modeNav picks the view, kit::modeHeader is drawn once); -1 outside
void navPicked(int* cur, int view, bool hasMap);   // kit::modeNav in a Command view: set *cur to view for this frame (the user's choice comes back after the view); hasMap: the mode has a Map view
bool inCommandView();                   // true while the Command view is being drawn (kit::modeNav then shows no pills and keeps the first view)

// "Simple | Advanced" (a small switch) for every mode that has a Simple view. 4.0: drawn at the right end of the one action row above the
// tabs (modeActionBar); rightAlign false draws it at the cursor. levelBarWidth() is its width, for the row's layout.
void levelBar(App& a, bool rightAlign = true);
float levelBarWidth();

// The Command view of the mode (draws the strip, then mu.command or the generic body). Called by mainTabs() instead of the tab bar in Simple.
void commandView(App& a, const ModeUi& mu);

// ---- layout helper for a mode's command(): panes side by side when they fit, stacked when they do not. A pane is a titled, scrolling child.
struct Pane {
    const char* id;                        // unique in the view; also the title when title is null
    const char* title;                     // already translated; null: no title line
    std::function<void()> draw;
    float weight = 1.f;                    // relative width in a row
    float minHeight = 180.f;               // when stacked (in unscaled pixels)
};
void panes(std::initializer_list<Pane> list, float minPaneWidth = 330.f);

}  // namespace modepanel
