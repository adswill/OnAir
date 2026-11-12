// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Configurable keys, mouse buttons and mouse wheel for the spectrum and waterfall (View > Controls). The tables, the defaults, the saved
// text, the conflict check and the wheel rule are plain code (no ImGui calls, only its key numbers): tested in tests/app/test_input_map.cpp.
// input_map.cpp polls the keys and draws the Controls window. The defaults are what the program always did, so nothing changes until
// somebody opens the window.
#pragma once
#include "imgui.h"
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace inputmap {

enum Mods : uint8_t { M_Shift = 1, M_Ctrl = 2, M_Alt = 4, M_Super = 8 };

#ifndef N_
#define N_(s) s   // marks a text for the translation catalogs (app/i18n); tr() runs when it is shown
#endif

// Every action that has (or can have) a key. The saved name (Def::id_s) is what the preferences keep: never rename one. New actions go at the
// end, before Count, so Mode1 + k and the saved names of older versions stay where they were.
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
    // 4.0: the shortcuts that were written into the code, now one registry (Settings > Keyboard and mouse)
    OpenSettings, CommandPalette, ModePicker, MemoryWin, FreqDbWin, ClipsWin, ControlsWin,
    LayoutToggle, RestoreView, DockToggle, Fullscreen, VideoOnly, IdentifyTool, UnifiedColumn,
    TextBigger, TextSmaller, TextReset, RecordIq,
    TvPause, TvRecord,
    HamGuide, HamWizard, HamLog, HamClearDx, HamMap, HamCq, HamAnswer, HamSpots, HamAddLog, HamFindLog,
    Count
};
constexpr int kActions = (int)Action::Count;

// Where an action listens. Global: anywhere (not while typing). Plot: only while the mouse is over the spectrum or the waterfall.
// The panel scopes listen only while their screen is shown; there a key of the panel wins over a global action with the same key.
enum class Scope : uint8_t { Global, Plot, Tv, Ham, Count };
inline const char* scopeName(Scope s) {
    switch (s) {
        case Scope::Plot: return N_("over the plots");
        case Scope::Tv: return N_("on the TV picture");
        case Scope::Ham: return N_("in the FT8 / FT4 / WSPR screen");
        default: return "";
    }
}
// Two scopes clash when both can fire on the same key press. A panel scope does not clash with Global: the panel claims its keys while shown.
inline bool scopesOverlap(Scope a, Scope b) {
    if (a == b) return true;
    return (a == Scope::Global && b == Scope::Plot) || (a == Scope::Plot && b == Scope::Global);
}

enum DefFlags : uint8_t {
    F_Cmd = 1,       // Ctrl in the binding also takes the Command / Super key (the old "Ctrl or Cmd" shortcuts)
    F_ShiftOk = 2,   // an extra Shift is ignored (Ctrl and + on keyboards where + is Shift and =)
    F_Repeat = 4,    // a held key repeats
    F_Typing = 8,    // fires while a text field has the keyboard too (the command palette's own search field)
};

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
    Binding def;         // the first key
    Scope scope = Scope::Global;
    Binding alt = Binding();   // a second key for the same action (Keypad + next to =, Alt+Enter next to F11)
    uint8_t flags = 0;
    bool plotOnly() const { return scope == Scope::Plot; }
};

