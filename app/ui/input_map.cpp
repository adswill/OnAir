// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Keyboard and mouse (Settings > Keyboard and mouse, and View > Controls): every action's keys, the mouse buttons and the wheel of the
// spectrum and waterfall, rebindable, with conflict warnings, search, import and export. The tables and rules are in input_map.h.
#include "input_map.h"
#include "../app.h"
#include "../i18n/i18n.h"
#include "../platform/platform.h"
#include "scale.h"
#include "theme.h"
#include <algorithm>
#include <cfloat>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/common/u8file.h"

namespace inputmap {

namespace {
Settings gS;
bool gLoaded = false, gOpen = false;
std::string gMsg;    // why the last key was refused
int gCapture = -1;   // the action whose binding is being recorded (-1: none)
int gCaptureSlot = 0;   // which of its two keys
int gScopeFrame[(int)Scope::Count] = {-10, -10, -10, -10};   // the frame each panel scope was last shown
char gFind[64] = "";    // the editor's search box
bool gResetAsk = false;

void loadNow() {
    plat::Prefs& p = plat::basePrefs();
    gS.reset();
    loadBindings(gS, p.getS("ctlKeys", ""));
    loadWheel(gS, Area::Spectrum, p.getS("ctlWhSpec", ""));
    loadWheel(gS, Area::Waterfall, p.getS("ctlWhWf", ""));
    const long st = p.getI("ctlStep", 0);
    gS.stepHz = st >= 0 && st <= 100000000 ? (int)st : 0;
    gS.wheelNotch = (int)std::max(1L, std::min(5L, p.getI("ctlNotch", 1)));
    gLoaded = true;
}

uint8_t modsNow() {
    const ImGuiIO& io = ImGui::GetIO();
    return (uint8_t)((io.KeyShift ? M_Shift : 0) | (io.KeyCtrl ? M_Ctrl : 0) | (io.KeyAlt ? M_Alt : 0) | (io.KeySuper ? M_Super : 0));
}

bool isModifierKey(int k) { return (k >= ImGuiKey_LeftCtrl && k <= ImGuiKey_RightSuper) || (k >= ImGuiKey_ReservedForModCtrl && k <= ImGuiKey_ReservedForModSuper); }

// The next key or mouse button pressed, as a binding (nothing: kind 0). Escape cancels (returns true with kind 0).
bool capture(Binding& out) {
    for (int b = 2; b < 5; b++)
        if (ImGui::IsMouseClicked(b, false)) { out = mouse(b, modsNow()); return true; }
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; k++) {
        if (isModifierKey(k) || (k >= ImGuiKey_MouseLeft && k <= ImGuiKey_MouseWheelY)) continue;
        if (!ImGui::IsKeyPressed((ImGuiKey)k, false)) continue;
        if (k == ImGuiKey_Escape) { out = Binding(); return true; }
        out = key((ImGuiKey)k, modsNow());
        return true;
    }
    return false;
}

// grey text that wraps at the window's edge
void hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

#ifdef __APPLE__
constexpr bool kMac = true;
#else
constexpr bool kMac = false;
#endif
std::string display(const Binding& b) { return bindingText(b, kMac); }

// the key event this frame for binding b (KeypadEnter counts as Enter)
bool keyHit(const Binding& b, bool repeat) {
    if (b.kind == 2) return ImGui::IsMouseClicked(b.code, false);
    if (ImGui::IsKeyPressed((ImGuiKey)b.code, repeat)) return true;
    return b.code == ImGuiKey_Enter && ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, repeat);
}
}  // namespace

Settings& settings() {
    if (!gLoaded) loadNow();
    return gS;
}

void save() {
    plat::Prefs& p = plat::basePrefs();
    const Settings& s = settings();
    p.setS("ctlKeys", saveBindings(s));
    const Settings d;
    p.setS("ctlWhSpec", saveWheel(s, Area::Spectrum) == saveWheel(d, Area::Spectrum) ? "" : saveWheel(s, Area::Spectrum));
    p.setS("ctlWhWf", saveWheel(s, Area::Waterfall) == saveWheel(d, Area::Waterfall) ? "" : saveWheel(s, Area::Waterfall));
    p.setI("ctlStep", s.stepHz);
    p.setI("ctlNotch", s.wheelNotch);
    p.flush();
}

void reload() { loadNow(); }

