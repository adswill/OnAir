// The new interface: small widgets used by every layout (bordered panes, flat buttons and links, the big frequency
// digits, the run button, section headings, meters, splitters, tiles and the faceplate modules).
// Split from ui2.cpp; the shared declarations are in ui2_internal.h.
#include <functional>
#include "panel_layout.h"
#include "freq_digits.h"
#include "ui2_internal.h"
#include "setpos_guard.h"
#include "../state/listen_freq.h"
#include "dect2/source/converter.h"
#include <cfloat>

using namespace ui2;

namespace ui2 {

// set the waterfall range from the spectrum: the noise floor is the 20th percentile, the top is the strongest signal (at most 45 dB above the floor)
void autoRange(App& a) { a.wf.fitRange(a.smooth, true); }

ImU32 u32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }

// A bordered pane with square corners. autoH: grow with the contents.
// A pane with autoH is as tall as its contents. ImGui sizes it from the last frame, so a status line whose values change width (and so
// wrap onto one line more or less) made it flick between heights, a line cut off every other frame: the height it needs is kept here
// and it shrinks only after a second without the extra line.
struct PaneH { ImGuiID key; bool autoH; };

std::vector<PaneH> gPanes;

constexpr int kPaneHold = 60;   // frames

void beginPane(const char* id, ImVec2 size, bool autoH) {
    ImGui::PushStyleColor(ImGuiCol_Border, kEdge);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7 * gUi, 4 * gUi));
    const ImGuiID key = ImGui::GetID(id);
    const float held = autoH ? ImGui::GetStateStorage()->GetFloat(key, 0.f) : 0.f;
    if (held > 0) size.y = held;
    gPanes.push_back({key, autoH});
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | (autoH && held <= 0 ? ImGuiChildFlags_AutoResizeY : 0));
}

void endPane() {
    const PaneH p = gPanes.back();
    gPanes.pop_back();
    // the height of what was drawn: down to the last line, and the padding below it
    const float need = p.autoH ? ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + ImGui::GetStyle().WindowPadding.y : 0.f;
    ui::closeCursorMove();   // a mode body that ends with SetCursorPos and no item: grow the pane instead of the Debug-build assert (#104)
    ImGui::EndChild();
    if (p.autoH) {
        ImGuiStorage* st = ImGui::GetStateStorage();
        const ImGuiID ageKey = p.key + 1;
        const float held = st->GetFloat(p.key, 0.f);
        const int age = st->GetInt(ageKey, 0);
        if (need > held + 0.5f || held <= 0) { st->SetFloat(p.key, need); st->SetInt(ageKey, 0); }   // grows at once
        else if (need < held - 0.5f) {                                                             // shrinks after a while
            if (age >= kPaneHold) { st->SetFloat(p.key, need); st->SetInt(ageKey, 0); } else st->SetInt(ageKey, age + 1);
        } else st->SetInt(ageKey, 0);
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

// A flat button with a 1 px outline; returns true when clicked.
bool flatButton(const char* label, float w, ImU32 textCol, ImU32 edgeCol) {
    const ImVec2 sz(w, ImGui::GetFrameHeight() + 4 * gUi);
    const bool clicked = ImGui::InvisibleButton(label, sz);
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hov) dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 14));
    dl->AddRect(p0, p1, hov ? textCol : edgeCol);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p0.x + (sz.x - ts.x) * 0.5f, p0.y + (sz.y - ts.y) * 0.5f), textCol, label);
    return clicked;
}

// a text link: dim, brighter on hover, underlined on hover
bool textLink(const char* label) {
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const bool clicked = ImGui::InvisibleButton(label, ts);
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 p = ImGui::GetItemRectMin();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(p, u32(hov ? kBright : kDim), label);
    if (hov) dl->AddLine(ImVec2(p.x, p.y + ts.y), ImVec2(p.x + ts.x, p.y + ts.y), u32(kBright));
    return clicked;
}