inline const Def* defs() {
    using S = Scope;
    static const Def d[] = {
        {Action::StartStop, "start", N_("Start or stop the receiver"), N_("Receiver"), key(ImGuiKey_Space)},
        {Action::Mode1, "mode1", N_("Mode 1 of the list"), N_("Modes"), key(ImGuiKey_1)}, {Action::Mode2, "mode2", N_("Mode 2 of the list"), N_("Modes"), key(ImGuiKey_2)},
        {Action::Mode3, "mode3", N_("Mode 3 of the list"), N_("Modes"), key(ImGuiKey_3)}, {Action::Mode4, "mode4", N_("Mode 4 of the list"), N_("Modes"), key(ImGuiKey_4)},
        {Action::Mode5, "mode5", N_("Mode 5 of the list"), N_("Modes"), key(ImGuiKey_5)}, {Action::Mode6, "mode6", N_("Mode 6 of the list"), N_("Modes"), key(ImGuiKey_6)},
        {Action::Mode7, "mode7", N_("Mode 7 of the list"), N_("Modes"), key(ImGuiKey_7)}, {Action::Mode8, "mode8", N_("Mode 8 of the list"), N_("Modes"), key(ImGuiKey_8)},
        {Action::Mode9, "mode9", N_("Mode 9 of the list"), N_("Modes"), key(ImGuiKey_9)},
        {Action::DbUp, "dbup", N_("Spectrum range up 5 dB"), N_("Spectrum"), key(ImGuiKey_UpArrow, M_Ctrl), S::Plot, Binding(), F_Repeat},
        {Action::DbDown, "dbdown", N_("Spectrum range down 5 dB"), N_("Spectrum"), key(ImGuiKey_DownArrow, M_Ctrl), S::Plot, Binding(), F_Repeat},
        {Action::DbWider, "dbwide", N_("Spectrum range wider"), N_("Spectrum"), key(ImGuiKey_UpArrow, M_Ctrl | M_Shift), S::Plot, Binding(), F_Repeat},
        {Action::DbNarrower, "dbnarrow", N_("Spectrum range narrower"), N_("Spectrum"), key(ImGuiKey_DownArrow, M_Ctrl | M_Shift), S::Plot, Binding(), F_Repeat},
        {Action::DbReset, "dbreset", N_("Spectrum range back to the default"), N_("Spectrum"), key(ImGuiKey_Home, M_Ctrl), S::Plot},
        {Action::PageUp, "pgup", N_("View one width up in frequency"), N_("View"), key(ImGuiKey_PageUp, M_Ctrl), S::Global, Binding(), F_Repeat},
        {Action::PageDown, "pgdown", N_("View one width down in frequency"), N_("View"), key(ImGuiKey_PageDown, M_Ctrl), S::Global, Binding(), F_Repeat},
        {Action::TuneUp, "tuneup", N_("Tune up by the step"), N_("Tuning"), key(ImGuiKey_RightArrow), S::Plot, Binding(), F_Repeat},
        {Action::TuneDown, "tunedown", N_("Tune down by the step"), N_("Tuning"), key(ImGuiKey_LeftArrow), S::Plot, Binding(), F_Repeat},
        {Action::ZoomIn, "zoomin", N_("Zoom in (frequency)"), N_("View"), Binding(), S::Plot, Binding(), F_Repeat},
        {Action::ZoomOut, "zoomout", N_("Zoom out (frequency)"), N_("View"), Binding(), S::Plot, Binding(), F_Repeat},
        {Action::ZoomReset, "zoomreset", N_("Show the whole band"), N_("View"), key(ImGuiKey_Home), S::Plot},
        {Action::PanLeft, "panleft", N_("Pan the view left"), N_("View"), Binding(), S::Plot, Binding(), F_Repeat},
        {Action::PanRight, "panright", N_("Pan the view right"), N_("View"), Binding(), S::Plot, Binding(), F_Repeat},
        {Action::StepNext, "stepnext", N_("Larger tuning step"), N_("Tuning"), key(ImGuiKey_UpArrow), S::Plot},
        {Action::StepPrev, "stepprev", N_("Smaller tuning step"), N_("Tuning"), key(ImGuiKey_DownArrow), S::Plot},
        {Action::WfPause, "wfpause", N_("Pause or resume the waterfall"), N_("View"), Binding(), S::Plot},
        {Action::Mute, "mute", N_("Mute or unmute the sound"), N_("Receiver"), Binding()},
        // 4.0: the defaults are the keys these had in 3.9
        {Action::OpenSettings, "settings", N_("Open or close Settings"), N_("Windows"), key(ImGuiKey_Comma, M_Ctrl), S::Global, Binding(), F_Cmd},
        {Action::CommandPalette, "palette", N_("Command palette"), N_("Windows"), key(ImGuiKey_K, M_Ctrl), S::Global, Binding(), F_Cmd | F_Typing},
        {Action::ModePicker, "modepicker", N_("Mode picker"), N_("Windows"), key(ImGuiKey_M)},
        {Action::MemoryWin, "memory", N_("Memory panel"), N_("Windows"), key(ImGuiKey_M, M_Ctrl), S::Global, Binding(), F_Cmd},
        {Action::FreqDbWin, "freqdb", N_("Frequencies database"), N_("Windows"), key(ImGuiKey_F, M_Ctrl | M_Shift), S::Global, Binding(), F_Cmd},
        {Action::ClipsWin, "clips", N_("Radio clips"), N_("Windows"), key(ImGuiKey_C)},
        {Action::ControlsWin, "controls", N_("Keyboard and mouse settings"), N_("Windows"), Binding()},
        {Action::LayoutToggle, "layout", N_("Classic or Unified layout"), N_("View"), key(ImGuiKey_L, M_Ctrl | M_Shift), S::Global, Binding(), F_Cmd},
        {Action::RestoreView, "restoreview", N_("Restore the default view"), N_("View"), key(ImGuiKey_M, M_Ctrl | M_Shift), S::Global, Binding(), F_Cmd},
        {Action::DockToggle, "dock", N_("Docking on or off"), N_("View"), key(ImGuiKey_D, M_Ctrl | M_Alt)},
        {Action::Fullscreen, "fullscreen", N_("Full screen"), N_("View"), key(ImGuiKey_F11), S::Global, key(ImGuiKey_Enter, M_Alt)},
        {Action::VideoOnly, "videoonly", N_("Video only (picture over the whole screen)"), N_("View"), key(ImGuiKey_F)},
        {Action::IdentifyTool, "identify", N_("Identify tool (click a signal to identify it)"), N_("View"), key(ImGuiKey_I)},
        {Action::UnifiedColumn, "unicol", N_("Unified layout: side column"), N_("View"), key(ImGuiKey_F6)},
        {Action::TextBigger, "textbigger", N_("Larger text"), N_("View"), key(ImGuiKey_Equal, M_Ctrl), S::Global, key(ImGuiKey_KeypadAdd, M_Ctrl), F_Cmd | F_ShiftOk | F_Repeat},
        {Action::TextSmaller, "textsmaller", N_("Smaller text"), N_("View"), key(ImGuiKey_Minus, M_Ctrl), S::Global, key(ImGuiKey_KeypadSubtract, M_Ctrl), F_Cmd | F_ShiftOk | F_Repeat},
        {Action::TextReset, "textreset", N_("Text size back to 100 %"), N_("View"), key(ImGuiKey_0, M_Ctrl), S::Global, key(ImGuiKey_Keypad0, M_Ctrl), F_Cmd},
        {Action::RecordIq, "recordiq", N_("Start or stop an IQ recording"), N_("Receiver"), Binding()},
        {Action::TvPause, "tvpause", N_("Pause or play the TV picture"), N_("TV"), key(ImGuiKey_Space), S::Tv},
        {Action::TvRecord, "tvrecord", N_("Record the TV service"), N_("TV"), key(ImGuiKey_R), S::Tv},
        {Action::HamGuide, "hamguide", N_("Guide tab"), N_("FT8 / FT4 / WSPR"), key(ImGuiKey_F1), S::Ham},
        {Action::HamWizard, "hamwizard", N_("Station setup"), N_("FT8 / FT4 / WSPR"), key(ImGuiKey_F2), S::Ham},
        {Action::HamLog, "hamlog", N_("Log tab"), N_("FT8 / FT4 / WSPR"), key(ImGuiKey_F3), S::Ham},
        {Action::HamClearDx, "hamcleardx", N_("Stop and clear the DX call"), N_("FT8 / FT4 / WSPR"), key(ImGuiKey_F4), S::Ham},
        {Action::HamMap, "hammap", N_("Map tab"), N_("FT8 / FT4 / WSPR"), key(ImGuiKey_F5), S::Ham},
        {Action::HamCq, "hamcq", N_("Call CQ"), N_("FT8 / FT4 / WSPR"), key(ImGuiKey_F6), S::Ham},
        {Action::HamAnswer, "hamanswer", N_("Answer the best caller"), N_("FT8 / FT4 / WSPR"), key(ImGuiKey_F7), S::Ham},
        {Action::HamSpots, "hamspots", N_("Spots tab"), N_("FT8 / FT4 / WSPR"), key(ImGuiKey_F8), S::Ham},
        {Action::HamAddLog, "hamaddlog", N_("Add a contact to the log"), N_("FT8 / FT4 / WSPR"), key(ImGuiKey_L, M_Ctrl), S::Ham},
        {Action::HamFindLog, "hamfindlog", N_("Search the log"), N_("FT8 / FT4 / WSPR"), key(ImGuiKey_F, M_Ctrl), S::Ham},
    };
    static_assert(sizeof d / sizeof *d == (size_t)Action::Count, "one Def per Action, in the enum's order");
    return d;
}
inline const Def& def(Action a) { return defs()[(int)a]; }
inline int actionIndex(const char* idS) { for (int i = 0; i < kActions; i++) if (!strcmp(defs()[i].id_s, idS)) return i; return -1; }

