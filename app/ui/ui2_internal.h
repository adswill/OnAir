// The shared declarations of the new interface (ui2*.cpp): the theme, the small widgets, the shell (menu bar, top and
// bottom bars, side bar) and the body layouts. Everything lives in namespace ui2 except the entry points in app.h.
// Included by ui2.cpp, ui2_theme.cpp, ui2_widgets.cpp, ui2_shell.cpp and ui2_faces.cpp only.
#pragma once
#include "touch.h"
#include "kit/kit.h"
#include "../app.h"
#include "typography.h"
#include "imgui_internal.h"   // RenderArrow for the folding mode groups

namespace ui2 {

// Palettes. 0: "terminal" (warm greys, amber, lower-case labels, key hints, log line); 1: "instrument" (mid greys, yellow trace, plain labels);
// 2: "mono" (neutral greys, white trace, no accent at all, plain labels).
struct UiTheme {
    ImVec4 bg, rail, edge, text, dim, bright, accent, frame, button, plotBg, series[8];
    ImU32 tone[5];      // what the classic interface's blues become: bright fill, dark fill, mid fill, soft icon, bright icon
    bool flair;         // lower-case labels, key hints and a log line
};
extern UiTheme T;
extern ImVec2 gViewMenuCentre;
}  // namespace ui2
#define kBg T.bg
#define kRail T.rail
#define kEdge T.edge
#define kText T.text
#define kDim T.dim
#define kBright T.bright
#define kAccent T.accent

namespace ui2 {

// ui2_theme.cpp
void accentMenu();
void themeChoice(App& a, bool menu);
void typographyChoice(bool menu);
void dataFolderChoice();
void timeDisplayChoice(bool menu);

// ui2_widgets.cpp
void autoRange(App& a);
ImU32 u32(ImVec4 c);
void beginPane(const char* id, ImVec2 size, bool autoH = false);
void endPane();
extern bool gFreqHovered;
float freqDigits(App& a, float fontSize = 30.f);
float freqDigitsFor(App& a, double& mhz, const std::function<void(double)>& tune, float fontSize = 30.f);   // the readout for another frequency (TX)
bool runButton(bool running, float size, const char* tip = nullptr);
bool section(const char* title, bool openByDefault = true);
void levelMeter(App& a, float x, float y, float w);
void volumeControl(App& a, float w);
bool splitter(const char* id, bool vertical, float thick, float len, float& val, float curPx, float minPx, float maxPx, float sign, App& a);
float clampPx(float v, float lo, float hi);
void beginTile(const char* id, const char* title, ImVec2 size);
extern const ImVec4 kChassis, kSilk;
void beginModule(const char* id, const char* label, ImVec2 size);
void endModule();
void chip(const char* label, bool sel, bool& clicked);

// ui2_shell.cpp
float statusStripBlockHeight();
void summary(App& a, float x, float y, float scale = 1.f, float maxW = FLT_MAX);
void modeList(App& a);
void sidebar(App& a);
float menuBar(App& a);
void topBar(App& a, ImVec2 disp);
void bottomBar(App& a, ImVec2 disp);
void tgRect(App& a, int id);
void logConsole(App& a, float h);
void modeCombo(App& a, float w);
void displayControls(App& a, bool wide);

// ui2.cpp
void ribbonPane(App& a);

// ui2_faces.cpp
void bodyFace(App& a, float bodyH, ImVec2 disp);
void bodyCinema(App& a, float bodyH, ImVec2 disp);
void bodyMeters(App& a, float bodyH, ImVec2 disp);
void bodyDock(App& a, float bodyH, ImVec2 disp);
void bodyPanel(App& a, float bodyH, ImVec2 disp);
extern const ImVec4 kPanelChassis;

// ui2_dock.cpp (View > Docking)
void dockBody(App& a, float bodyH);   // the body with docking on: the status line and the dock space
void dockPanes(App& a);               // the built-in panels as windows, after the root window
void dockMenu(App& a);                // View > Docking

}  // namespace ui2