// ---- the SDR++ style frequency readout: digits you can click (upper half up, lower half down) or scroll. With the mouse over it, typing a
// number sets the digit under the mouse and moves on to the next one, the arrow keys step the digit (up/down) or pick another (left/right),
// and a double click opens a box to type the whole frequency.
bool gFreqHovered = false;   // last frame: the mouse was over the digits (the 1-9 mode keys stay off then)

float freqDigitsImpl(App& a, double& mhzRef, double lo, double hi, const std::function<void(double)>& tune, float fontSize) {
    static int kbIdx = -1;               // the digit the keyboard works on after typing or left/right (-1: the one under the mouse)
    static ImVec2 kbMouse;               // where the mouse was then: moving it gives the digit back to the mouse
    static double preClickMhz = 0;       // the frequency before the first click of a double click, which the double click undoes
    static char typed[32] = "";
    long long hz = (long long)std::llround(mhzRef * 1e6);
    constexpr int kD = fdigits::kDigits;   // thirteen digits: up to 1000 GHz (10^12 Hz); the arithmetic is ui/freq_digits.h
    char buf[24];
    snprintf(buf, sizeof buf, "%013lld", hz);
    ImGui::PushFont(typo::fonts().monoBold ? typo::fonts().monoBold : a.mono, fontSize);
    const ImVec2 cs = ImGui::CalcTextSize("0");
    const float cw = cs.x + 1 * gUi, ch = cs.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    float x = o.x;
    long long delta = 0;
    bool leading = true, dbl = false;
    int hovIdx = -1;
    float digitX[kD];
    for (int i = 0; i < kD; i++) {
        digitX[i] = x;
        const long long place = fdigits::placeOf(i);
        ImGui::SetCursorScreenPos(ImVec2(x, o.y));
        ImGui::PushID(i);
        ImGui::InvisibleButton("##d", ImVec2(cw, ch));
        ImGui::PopID();
        const bool hov = ImGui::IsItemHovered();
        const ImVec2 p0 = ImGui::GetItemRectMin();
        const bool upper = ImGui::GetIO().MousePos.y < p0.y + ch * 0.5f;
        if (hov) hovIdx = i;
        if (hov && ImGui::IsMouseDoubleClicked(0)) dbl = true;
        else if (ImGui::IsItemClicked(0)) { preClickMhz = mhzRef; delta = upper ? place : -place; }
        if (hov && ImGui::GetIO().MouseWheel != 0) {
            // a mouse wheel notch is 1.0 (one step of this digit); a trackpad sends many small values, which add up to a step each time they reach 1
            static float wheelAcc = 0;
            const int steps = fdigits::wheelSteps(wheelAcc, ImGui::GetIO().MouseWheel);
            if (steps != 0) delta = (long long)steps * place;
            ImGui::GetIO().MouseWheel = 0;
        }
        if (hov) {
            dl->AddRectFilled(p0, ImVec2(p0.x + cw, p0.y + ch), IM_COL32(255, 255, 255, 18));
            const float my = upper ? p0.y + 2 * gUi : p0.y + ch - 2 * gUi;
            dl->AddTriangleFilled(ImVec2(p0.x + cw * 0.5f - 3 * gUi, upper ? my + 4 * gUi : my - 4 * gUi), ImVec2(p0.x + cw * 0.5f + 3 * gUi, upper ? my + 4 * gUi : my - 4 * gUi), ImVec2(p0.x + cw * 0.5f, my), u32(T.dim));
        }
        if (buf[i] != '0') leading = false;
        const bool dim = leading && i < kD - 3;   // leading zeros are shown dim, like SDR++
        char c[2] = {buf[i], 0};
        dl->AddText(ImVec2(x + (cw - cs.x) * 0.5f, o.y), u32(dim ? ImVec4(T.dim.x * 0.8f, T.dim.y * 0.8f, T.dim.z * 0.8f, 1) : T.bright), c);
        x += cw;
        if (i < kD - 1 && (kD - 1 - i) % 3 == 0) { dl->AddText(ImVec2(x - cs.x * 0.275f, o.y), u32(T.dim), "."); x += cs.x * 0.45f; }   // the dot centred in the gap
    }
    ImGui::SetCursorScreenPos(ImVec2(x + 8 * gUi, o.y + ch * 0.5f - ImGui::GetTextLineHeight() * 0.35f));
    ImGui::PopFont();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Hz"));
    const float endX = ImGui::GetItemRectMax().x;
    ImGuiIO& io = ImGui::GetIO();
    gFreqHovered = hovIdx >= 0 || (kbIdx >= 0 && io.MousePos.x == kbMouse.x && io.MousePos.y == kbMouse.y);
    if (kbIdx >= 0 && (io.MousePos.x != kbMouse.x || io.MousePos.y != kbMouse.y)) kbIdx = -1;
    const int cur = kbIdx >= 0 ? kbIdx : hovIdx;
    if (cur >= 0 && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper) {
        const long long place = fdigits::placeOf(cur);
        for (int k = 0; k < 10; k++)
            if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_0 + k), false) || ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_Keypad0 + k), false)) {
                delta = fdigits::typeDelta(hz, cur, k);
                kbIdx = std::min(kD - 1, cur + 1); kbMouse = io.MousePos;
            }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) delta = place;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) delta = -place;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) { kbIdx = std::max(0, cur - 1); kbMouse = io.MousePos; }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { kbIdx = std::min(kD - 1, cur + 1); kbMouse = io.MousePos; }
        if (kbIdx >= 0 && kbIdx != hovIdx) {   // show where the keyboard is when it is not under the mouse
            const float kx = digitX[kbIdx];
            dl->AddRect(ImVec2(kx, o.y), ImVec2(kx + cw, o.y + ch), u32(T.dim), 2.f);
        }
    }
    if (dbl) {
        if (std::fabs(preClickMhz - mhzRef) > 1e-9 && preClickMhz > 0) tune(preClickMhz);   // undo the first click's step
        snprintf(typed, sizeof typed, "%.6f", preClickMhz > 0 ? preClickMhz : mhzRef);
        ImGui::OpenPopup("##freqtype");
    }
    if (ImGui::BeginPopup("##freqtype")) {
        ImGui::TextDisabled(tr("Frequency in MHz (%.3f to %.3f)"), lo, hi);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(170 * gUi);
        const bool enter = ImGui::InputText("##ftext", typed, sizeof typed, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        ImGui::SameLine();
        if (enter || ImGui::Button(tr("Tune"))) {
            char* e = nullptr;
            const double v = strtod(typed, &e);
            if (e != typed && v > 0) tune(std::min(hi, std::max(lo, v)));
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (delta) {
        const double mhz = fdigits::applyMhz(hz, delta, lo, hi);
        if (std::fabs(mhz - mhzRef) > 1e-9) tune(mhz);
    }
    return endX;
}


// The big readout: the frequency being LISTENED to (SDR++), or the radio centre (right click: the choice, kept). Beside it a small line
// names the other one, and the hardware frequency when a converter moves it (freq_model.h), so the three are never confused (user report:
// an AM station on 11.710 MHz while the readout said 11.520.000, the radio centre).
float freqDigits(App& a, float fontSize) {
    const ModeTuning* mt = a.family >= 6 ? modeTuning(a.family + 2) : nullptr;
    static int showCentre = -1;
    if (showCentre < 0) showCentre = plat::prefs().getB("bigReadoutCentre", false) ? 1 : 0;
    const double centreMhz = radioCenterMhz(a);
    double shown = showCentre ? centreMhz : listenMhz(a);
    float end = freqDigitsImpl(a, shown, mt ? mt->minMhz : 0.001, dect2::kMaxTuneMhz,
                               [&a](double v) {
                                   if (!showCentre) { tuneListen(a, v); return; }
                                   if (fileVfoActive(a)) { tuneFreq(a, v); return; }   // a recording: the axis never moves, no tune offset
                                   // the readout is the radio's centre, which sits the mode's tune offset above the dial (#304): tuneFreq wants the dial, so a click
                                   // on a digit of 3.876 MHz did not make 3.886 but moved the radio by the digit plus the offset every time
                                   const double shiftMhz = a.engine.running() ? a.engine.decodeShiftHz() / 1e6 : 0.0;
                                   tuneFreq(a, v - (radioCenterMhz(a) - (a.freqMhz - shiftMhz)));
                               }, fontSize);
    const ImVec2 hzPos = ImGui::GetItemRectMin();   // the "Hz" label
    if (gFreqHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##readoutopt");
    if (ImGui::BeginPopup("##readoutopt")) {
        ImGui::TextDisabled("%s", tr("The big readout shows"));
        if (ImGui::RadioButton(tr("Listening frequency (the VFO)"), showCentre == 0)) { showCentre = 0; plat::prefs().setB("bigReadoutCentre", false); }
        if (ImGui::RadioButton(tr("Radio centre"), showCentre == 1)) { showCentre = 1; plat::prefs().setB("bigReadoutCentre", true); }
        ImGui::EndPopup();
    }
    // the small line: the other frequency, and the radio's own when a converter is in the way
    double hwHz = centreMhz * 1e6;
    if (a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].isRadio()) {
        const dect2::Converter c = dect2::converterFromRadio(a.tune.radio);
        if (c.active && c.reachable(hwHz)) hwHz = c.rfToIf(hwHz);
    }
    const fmodel::Freqs f = fmodel::compute(listenMhz(a) * 1e6, 0, centreMhz * 1e6, hwHz);
    const std::string line = fmodel::secondaryLine(f, showCentre == 1);
    if (!line.empty()) {
        const ImVec2 p = hzPos;
        const float fs = ImGui::GetFontSize() * 0.85f;
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), fs, ImVec2(end + 10 * gUi, p.y + 1 * gUi), u32(T.dim), line.c_str());
        end += 10 * gUi + ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, line.c_str()).x;
    }
    return end;
}