// Does a key press with modifiers `now` match binding `b` of an action with `flags`? (F_Cmd: Super stands for Ctrl; F_ShiftOk: Shift is ignored)
inline bool modsMatch(const Binding& b, uint8_t now, uint8_t flags) {
    uint8_t m = now;
    if ((flags & F_Cmd) && (b.mods & M_Ctrl) && !(b.mods & M_Super) && (m & M_Super) && !(m & M_Ctrl)) m = (uint8_t)((m & ~M_Super) | M_Ctrl);
    if ((flags & F_ShiftOk) && !(b.mods & M_Shift)) m = (uint8_t)(m & ~M_Shift);
    return m == b.mods;
}

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
    Binding alt[kActions];                           // the second key of each action (4.0)
    uint8_t wheel[(int)Area::Count][kCombos] = {};   // Wheel per area and modifier combination
    int stepHz = 0;                                  // 0: the mode's own step
    int wheelNotch = 1;                              // notches of the wheel per step when it tunes (1..5)
    Settings() { reset(); }
    void reset() {
        for (int i = 0; i < kActions; i++) { bind[i] = defs()[i].def; alt[i] = defs()[i].alt; }
        for (int a = 0; a < (int)Area::Count; a++) for (int c = 0; c < kCombos; c++) wheel[a][c] = (uint8_t)wheelDefault((Area)a, c);
        stepHz = 0; wheelNotch = 1;
    }
    void resetAction(int i) { bind[i] = defs()[i].def; alt[i] = defs()[i].alt; }
    Binding& slot(int i, int which) { return which ? alt[i] : bind[i]; }
    const Binding& slot(int i, int which) const { return which ? alt[i] : bind[i]; }
    bool actionIsDefault(int i) const { return bind[i] == defs()[i].def && alt[i] == defs()[i].alt; }
    Wheel wheelFor(Area a, int combo) const { return (Wheel)wheel[(int)a][combo]; }
    bool wheelIsDefault() const {
        for (int a = 0; a < (int)Area::Count; a++) for (int c = 0; c < kCombos; c++) if (wheel[a][c] != (uint8_t)wheelDefault((Area)a, c)) return false;
        return true;
    }
    bool keysAreDefault() const {
        for (int i = 0; i < kActions; i++) if (!actionIsDefault(i)) return false;
        return true;
    }
    bool isDefault() const {
        const Settings d;
        return keysAreDefault() && wheelIsDefault() && stepHz == d.stepHz && wheelNotch == d.wheelNotch;
    }
};