bool& windowOpen() { return gOpen; }

bool pressed(Action a, bool repeat) {
    if (gCapture >= 0) return false;
    ImGuiIO& io = ImGui::GetIO();
    const Settings& st = settings();
    const int i = (int)a;
    const Def& d = defs()[i];
    if (io.WantTextInput && !(d.flags & F_Typing)) return false;
    if ((d.flags & F_Typing) && !(st.bind[i].mods & (M_Ctrl | M_Alt | M_Super)) && io.WantTextInput) return false;   // a plain letter while typing is text
    const uint8_t now = modsNow();
    const int frame = ImGui::GetFrameCount();
    const bool rep = repeat || (d.flags & F_Repeat);
    for (int w = 0; w < 2; w++) {
        const Binding& b = st.slot(i, w);
        if (!b.bound() || !modsMatch(b, now, d.flags) || !keyHit(b, rep)) continue;
        if (d.scope == Scope::Global) {   // a panel that is shown takes its own keys
            bool taken = false;
            for (int j = 0; j < kActions && !taken; j++) {
                const Scope sc = defs()[j].scope;
                if (sc == Scope::Global || sc == Scope::Plot || gScopeFrame[(int)sc] < frame - 1) continue;
                for (int y = 0; y < 2; y++) if (st.slot(j, y).bound() && st.slot(j, y) == b) taken = true;
            }
            if (taken) continue;
        }
        return true;
    }
    return false;
}

void scopeActive(Scope s) { if ((int)s < (int)Scope::Count) gScopeFrame[(int)s] = ImGui::GetFrameCount(); }

std::string shortcut(Action a) { return actionText(settings(), (int)a, kMac); }
std::string tip(const char* translated, Action a) { return tipWithKeys(translated, settings(), (int)a, kMac); }

std::string exportPath() { return plat::dataDir() + "/onair-keys.txt"; }

bool exportTo(const std::string& path, std::string& msg) {
    std::ofstream f(dect2::u8file(path));
    f << exportText(settings());
    msg = f ? std::string(tr("Exported to ")) + path : std::string(tr("Could not write ")) + path;
    return (bool)f;
}

bool importFrom(const std::string& path, std::string& msg) {
    std::ifstream f(dect2::u8file(path));
    if (!f) { msg = std::string(tr("No file at ")) + path; return false; }
    std::stringstream ss; ss << f.rdbuf();
    std::vector<std::string> bad;
    Settings s = settings();
    const int n = importText(s, ss.str(), &bad);
    if (n == 0) { msg = tr("Nothing in that file could be read: the keys are unchanged."); return false; }
    settings() = s;
    save();
    char b[160];
    snprintf(b, sizeof b, tr("Imported %d lines, %d not understood"), n, (int)bad.size());
    msg = b;
    if (!bad.empty()) msg += ": " + bad[0];
    return true;
}

