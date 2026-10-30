// Configurable keys, mouse buttons and mouse wheel for the spectrum and waterfall (View > Controls). The tables, the defaults, the saved
// text, the conflict check and the wheel rule are plain code (no ImGui calls, only its key numbers): tested in tests/app/test_input_map.cpp.
// input_map.cpp polls the keys and draws the Controls window. The defaults are what the program always did, so nothing changes until
// somebody opens the window.
#pragma once
#include "imgui.h"
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace inputmap {

enum Mods : uint8_t { M_Shift = 1, M_Ctrl = 2, M_Alt = 4, M_Super = 8 };

enum class Action : uint8_t {
    StartStop,
    Mode1, Mode2, Mode3, Mode4, Mode5, Mode6, Mode7, Mode8, Mode9,   // keep together: Mode1 + k
    DbUp, DbDown, DbWider, DbNarrower, DbReset,                       // the spectrum dB range (only while the spectrum is hovered)
    PageUp, PageDown,                                                 // the view steps by its own width
    TuneUp, TuneDown,                                                 // tune by the step
    ZoomIn, ZoomOut, ZoomReset,                                       // the frequency view
    PanLeft, PanRight,
    StepNext, StepPrev,                                               // the tuning step
    WfPause, Mute,
    Count
};
constexpr int kActions = (int)Action::Count;

// One binding: nothing, a key (ImGuiKey) or a mouse button (ImGuiMouseButton: middle 2, back 3, forward 4), with modifiers.
struct Binding {
    uint8_t kind = 0;   // 0 none, 1 key, 2 mouse button
    int16_t code = 0;
    uint8_t mods = 0;
    bool bound() const { return kind == 2 || (kind == 1 && code != 0); }   // (mouse button 0 is the left button)
    bool operator==(const Binding& o) const { return kind == o.kind && code == o.code && mods == o.mods; }
    bool operator!=(const Binding& o) const { return !(*this == o); }
};
inline Binding key(ImGuiKey k, uint8_t m = 0) { Binding b; b.kind = 1; b.code = (int16_t)k; b.mods = m; return b; }
inline Binding mouse(int btn, uint8_t m = 0) { Binding b; b.kind = 2; b.code = (int16_t)btn; b.mods = m; return b; }

struct Def {
    Action id;
    const char* id_s;    // the saved name
    const char* label;   // English; translated when drawn
    const char* group;
    Binding def;
    bool plotOnly;       // fires only while the mouse is over the spectrum or waterfall
};

inline const Def* defs() {
    static const Def d[] = {
        {Action::StartStop, "start", "Start or stop the receiver", "Receiver", key(ImGuiKey_Space), false},
        {Action::Mode1, "mode1", "Mode 1 of the list", "Modes", key(ImGuiKey_1), false}, {Action::Mode2, "mode2", "Mode 2 of the list", "Modes", key(ImGuiKey_2), false},
        {Action::Mode3, "mode3", "Mode 3 of the list", "Modes", key(ImGuiKey_3), false}, {Action::Mode4, "mode4", "Mode 4 of the list", "Modes", key(ImGuiKey_4), false},
        {Action::Mode5, "mode5", "Mode 5 of the list", "Modes", key(ImGuiKey_5), false}, {Action::Mode6, "mode6", "Mode 6 of the list", "Modes", key(ImGuiKey_6), false},
        {Action::Mode7, "mode7", "Mode 7 of the list", "Modes", key(ImGuiKey_7), false}, {Action::Mode8, "mode8", "Mode 8 of the list", "Modes", key(ImGuiKey_8), false},
        {Action::Mode9, "mode9", "Mode 9 of the list", "Modes", key(ImGuiKey_9), false},
        {Action::DbUp, "dbup", "Spectrum range up 5 dB", "Spectrum", key(ImGuiKey_UpArrow, M_Ctrl), true},
        {Action::DbDown, "dbdown", "Spectrum range down 5 dB", "Spectrum", key(ImGuiKey_DownArrow, M_Ctrl), true},
        {Action::DbWider, "dbwide", "Spectrum range wider", "Spectrum", key(ImGuiKey_UpArrow, M_Ctrl | M_Shift), true},
        {Action::DbNarrower, "dbnarrow", "Spectrum range narrower", "Spectrum", key(ImGuiKey_DownArrow, M_Ctrl | M_Shift), true},
        {Action::DbReset, "dbreset", "Spectrum range back to the default", "Spectrum", key(ImGuiKey_Home, M_Ctrl), true},
        {Action::PageUp, "pgup", "View one width up in frequency", "View", key(ImGuiKey_PageUp, M_Ctrl), false},
        {Action::PageDown, "pgdown", "View one width down in frequency", "View", key(ImGuiKey_PageDown, M_Ctrl), false},
        {Action::TuneUp, "tuneup", "Tune up by the step", "Tuning", key(ImGuiKey_RightArrow), true},
        {Action::TuneDown, "tunedown", "Tune down by the step", "Tuning", key(ImGuiKey_LeftArrow), true},
        {Action::ZoomIn, "zoomin", "Zoom in (frequency)", "View", Binding(), true},
        {Action::ZoomOut, "zoomout", "Zoom out (frequency)", "View", Binding(), true},
        {Action::ZoomReset, "zoomreset", "Show the whole band", "View", Binding(), true},
        {Action::PanLeft, "panleft", "Pan the view left", "View", Binding(), true},
        {Action::PanRight, "panright", "Pan the view right", "View", Binding(), true},
        {Action::StepNext, "stepnext", "Larger tuning step", "Tuning", key(ImGuiKey_UpArrow), true},
        {Action::StepPrev, "stepprev", "Smaller tuning step", "Tuning", key(ImGuiKey_DownArrow), true},
        {Action::WfPause, "wfpause", "Pause or resume the waterfall", "View", Binding(), true},
        {Action::Mute, "mute", "Mute or unmute the sound", "Receiver", Binding(), false},
    };
    return d;
}
inline const Def& def(Action a) { return defs()[(int)a]; }

// ------------------------------------------------------------------ the wheel

enum class Area : uint8_t { Spectrum, Waterfall, Count };
enum class Wheel : uint8_t {
    ZoomBoth,    // frequency and level (on the waterfall: frequency, and the time when paused): what the program always did
    ZoomFreq,    // frequency only
    ZoomLevel,   // level only (on the waterfall: the time axis, when paused)
    Tune,        // tune up or down by the step
    PanFreq,     // slide the view along the frequency axis
    ScrollTime,  // waterfall: look back through the rows (pauses the waterfall, the picture then scrolls)
    Nothing,
    Count
};
// The modifier combinations the wheel table has a row for
enum Combo : uint8_t { C_None, C_Shift, C_Ctrl, C_Alt, C_CtrlShift, kCombos };
inline const char* comboName(int c) {
    static const char* n[kCombos] = {"Wheel", "Shift + wheel", "Ctrl + wheel", "Alt + wheel", "Ctrl + Shift + wheel"};
    return n[c];
}
inline int comboOf(bool shift, bool ctrl, bool alt) {
    if (alt) return C_Alt;
    if (ctrl && shift) return C_CtrlShift;
    if (ctrl) return C_Ctrl;
    if (shift) return C_Shift;
    return C_None;
}
inline const char* wheelName(Wheel w) {
    switch (w) {
        case Wheel::ZoomBoth: return "Zoom (frequency and level)";
        case Wheel::ZoomFreq: return "Zoom the frequency only";
        case Wheel::ZoomLevel: return "Zoom the level / time only";
        case Wheel::Tune: return "Tune by the step";
        case Wheel::PanFreq: return "Pan along the frequency";
        case Wheel::ScrollTime: return "Scroll the waterfall back in time";
        case Wheel::Nothing: return "Nothing";
        default: return "";
    }
}
// What the wheel did before this window existed: both axes; Ctrl the frequency only; Alt both. With Shift held the plots took no wheel at all
// (Shift+drag selects a signal, and the plot treated the whole mouse as taken), so Shift and Ctrl+Shift were "nothing".
inline Wheel wheelDefault(Area, int combo) {
    switch (combo) {
        case C_Ctrl: return Wheel::ZoomFreq;
        case C_Shift: case C_CtrlShift: return Wheel::Nothing;
        default: return Wheel::ZoomBoth;
    }
}
// Which axes a zoom action moves (the plot code asks): bit 0 frequency, bit 1 level / time
inline int zoomAxes(Wheel w) { return w == Wheel::ZoomBoth ? 3 : w == Wheel::ZoomFreq ? 1 : w == Wheel::ZoomLevel ? 2 : 0; }

// The tuning steps the Step action and the wheel use (Hz). 0 in the settings: the mode's own step (the click snap)
inline const std::vector<int>& stepList() {
    static const std::vector<int> s = {10, 100, 500, 1000, 5000, 6250, 8330, 10000, 12500, 25000, 100000, 200000, 1000000};
    return s;
}

struct Settings {
    Binding bind[kActions];
    uint8_t wheel[(int)Area::Count][kCombos] = {};   // Wheel per area and modifier combination
    int stepHz = 0;                                  // 0: the mode's own step
    int wheelNotch = 1;                              // notches of the wheel per step when it tunes (1..5)
    Settings() { reset(); }
    void reset() {
        for (int i = 0; i < kActions; i++) bind[i] = defs()[i].def;
        for (int a = 0; a < (int)Area::Count; a++) for (int c = 0; c < kCombos; c++) wheel[a][c] = (uint8_t)wheelDefault((Area)a, c);
        stepHz = 0; wheelNotch = 1;
    }
    Wheel wheelFor(Area a, int combo) const { return (Wheel)wheel[(int)a][combo]; }
    bool wheelIsDefault() const {
        for (int a = 0; a < (int)Area::Count; a++) for (int c = 0; c < kCombos; c++) if (wheel[a][c] != (uint8_t)wheelDefault((Area)a, c)) return false;
        return true;
    }
    bool isDefault() const {
        Settings d;
        for (int i = 0; i < kActions; i++) if (bind[i] != d.bind[i]) return false;
        for (int a = 0; a < (int)Area::Count; a++) for (int c = 0; c < kCombos; c++) if (wheel[a][c] != d.wheel[a][c]) return false;
        return stepHz == d.stepHz && wheelNotch == d.wheelNotch;
    }
};

// ------------------------------------------------------------------ names

inline std::string keyName(int code) {
    const char* n = ImGui::GetKeyName((ImGuiKey)code);
    return n && *n ? n : "?";
}
inline std::string bindingText(const Binding& b) {
    if (!b.bound()) return "";
    std::string s;
    if (b.mods & M_Ctrl) s += "Ctrl+";
    if (b.mods & M_Alt) s += "Alt+";
    if (b.mods & M_Shift) s += "Shift+";
    if (b.mods & M_Super) s += "Super+";
    if (b.kind == 2) {
        static const char* mb[] = {"Left button", "Right button", "Middle button", "Back button", "Forward button"};
        s += b.code >= 0 && b.code < 5 ? mb[b.code] : "Button";
    } else s += keyName(b.code);
    return s;
}

// ------------------------------------------------------------------ conflicts

// The other actions that use the same binding (both would fire). Actions that need the mouse over the plots do not clash with the ones
// that do not only when the keys differ, so the check is on the binding alone, but a plot-only action cannot clash with a mouse button
// binding of the same button on a different scope: both are plot-only or both are not for it to count.
inline std::vector<int> conflictsWith(const Settings& s, int idx) {
    std::vector<int> out;
    if (!s.bind[idx].bound()) return out;
    for (int i = 0; i < kActions; i++)
        if (i != idx && s.bind[i] == s.bind[idx]) out.push_back(i);
    return out;
}
inline bool anyConflict(const Settings& s) {
    for (int i = 0; i < kActions; i++) if (!conflictsWith(s, i).empty()) return true;
    return false;
}
// Bindings that other parts of the program take for themselves: refused with a reason (nullptr: free)
inline const char* reservedReason(const Binding& b) {
    if (!b.bound()) return nullptr;
    if (b.kind == 2 && b.code <= 1 && b.mods == 0) return "The left and right mouse buttons are click-to-tune and the menu";
    if (b.kind == 1) {
        if (b.code == ImGuiKey_Escape) return "Esc closes dialogs";
        if (b.code == ImGuiKey_K && (b.mods & M_Ctrl) && !(b.mods & ~M_Ctrl)) return "Ctrl+K is the command palette";
        if (b.code == ImGuiKey_L && (b.mods & M_Ctrl) && (b.mods & M_Shift)) return "Ctrl+Shift+L switches the layout";
        if (b.code == ImGuiKey_D && (b.mods & M_Ctrl) && (b.mods & M_Alt)) return "Ctrl+Alt+D is the dock";
        if (b.code >= ImGuiKey_LeftCtrl && b.code <= ImGuiKey_RightSuper) return "A modifier alone cannot be a key";
    }
    return nullptr;
}

// ------------------------------------------------------------------ saved text

// Only what differs from the defaults is written: "id=k:code:mods;..." for the keys, and the wheel rows as digits.
inline std::string saveBindings(const Settings& s) {
    const Settings d;
    std::string o;
    char b[48];
    for (int i = 0; i < kActions; i++) {
        if (s.bind[i] == d.bind[i]) continue;
        snprintf(b, sizeof b, "%s=%d:%d:%d;", defs()[i].id_s, (int)s.bind[i].kind, (int)s.bind[i].code, (int)s.bind[i].mods);
        o += b;
    }
    return o;
}
inline void loadBindings(Settings& s, const std::string& t) {
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find(';', p);
        if (e == std::string::npos) e = t.size();
        const std::string item = t.substr(p, e - p);
        p = e + 1;
        const size_t q = item.find('=');
        if (q == std::string::npos) continue;
        const std::string id = item.substr(0, q);
        int k = 0, c = 0, m = 0;
        if (sscanf(item.c_str() + q + 1, "%d:%d:%d", &k, &c, &m) != 3) continue;
        if (k < 0 || k > 2 || m < 0 || m > 15) continue;
        if (k == 1 && (c <= ImGuiKey_None || c >= ImGuiKey_NamedKey_END)) continue;
        if (k == 2 && (c < 0 || c > 4)) continue;
        for (int i = 0; i < kActions; i++) if (id == defs()[i].id_s) {
            Binding b; b.kind = (uint8_t)k; b.code = (int16_t)c; b.mods = (uint8_t)m;
            if (k == 0) b = Binding();
            if (!reservedReason(b)) s.bind[i] = b;
            break;
        }
    }
}
inline std::string saveWheel(const Settings& s, Area a) {
    std::string o;
    for (int c = 0; c < kCombos; c++) o += (char)('0' + s.wheel[(int)a][c]);
    return o;
}
inline void loadWheel(Settings& s, Area a, const std::string& t) {
    if ((int)t.size() != kCombos) return;
    for (int c = 0; c < kCombos; c++) {
        const int v = t[c] - '0';
        if (v >= 0 && v < (int)Wheel::Count) s.wheel[(int)a][c] = (uint8_t)v;
    }
}