// ------------------------------------------------------------------ names

// The key as ImGui names it (the saved text form uses these names: "UpArrow", "Comma", "KeypadAdd")
inline std::string keyName(int code) {
    if (code <= ImGuiKey_None || code >= ImGuiKey_NamedKey_END) return "?";
    const char* n = ImGui::GetKeyName((ImGuiKey)code);
    return n && *n ? n : "?";
}
// The key as a menu shows it: "," instead of Comma, Up instead of UpArrow
inline std::string keyLabel(int code) {
    static const struct { ImGuiKey k; const char* s; } pretty[] = {
        {ImGuiKey_Comma, ","}, {ImGuiKey_Period, "."}, {ImGuiKey_Minus, "-"}, {ImGuiKey_Equal, "="}, {ImGuiKey_Slash, "/"}, {ImGuiKey_Semicolon, ";"},
        {ImGuiKey_Apostrophe, "'"}, {ImGuiKey_LeftBracket, "["}, {ImGuiKey_RightBracket, "]"}, {ImGuiKey_Backslash, "\\"}, {ImGuiKey_GraveAccent, "`"},
        {ImGuiKey_KeypadAdd, "Keypad +"}, {ImGuiKey_KeypadSubtract, "Keypad -"}, {ImGuiKey_KeypadMultiply, "Keypad *"}, {ImGuiKey_KeypadDivide, "Keypad /"},
        {ImGuiKey_Escape, "Esc"}, {ImGuiKey_LeftArrow, "Left"}, {ImGuiKey_RightArrow, "Right"}, {ImGuiKey_UpArrow, "Up"}, {ImGuiKey_DownArrow, "Down"},
        {ImGuiKey_PageUp, "Page Up"}, {ImGuiKey_PageDown, "Page Down"},
    };
    for (const auto& p : pretty) if (p.k == code) return p.s;
    return keyName(code);
}
// mac: the names on a Mac keyboard (ImGui reports the Command key as Ctrl and the Control key as Super there)
inline std::string bindingText(const Binding& b, bool mac = false) {
    if (!b.bound()) return "";
    std::string s;
    if (mac) {
        if (b.mods & M_Super) s += "Ctrl+";
        if (b.mods & M_Alt) s += "Option+";
        if (b.mods & M_Shift) s += "Shift+";
        if (b.mods & M_Ctrl) s += "Cmd+";
    } else {
        if (b.mods & M_Ctrl) s += "Ctrl+";
        if (b.mods & M_Alt) s += "Alt+";
        if (b.mods & M_Shift) s += "Shift+";
        if (b.mods & M_Super) s += "Super+";
    }
    if (b.kind == 2) {
        static const char* mb[] = {"Left button", "Right button", "Middle button", "Back button", "Forward button"};
        s += b.code >= 0 && b.code < 5 ? mb[b.code] : "Button";
    } else s += keyLabel(b.code);
    return s;
}
// Both keys of an action as a menu shows them: "F11 / Alt+Enter"; empty when it has none
inline std::string actionText(const Settings& s, int i, bool mac = false);