// the same readout for any frequency (the transmitter's): `tune` gets every change
float freqDigitsFor(App& a, double& mhz, const std::function<void(double)>& tune, float fontSize) {
    return freqDigitsImpl(a, mhz, 0.001, dect2::kMaxTuneMhz, tune, fontSize);
}

// a start / stop button drawn as a play triangle or a stop square
bool runButton(bool running, float size, const char* tip) {
    const bool clicked = ImGui::InvisibleButton("##run", ImVec2(size, size));
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hov) dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 16));
    dl->AddRect(p0, p1, u32(running ? pal::badRed() : T.edge));
    const float m = size * 0.28f;
    if (running) dl->AddRectFilled(ImVec2(p0.x + m, p0.y + m), ImVec2(p1.x - m, p1.y - m), u32(pal::badRed()));
    else dl->AddTriangleFilled(ImVec2(p0.x + m + 2 * gUi, p0.y + m), ImVec2(p0.x + m + 2 * gUi, p1.y - m), ImVec2(p1.x - m + 1 * gUi, (p0.y + p1.y) * 0.5f), u32(pal::okGreen()));
    if (hov) ImGui::SetTooltip(tr("%s (space)"), tip ? tip : tr(running ? "Stop the receiver" : "Start the receiver"));
    return clicked;
}

