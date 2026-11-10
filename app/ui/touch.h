// The touch interface (Settings > Touch interface, --touch): App::touchUi switches the existing screens to big targets and
// gives them a thumb-friendly shell (touch_shell.cpp). It is a theme / layout mode, not a second set of panels: the widgets
// the screens already use pick up the metrics below, and the few touch-only pieces (keypad, mode picker, sheets) live in
// touch_sheets.cpp. Off, nothing here changes how the program looks or works.
//   touch.cpp          metrics (>= 48 px targets), per-frame gestures, long press = right click, tooltips on hold, kinetic scroll,
//                      bottom sheets and full-screen windows (an ImGui hook, see imgui.h)
//   touch_sheets.cpp   the numeric keypad, the step picker, the mode picker, the menu sheet
//   touch_shell.cpp    the shell: top bar, body, bottom dock
//   touch_logic.h      the pure parts (keypad entry, gesture recogniser, kinetic scroll), unit tested
#pragma once
#include "imgui.h"
#include "touch_logic.h"
#include <string>

struct App;

namespace touch {

bool on();                       // the touch interface is active
void setOn(bool v);              // main.cpp, every frame from App::touchUi: installs or removes the ImGui hooks
void setDisplayScale(float dpi); // main.cpp: the display scale without the text scales (48 px targets are 48 * this)
float dpi();
inline float target() { return 48.f * dpi(); }   // the smallest hit target, in pixels

// Raise the style's paddings, spacings, scrollbar and grab sizes to touch size. Only ever raises, so repeating it every frame is harmless.
void applyMetrics(ImGuiStyle& st);

// Once per frame at the start of the UI: feeds the recogniser from the pointer (and from feedPointer() where a backend has multi-touch),
// turns a long press into a right click, and decides whether tooltips may show.
void frame();
// Once per frame at the end: remembers whether the press landed on a widget (a swipe only counts on the background).
void endFrame();
const GestureFrame& gesture();
// Drag to tune (gfx/drag_tune.h): a plot claims its rectangle each frame; a long press inside it is then held for tuning (longPressHeld)
// instead of becoming a right click, and the plot opens its own menu when the finger lets go without moving.
void claimLongPress(ImVec2 mn, ImVec2 mx);
bool longPressHeld();
bool pressedOnItem();            // the current press started on a widget (slider, plot, button): not a scroll or swipe
// A backend with real multi-touch calls this for each finger (id, position, down) before frame(); without it one pointer is used.
void feedPointer(int id, float x, float y, bool down);

void noteTab(const char* name, bool open);   // tabItem() tells which tabs exist and which is open (swipe between tabs)
void swipeTabs(int dir);                     // select the next / previous tab of the last frame (dir -1 / +1)

// A button at least as big as a target, optionally filled with the accent (the primary action of a row).
bool bigButton(const char* label, ImVec2 size = ImVec2(0, 0), bool primary = false);

// ---- sheets and pickers (touch_sheets.cpp)
void openKeypad(double hz);                            // the numeric keypad for a frequency; the result goes to tuneFreq()
void keypadSheet(App& a);                              // draw it (every frame; a no-op while closed)
void openStepPicker();
void stepSheet(App& a);
void openModePicker();
void modePickerSheet(App& a, ImVec2 disp);
void menuSheet(App& a, ImVec2 disp);
void openMenu();
void devOpen(const char* spec);                        // --touchsheet <name>[:keys] (dev, with --shot)
extern std::string gDevOpen;
void showSystemKeyboard();                             // the OS on-screen keyboard where there is one (Windows, a Linux touch PC)
bool keyboardAvailable();
double& stepHz();                                      // the tuning step of the + and - buttons
// A modal bottom sheet. Returns true while open (call EndSheet after). Sheets are ordinary popups: the window hook seats them.
bool beginSheet(const char* id, const char* title, bool fullScreen = false);
void endSheet();
void closeSheet();

// ---- the shell (touch_shell.cpp)
void shell(App& a, ImVec2 disp);

}   // namespace touch