inline std::string actionText(const Settings& s, int i, bool mac) {
    std::string t = bindingText(s.bind[i], mac);
    const std::string u = bindingText(s.alt[i], mac);
    if (!u.empty()) t += t.empty() ? u : " / " + u;
    return t;
}

// A help text that names an action's default keys ("... (Ctrl+M)") with the user's keys in their place: the translated text keeps its catalog
// entry and only the key names change. A key the user cleared is dropped with its brackets.
inline std::string swapKeyText(std::string text, const std::string& oldKey, const std::string& now) {
    if (oldKey.empty() || oldKey == now) return text;
    if (now.empty()) {
        for (const std::string& f : {" (" + oldKey + ")", " or " + oldKey, oldKey + " or ", oldKey}) {
            size_t p;
            while ((p = text.find(f)) != std::string::npos) text.erase(p, f.size());
        }
        return text;
    }
    size_t p = 0;
    while ((p = text.find(oldKey, p)) != std::string::npos) { text.replace(p, oldKey.size(), now); p += now.size(); }
    return text;
}
inline std::string tipWithKeys(const char* translated, const Settings& s, int i, bool mac) {
    std::string t = translated;
    const std::string d1 = bindingText(defs()[i].def), d2 = bindingText(defs()[i].alt);
    const std::string n1 = bindingText(s.bind[i], mac), n2 = bindingText(s.alt[i], mac);
    if (!d2.empty() && d2 != n2) t = swapKeyText(t, d2, n2);   // the second key first
    if (d1 != n1) t = swapKeyText(t, d1, n1);
    return t;
}

