// The General receiver screen of 4.0 (general_screen.cpp, docs/GENERAL-4.0.md): the pieces general_ui.cpp draws into it, and the hooks of the
// shared spectrum (plots.cpp) and top bar (ui2_shell.cpp). The layout state and the maths under it are in general_layout.h.
#pragma once
#include "imgui.h"
#include <string>

struct App;

bool generalScreenActive(const App& a);          // general_screen.cpp: the 4.0 screen is the one on screen (the General family, the new interface)
void generalScreen(App& a, float bodyH, ImVec2 disp);   // general_screen.cpp: the whole body: the sidebar, the bar, the spectrum and waterfall, the drawer

// general_ui.cpp: the sidebar sections (one narrow column)
void generalSideRadio(App& a);       // the demodulator grid, Auto, the bandwidth, the squelch, AGC / gain, de-emphasis, CW pitch, AM, blanker
void generalSideAudio(App& a);       // audio filters and the noise remover (the screen draws the volume above)
void generalSideRecording(App& a);
void generalSideBookmarks(App& a);   // memories, frequencies, the ham voice segments, the broadcast bands
void generalSideSignals(App& a);     // "What is this signal?" and the signals spotted automatically
void generalSideChannels(App& a);    // more channels from the same capture
void generalBar(App& a);             // the thin bar over the spectrum: bandwidth, step, snap, centre, the channel level with its SNR, squelch
float generalDemodChip(App& a);      // the demodulator as a small button (the top bar, next to the frequency): a click opens the same choices; its right edge
void generalShortcuts(App& a);       // Alt + a letter picks a demodulator (docs/GENERAL-4.0.md); called once a frame by the screen
const char* generalShortcutHelp();   // the list, for the "?" tooltips

// the shared spectrum (plots.cpp)
void generalVfoWheel(App& a, ImVec2 plotPos, ImVec2 plotSize, double viewLoMhz, double viewHiMhz);   // before BeginPlot: the wheel over the VFO steps it
bool generalVfoMenuTaken();          // the right click of this frame opened the VFO menu: the spectrum's own menu stays shut
void generalVfoMenu(App& a);         // after the plots, once a frame: that menu (the demodulators and the filter presets)

// app/specx/general_auto_ui.cpp: the list of the spotted signals (the corner popup's body), for the sidebar
void generalAutoListBody(App& a);
// app/bandplan/bandplan_ui.cpp: the service of the active band plan's band at hz ("fm", "am", "air", "ham" ...) and its channel width; false: none
bool bandPlanServiceAt(const App& a, double hz, std::string& service, double& widthHz, std::string& bandName);
