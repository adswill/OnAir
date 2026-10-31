// Workspaces: a named set of the current source, mode, frequency, sample rate, gains and open windows, saved in the settings
// (prefs key "workspaces") and restored from the File menu. Also the last-session file (dataDir()/last-session.txt): written every
// few seconds while OnAir runs and marked clean at a normal exit, so the next start can offer to restore a session that did not end.
// No DSP here: a restore goes through the same selectMode / tuneFreq / applyBandwidth that the controls use.
#pragma once
#include "../app.h"
#include <string>
#include <vector>

struct Workspace {
    std::string name;
    std::string devName;      // the radio's name, matched again at the start; empty when none
    int family = 0;
    std::string subId;        // the protocol entry's id, empty for none
    double freqMhz = 0;
    double sampleRateHz = 0;  // 0 = the mode's own rate
    int lnaDb = 0, vgaDb = 0;
    bool ampOn = false;
    std::string panels;       // comma-separated names of the open windows
    std::string modes;        // the Modes shown rules (modevis text, or "default"); empty in a workspace saved before 3.5: left as they are
};

Workspace workspaceCapture(const App& a, const std::string& name);    // the live settings of this receiver
bool workspaceApply(App& a, const Workspace& w, std::string& why);      // false (and why) when the receiver is running or the mode refuses

std::vector<Workspace> workspaceList();                  // the saved workspaces, in the order they were saved
void workspaceStore(const std::vector<Workspace>& v);    // writes them to the settings

// the last-session file (crash restore)
void workspaceSessionStart(const App& a, bool offerRestore);  // at start-up; a previous session that did not end is kept for the dialog
void workspaceSessionTick(const App& a);                      // every frame: rewrites the file every few seconds while running
void workspaceSessionEnd(const App& a);                       // at a normal exit: the file is marked clean
bool workspaceRestorePending();                               // the dialog is still to be answered
const Workspace& workspaceRecovered();                        // the session that did not end
void workspaceRestoreAnswer(App& a, bool restore, std::string& why);   // Restore (false: Start fresh)

// the File menu entries and the two dialogs (save name, restore last session); called from the main menu bar every frame
void workspaceMenu(App& a);
void workspaceDialogs(App& a);
