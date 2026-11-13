// The touch interface's core: metrics, the per-frame gesture feed, long press = right click, tooltips on hold, kinetic scrolling and the
// window hook that makes popups bottom sheets and floating windows full screen on a narrow display. See touch.h for the map.
#include "touch.h"
#include "theme.h"
#include "scale.h"
#include "imgui_internal.h"
#include <cfloat>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

extern std::string gForceTab;   // main.cpp: select this tab next frame

namespace touch {

std::string gDevOpen;
void kineticScrollWindow(ImGuiWindow* w);

namespace {
bool gOn = false;
float gDpi = 1.f;
float gSize = 1.f;                   // Settings > Touch control size (touch_logic.h touchSizeFactor)
Gestures gRec;
GestureFrame gFrame;
bool gPressOnItem = false;
// drag to tune: a long press on a plot that claims it (claimLongPress) is held for tuning instead of becoming a right click
ImVec2 gClaimMin(1, 1), gClaimMax(0, 0), gClaimNextMin(1, 1), gClaimNextMax(0, 0);
bool gLongHeld = false;
double gHintUntil = 0;               // tooltips may show until then (a hold of 0.4 s starts it)
std::vector<Pointer> gFed;           // fingers a backend reported this frame
std::vector<std::pair<std::string, bool>> gTabsNow, gTabsLast;
std::unordered_map<ImGuiID, Kinetic> gKin;
ImGuiID gDragOwner = 0;              // the window the current drag may scroll
bool gDragOk = false;

bool tooltipGate() { return !gOn || ImGui::GetTime() < gHintUntil; }

// a sheet is wider than a phone-sized screen can fit comfortably only when the display is narrow: then it takes the whole width
bool narrow(const ImVec2& disp) { return disp.x < 1000.f * gDpi; }

// Every popup becomes a bottom sheet (context menus, small dialogs, modals); combos and menus keep dropping down from their widget.
// Titled floating windows (passes, scanner, gallery, map...) fill a narrow display. The window's own size is kept otherwise.
void placeWindow(ImGuiWindow* w) {
    if (!gOn) return;
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (!(disp.x > 0 && disp.y > 0)) return;
    const ImGuiWindowFlags f = w->Flags;
    const char* nm = w->Name;
    if (f & ImGuiWindowFlags_Popup) {
        if (!strncmp(nm, "##Combo_", 8) || !strncmp(nm, "##Menu_", 7) || (f & ImGuiWindowFlags_ChildMenu)) {
            const float need = target();   // a drop-down never narrower than a finger-sized row list, never off the screen
            w->Size.x = w->SizeFull.x = std::min(disp.x, std::max(w->SizeFull.x, need * 3));
            return;
        }
        if (!strncmp(nm, "tsheet_full_", 12)) { w->Pos = ImVec2(0, 0); w->Size = w->SizeFull = disp; return; }
        const float width = narrow(disp) ? disp.x : std::min(disp.x, std::max(w->SizeFull.x, 760.f * gDpi));
        const float h = std::min(w->SizeFull.y, disp.y * 0.9f);
        w->Size.x = w->SizeFull.x = width;
        w->Size.y = w->SizeFull.y = h;
        w->Pos = ImVec2(std::floor((disp.x - width) * 0.5f), disp.y - h);
        return;
    }
    if (!(f & ImGuiWindowFlags_NoTitleBar) && narrow(disp)) { w->Pos = ImVec2(0, 0); w->Size = w->SizeFull = disp; }
}
}   // namespace

bool on() { return gOn; }
float dpi() { return gDpi; }
void setDisplayScale(float d) { gDpi = std::max(0.5f, d); }
void setSizeFactor(float f) { gSize = std::max(0.5f, std::min(2.f, f)); }
float sizeFactor() { return gSize; }
const GestureFrame& gesture() { return gFrame; }
bool pressedOnItem() { return gPressOnItem; }

void setOn(bool v) {
    if (v == gOn) return;
    gOn = v;
    ImGuiTouchTooltipGate = v ? tooltipGate : nullptr;
    ImGuiTouchPlaceWindow = v ? placeWindow : nullptr;
    ImGuiTouchEndChild = v ? kineticScrollWindow : nullptr;
    if (!v) { ImGuiTouchRowMin = 0; gFrame = GestureFrame(); gRec.reset(); gKin.clear(); }
}

void applyMetrics(ImGuiStyle& st) {
    if (!gOn) return;
    const float d = gDpi * gSize, tgt = target(), fs = ImGui::GetFontSize();   // paddings follow the control size; Small never under 32 px targets
    auto up = [](float& v, float m) { v = std::max(v, m); };
    up(st.FramePadding.x, 12 * d); up(st.FramePadding.y, std::max(0.f, (tgt - fs) * 0.5f));   // a frame (button, input, combo, checkbox square) is >= 48 px tall
    up(st.ItemSpacing.x, 10 * d); up(st.ItemSpacing.y, 8 * d); up(st.ItemInnerSpacing.x, 8 * d);
    up(st.WindowPadding.x, 12 * d); up(st.WindowPadding.y, 10 * d);
    up(st.CellPadding.x, 8 * d); up(st.CellPadding.y, 8 * d);
    up(st.IndentSpacing, 28 * d);
    up(st.ScrollbarSize, 34 * d); up(st.GrabMinSize, tgt);   // the slider grab, and a scrollbar thumb a finger can hold
    up(st.TouchExtraPadding.x, 6 * d); up(st.TouchExtraPadding.y, 6 * d);
    up(st.FrameRounding, 6 * d); up(st.GrabRounding, 6 * d); up(st.ScrollbarRounding, 8 * d); up(st.PopupRounding, 10 * d); up(st.TabRounding, 6 * d);
    ImGuiTouchRowMin = tgt;   // menu, combo and list rows (Selectable) and small buttons: see imgui.h
}

void feedPointer(int id, float x, float y, bool down) {
    Pointer p; p.id = id; p.x = x; p.y = y; p.down = down;
    gFed.push_back(p);
}

void frame() {
    static int lastFrame = -1;
    if (ImGui::GetFrameCount() == lastFrame) return;
    lastFrame = ImGui::GetFrameCount();
    gTabsLast.swap(gTabsNow); gTabsNow.clear();
    if (!gOn) { gFrame = GestureFrame(); gFed.clear(); return; }
    ImGuiIO& io = ImGui::GetIO();
    std::vector<Pointer> p;
    if (!gFed.empty()) p = gFed;   // a backend with multi-touch: its fingers
    else { Pointer m; m.id = 0; m.x = io.MousePos.x; m.y = io.MousePos.y; m.down = io.MouseDown[0]; p.push_back(m); }   // otherwise the mouse / the emulated touch
    gFed.clear();
    const double now = ImGui::GetTime();
    gFrame = gRec.update(now, p.data(), (int)p.size());
    // a finger resting still shows the tooltip of what is under it for a moment (the replacement of hover)
    if (io.MouseDown[0] && io.MouseDownDuration[0] >= 0.4f && io.MouseDragMaxDistanceSqr[0] < 100.f) gHintUntil = std::max(gHintUntil, now + 1.6);
    gClaimMin = gClaimNextMin; gClaimMax = gClaimNextMax; gClaimNextMin = ImVec2(1, 1); gClaimNextMax = ImVec2(0, 0);   // what the plots claimed last frame
    if (!io.MouseDown[0]) gLongHeld = false;
    const bool claimed = gFrame.longPress && io.MousePos.x >= gClaimMin.x && io.MousePos.x <= gClaimMax.x && io.MousePos.y >= gClaimMin.y && io.MousePos.y <= gClaimMax.y;
    if (claimed) gLongHeld = true;   // the plot tunes with the finger; it opens its own menu when the finger lets go without moving
    else if (gFrame.longPress && !io.WantTextInput) {   // long press = right click: the left press is cancelled (moved off the screen first so the release is no click)
        const ImVec2 pos = io.MousePos;
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.AddMouseButtonEvent(0, false);
        io.AddMousePosEvent(pos.x, pos.y);
        io.AddMouseButtonEvent(1, true);
        io.AddMouseButtonEvent(1, false);
    }
}

void claimLongPress(ImVec2 mn, ImVec2 mx) {
    if (gClaimNextMin.x > gClaimNextMax.x) { gClaimNextMin = mn; gClaimNextMax = mx; return; }
    gClaimNextMin = ImVec2(std::min(gClaimNextMin.x, mn.x), std::min(gClaimNextMin.y, mn.y));
    gClaimNextMax = ImVec2(std::max(gClaimNextMax.x, mx.x), std::max(gClaimNextMax.y, mx.y));
}
bool longPressHeld() { return gOn && gLongHeld && ImGui::GetIO().MouseDown[0]; }

void endFrame() {
    if (gOn && ImGui::IsMouseClicked(0)) gPressOnItem = ImGui::IsAnyItemActive();
}

void noteTab(const char* name, bool open) { gTabsNow.emplace_back(name, open); }

void swipeTabs(int dir) {
    int cur = -1;
    for (size_t i = 0; i < gTabsLast.size(); i++) if (gTabsLast[i].second) cur = (int)i;
    if (cur < 0) return;
    const int n = cur + (dir < 0 ? -1 : 1);
    if (n >= 0 && n < (int)gTabsLast.size()) { gForceTab = gTabsLast[(size_t)n].first; }
}

// Drag-scroll and fling for the child window that is ending. A press on the background scrolls; on a row of a table or of the mode picker (items
// that are only tapped, never dragged) it scrolls too, and the row is not "clicked" because the finger has left it by the release. A press on
// a slider, a plot or a text box stays theirs. The innermost scrollable window under the finger claims the drag.
void kineticScrollWindow(ImGuiWindow* w) {
    if (!gOn) return;
    ImGuiIO& io = ImGui::GetIO();
    Kinetic& k = gKin[w->ID];
    static int claimFrame = -1;
    const bool rows = strstr(w->Name, "Table_") || strstr(w->Name, "##tmodes");
    if (w->ScrollMax.y > 0 && ImGui::IsMouseClicked(0) && claimFrame != ImGui::GetFrameCount() && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        claimFrame = ImGui::GetFrameCount();
        k.stop();
        gDragOwner = w->ID;
        gDragOk = rows || !ImGui::IsAnyItemActive();
    }
    const bool mine = gDragOwner == w->ID && gDragOk && w->ScrollMax.y > 0;
    if (mine && gFrame.panning) ImGui::SetScrollY(w, std::max(0.f, std::min(w->ScrollMax.y, w->Scroll.y - gFrame.panDy)));
    if (mine && !io.MouseDown[0] && gFrame.flickVy != 0) { k.fling(-gFrame.flickVy); gDragOwner = 0; }
    const float d = k.step(io.DeltaTime);
    if (d != 0) {
        const float y = w->Scroll.y + d;
        if (y <= 0 || y >= w->ScrollMax.y) k.stop();
        ImGui::SetScrollY(w, std::max(0.f, std::min(w->ScrollMax.y, y)));
    }
}

bool bigButton(const char* label, ImVec2 size, bool primary) {
    size.y = std::max(size.y, target());
    if (primary) {
        ImVec4 c = pal::accent(0.85f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c.x * 0.55f, c.y * 0.55f, c.z * 0.55f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c.x * 0.7f, c.y * 0.7f, c.z * 0.7f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(c.x * 0.9f, c.y * 0.9f, c.z * 0.9f, 1));
    }
    const bool r = ImGui::Button(label, size);
    if (primary) ImGui::PopStyleColor(3);
    return r;
}

}   // namespace touch