namespace {
// one key cell: the key text (orange when it clashes), Set / Clear for that key
bool keyCell(Settings& s, int i, int w, const std::vector<int>& cf) {
    bool changed = false;
    ImGui::PushID(w);
    const Binding& b = s.slot(i, w);
    ImGui::AlignTextToFramePadding();
    if (gCapture == i && gCaptureSlot == w) ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.f, 1.f), "%s", tr("press a key ..."));
    else if (!b.bound()) ImGui::TextDisabled("%s", w ? "-" : tr("not set"));
    else if (!cf.empty()) ImGui::TextColored(ImVec4(1.f, 0.55f, 0.2f, 1.f), "%s", display(b).c_str());
    else ImGui::TextUnformatted(display(b).c_str());
    if (gCapture == i && gCaptureSlot == w) {
        Binding nb;
        if (capture(nb)) {
            gCapture = -1;
            if (nb.bound()) {
                std::vector<int> clash;
                if (const char* why = assign(s, i, w, nb, &clash)) gMsg = why;
                else {
                    changed = true;
                    gMsg.clear();
                    if (!clash.empty()) {
                        gMsg = std::string(tr("Warning: this key is also used by")) + " " + tr(defs()[clash[0]].label) + (clash.size() > 1 ? " (+" + std::to_string(clash.size() - 1) + ")" : "");
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Cancel"))) gCapture = -1;
    } else if (ImGui::IsItemHovered() && b.bound()) ImGui::SetTooltip("%s", tr("Click Set to record another key, Clear to remove it."));
    ImGui::PopID();
    return changed;
}
}  // namespace

void setFind(const std::string& q) { snprintf(gFind, sizeof gFind, "%s", q.c_str()); }

void editor() {
    Settings& s = settings();
    bool changed = false;
    const int nClash = conflictCount(s);
    if (nClash) {
        char b[160];
        snprintf(b, sizeof b, tr("%d actions share a key with another action: both fire. They are marked in orange."), nClash);
        ImGui::TextColored(ImVec4(1.f, 0.55f, 0.2f, 1.f), "%s", b);
    }
    if (!gMsg.empty()) ImGui::TextColored(ImVec4(1.f, 0.55f, 0.2f, 1.f), "%s", tr(gMsg.c_str()));

    if (ImGui::CollapsingHeader(trw("Keys and mouse buttons"), ImGuiTreeNodeFlags_DefaultOpen)) {
        hint(tr("Click Set, then press the key (with Ctrl, Shift or Alt if you like) or the middle / side mouse button. Esc cancels. Every action can have two keys."));
        ImGui::SetNextItemWidth(260 * gUi);
        ImGui::InputTextWithHint("##keyfind", tr("Search actions or keys"), gFind, sizeof gFind);
        ImGui::SameLine();
        if (ImGui::Button(tr("Export..."))) {
            const std::string p = plat::saveFileDialog("onair-keys.txt");
            exportTo(p.empty() ? exportPath() : p, gMsg);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Import..."))) {
            const std::string p = plat::openFileDialog();
            importFrom(p.empty() ? exportPath() : p, gMsg);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Reads a file written by Export (one action per line). Without a file dialog, onair-keys.txt in the data folder is used."));
        if (ImGui::BeginTable("##keys", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn(tr("Action"));
            ImGui::TableSetupColumn(tr("Key"), ImGuiTableColumnFlags_WidthFixed, 170 * gUi);
            ImGui::TableSetupColumn(tr("Second key"), ImGuiTableColumnFlags_WidthFixed, 150 * gUi);
            ImGui::TableSetupColumn("##b", ImGuiTableColumnFlags_WidthFixed, 230 * gUi);
            ImGui::TableHeadersRow();
            const char* lastGroup = "";
            const std::string q = gFind;
            for (int i = 0; i < kActions; i++) {
                const Def& d = defs()[i];
                if (!q.empty() && !actionMatches(s, i, q)) {   // the search looks at the translated label too
                    std::string hay = std::string(tr(d.label)) + " " + tr(d.group);
                    for (char& c : hay) c = (char)std::tolower((unsigned char)c);
                    std::string lq = q; for (char& c : lq) c = (char)std::tolower((unsigned char)c);
                    if (hay.find(lq) == std::string::npos) continue;
                }
                if (strcmp(d.group, lastGroup) != 0) {
                    lastGroup = d.group;
                    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("%s", tr(d.group));
                }
                ImGui::TableNextRow();
                ImGui::PushID(i);
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                if (!s.actionIsDefault(i)) { ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1), "*"); ImGui::SameLine(0, 4 * gUi); }
                ImGui::TextUnformatted(tr(d.label));
                if (d.scope != Scope::Global) { ImGui::SameLine(); ImGui::TextDisabled("(%s)", tr(scopeName(d.scope))); }
                const std::vector<int> cf = conflictsWith(s, i);
                const std::vector<int> sh = shadowedBy(s, i);
                if ((!cf.empty() || !sh.empty()) && ImGui::IsItemHovered()) {
                    std::string t;
                    if (!cf.empty()) { t = tr("Also used by:"); for (int o : cf) t += std::string("\n  ") + tr(defs()[o].label); }
                    if (!sh.empty()) { if (!t.empty()) t += "\n"; t += tr("These take the key while their screen is shown:"); for (int o : sh) t += std::string("\n  ") + tr(defs()[o].label) + " (" + tr(scopeName(defs()[o].scope)) + ")"; }
                    ImGui::SetTooltip("%s", t.c_str());
                }
                ImGui::TableSetColumnIndex(1);
                changed |= keyCell(s, i, 0, cf);
                ImGui::TableSetColumnIndex(2);
                changed |= keyCell(s, i, 1, cf);
                ImGui::TableSetColumnIndex(3);
                if (gCapture != i) {
                    if (ImGui::SmallButton(tr("Set"))) { gCapture = i; gCaptureSlot = 0; }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(tr("Set second"))) { gCapture = i; gCaptureSlot = 1; }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(tr("Clear"))) { s.bind[i] = Binding(); s.alt[i] = Binding(); changed = true; }
                    if (!s.actionIsDefault(i)) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton(tr("Default"))) { s.resetAction(i); changed = true; }
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        hint(tr("Not available for rebinding: the left and right mouse buttons, Esc and Tab. A key of the TV picture or the FT8 screen wins over a global key while that screen is shown."));
    }

    if (ImGui::CollapsingHeader(trw("Mouse wheel"), ImGuiTreeNodeFlags_DefaultOpen)) {
        hint(tr("By default the wheel zooms the frequency and the dB range together (Ctrl: frequency only, Shift: dB only). Pick Zoom the frequency only on the plain wheel to leave the dB range alone."));
        if (ImGui::BeginTable("##whl", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn(tr("Keys held"), ImGuiTableColumnFlags_WidthFixed, 150 * gUi);
            ImGui::TableSetupColumn(tr("Over the spectrum"));
            ImGui::TableSetupColumn(tr("Over the waterfall"));
            ImGui::TableHeadersRow();
            for (int c = 0; c < kCombos; c++) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(tr(comboName(c)));
                for (int ar = 0; ar < (int)Area::Count; ar++) {
                    ImGui::TableSetColumnIndex(1 + ar);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::PushID(ar * 16 + c);
                    if (ImGui::BeginCombo("##w", tr(wheelName((Wheel)s.wheel[ar][c])))) {
                        for (int w = 0; w < (int)Wheel::Count; w++) {
                            if (ImGui::Selectable(tr(wheelName((Wheel)w)), s.wheel[ar][c] == w)) { s.wheel[ar][c] = (uint8_t)w; changed = true; }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr("Tuning step"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160 * gUi);
        char cur[40];
        if (s.stepHz <= 0) snprintf(cur, sizeof cur, "%s", tr("The mode's own step"));
        else if (s.stepHz >= 1000000) snprintf(cur, sizeof cur, "%g MHz", s.stepHz / 1e6);
        else if (s.stepHz >= 1000) snprintf(cur, sizeof cur, "%g kHz", s.stepHz / 1e3);
        else snprintf(cur, sizeof cur, "%d Hz", s.stepHz);
        if (ImGui::BeginCombo("##step", cur)) {
            if (ImGui::Selectable(tr("The mode's own step"), s.stepHz <= 0)) { s.stepHz = 0; changed = true; }
            for (int v : stepList()) {
                char l[32];
                if (v >= 1000000) snprintf(l, sizeof l, "%g MHz", v / 1e6); else if (v >= 1000) snprintf(l, sizeof l, "%g kHz", v / 1e3); else snprintf(l, sizeof l, "%d Hz", v);
                if (ImGui::Selectable(l, s.stepHz == v)) { s.stepHz = v; changed = true; }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr("Wheel notches per step"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70 * gUi);
        if (ImGui::SliderInt("##notch", &s.wheelNotch, 1, 5)) changed = true;
        hint(tr("The step is used by Tune by the step on the wheel and by the tune up / down and step keys above."));
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(s.isDefault());
    if (!gResetAsk) { if (ImGui::Button(tr("Reset every key and the wheel to the defaults"))) gResetAsk = true; }
    else {
        ImGui::TextUnformatted(tr("Every key and the wheel go back to the defaults. Continue?"));
        ImGui::SameLine();
        if (ImGui::Button(tr("Yes, reset all"))) { s.reset(); gCapture = -1; changed = true; gResetAsk = false; gMsg.clear(); }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) gResetAsk = false;
    }
    ImGui::EndDisabled();
    if (changed) save();
}

void window() {
    if (!gOpen) { gCapture = -1; return; }
    firstWindowSize(820 * gUi, 640 * gUi);
    if (!ImGui::Begin(trw("Controls"), &gOpen)) { ImGui::End(); return; }
    ImGui::TextWrapped("%s", tr("Choose what the keys, the extra mouse buttons and the mouse wheel do. Nothing here is needed: the defaults are what OnAir has always done. The same page is in Settings > Keyboard and mouse."));
    ImGui::Spacing();
    editor();
    ImGui::End();
}

}  // namespace inputmap
