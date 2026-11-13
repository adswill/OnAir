// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// View > Controls: the defaults are what the program always did, the saved text round-trips and survives garbage, conflicts and reserved keys
// are found, the wheel steps and the tuning step list behave. (app/ui/input_map.h; no ImGui context needed)
#include "../../app/ui/input_map.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace inputmap;
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }

int main() {
    Settings s;
    check(s.isDefault() && s.wheelIsDefault(), "fresh settings are the defaults");
    // the wheel defaults are the old rule: plain and Alt both axes, Ctrl the frequency only, with Shift (which selects signals) nothing
    check(zoomAxes(s.wheelFor(Area::Spectrum, C_None)) == 3, "plain wheel zooms both axes");
    check(zoomAxes(s.wheelFor(Area::Spectrum, C_Ctrl)) == 1, "Ctrl+wheel zooms the frequency only");
    check(s.wheelFor(Area::Spectrum, C_Shift) == Wheel::Nothing && s.wheelFor(Area::Waterfall, C_CtrlShift) == Wheel::Nothing, "Shift+wheel does nothing, as before");
    check(zoomAxes(s.wheelFor(Area::Waterfall, C_Alt)) == 3, "Alt+wheel zooms both");
    check(comboOf(false, false, false) == C_None && comboOf(true, false, false) == C_Shift && comboOf(false, true, false) == C_Ctrl && comboOf(true, true, false) == C_CtrlShift && comboOf(false, false, true) == C_Alt, "combo from modifiers");
    // the old keys
    check(s.bind[(int)Action::StartStop] == key(ImGuiKey_Space), "Space starts and stops");
    check(s.bind[(int)Action::Mode3] == key(ImGuiKey_3), "3 is mode 3");
    check(s.bind[(int)Action::DbUp] == key(ImGuiKey_UpArrow, M_Ctrl) && s.bind[(int)Action::DbWider] == key(ImGuiKey_UpArrow, M_Ctrl | M_Shift), "Ctrl+Up / Ctrl+Shift+Up");
    check(s.bind[(int)Action::PageDown] == key(ImGuiKey_PageDown, M_Ctrl), "Ctrl+PageDown");
    check(s.bind[(int)Action::TuneUp] == key(ImGuiKey_RightArrow) && s.bind[(int)Action::TuneDown] == key(ImGuiKey_LeftArrow) && !s.bind[(int)Action::Mute].bound(), "arrow keys tune by default (feat arrow keys); Mute starts unbound");
    check(!anyConflict(s), "no conflicts in the defaults");
    check(saveBindings(s).empty(), "nothing to save for the defaults");
    for (int i = 0; i < kActions; i++) check((int)defs()[i].id == i, "the table is in the enum's order");

    // change, save, load
    s.bind[(int)Action::TuneUp] = key(ImGuiKey_RightBracket, M_Shift);
    s.bind[(int)Action::StartStop] = mouse(2);
    s.bind[(int)Action::Mode1] = Binding();
    s.wheel[(int)Area::Spectrum][C_None] = (uint8_t)Wheel::ZoomFreq;
    s.wheel[(int)Area::Spectrum][C_Shift] = (uint8_t)Wheel::Tune;
    s.wheel[(int)Area::Waterfall][C_None] = (uint8_t)Wheel::ScrollTime;
    check(!s.isDefault() && !s.wheelIsDefault(), "changes are noticed");
    Settings t;
    loadBindings(t, saveBindings(s));
    loadWheel(t, Area::Spectrum, saveWheel(s, Area::Spectrum));
    loadWheel(t, Area::Waterfall, saveWheel(s, Area::Waterfall));
    bool same = true;
    for (int i = 0; i < kActions; i++) same = same && t.bind[i] == s.bind[i];
    check(same, "bindings round-trip");
    check(t.wheelFor(Area::Spectrum, C_Shift) == Wheel::Tune && t.wheelFor(Area::Waterfall, C_None) == Wheel::ScrollTime && t.wheelFor(Area::Spectrum, C_Ctrl) == Wheel::ZoomFreq, "wheel table round-trips");
    check(!t.bind[(int)Action::Mode1].bound(), "a cleared key stays cleared");

    // garbage in the saved text is ignored, not crashed on
    Settings g;
    loadBindings(g, "nonsense;start=9:1:1;mute=1:99999:0;tuneup=1:abc;=;;dbup=2:9:0;pgup=1:" + std::to_string((int)ImGuiKey_Escape) + ":0;stepnext=1:" + std::to_string((int)ImGuiKey_F5) + ":3");
    check(g.bind[(int)Action::StartStop] == key(ImGuiKey_Space), "a bad kind is ignored");
    check(!g.bind[(int)Action::Mute].bound(), "a key number out of range is ignored");
    check(g.bind[(int)Action::DbUp] == key(ImGuiKey_UpArrow, M_Ctrl), "a mouse button out of range is ignored");
    check(g.bind[(int)Action::PageUp] == key(ImGuiKey_PageUp, M_Ctrl), "Esc cannot be bound (loaded text)");
    check(g.bind[(int)Action::StepNext] == key(ImGuiKey_F5, M_Shift | M_Ctrl), "a good item after bad ones still loads");
    loadWheel(g, Area::Spectrum, "12");     // too short
    loadWheel(g, Area::Spectrum, "99999");  // out of range digits
    check(g.wheelIsDefault(), "a bad wheel row is ignored");

    // conflicts and reserved keys
    Settings c;
    c.bind[(int)Action::TuneUp] = key(ImGuiKey_Space);
    check(conflictsWith(c, (int)Action::TuneUp).size() == 1 && conflictsWith(c, (int)Action::StartStop).size() == 1 && anyConflict(c), "same key twice is a conflict, both rows see it");
    c.bind[(int)Action::TuneUp] = key(ImGuiKey_Space, M_Shift);
    check(!anyConflict(c), "another modifier is not a conflict");
    check(reservedReason(key(ImGuiKey_Escape)) && reservedReason(mouse(0)) && reservedReason(mouse(1)) && reservedReason(key(ImGuiKey_Tab)), "Esc, Tab and the main buttons are reserved");
    check(!reservedReason(key(ImGuiKey_K, M_Ctrl)), "4.0: Ctrl+K is the palette's action, rebindable, no longer reserved");
    check(!reservedReason(mouse(2)) && !reservedReason(key(ImGuiKey_K)) && !reservedReason(Binding()), "middle button, a bare K and nothing are free");
    check(reservedReason(key(ImGuiKey_LeftCtrl)), "a modifier alone is refused");

    // wheel notches to steps: a mouse wheel and a trackpad
    float acc = 0;
    check(wheelSteps(acc, 1.f, 1) == 1 && wheelSteps(acc, -1.f, 1) == -1, "one notch is one step");
    acc = 0; int n = 0;
    for (int i = 0; i < 25; i++) n += wheelSteps(acc, 0.1f, 1);
    check(n == 2, "a trackpad adds up (25 x 0.1 = 2 steps)");
    acc = 0; n = 0;
    for (int i = 0; i < 6; i++) n += wheelSteps(acc, 1.f, 3);
    check(n == 2, "three notches per step");
    acc = 0;
    check(wheelSteps(acc, -2.f, 1) == -2, "a fast spin gives several steps");

    // the tuning step
    check(stepHzFor(s, 12500) == 12500, "setting 0: the mode's own step");
    Settings u; u.stepHz = 5000;
    check(stepHzFor(u, 12500) == 5000, "the chosen step wins");
    check(stepHzFor(Settings(), 0) == 1000, "no step known: 1 kHz");
    check(stepShift(0, 1) == stepList().front() && stepShift(0, -1) == stepList().back(), "from the mode's step: the smallest or the largest");
    check(stepShift(1000, 1) == 5000 && stepShift(1000, -1) == 500, "next and previous in the list");
    check(stepShift(stepList().back(), 1) == stepList().back() && stepShift(stepList().front(), -1) == stepList().front(), "the ends hold");
    check(stepShift(7000, 1) == 8330 && stepShift(7000, -1) == 6250, "between two entries");
    // ---------------------------------------------------------------- 4.0: the action registry
    {
        Settings d;
        // the 3.9 keys that were written into the code are the defaults
        check(d.bind[(int)Action::OpenSettings] == key(ImGuiKey_Comma, M_Ctrl) && d.bind[(int)Action::CommandPalette] == key(ImGuiKey_K, M_Ctrl), "Ctrl+, and Ctrl+K");
        check(d.bind[(int)Action::Fullscreen] == key(ImGuiKey_F11) && d.alt[(int)Action::Fullscreen] == key(ImGuiKey_Enter, M_Alt), "F11 and Alt+Enter");
        check(d.bind[(int)Action::TextBigger] == key(ImGuiKey_Equal, M_Ctrl) && d.alt[(int)Action::TextBigger] == key(ImGuiKey_KeypadAdd, M_Ctrl), "Ctrl+= and Ctrl+Keypad+");
        check(d.bind[(int)Action::HamCq] == key(ImGuiKey_F6) && d.bind[(int)Action::UnifiedColumn] == key(ImGuiKey_F6), "F6 is CQ in the FT8 screen and the side column elsewhere");
        check(d.bind[(int)Action::LayoutToggle] == key(ImGuiKey_L, M_Ctrl | M_Shift) && d.bind[(int)Action::HamAddLog] == key(ImGuiKey_L, M_Ctrl), "Ctrl+Shift+L and Ctrl+L");
        check(!anyConflict(d) && conflictCount(d) == 0, "the 4.0 defaults have no conflicts");
        // ids are unique and found by name
        bool uniq = true;
        for (int i = 0; i < kActions; i++) for (int j = i + 1; j < kActions; j++) if (!strcmp(defs()[i].id_s, defs()[j].id_s)) uniq = false;
        check(uniq, "saved names are unique");
        check(actionIndex("palette") == (int)Action::CommandPalette && actionIndex("nothing") == -1, "actionIndex");
        // scopes: a panel key shadows a global one (not a conflict); global and plot clash
        check(shadowedBy(d, (int)Action::UnifiedColumn).size() == 1 && shadowedBy(d, (int)Action::UnifiedColumn)[0] == (int)Action::HamCq, "F6 of the FT8 screen shadows the global F6");
        check(shadowedBy(d, (int)Action::StartStop).size() == 1 && shadowedBy(d, (int)Action::StartStop)[0] == (int)Action::TvPause, "Space of the TV picture shadows Start");
        Settings c2 = d;
        c2.bind[(int)Action::ModePicker] = key(ImGuiKey_Home);   // a global Home and the plots' Home both fire
        check(conflictsWith(c2, (int)Action::ModePicker).size() == 1 && conflictsWith(c2, (int)Action::ModePicker)[0] == (int)Action::ZoomReset, "global vs plot is a conflict");
        Settings c3 = d;
        c3.bind[(int)Action::HamGuide] = key(ImGuiKey_F2);   // two FT8 actions on F2
        check(conflictsWith(c3, (int)Action::HamGuide).size() == 1, "same scope, same key: conflict");
        c3.bind[(int)Action::HamGuide] = key(ImGuiKey_R);   // R of the TV picture: another panel, no clash
        check(conflictsWith(c3, (int)Action::HamGuide).empty(), "two panel scopes never clash");
        // a conflict through the second key
        Settings c4 = d;
        c4.alt[(int)Action::Mute] = key(ImGuiKey_F11);
        check(conflictsWith(c4, (int)Action::Mute).size() == 1 && conflictsWith(c4, (int)Action::Fullscreen).size() == 1, "a second key can clash with a first key");
        // rebinding through assign(): refused keys, the clash list, the same key twice on one action
        Settings r = d;
        std::vector<int> clash;
        check(assign(r, (int)Action::Mute, 0, key(ImGuiKey_Escape), &clash) != nullptr && !r.bind[(int)Action::Mute].bound(), "Esc is refused");
        check(assign(r, (int)Action::Mute, 0, key(ImGuiKey_C), &clash) == nullptr && clash.size() == 1 && clash[0] == (int)Action::ClipsWin, "the clash is reported");
        check(assign(r, (int)Action::Mute, 1, key(ImGuiKey_C), &clash) == nullptr && !r.alt[(int)Action::Mute].bound(), "the same key as second key is dropped");
        r.resetAction((int)Action::Mute);
        check(r.actionIsDefault((int)Action::Mute), "reset one action");
        // the Ctrl-or-Cmd and the Shift-ignored rules
        check(modsMatch(key(ImGuiKey_K, M_Ctrl), M_Super, F_Cmd) && !modsMatch(key(ImGuiKey_K, M_Ctrl), M_Super, 0), "Super stands for Ctrl only with F_Cmd");
        check(modsMatch(key(ImGuiKey_Equal, M_Ctrl), M_Ctrl | M_Shift, F_ShiftOk) && !modsMatch(key(ImGuiKey_Equal, M_Ctrl), M_Ctrl | M_Shift, 0), "an extra Shift only with F_ShiftOk");
        check(!modsMatch(key(ImGuiKey_L, M_Ctrl), M_Ctrl | M_Shift, 0), "Ctrl+L does not fire on Ctrl+Shift+L (3.9 fired both)");
        // persistence: second keys round-trip, the 3.9 text still loads
        Settings p = d;
        p.alt[(int)Action::StartStop] = mouse(3, M_Shift);
        p.bind[(int)Action::OpenSettings] = key(ImGuiKey_F12, M_Alt);
        p.alt[(int)Action::Fullscreen] = Binding();
        Settings q;
        loadBindings(q, saveBindings(p));
        bool same = true;
        for (int i = 0; i < kActions; i++) same = same && q.bind[i] == p.bind[i] && q.alt[i] == p.alt[i];
        check(same, "first and second keys round-trip through the preferences");
        Settings old39;
        loadBindings(old39, "tuneup=1:" + std::to_string((int)ImGuiKey_RightBracket) + ":1;start=2:2:0;");
        check(old39.bind[(int)Action::TuneUp] == key(ImGuiKey_RightBracket, M_Shift) && old39.bind[(int)Action::StartStop] == mouse(2) && old39.alt[(int)Action::Fullscreen] == key(ImGuiKey_Enter, M_Alt), "a 3.9 saved text loads, the new actions keep their defaults");
        // the export file: readable names, full round trip, garbage reported
        Settings e = p;
        e.wheel[(int)Area::Waterfall][C_Ctrl] = (uint8_t)Wheel::Tune;
        e.stepHz = 12500; e.wheelNotch = 3;
        const std::string txt = exportText(e);
        check(txt.find("palette = Ctrl+K") != std::string::npos && txt.find("fullscreen = F11\n") != std::string::npos && txt.find("start = Space, Shift+Mouse3") != std::string::npos, "export writes names");
        Settings f;
        f.bind[(int)Action::Mute] = key(ImGuiKey_Z);
        std::vector<std::string> bad;
        const int n = importText(f, txt, &bad);
        same = true;
        for (int i = 0; i < kActions; i++) same = same && f.bind[i] == e.bind[i] && f.alt[i] == e.alt[i];
        check(same && bad.empty() && n == kActions + 4, "export / import round trip of every action");
        check(f.wheelFor(Area::Waterfall, C_Ctrl) == Wheel::Tune && f.stepHz == 12500 && f.wheelNotch == 3, "the wheel, step and notches round-trip");
        Settings g2;
        bad.clear();
        importText(g2, "# c\nmute = Ctrl+Shift+F9\nnope = A\nstart = Escape\nmode1 = Hyper+Q\nzoomin = none\nwheel.spectrum = 99\n", &bad);
        check(g2.bind[(int)Action::Mute] == key(ImGuiKey_F9, M_Ctrl | M_Shift) && g2.bind[(int)Action::StartStop] == key(ImGuiKey_Space) && bad.size() == 4, "import: a good line loads, unknown action, reserved key, bad modifier and bad wheel are reported");
        Binding b;
        check(parseSpec("cmd+option+UpArrow", b) && b == key(ImGuiKey_UpArrow, M_Ctrl | M_Alt) && parseSpec("mouse4", b) && b == mouse(4) && !parseSpec("Ctrl+Nokey", b), "spec parser: Mac names, mouse buttons, unknown keys");
        for (int i = 0; i < kActions; i++) {
            Binding x;
            if (d.bind[i].bound()) { const bool ok = parseSpec(bindingSpec(d.bind[i]), x) && x == d.bind[i]; if (!ok) printf("spec %s\n", defs()[i].id_s); check(ok, "every default key round-trips through its text"); }
        }
        // labels for menus and tooltips
        check(bindingText(key(ImGuiKey_Comma, M_Ctrl)) == "Ctrl+," && bindingText(key(ImGuiKey_Comma, M_Ctrl), true) == "Cmd+," && bindingText(key(ImGuiKey_UpArrow, M_Super), true) == "Ctrl+Up", "menu text, Windows and Mac");
        check(actionText(d, (int)Action::Fullscreen) == "F11 / Alt+Enter" && actionText(d, (int)Action::Mute).empty(), "both keys in a menu");
        Settings t2 = d;
        t2.bind[(int)Action::MemoryWin] = key(ImGuiKey_F9);
        check(tipWithKeys("Memories (Ctrl+M)", t2, (int)Action::MemoryWin, false) == "Memories (F9)", "a tooltip shows the user's key");
        t2.bind[(int)Action::MemoryWin] = Binding();
        check(tipWithKeys("Memories (Ctrl+M)", t2, (int)Action::MemoryWin, false) == "Memories", "a cleared key leaves the tooltip");
        check(tipWithKeys("Full (F11 or Alt+Enter).", d, (int)Action::Fullscreen, false) == "Full (F11 or Alt+Enter).", "defaults: the text as it is");
        // the editor's search
        check(actionMatches(d, (int)Action::CommandPalette, "palette") && actionMatches(d, (int)Action::CommandPalette, "ctrl+k") && !actionMatches(d, (int)Action::Mute, "palette"), "search by name and by key");
        check(actionMatches(d, (int)Action::HamCq, "ft8 cq"), "every word, group included");
    }
    printf(fails ? "%d FAILED\n" : "input_map: all passed\n", fails);
    return fails ? 1 : 0;
}