// a collapsible sidebar section; returns whether it is open
bool section(const char* title, bool openByDefault) {
    if (kit::tokens().devLook && pal::dev()) {   // Dev looks: the kit's flat section
        const bool o = kit::collapsingSection(trw(title), openByDefault);
        if (o) { panel::gSectionTopY = ImGui::GetCursorPosY(); panel::gSectionTopFrame = ImGui::GetFrameCount(); }
        return o;
    }
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(1, 1, 1, 0.045f));
    const bool open = typo::collapsingHeader(trw(title), openByDefault ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    ImGui::PopStyleColor();
    if (open) { panel::gSectionTopY = ImGui::GetCursorPosY(); panel::gSectionTopFrame = ImGui::GetFrameCount(); }
    return open;
}

// the ADC level meter: segments, the last two mean overload
void levelMeter(App& a, float x, float y, float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool running = a.engine.running();
    const float db = running ? a.spec.stats.rmsDbfs : -120.f;
    const int n = 26, lit = (int)std::lround(std::min(1.f, std::max(0.f, (db + 60.f) / 60.f)) * n);
    const float sw = (w - (n - 1) * 2 * gUi) / n, sy = y + 20 * gUi, sh = 12 * gUi;
    char lv[32]; snprintf(lv, sizeof lv, running ? "%.1f dBFS" : "-", db);
    const float lw = ImGui::CalcTextSize(lv).x, room = w - lw - 8 * gUi;   // a narrow meter: a shorter caption, or none
    const char* cap = ImGui::CalcTextSize(tr("ADC level")).x <= room ? tr("ADC level") : ImGui::CalcTextSize("ADC").x <= room ? "ADC" : "";
    dl->AddText(ImVec2(x, y), u32(T.dim), cap);
    dl->AddText(ImVec2(x + w - lw, y), u32(T.text), lv);
    for (int k = 0; k < n; k++) {
        const bool on = k < lit, hot = k >= n - 2;
        const ImU32 col = on ? (hot ? u32(pal::badRed()) : k >= n - 5 ? u32(pal::warnAmber()) : u32(ImVec4(0.62f, 0.70f, 0.64f, 1))) : u32(ImVec4(0.25f, 0.26f, 0.27f, 1));
        dl->AddRectFilled(ImVec2(x + k * (sw + 2 * gUi), sy), ImVec2(x + k * (sw + 2 * gUi) + sw, sy + sh), col);
    }
}

