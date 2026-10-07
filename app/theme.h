// Dark "instrument" theme: flat neutral greys, thin rules, one muted accent.
#pragma once
#include "imgui.h"
#include "plot.h"

// One accent colour for data and highlights; red / amber / green appear only where they mean a status.
namespace pal {
inline ImVec4& accentRef() { static ImVec4 v(0.45f, 0.72f, 0.82f, 1.f); return v; }   // the new interface recolours it per mode
inline ImVec4 accent(float a = 1.f) { ImVec4 c = accentRef(); c.w = a; return c; }
inline ImVec4 okGreen() { return ImVec4(0.40f, 0.76f, 0.52f, 1); }
inline ImVec4 warnAmber() { return ImVec4(0.90f, 0.70f, 0.28f, 1); }
inline ImVec4 badRed() { return ImVec4(0.88f, 0.38f, 0.34f, 1); }
inline bool& dev() { static bool v = false; return v; }
// heading text in panels: blue in the classic interface, soft white in the new one
inline ImVec4 heading() { return dev() ? ImVec4(0.92f, 0.90f, 0.84f, 1) : ImVec4(0.45f, 0.75f, 1, 1); }
inline bool& panel() { static bool v = false; return v; }   // the Panel layout: status lamps are round LEDs
inline int& wfMode() { static int m = 0; return m; }   // waterfall colours: 0 classic blue-teal, 1 plain grey, 2 turbo (the dev palettes)
inline float rnd(float r) { return dev() ? 0.f : r; }   // corner radius: none in the dev palette   // the new interface's palette: warm greys and one amber accent
// the classic interface's blue and teal fills, mapped to the dev palette when it is on
inline ImU32* tones() { static ImU32 t[5] = {IM_COL32(166, 122, 46, 255), IM_COL32(86, 68, 36, 255), IM_COL32(110, 84, 36, 255), IM_COL32(168, 146, 104, 255), IM_COL32(232, 190, 112, 255)}; return t; }
inline ImU32 remap(ImU32 c) {
    if (!dev()) return c;
    struct M { ImU32 from; int tone; };
    static const M m[] = {
        {IM_COL32(40, 112, 150, 255), 0}, {IM_COL32(52, 92, 108, 255), 1}, {IM_COL32(24, 106, 166, 255), 2}, {IM_COL32(32, 96, 140, 255), 1},
        {IM_COL32(44, 124, 178, 255), 2}, {IM_COL32(132, 158, 184, 255), 3}, {IM_COL32(115, 184, 209, 255), 4}, {IM_COL32(120, 200, 255, 255), 4},
        {IM_COL32(56, 94, 110, 255), 2}, {IM_COL32(40, 70, 82, 255), 1},
    };
    for (const M& x : m) if (x.from == c) return tones()[x.tone];
    return c;
}
inline ImVec4 grey(float a = 1.f) { return ImVec4(0.58f, 0.60f, 0.62f, a); }
}

inline void applyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    // raw instrument look: small rounded corners (3 px) on controls, thin 1 px rules, dense spacing, no decoration
    s.WindowRounding = 0; s.FrameRounding = 3; s.TabRounding = 3; s.ChildRounding = 4; s.GrabRounding = 2; s.PopupRounding = 4; s.ScrollbarRounding = 4;
    s.FramePadding = ImVec2(6, 2); s.ItemSpacing = ImVec2(6, 3); s.WindowPadding = ImVec2(8, 6); s.CellPadding = ImVec2(5, 1);
    s.FrameBorderSize = 1; s.ChildBorderSize = 1; s.PopupBorderSize = 1; s.TabBarBorderSize = 1; s.ScrollbarSize = 9; s.GrabMinSize = 5;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]      = ImVec4(0.055f, 0.058f, 0.062f, 1);
    c[ImGuiCol_ChildBg]       = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]       = ImVec4(0.075f, 0.078f, 0.083f, 1);
    c[ImGuiCol_Border]        = ImVec4(0.20f, 0.21f, 0.22f, 1);
    c[ImGuiCol_FrameBg]       = ImVec4(0.035f, 0.037f, 0.040f, 1);
    c[ImGuiCol_FrameBgHovered]= ImVec4(0.10f, 0.105f, 0.11f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.14f, 0.145f, 0.15f, 1);
    c[ImGuiCol_Button]        = ImVec4(0.10f, 0.105f, 0.11f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.17f, 0.18f, 0.19f, 1);
    c[ImGuiCol_ButtonActive]  = ImVec4(0.30f, 0.46f, 0.55f, 1);
    c[ImGuiCol_Header]        = ImVec4(0.13f, 0.14f, 0.15f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.17f, 0.18f, 0.19f, 1);
    c[ImGuiCol_HeaderActive]  = ImVec4(0.30f, 0.46f, 0.55f, 1);
    c[ImGuiCol_Tab]           = ImVec4(0.055f, 0.058f, 0.062f, 1);
    c[ImGuiCol_TabHovered]    = ImVec4(0.14f, 0.15f, 0.16f, 1);
    c[ImGuiCol_TabSelected]   = ImVec4(0.12f, 0.13f, 0.14f, 1);
    c[ImGuiCol_TabDimmed]     = ImVec4(0.055f, 0.058f, 0.062f, 1);
    c[ImGuiCol_TabDimmedSelected] = ImVec4(0.10f, 0.11f, 0.12f, 1);
    c[ImGuiCol_TabSelectedOverline] = ImVec4(0.45f, 0.72f, 0.82f, 1);
    c[ImGuiCol_SliderGrab]    = ImVec4(0.55f, 0.58f, 0.60f, 1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.72f, 0.82f, 1);
    c[ImGuiCol_CheckMark]     = ImVec4(0.45f, 0.72f, 0.82f, 1);
    c[ImGuiCol_Separator]     = ImVec4(0.20f, 0.21f, 0.22f, 1);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.02f);
    c[ImGuiCol_Text]          = ImVec4(0.82f, 0.84f, 0.85f, 1);
    c[ImGuiCol_TextDisabled]  = ImVec4(0.45f, 0.47f, 0.49f, 1);

    plt::Style& p = plt::GetStyle();   // the plots: dark plate, thin border, faint grid
    p.PlotBg  = ImVec4(0.02f, 0.021f, 0.023f, 1);
    p.Border  = ImVec4(0.24f, 0.25f, 0.26f, 1);
    p.Grid    = ImVec4(0.60f, 0.62f, 0.64f, 0.16f);
    p.Text    = ImVec4(0.62f, 0.64f, 0.66f, 1);
}
