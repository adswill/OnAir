// The touch interface's pickers and sheets: the numeric keypad for the frequency, the tuning step picker, the full-screen mode picker
// (a grid of big tiles in the mode list's group colours, searchable) and the menu sheet that replaces the menu bar.
#include "touch.h"
#include "../app.h"
#include "ui2_internal.h"
#include "../platform/platform.h"
#include "theme.h"
#include "scale.h"
#include <cstdlib>

namespace touch {

namespace {
bool gOpenKeypad = false, gOpenStep = false, gOpenModes = false, gOpenMenu = false;
Keypad gKp;
double gStep = -1;   // Hz


// the heading row of a sheet: a title and a Close button on the right
bool sheetHeader(const char* title) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    const float w = ImGui::CalcTextSize(tr("Close")).x + ImGui::GetStyle().FramePadding.x * 2 + 8 * gUi;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - w);
    return bigButton(tr("Close"), ImVec2(w, 0));
}
}   // namespace

double& stepHz() {
    if (gStep < 0) gStep = plat::prefs().getD("touchStepHz", 100e3);
    return gStep;
}

bool beginSheet(const char* id, const char* title, bool fullScreen) {
    char name[96];
    snprintf(name, sizeof name, fullScreen ? "tsheet_full_%s" : "tsheet_%s", id);
    ImGuiWindowFlags f = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | (fullScreen ? ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove : ImGuiWindowFlags_AlwaysAutoResize);
    if (!ImGui::BeginPopupModal(name, nullptr, f)) return false;
    if (sheetHeader(title)) { ImGui::CloseCurrentPopup(); }
    ImGui::Separator();
    return true;
}
void endSheet() { ImGui::EndPopup(); }
void closeSheet() { ImGui::CloseCurrentPopup(); }

bool keyboardAvailable() {
#if defined(_WIN32)
    return true;
#elif defined(__linux__)
    static int have = -1;
    if (have < 0) have = system("command -v onboard >/dev/null 2>&1") == 0 || system("command -v squeekboard >/dev/null 2>&1") == 0;
    return have == 1;
#else
    return false;   // macOS: the system input methods and the Character Viewer are the answer
#endif
}

void showSystemKeyboard() {
#if defined(_WIN32)
    system("start \"\" \"%CommonProgramFiles%\\microsoft shared\\ink\\TabTip.exe\"");
#elif defined(__linux__)
    if (system("command -v onboard >/dev/null 2>&1") == 0) { if (system("onboard >/dev/null 2>&1 &")) {} }
    else if (system("squeekboard >/dev/null 2>&1 &")) {}
#endif
}

// ---------------------------------------------------------------------------------------------------------------------------- keypad
void openKeypad(double hz) { (void)hz; gKp.clear(); gKp.unit = Unit::MHz; gOpenKeypad = true; }

// --touchsheet <name>[:keys] (dev, with --shot): open a sheet at the start; "keypad:7.1" also types keys
void devOpen(const char* spec) {
    const std::string s = spec ? spec : "";
    const std::string name = s.substr(0, s.find(':'));
    if (name == "keypad") { openKeypad(0); if (s.find(':') != std::string::npos) for (char c : s.substr(s.find(':') + 1)) gKp.key(c); }
    else if (name == "step") openStepPicker();
    else if (name == "modes") openModePicker();
    else if (name == "menu") openMenu();
    else gDevOpen = name;
}

