// View > Controls: the defaults are what the program always did, the saved text round-trips and survives garbage, conflicts and reserved keys
// are found, the wheel steps and the tuning step list behave. (app/ui/input_map.h; no ImGui context needed)
#include "../../app/ui/input_map.h"
#include <cstdio>
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
    check(reservedReason(key(ImGuiKey_Escape)) && reservedReason(key(ImGuiKey_K, M_Ctrl)) && reservedReason(mouse(0)) && reservedReason(mouse(1)), "Esc, Ctrl+K and the main buttons are reserved");
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
    printf(fails ? "%d FAILED\n" : "input_map: all passed\n", fails);
    return fails ? 1 : 0;
}