// The text form of a binding for an exported file: "Ctrl+Shift+UpArrow", "Mouse2", "none". parseSpec() reads it back.
inline std::string bindingSpec(const Binding& b) {
    if (!b.bound()) return "none";
    std::string s;
    if (b.mods & M_Ctrl) s += "Ctrl+";
    if (b.mods & M_Alt) s += "Alt+";
    if (b.mods & M_Shift) s += "Shift+";
    if (b.mods & M_Super) s += "Super+";
    if (b.kind == 2) s += "Mouse" + std::to_string((int)b.code);
    else s += keyName(b.code);
    return s;
}
inline bool sameText(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}
inline bool parseSpec(const std::string& text, Binding& out) {
    std::string t;
    for (char c : text) if (c != ' ' && c != '\t' && c != '\r') t += c;
    if (t.empty() || sameText(t, "none")) { out = Binding(); return true; }
    uint8_t mods = 0;
    size_t p = 0;
    for (;;) {
        const size_t q = t.find('+', p);
        if (q == std::string::npos || q == t.size() - 1) break;   // the last piece is the key ("Ctrl++" is not a form we write)
        const std::string m = t.substr(p, q - p);
        if (sameText(m, "Ctrl") || sameText(m, "Cmd")) mods |= M_Ctrl;
        else if (sameText(m, "Shift")) mods |= M_Shift;
        else if (sameText(m, "Alt") || sameText(m, "Option")) mods |= M_Alt;
        else if (sameText(m, "Super") || sameText(m, "Win")) mods |= M_Super;
        else return false;
        p = q + 1;
    }
    const std::string k = t.substr(p);
    if (k.size() == 6 && sameText(k.substr(0, 5), "Mouse") && k[5] >= '2' && k[5] <= '4') { out = mouse(k[5] - '0', mods); return true; }
    for (int c = ImGuiKey_NamedKey_BEGIN; c < ImGuiKey_NamedKey_END; c++) {
        if (c >= ImGuiKey_MouseLeft) break;   // keys only (the mouse "keys" and the modifier flags are not bindable keys)
        const char* n = ImGui::GetKeyName((ImGuiKey)c);
        if (n && sameText(k, n)) { out = key((ImGuiKey)c, mods); return true; }
    }
    return false;
}

// ------------------------------------------------------------------ conflicts

// The other actions that a key press of action idx would also fire: the same binding (either key of either action) in a scope that overlaps.
inline bool sharesKey(const Settings& s, int a, int b) {
    for (int x = 0; x < 2; x++) for (int y = 0; y < 2; y++) {
        const Binding& p = s.slot(a, x);
        if (p.bound() && p == s.slot(b, y)) return true;
    }
    return false;
}
inline std::vector<int> conflictsWith(const Settings& s, int idx) {
    std::vector<int> out;
    for (int i = 0; i < kActions; i++)
        if (i != idx && scopesOverlap(defs()[i].scope, defs()[idx].scope) && sharesKey(s, i, idx)) out.push_back(i);
    return out;
}
// The panel actions that take this global action's key while their screen is shown (not a clash: shown as a note)
inline std::vector<int> shadowedBy(const Settings& s, int idx) {
    std::vector<int> out;
    if (defs()[idx].scope != Scope::Global) return out;
    for (int i = 0; i < kActions; i++)
        if (i != idx && defs()[i].scope != Scope::Global && defs()[i].scope != Scope::Plot && sharesKey(s, i, idx)) out.push_back(i);
    return out;
}
inline bool anyConflict(const Settings& s) {
    for (int i = 0; i < kActions; i++) if (!conflictsWith(s, i).empty()) return true;
    return false;
}
inline int conflictCount(const Settings& s) {
    int n = 0;
    for (int i = 0; i < kActions; i++) if (!conflictsWith(s, i).empty()) n++;
    return n;
}
// Bindings that other parts of the program take for themselves: refused with a reason (nullptr: free)
inline const char* reservedReason(const Binding& b) {
    if (!b.bound()) return nullptr;
    if (b.kind == 2 && b.code <= 1 && b.mods == 0) return N_("The left and right mouse buttons are click-to-tune and the menu");
    if (b.kind == 1) {
        if (b.code == ImGuiKey_Escape) return N_("Esc closes dialogs and stops the transmitter");
        if (b.code >= ImGuiKey_LeftCtrl && b.code <= ImGuiKey_RightSuper) return N_("A modifier alone cannot be a key");
        if (b.code == ImGuiKey_D && (b.mods & M_Ctrl) && !(b.mods & ~M_Ctrl)) return N_("Ctrl+D makes a debug report");
        if (b.code == ImGuiKey_Tab && b.mods == 0) return N_("Tab moves between the controls");
        if (b.code >= ImGuiKey_ReservedForModCtrl && b.code <= ImGuiKey_ReservedForModSuper) return N_("A modifier alone cannot be a key");
    }
    return nullptr;
}
// Put binding b on action idx (slot 0 or 1). Returns the reason when refused; the actions that now share it are in `clash`.
inline const char* assign(Settings& s, int idx, int which, const Binding& b, std::vector<int>* clash = nullptr) {
    if (const char* why = reservedReason(b)) return why;
    s.slot(idx, which) = b;
    if (which == 1 && b.bound() && s.bind[idx] == b) s.alt[idx] = Binding();   // the same key twice on one action is one key
    if (clash) *clash = conflictsWith(s, idx);
    return nullptr;
}