void keypadSheet(App& a) {
    if (gOpenKeypad) { ImGui::OpenPopup("tsheet_keypad"); gOpenKeypad = false; }
    if (!beginSheet("keypad", tr("Frequency"))) return;
    const float d = dpi();
    ImGui::TextDisabled(tr("now %s MHz"), mhzText(a.freqMhz).c_str());
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.4f);
    char big[64]; snprintf(big, sizeof big, "%s %s", gKp.shown().c_str(), unitName(gKp.unit));
    ImGui::TextColored(gKp.empty() ? pal::grey() : pal::accent(), "%s", big);
    ImGui::PopFont();
    const float w = ImGui::GetContentRegionAvail().x, sp = ImGui::GetStyle().ItemSpacing.x;
    const float uw = (w - 3 * sp) / 4;
    for (int u = 0; u < 4; u++) {   // the unit
        if (u) ImGui::SameLine();
        const bool sel = (int)gKp.unit == u;
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? pal::accent(0.55f) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
        if (ImGui::Button(unitName((Unit)u), ImVec2(uw, target()))) gKp.unit = (Unit)u;
        ImGui::PopStyleColor();
    }
    const float bw = (w - 2 * sp) / 3, bh = target() * 1.25f;
    static const char* keys[12] = {"7", "8", "9", "4", "5", "6", "1", "2", "3", ".", "0", "<"};
    for (int i = 0; i < 12; i++) {
        if (i % 3) ImGui::SameLine();
        ImGui::PushID(i);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
        if (ImGui::Button(i == 11 ? "<-" : keys[i], ImVec2(bw, bh))) { if (i == 11) gKp.backspace(); else gKp.key(keys[i][0]); }
        ImGui::PopFont();
        ImGui::PopID();
    }
    const float hw = (w - sp) / 2;
    if (bigButton(tr("Cancel"), ImVec2(hw, 0))) closeSheet();
    ImGui::SameLine();
    ImGui::BeginDisabled(gKp.empty());
    if (bigButton(tr("OK"), ImVec2(hw, 0), true)) { tuneFreq(a, gKp.hz() / 1e6); closeSheet(); }
    ImGui::EndDisabled();
    (void)d;
    endSheet();
}

// ---------------------------------------------------------------------------------------------------------------------------- step
void openStepPicker() { gOpenStep = true; }

void stepSheet(App& a) {
    (void)a;
    if (gOpenStep) { ImGui::OpenPopup("tsheet_step"); gOpenStep = false; }
    if (!beginSheet("step", tr("Tuning step"))) return;
    int n = 0;
    const double* s = tuneSteps(n);
    const float w = ImGui::GetContentRegionAvail().x, sp = ImGui::GetStyle().ItemSpacing.x;
    const int cols = w > 700 * dpi() ? 5 : 3;
    const float bw = (w - (cols - 1) * sp) / cols;
    for (int i = 0; i < n; i++) {
        if (i % cols) ImGui::SameLine();
        char l[32]; stepLabel(s[i], l, sizeof l);
        ImGui::PushID(i);
        const bool sel = s[i] == stepHz();
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? pal::accent(0.55f) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
        if (ImGui::Button(l, ImVec2(bw, target() * 1.2f))) { stepHz() = s[i]; plat::prefs().setD("touchStepHz", s[i]); closeSheet(); }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    endSheet();
}

// ---------------------------------------------------------------------------------------------------------------------------- mode picker
void openModePicker() { gOpenModes = true; }

static ImU32 shade(ImU32 c, float k) {
    const int r = (int)((c & 255) * k), g = (int)(((c >> 8) & 255) * k), b = (int)(((c >> 16) & 255) * k);
    return IM_COL32(r, g, b, 255);
}

void modePickerSheet(App& a, ImVec2 disp) {
    (void)disp;
    if (gOpenModes) { ImGui::OpenPopup("tsheet_full_modes"); gOpenModes = false; }
    if (!beginSheet("modes", tr("Mode"), true)) return;
    static char q[48] = "";
    if (ImGui::IsWindowAppearing()) q[0] = 0;
    const float kb = keyboardAvailable() ? ImGui::CalcTextSize(tr("Keyboard")).x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x : 0.f;
    ImGui::SetNextItemWidth(kb > 0 ? -kb : -FLT_MIN);
    ImGui::InputTextWithHint("##tmodeq", tr("search (FT8, LoRa, APRS...)"), q, sizeof q);
    if (kb > 0) { ImGui::SameLine(); if (ImGui::Button(tr("Keyboard"), ImVec2(0, target()))) showSystemKeyboard(); }
    if (a.engine.running()) ImGui::TextColored(pal::warnAmber(), "%s", tr("Stop the receiver to change the mode."));
    ImGui::BeginChild("##tmodes", ImVec2(0, 0), 0);
    const float w = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize, sp = ImGui::GetStyle().ItemSpacing.x;
    const int cols = std::max(2, (int)(w / (165 * dpi())));
    const float tw = (w - (cols - 1) * sp) / cols, th = target() * 1.6f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int g = 0; g < kNumGroups; g++) {
        if (!modeGroupListed(a, g, q)) continue;
        ImGui::Spacing();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kGroupCols[g]), "%s", tr(kGroupNames[g]));
        int i = 0;
        for (const auto& m : allModes()) {
            if (m.group != g || !modeListed(a, m, q)) continue;
            if (i++ % cols) ImGui::SameLine();
            ImGui::PushID(&m);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##tile", ImVec2(tw, th))) { selectMode(a, m.family, m.subId); if (!a.engine.running()) closeSheet(); }
            const bool sel = modeSelected(a, m), held = ImGui::IsItemActive();
            dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + th), shade(m.col, sel ? 0.55f : held ? 0.45f : 0.28f), 8.f * dpi());
            dl->AddRectFilled(p, ImVec2(p.x + 6 * dpi(), p.y + th), m.col, 8.f * dpi(), ImDrawFlags_RoundCornersLeft);
            if (sel) dl->AddRect(p, ImVec2(p.x + tw, p.y + th), IM_COL32(255, 255, 255, 230), 8.f * dpi(), 0, 2.f * dpi());
            const float tx = p.x + 14 * dpi();
            ImGui::PushClipRect(p, ImVec2(p.x + tw - 4 * dpi(), p.y + th), true);
            dl->AddText(ImVec2(tx, p.y + 8 * dpi()), IM_COL32(255, 255, 255, 255), m.name);
            if (m.sub && *m.sub) dl->AddText(ImVec2(tx, p.y + 8 * dpi() + ImGui::GetTextLineHeight() + 2 * dpi()), IM_COL32(210, 218, 228, 200), tr(m.sub));
            ImGui::PopClipRect();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    endSheet();
}