// ------------------------------------------------------------------ helpers the callers share

// Wheel notches to whole steps: a mouse wheel notch is 1.0, a trackpad sends many small values that add up. acc keeps the rest.
inline int wheelSteps(float& acc, float wheel, int notchPerStep) {
    acc += wheel;
    const float per = (float)(notchPerStep < 1 ? 1 : notchPerStep);
    int n = 0;
    while (acc >= per) { acc -= per; n++; }
    while (acc <= -per) { acc += per; n--; }
    return n;
}
// A tuning step (Hz) from the settings, or the mode's own (ownHz) when the setting is 0; never below 1 Hz
inline double stepHzFor(const Settings& s, double ownHz) {
    const double v = s.stepHz > 0 ? s.stepHz : ownHz;
    return v >= 1 ? v : 1000.0;
}
// The next or previous entry of stepList() from the current value (0 = the mode's own: next goes to the smallest, previous to the largest)
inline int stepShift(int cur, int dir) {
    const std::vector<int>& l = stepList();
    if (cur <= 0) return dir > 0 ? l.front() : l.back();
    int i = 0;
    while (i + 1 < (int)l.size() && l[i + 1] <= cur) i++;
    if (l[i] < cur) return dir > 0 ? l[std::min((int)l.size() - 1, i + 1)] : l[i];   // between two entries: the next above or the one below
    i += dir;
    if (i < 0) i = 0;
    if (i >= (int)l.size()) i = (int)l.size() - 1;
    return l[i];
}

// ------------------------------------------------------------------ app side (input_map.cpp)

Settings& settings();                         // the live settings (loaded from the saved preferences on first use)
void save();                                  // write them to the preferences
void reload();                                // read them again (a restored backup)
bool& windowOpen();                           // View > Controls
void window();                                // the Controls window, once a frame
// True in the frame the action's key (or mouse button) was pressed, with exactly its modifiers. Not while typing in a text field.
// repeat: a held key repeats (the arrow keys of the range). Actions that are plotOnly are asked with the plots hovered by the caller.
bool pressed(Action a, bool repeat = false);

}  // namespace inputmap