void volumeControl(App& a, float w) {
    if (iconFlat(a.muted ? Ic::Mute : Ic::Speaker, tr(a.muted ? "Unmute" : "Mute"))) {
        a.muted = !a.muted;
        a.engine.player().setMuted(a.muted); a.engine.dabAudio().setMuted(a.muted);
    }
    ImGui::SameLine(0, 6 * gUi);
    // the speaker, the slider and three icon buttons (each one frame high, 4 apart) all fit in w: the slider takes what is left
    ImGui::SetNextItemWidth(std::max(30.f * gUi, w - 4 * ImGui::GetFrameHeight() - 18 * gUi));
    float vol = a.volume * 100.f;
    if (ImGui::SliderFloat("##vol", &vol, 0, 100, "%.0f %%")) { a.volume = vol / 100.f; a.engine.player().setVolume(a.volume); a.engine.dabAudio().setVolume(a.volume); a.engine.setWideVolume(a.volume); }
    if (ImGui::IsItemHovered() && a.family == 9) ImGui::SetTooltip("%s", tr("DMR voice is played when an AMBE+2 codec plug-in is installed (DMR panel, ? button); the volume applies to it."));
    ImGui::SameLine(0, 4 * gUi);
    audioControlButton(a);
    ImGui::SameLine(0, 4 * gUi);
    audioCleanupButton(a);
    ImGui::SameLine(0, 4 * gUi);
    voiceInversionButton(a);
}