// ---------------------------------------------------------------------------------------------------------------------------- menu
void openMenu() { gOpenMenu = true; }

void menuSheet(App& a, ImVec2 disp) {
    (void)disp;
    if (gOpenMenu) { ImGui::OpenPopup("tsheet_menu"); gOpenMenu = false; }
    if (!beginSheet("menu", tr("Menu"))) return;
    const float w = ImGui::GetContentRegionAvail().x, sp = ImGui::GetStyle().ItemSpacing.x;
    if (bigButton(tr("Desktop interface"), ImVec2(w, 0), true)) { a.touchUi = false; savePrefs(a); closeSheet(); }
    ImGui::TextDisabled("%s", tr("Text size"));
    int pct = (int)std::lround(a.touchScale * 100);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##ttxt", &pct, 100, 200, "%d%%")) a.touchScale = std::max(1.f, std::min(2.f, pct / 100.f));
    if (ImGui::IsItemDeactivatedAfterEdit()) savePrefs(a);
    ImGui::TextDisabled("%s", tr("Control size"));   // #139: button height, paddings, top bar and dock, applied at once (touch_logic.h)
    {
        const float cw = (w - sp * (kTouchSizes - 1)) / kTouchSizes;
        for (int i = 0; i < kTouchSizes; i++) {
            if (i) ImGui::SameLine();
            ImGui::PushID(i);
            if (bigButton(tr(touchSizeNames()[i]), ImVec2(cw, 0), a.touchSize == i)) { a.touchSize = i; savePrefs(a); }
            ImGui::PopID();
        }
    }
    ui2::themeChoice(a, false);
    const float hw = (w - sp) / 2;
    if (bigButton(txViewFlag() ? tr("Receive view") : tr("Transmit view"), ImVec2(hw, 0))) { hamViewFlag() = false; txViewFlag() = !txViewFlag(); closeSheet(); }
    if (bigButton(hamViewFlag() ? tr("Receive view") : tr("Ham view"), ImVec2(w, 0))) { txViewFlag() = false; hamViewFlag() = !hamViewFlag(); closeSheet(); }
    ImGui::SameLine();
    if (bigButton(tr("Tour"), ImVec2(hw, 0))) { tourStart(a); closeSheet(); }
    if (bigButton(tr("Lessons"), ImVec2(hw, 0))) { a.lessonsOpen = true; closeSheet(); }
    ImGui::SameLine();
    if (bigButton(tr("Concepts (glossary)"), ImVec2(hw, 0))) { a.glossaryOpen = true; closeSheet(); }
    languageButton(a);
    endSheet();
}

}   // namespace touch