// ------------------------------------------------------------------ saved text

// Only what differs from the defaults is written: "id=k:code:mods;..." for the first key, "id.2=..." for the second, and the wheel rows as digits.
inline std::string saveBindings(const Settings& s) {
    std::string o;
    char b[64];
    for (int i = 0; i < kActions; i++) {
        if (s.bind[i] != defs()[i].def) {
            snprintf(b, sizeof b, "%s=%d:%d:%d;", defs()[i].id_s, (int)s.bind[i].kind, (int)s.bind[i].code, (int)s.bind[i].mods);
            o += b;
        }
        if (s.alt[i] != defs()[i].alt) {
            snprintf(b, sizeof b, "%s.2=%d:%d:%d;", defs()[i].id_s, (int)s.alt[i].kind, (int)s.alt[i].code, (int)s.alt[i].mods);
            o += b;
        }
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
        std::string id = item.substr(0, q);
        int which = 0;
        if (id.size() > 2 && id.compare(id.size() - 2, 2, ".2") == 0) { which = 1; id.resize(id.size() - 2); }
        int k = 0, c = 0, m = 0;
        if (sscanf(item.c_str() + q + 1, "%d:%d:%d", &k, &c, &m) != 3) continue;
        if (k < 0 || k > 2 || m < 0 || m > 15) continue;
        if (k == 1 && (c <= ImGuiKey_None || c >= ImGuiKey_NamedKey_END)) continue;
        if (k == 2 && (c < 0 || c > 4)) continue;
        const int i = actionIndex(id.c_str());
        if (i < 0) continue;
        Binding b; b.kind = (uint8_t)k; b.code = (int16_t)c; b.mods = (uint8_t)m;
        if (k == 0) b = Binding();
        if (!reservedReason(b)) s.slot(i, which) = b;
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

// The exported file (Settings > Keyboard and mouse > Export): every action on its own line by name, readable and editable by hand.
//   start = Space
//   fullscreen = F11, Alt+Enter
//   wheel.spectrum = 01002
// importText() starts from the defaults, so a file that lists only some actions leaves the others at their defaults.
inline std::string exportText(const Settings& s) {
    std::string o = "# OnAir keyboard and mouse bindings. One action per line: name = key[, second key]. none clears a key.\n";
    for (int i = 0; i < kActions; i++) {
        o += defs()[i].id_s;
        o += " = " + bindingSpec(s.bind[i]);
        if (s.alt[i].bound()) o += ", " + bindingSpec(s.alt[i]);
        o += "\n";
    }
    o += "wheel.spectrum = " + saveWheel(s, Area::Spectrum) + "\n";
    o += "wheel.waterfall = " + saveWheel(s, Area::Waterfall) + "\n";
    o += "step = " + std::to_string(s.stepHz) + "\n";
    o += "notch = " + std::to_string(s.wheelNotch) + "\n";
    return o;
}
// How many lines were taken; `bad` gets the lines that were not understood (unknown action, unknown key, reserved key)
inline int importText(Settings& s, const std::string& text, std::vector<std::string>* bad = nullptr) {
    Settings r;
    int n = 0;
    size_t p = 0;
    auto trim = [](std::string x) { while (!x.empty() && (x.back() == ' ' || x.back() == '\r' || x.back() == '\t')) x.pop_back(); size_t b = 0; while (b < x.size() && (x[b] == ' ' || x[b] == '\t')) b++; return x.substr(b); };
    while (p <= text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        const std::string line = trim(text.substr(p, e - p));
        p = e + 1;
        if (line.empty() || line[0] == '#') continue;
        const size_t q = line.find('=');
        if (q == std::string::npos) { if (bad) bad->push_back(line); continue; }
        const std::string name = trim(line.substr(0, q)), val = trim(line.substr(q + 1));
        if (name == "wheel.spectrum" || name == "wheel.waterfall") {
            const Area a = name == "wheel.spectrum" ? Area::Spectrum : Area::Waterfall;
            const std::string before = saveWheel(r, a);
            loadWheel(r, a, val);
            if (saveWheel(r, a) == val) n++; else if (bad) { bad->push_back(line); (void)before; }
            continue;
        }
        if (name == "step" || name == "notch") {
            char* end = nullptr;
            const long v = strtol(val.c_str(), &end, 10);
            if (end == val.c_str()) { if (bad) bad->push_back(line); continue; }
            if (name == "step") r.stepHz = v >= 0 && v <= 100000000 ? (int)v : 0;
            else r.wheelNotch = (int)std::max(1L, std::min(5L, v));
            n++;
            continue;
        }
        const int i = actionIndex(name.c_str());
        if (i < 0) { if (bad) bad->push_back(line); continue; }
        const size_t c = val.find(',');
        Binding b1, b2;
        const bool ok1 = parseSpec(c == std::string::npos ? val : val.substr(0, c), b1);
        const bool ok2 = c == std::string::npos ? true : parseSpec(val.substr(c + 1), b2);
        if (!ok1 || !ok2 || reservedReason(b1) || reservedReason(b2)) { if (bad) bad->push_back(line); continue; }
        r.bind[i] = b1; r.alt[i] = b1 == b2 ? Binding() : b2;
        n++;
    }
    s = r;
    return n;
}

// The actions whose label, group, saved name or key text contain every word of the query (the search box of the editor)
inline bool actionMatches(const Settings& s, int i, const std::string& query) {
    if (query.empty()) return true;
    std::string hay = std::string(defs()[i].label) + " " + defs()[i].group + " " + defs()[i].id_s + " " + bindingText(s.bind[i]) + " " + bindingText(s.alt[i]);
    for (char& c : hay) c = (char)std::tolower((unsigned char)c);
    size_t p = 0;
    while (p < query.size()) {
        size_t e = query.find(' ', p);
        if (e == std::string::npos) e = query.size();
        std::string w = query.substr(p, e - p);
        p = e + 1;
        for (char& c : w) c = (char)std::tolower((unsigned char)c);
        if (!w.empty() && hay.find(w) == std::string::npos) return false;
    }
    return true;
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
void setFind(const std::string& q);           // put a search into the editor's search box (Settings' search leads there)
void editor();                                // the editor itself (the Controls window and Settings > Keyboard and mouse draw it)
// True in the frame the action's key (or mouse button) was pressed, with exactly its modifiers (see modsMatch). Not while typing in a
// text field, not while a key is being recorded. repeat: a held key repeats (also when the action has F_Repeat). Plot actions are asked by
// the plots while hovered; a global action stays quiet while a shown panel (scopeActive) has the same key.
bool pressed(Action a, bool repeat = false);
void scopeActive(Scope s);                    // a panel scope's screen is shown this frame (its keys win over global ones)
std::string shortcut(Action a);               // the user's keys for a menu or tooltip ("Ctrl+,"; Cmd on a Mac); "" when unbound
std::string tip(const char* translated, Action a);   // a translated help text with the action's default keys replaced by the user's
std::string exportPath();                     // the default export file in the data folder
bool exportTo(const std::string& path, std::string& msg);
bool importFrom(const std::string& path, std::string& msg);   // replaces the live bindings and saves them

}  // namespace inputmap