// ---- draggable splitters between the fixed panels: sizes are kept in unscaled points (0 = automatic), a double click puts the automatic size back
// sign -1: the value is the size of the panel to the right of / below the bar
bool splitter(const char* id, bool vertical, float thick, float len, float& val, float curPx, float minPx, float maxPx, float sign, App& a) {
    ImGui::InvisibleButton(id, vertical ? ImVec2(thick, len) : ImVec2(len, thick));
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    if (hov || act) ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    if (hov || act) {
        const float m = vertical ? (p0.x + p1.x) * 0.5f : (p0.y + p1.y) * 0.5f;
        ImGui::GetWindowDrawList()->AddRectFilled(vertical ? ImVec2(m - 1 * gUi, p0.y) : ImVec2(p0.x, m - 1 * gUi), vertical ? ImVec2(m + 1 * gUi, p1.y) : ImVec2(p1.x, m + 1 * gUi), u32(act ? kBright : kDim));
        if (hov && !act) ImGui::SetTooltip("%s", tr("Drag to resize, double click for the automatic size"));
    }
    bool changed = false;
    if (hov && ImGui::IsMouseDoubleClicked(0)) { if (val != 0) { val = 0; changed = true; } }
    else if (act && ImGui::IsMouseDragging(0, 0)) {
        const float d = vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
        if (d != 0) { val = std::max(minPx, std::min(std::max(minPx, maxPx), curPx + sign * d)) / gUi; changed = true; }
    }
    if (ImGui::IsItemDeactivated() || changed) savePrefs(a);
    return changed;
}

float clampPx(float v, float lo, float hi) { return std::max(lo, std::min(std::max(lo, hi), v)); }

// ---------------------------------------------------------------- variant 2: tiles (no tabs in the way: everything on screen at once, in titled tiles)
void beginTile(const char* id, const char* title, ImVec2 size) {
    beginPane(id, size);
    ImGui::TextDisabled("%s", tr(title));
    ImGui::Separator();
}

// ---------------------------------------------------------------- variant 3: faceplate (a hardware-like front: a light chassis with dark display modules and printed labels)
const ImVec4 kChassis(0.705f, 0.712f, 0.720f, 1), kSilk(0.20f, 0.21f, 0.22f, 1);

void beginModule(const char* id, const char* label, ImVec2 size) {
    label = tr(label);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddText(ImVec2(p.x + 2 * gUi, p.y), u32(kSilk), label);
    dl->AddLine(ImVec2(p.x + ImGui::CalcTextSize(label).x + 8 * gUi, p.y + ImGui::GetTextLineHeight() * 0.5f), ImVec2(p.x + (size.x > 0 ? size.x : ImGui::GetContentRegionAvail().x), p.y + ImGui::GetTextLineHeight() * 0.5f), u32(ImVec4(0.45f, 0.46f, 0.47f, 1)));
    ImGui::Dummy(ImVec2(1, ImGui::GetTextLineHeight() + 1 * gUi));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBg);
    beginPane(id, ImVec2(size.x, size.y > 0 ? size.y - ImGui::GetTextLineHeight() - 3 * gUi : 0));
}

void endModule() {
    endPane();
    const ImVec2 a0 = ImGui::GetItemRectMin(), a1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(a0.x, a1.y + 1), ImVec2(a1.x + 1, a1.y + 1), IM_COL32(255, 255, 255, 110));   // inset: light edge below and right
    dl->AddLine(ImVec2(a1.x + 1, a0.y), ImVec2(a1.x + 1, a1.y + 1), IM_COL32(255, 255, 255, 110));
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- variant 4: scope / cinema (the biggest canvas: controls fold away, stations are bookmark chips)
void chip(const char* label, bool sel, bool& clicked) {
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 sz(ts.x + 14 * gUi, ImGui::GetFrameHeight());
    clicked = ImGui::InvisibleButton(label, sz);
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (sel) dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 30));
    else if (hov) dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 14));
    dl->AddRect(p0, p1, u32(sel ? T.bright : T.edge));
    dl->AddText(ImVec2(p0.x + 7 * gUi, p0.y + (sz.y - ts.y) * 0.5f), u32(sel ? T.bright : T.text), label);
}

}  // namespace ui2
