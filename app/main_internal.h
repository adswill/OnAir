// Shared by main.cpp, main_window.cpp, main_start.cpp and the main-window layout in ui/ (draw_ui.cpp, main_tabs.cpp):
// the developer-flag state, the few globals and the helpers that were one file before.
// Nothing in it changes behaviour: it only lets main.cpp be smaller.
#pragma once
#include "app.h"

// The developer and test flags (--autostart, --shot, --stress ...): what main() and its frame loop act on.
struct DevOpts {
    bool autostart = false, hackrfStart = false, fileStart = false, stress = false;
    std::string startDevice;   // --device: an index (of --list-devices), a serial, or a part of the name (an exact name still works: select an RSPduo input without relying on enumeration order)
    std::string startAntenna;  // --antenna: the antenna socket of a radio that has several
    int stressFrame = 0, stressStep = 0;
    const char* shotPath = nullptr;
    double shotAfter = 0;      // dev: take the --shot after this many seconds instead of after a number of frames
    bool fmScanAuto = false;   // dev: start the FM band scan as soon as the receiver runs
    double fileVfoMhz = 0;     // dev: --vfo MHz, a recording: put the tune box there once the receiver runs (what a click or drag on the spectrum does)
    int shotFrame = 0;
    bool modeSelfTest = false; int modeFrame = 0;   // dev: --mode-selftest
    bool memSelfTest = false; int memFrame = 0;   // dev: --memory-selftest: apply memories to this receiver, check, exit
};

extern std::string gCrashNote;   // main.cpp: set when the last run crashed or the startup guard acted: shown once in a small window
extern bool gIdentifyDev;        // ui/draw_ui.cpp: --identify
extern bool gWayland;            // main_window.cpp: running on a native Wayland display (Linux)

extern int gWorkAreaW, gWorkAreaH;                                    // main_window.cpp: the primary monitor's usable size in points (0: unknown)
GLFWwindow* createMainWindow(int argc, char** argv);            // main_window.cpp: null when there is no window
void loadFonts(App& app, ImGuiIO& io);                           // main_window.cpp
void startApp(App& app, int argc, char** argv);                               // main_start.cpp: startup guard, first-run dialogs, radios, last results
void parseDevFlags(App& app, int argc, char** argv, DevOpts& o); // main_start.cpp
bool anyRunning();                                               // ui/main_tabs.cpp
void lightenDrawData(ImDrawData* dd, bool light = true, bool highContrast = false);   // ui/light_theme.cpp
