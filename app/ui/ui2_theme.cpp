// The new interface: its three palettes (terminal, instrument, mono), the accent colour, the Dark / Light / High
// contrast choice and applying the theme to ImGui's style. The shared declarations are in ui2_internal.h.
// Split from ui2.cpp; the shared declarations are in ui2_internal.h.
#include "dect2/common/time_source.h"
#include "ui2_internal.h"

using namespace ui2;

namespace ui2 {

UiTheme T;

void loadTheme(int n) {
    T = UiTheme();
    if (n == 1) {
        T.bg = ImVec4(0.165f, 0.168f, 0.172f, 1); T.rail = ImVec4(0.135f, 0.138f, 0.142f, 1); T.edge = ImVec4(0.33f, 0.335f, 0.34f, 1);
        T.text = ImVec4(0.86f, 0.87f, 0.88f, 1); T.dim = ImVec4(0.60f, 0.61f, 0.62f, 1); T.bright = ImVec4(1, 1, 1, 1); T.accent = ImVec4(0.90f, 0.91f, 0.92f, 1);
        T.frame = ImVec4(0.095f, 0.098f, 0.102f, 1); T.button = ImVec4(0.235f, 0.240f, 0.247f, 1); T.plotBg = ImVec4(0.015f, 0.016f, 0.018f, 1);
        const ImVec4 s[8] = {ImVec4(0.90f, 0.91f, 0.92f, 1), ImVec4(0.55f, 0.80f, 0.62f, 1), ImVec4(0.88f, 0.68f, 0.42f, 1), ImVec4(0.62f, 0.72f, 0.92f, 1), ImVec4(0.80f, 0.58f, 0.78f, 1), ImVec4(0.55f, 0.56f, 0.58f, 1), ImVec4(0.88f, 0.84f, 0.56f, 1), ImVec4(0.35f, 0.36f, 0.38f, 1)};
        for (int k = 0; k < 8; k++) T.series[k] = s[k];
        const ImU32 t[5] = {IM_COL32(120, 128, 138, 255), IM_COL32(58, 62, 68, 255), IM_COL32(84, 90, 98, 255), IM_COL32(170, 176, 184, 255), IM_COL32(225, 228, 232, 255)};
        for (int k = 0; k < 5; k++) T.tone[k] = t[k];
        T.flair = false;
    } else if (n == 2) {
        T.bg = ImVec4(0.075f, 0.075f, 0.075f, 1); T.rail = ImVec4(0.055f, 0.055f, 0.055f, 1); T.edge = ImVec4(0.22f, 0.22f, 0.22f, 1);
        T.text = ImVec4(0.84f, 0.84f, 0.84f, 1); T.dim = ImVec4(0.50f, 0.50f, 0.50f, 1); T.bright = ImVec4(1, 1, 1, 1); T.accent = ImVec4(0.92f, 0.92f, 0.92f, 1);
        T.frame = ImVec4(0.045f, 0.045f, 0.045f, 1); T.button = ImVec4(0.115f, 0.115f, 0.115f, 1); T.plotBg = ImVec4(0.03f, 0.03f, 0.03f, 1);
        const ImVec4 s[8] = {ImVec4(0.93f, 0.93f, 0.93f, 1), ImVec4(0.62f, 0.62f, 0.62f, 1), ImVec4(0.42f, 0.42f, 0.42f, 1), ImVec4(0.80f, 0.80f, 0.80f, 1), ImVec4(0.55f, 0.55f, 0.55f, 1), ImVec4(0.35f, 0.35f, 0.35f, 1), ImVec4(0.72f, 0.72f, 0.72f, 1), ImVec4(0.28f, 0.28f, 0.28f, 1)};
        for (int k = 0; k < 8; k++) T.series[k] = s[k];
        const ImU32 t[5] = {IM_COL32(150, 150, 150, 255), IM_COL32(70, 70, 70, 255), IM_COL32(100, 100, 100, 255), IM_COL32(170, 170, 170, 255), IM_COL32(235, 235, 235, 255)};
        for (int k = 0; k < 5; k++) T.tone[k] = t[k];
        T.flair = false;
    } else if (n == 3 || n == 4) {
        // Dev A (graphite, after a dark code editor) and Dev B (terminal): flat greys, ONE accent for what is selected, colour elsewhere only for meaning
        auto c8 = [](int r, int g, int b) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, 1); };
        if (n == 3) {
            T.bg = c8(30, 30, 30); T.rail = c8(37, 37, 38); T.edge = c8(60, 60, 60);
            T.text = c8(204, 204, 204); T.dim = c8(133, 133, 133); T.bright = c8(240, 240, 240); T.accent = c8(59, 130, 196);
            T.frame = c8(24, 24, 24); T.button = c8(51, 51, 52); T.plotBg = c8(18, 18, 18);
        } else {
            T.bg = c8(12, 12, 12); T.rail = c8(26, 26, 26); T.edge = c8(51, 51, 51);
            T.text = c8(200, 200, 200); T.dim = c8(120, 120, 120); T.bright = c8(235, 235, 235); T.accent = c8(127, 217, 98);
            T.frame = c8(8, 8, 8); T.button = c8(34, 34, 34); T.plotBg = c8(6, 6, 6);
        }
        const ImVec4 s[8] = {T.accent, c8(210, 160, 70), c8(200, 90, 80), c8(150, 150, 150), c8(120, 170, 210), c8(180, 130, 190), c8(110, 180, 150), c8(230, 230, 230)};
        for (int k = 0; k < 8; k++) T.series[k] = s[k];
        auto u = [](const ImVec4& v, float m) { return IM_COL32((int)(v.x * 255 * m), (int)(v.y * 255 * m), (int)(v.z * 255 * m), 255); };
        T.tone[0] = u(T.accent, 0.85f); T.tone[1] = u(T.accent, 0.30f); T.tone[2] = u(T.accent, 0.50f); T.tone[3] = u(T.accent, 0.75f); T.tone[4] = u(T.accent, 1.0f);
        T.flair = false;
    } else {
        T.bg = ImVec4(0.045f, 0.045f, 0.044f, 1); T.rail = ImVec4(0.037f, 0.037f, 0.036f, 1); T.edge = ImVec4(0.175f, 0.172f, 0.165f, 1);
        T.text = ImVec4(0.82f, 0.81f, 0.78f, 1); T.dim = ImVec4(0.46f, 0.45f, 0.42f, 1); T.bright = ImVec4(0.96f, 0.95f, 0.92f, 1); T.accent = ImVec4(0.86f, 0.65f, 0.30f, 1);
        T.frame = ImVec4(0.030f, 0.030f, 0.030f, 1); T.button = ImVec4(0.045f, 0.045f, 0.044f, 1); T.plotBg = ImVec4(0.030f, 0.030f, 0.030f, 1);
        const ImVec4 s[8] = {ImVec4(0.86f, 0.65f, 0.30f, 1), ImVec4(0.82f, 0.81f, 0.77f, 1), ImVec4(0.56f, 0.55f, 0.51f, 1), ImVec4(0.74f, 0.50f, 0.30f, 1), ImVec4(0.62f, 0.68f, 0.52f, 1), ImVec4(0.45f, 0.44f, 0.41f, 1), ImVec4(0.86f, 0.74f, 0.50f, 1), ImVec4(0.35f, 0.35f, 0.33f, 1)};
        for (int k = 0; k < 8; k++) T.series[k] = s[k];
        const ImU32 t[5] = {IM_COL32(166, 122, 46, 255), IM_COL32(86, 68, 36, 255), IM_COL32(110, 84, 36, 255), IM_COL32(168, 146, 104, 255), IM_COL32(232, 190, 112, 255)};
        for (int k = 0; k < 5; k++) T.tone[k] = t[k];
        T.flair = true;
    }
    for (int k = 0; k < 5; k++) pal::tones()[k] = T.tone[k];
    kit::syncTokens();
    kit::tokens().devLook = n >= 3;
}

}  // namespace ui2

// Classic or new colours and shapes. The classic style is captured once and restored when switching back.
static bool gAccentDirty = false;

static void applyUiThemeBase(App& a);

// the colour the user picked for highlights (Appearance > Accent colour); off = each palette's own
static struct { bool loaded = false, on = false; float c[3] = {0.45f, 0.72f, 0.82f}; } gUserAccent;

static void loadUserAccent() {
    if (gUserAccent.loaded) return;
    gUserAccent.loaded = true;
    plat::Prefs& p = plat::prefs();
    gUserAccent.on = p.getB("userAccentOn", false);
    for (int i = 0; i < 3; i++) { char k[24]; snprintf(k, sizeof k, "userAccent%d", i); gUserAccent.c[i] = (float)p.getD(k, gUserAccent.c[i]); }
}

static void saveUserAccent() {
    plat::Prefs& p = plat::prefs();
    p.setB("userAccentOn", gUserAccent.on);
    for (int i = 0; i < 3; i++) { char k[24]; snprintf(k, sizeof k, "userAccent%d", i); p.setD(k, gUserAccent.c[i]); }
}

namespace ui2 {

void accentMenu() {
    loadUserAccent();
    if (ImGui::Checkbox(tr("Own accent colour"), &gUserAccent.on)) { gAccentDirty = true; saveUserAccent(); }
    ImGui::SameLine();
    ImGui::BeginDisabled(!gUserAccent.on);
    if (ImGui::ColorEdit3("##useraccent", gUserAccent.c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) saveUserAccent();
    ImGui::EndDisabled();
}

// View > Font look: Current (the default), Clean, Compact. Saved now; the fonts are built at start-up, so it applies after a restart.
void typographyChoice(bool menu) {
    static const char* names[3] = {N_("Current"), N_("Clean"), N_("Compact")};
    const int cur = (int)plat::prefs().getI("uiTypography", 0);
    ImGui::TextDisabled("%s", tr("Font look (applies after a restart)"));
    for (int i = 0; i < 3; i++) {
        const bool hit = menu ? ImGui::MenuItem(tr(names[i]), nullptr, cur == i) : ImGui::RadioButton(tr(names[i]), cur == i);
        if (hit && i != cur) plat::prefs().setI("uiTypography", i);
    }
}

// Data folder (issue #383): where OnAir keeps maps, recordings and settings files. Chosen here, used from the next start; nothing is moved.
void dataFolderChoice() {
    static char buf[512];
    static bool init = false;
    if (!init) { init = true; snprintf(buf, sizeof buf, "%s", plat::dataDir().c_str()); }
    ImGui::TextDisabled("%s", tr("Data folder (applies after a restart; existing files are not moved)"));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##datafolder", buf, sizeof buf);
    if (ImGui::SmallButton(tr("Browse..."))) { const std::string d = plat::openFolderDialog(); if (!d.empty()) snprintf(buf, sizeof buf, "%s", d.c_str()); }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Use this folder"))) plat::setDataDir(buf);
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Default"))) { plat::setDataDir(""); snprintf(buf, sizeof buf, "%s", plat::defaultDataDir().c_str()); }
}

// View > Time display: every time shown or exported (message lists, logs, status bars) in UTC or local time. Saved data stays UTC.
void timeDisplayChoice(bool menu) {
    const bool local = dect2::timefmt::zone() == dect2::timefmt::Zone::Local;
    ImGui::TextDisabled("%s", tr("Time display"));
    auto pick = [&](const char* name, bool isLocal) {
        const bool hit = menu ? ImGui::MenuItem(tr(name), nullptr, local == isLocal) : ImGui::RadioButton(tr(name), local == isLocal);
        if (hit && local != isLocal) {
            dect2::timefmt::setZone(isLocal ? dect2::timefmt::Zone::Local : dect2::timefmt::Zone::Utc);
            plat::prefs().setB("timeLocal", isLocal);
        }
    };
    pick(N_("UTC"), false);
    pick(N_("Local (this computer)"), true);
    bool lab = dect2::timefmt::label();
    const bool hit = menu ? ImGui::MenuItem(tr("Show (UTC) / (local) after times"), nullptr, lab) : ImGui::Checkbox(tr("Show (UTC) / (local) after times"), &lab);
    if (hit) { if (menu) lab = !lab; dect2::timefmt::setLabel(lab); plat::prefs().setB("timeLabel", lab); }
    // #218: one clock for every decoder's logs and exports. The computer clock is read when a result is decoded, so lost samples and
    // Stop / Start cannot shift it; signal time counts from the start of the stream (files and the time machine: the same on every run).
    ImGui::TextDisabled("%s", tr("Timestamps in logs and exports"));
    const bool sig = dect2::timesrc::source() == dect2::timesrc::Source::Signal;
    auto pickSrc = [&](const char* name, bool isSig) {
        const bool h = menu ? ImGui::MenuItem(tr(name), nullptr, sig == isSig) : ImGui::RadioButton(tr(name), sig == isSig);
        if (h && sig != isSig) {
            dect2::timesrc::setSource(isSig ? dect2::timesrc::Source::Signal : dect2::timesrc::Source::Computer);
            plat::prefs().setB("timeFromSignal", isSig);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(isSig ? "Times counted from the start of the signal: repeatable for recorded files, but they fall behind the clock when samples are lost." : "The computer's clock, read when each result is decoded. Best for a live radio."));
    };
    pickSrc(N_("From the computer clock"), false);
    pickSrc(N_("From the signal time"), true);
}

// View > Theme: Dark (the palettes), Light (every colour's lightness turned over, main.cpp), High contrast (below)
void themeChoice(App& a, bool menu) {
    static const char* names[3] = {N_("Dark"), N_("Light (dark on white)"), N_("High contrast (black, white, yellow)")};
    const int cur = a.highContrast ? 2 : a.lightUi ? 1 : 0;
    for (int i = 0; i < 3; i++) {
        const bool hit = menu ? ImGui::MenuItem(tr(names[i]), nullptr, cur == i) : ImGui::RadioButton(tr(names[i]), cur == i);
        if (hit && i != cur) { a.lightUi = i == 1; a.highContrast = i == 2; savePrefs(a); }
    }
}

}  // namespace ui2

// High contrast: black panes, white text and rules, yellow for what is selected or focused, 2 px outlines on everything, a 3 px tab
// overline. Dim text drawn from hard-coded greys is lifted to 7:1 on black when the frame is drawn (main.cpp, contrast.h).
static void applyHighContrast() {
    const ImVec4 black(0, 0, 0, 1), white(1, 1, 1, 1), yellow(1.f, 0.90f, 0.f, 1), grey(0.07f, 0.07f, 0.07f, 1), hov(0.22f, 0.22f, 0.22f, 1), act(0.05f, 0.05f, 0.55f, 1);
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowBorderSize = s.FrameBorderSize = s.ChildBorderSize = s.PopupBorderSize = s.TabBarBorderSize = std::max(2.f, 2.f * gUi);
    s.TabBarOverlineSize = std::max(3.f, 3.f * gUi);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = black; c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_PopupBg] = black; c[ImGuiCol_MenuBarBg] = black;
    c[ImGuiCol_Border] = white; c[ImGuiCol_Separator] = white; c[ImGuiCol_SeparatorHovered] = yellow; c[ImGuiCol_SeparatorActive] = yellow;
    c[ImGuiCol_FrameBg] = grey; c[ImGuiCol_FrameBgHovered] = hov; c[ImGuiCol_FrameBgActive] = act;
    c[ImGuiCol_Button] = grey; c[ImGuiCol_ButtonHovered] = hov; c[ImGuiCol_ButtonActive] = act;
    c[ImGuiCol_Header] = ImVec4(0.14f, 0.14f, 0.14f, 1); c[ImGuiCol_HeaderHovered] = hov; c[ImGuiCol_HeaderActive] = act;
    c[ImGuiCol_Tab] = black; c[ImGuiCol_TabHovered] = hov; c[ImGuiCol_TabSelected] = ImVec4(0.16f, 0.16f, 0.16f, 1);
    c[ImGuiCol_TabDimmed] = black; c[ImGuiCol_TabDimmedSelected] = ImVec4(0.12f, 0.12f, 0.12f, 1); c[ImGuiCol_TabSelectedOverline] = yellow; c[ImGuiCol_TabDimmedSelectedOverline] = white;
    c[ImGuiCol_SliderGrab] = yellow; c[ImGuiCol_SliderGrabActive] = white; c[ImGuiCol_CheckMark] = yellow;
    c[ImGuiCol_ScrollbarBg] = black; c[ImGuiCol_ScrollbarGrab] = ImVec4(0.8f, 0.8f, 0.8f, 1); c[ImGuiCol_ScrollbarGrabHovered] = yellow; c[ImGuiCol_ScrollbarGrabActive] = white;
    c[ImGuiCol_ResizeGrip] = ImVec4(1.f, 0.90f, 0.f, 0.6f); c[ImGuiCol_ResizeGripHovered] = yellow; c[ImGuiCol_ResizeGripActive] = white;
    c[ImGuiCol_Text] = white; c[ImGuiCol_TextDisabled] = ImVec4(0.78f, 0.78f, 0.78f, 1); c[ImGuiCol_TextSelectedBg] = ImVec4(0.05f, 0.05f, 0.55f, 1);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.14f, 0.14f, 0.14f, 1); c[ImGuiCol_TableBorderStrong] = white; c[ImGuiCol_TableBorderLight] = ImVec4(0.6f, 0.6f, 0.6f, 1);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_NavCursor] = yellow; c[ImGuiCol_NavWindowingHighlight] = yellow;
    pal::accentRef() = yellow;
    plt::Style& p = plt::GetStyle();
    const ImVec4 ser[8] = {yellow, ImVec4(0.f, 0.9f, 1.f, 1), ImVec4(1.f, 0.45f, 0.45f, 1), ImVec4(0.4f, 1.f, 0.4f, 1), ImVec4(1.f, 0.5f, 1.f, 1), white, ImVec4(1.f, 0.65f, 0.f, 1), ImVec4(0.6f, 0.75f, 1.f, 1)};
    for (int i = 0; i < 8; i++) p.AutoColors[i] = ser[i];
    p.PlotBg = black; p.Border = white; p.Grid = ImVec4(0.8f, 0.8f, 0.8f, 0.40f); p.Text = white;
}

static void applyUiThemeAccent(App& a);
void applyUiTheme(App& a) {
    applyUiThemeBase(a);
    applyUiThemeAccent(a);
    pal::tabColours(ImGui::GetStyle().Colors);   // the selected tab is blue in every palette, with no bar above it
    touch::applyMetrics(ImGui::GetStyle());   // last, and only ever raises sizes, so it is safe to repeat every frame
}
static void applyUiThemeAccent(App& a) {
    loadUserAccent();
    if (a.highContrast) { applyHighContrast(); return; }   // its yellow, not the user's accent
    if (!gUserAccent.on) return;
    const ImVec4 ac(gUserAccent.c[0], gUserAccent.c[1], gUserAccent.c[2], 1.f);
    pal::accentRef() = ac;
    ImVec4* c = ImGui::GetStyle().Colors;
    c[ImGuiCol_CheckMark] = ac; c[ImGuiCol_SliderGrabActive] = ac; c[ImGuiCol_TabSelectedOverline] = ac;
    c[ImGuiCol_ButtonActive] = ImVec4(ac.x * 0.7f, ac.y * 0.7f, ac.z * 0.7f, 1.f);
    c[ImGuiCol_HeaderActive] = c[ImGuiCol_ButtonActive];
}

static void applyUiThemeBase(App& a) {
    pal::panel() = a.newUi && a.uiVariant == 7;   // round LEDs only in the Panel layout
    static ImGuiStyle classic;
    static plt::Style classicPlot;
    static ImVec4 classicAccent;
    static bool have = false;
    static int key = -1;
    static float scale = 0;
    static int epoch = -1;
    if (!have) { classic = ImGui::GetStyle(); classicPlot = plt::GetStyle(); classicAccent = pal::accentRef(); have = true; scale = gUi; }
    // the window went to a monitor with another display scale: main.cpp has just reset the style to the classic one at that scale
    // (taken again as the classic style), and the chosen theme is applied again with its sizes at the new scale
    if (gUi != scale || epoch != gStyleEpoch) { classic = ImGui::GetStyle(); classicPlot.Scale = gUi; scale = gUi; epoch = gStyleEpoch; key = -1; }
    if (gAccentDirty) { key = -1; gAccentDirty = false; }
    const int want = (a.newUi ? 1 + a.uiTheme : 0) + (a.highContrast ? 8 : 0);   // High contrast: its own key so leaving it restores the palette
    if (want == key) return;
    key = want;
    ImGui::GetStyle() = classic;
    plt::GetStyle() = classicPlot;
    pal::accentRef() = classicAccent;
    pal::dev() = a.newUi;
    if (a.newUi) loadTheme(a.uiTheme);
    pal::wfMode() = !a.newUi ? 0 : a.uiTheme == 2 ? 1 : 2;
    const bool devLook = a.newUi && a.uiTheme >= 3;
    a.wf.palette();
    // the colour map needs a tighter range to show the noise floor as dark; the classic range comes back when switching back
    if (a.newUi && a.wf.minDb == -100 && a.wf.maxDb == -32) { a.wf.minDb = -88; a.wf.maxDb = -36; }
    else if (!a.newUi && a.wf.minDb == -88 && a.wf.maxDb == -36) { a.wf.minDb = -100; a.wf.maxDb = -32; }
    if (!a.newUi) return;
    pal::accentRef() = kAccent;

    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0; s.FrameRounding = 1; s.ChildRounding = 0; s.PopupRounding = 0; s.TabRounding = 0; s.GrabRounding = 0; s.ScrollbarRounding = 0;
    s.FramePadding = ImVec2(5 * gUi, 1.5f * gUi); s.ItemSpacing = ImVec2(6 * gUi, 2.5f * gUi); s.CellPadding = ImVec2(4 * gUi, 1 * gUi);
    s.FrameBorderSize = 1; s.ChildBorderSize = 1; s.PopupBorderSize = 1; s.TabBarBorderSize = 1; s.TabBarOverlineSize = 2 * gUi;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = kBg; c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_PopupBg] = ImVec4(0.062f, 0.062f, 0.060f, 1);
    c[ImGuiCol_Border] = kEdge; c[ImGuiCol_Separator] = kEdge;
    c[ImGuiCol_FrameBg] = T.frame; c[ImGuiCol_FrameBgHovered] = ImVec4(T.frame.x + 0.05f, T.frame.y + 0.05f, T.frame.z + 0.05f, 1); c[ImGuiCol_FrameBgActive] = ImVec4(T.frame.x + 0.08f, T.frame.y + 0.08f, T.frame.z + 0.08f, 1);
    c[ImGuiCol_Button] = T.button; c[ImGuiCol_ButtonHovered] = ImVec4(T.button.x + 0.06f, T.button.y + 0.06f, T.button.z + 0.06f, 1); c[ImGuiCol_ButtonActive] = ImVec4(T.accent.x * 0.45f, T.accent.y * 0.45f, T.accent.z * 0.45f, 1);
    c[ImGuiCol_Header] = ImVec4(1, 0.95f, 0.85f, 0.06f); c[ImGuiCol_HeaderHovered] = ImVec4(1, 0.95f, 0.85f, 0.10f); c[ImGuiCol_HeaderActive] = ImVec4(0.86f, 0.65f, 0.30f, 0.30f);
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0); c[ImGuiCol_TabHovered] = ImVec4(1, 0.95f, 0.85f, 0.06f); c[ImGuiCol_TabSelected] = ImVec4(1, 0.95f, 0.85f, 0.05f);
    c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0); c[ImGuiCol_TabDimmedSelected] = ImVec4(1, 0.95f, 0.85f, 0.04f); c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_SliderGrab] = ImVec4(0.58f, 0.56f, 0.52f, 1); c[ImGuiCol_SliderGrabActive] = kAccent; c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_Text] = kText; c[ImGuiCol_TextDisabled] = kDim; c[ImGuiCol_MenuBarBg] = kRail;
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 0.95f, 0.85f, 0.02f); c[ImGuiCol_TableHeaderBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_TableBorderLight] = kEdge; c[ImGuiCol_TableBorderStrong] = kEdge;
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.86f, 0.65f, 0.30f, 0.30f);
    if (devLook) {   // Dev A / Dev B: tool-window look, flat fills, 1 px lines, tight rows, the accent marks the selection only
        auto fade = [](const ImVec4& v, float al) { return ImVec4(v.x, v.y, v.z, al); };
        auto mixc = [](const ImVec4& a, const ImVec4& b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1); };
        s.FrameRounding = 0; s.GrabRounding = 0; s.ScrollbarSize = 11 * gUi; s.GrabMinSize = 7 * gUi;
        s.FramePadding = ImVec2(5 * gUi, 2 * gUi); s.ItemSpacing = ImVec2(6 * gUi, 3 * gUi); s.TabBarOverlineSize = 2 * gUi; s.TabBarBorderSize = 1;
        c[ImGuiCol_PopupBg] = T.rail; c[ImGuiCol_MenuBarBg] = T.rail; c[ImGuiCol_WindowBg] = T.bg;
        c[ImGuiCol_TitleBg] = T.rail; c[ImGuiCol_TitleBgActive] = T.rail; c[ImGuiCol_TitleBgCollapsed] = T.rail;
        const ImVec4 hov = mixc(T.button, T.bright, 0.10f), sel = mixc(T.bg, T.accent, 0.22f);
        c[ImGuiCol_Button] = T.button; c[ImGuiCol_ButtonHovered] = hov; c[ImGuiCol_ButtonActive] = mixc(T.button, T.accent, 0.5f);
        c[ImGuiCol_FrameBgHovered] = mixc(T.frame, T.bright, 0.08f); c[ImGuiCol_FrameBgActive] = mixc(T.frame, T.accent, 0.25f);
        c[ImGuiCol_Header] = fade(T.accent, 0.20f); c[ImGuiCol_HeaderHovered] = fade(T.bright, 0.08f); c[ImGuiCol_HeaderActive] = fade(T.accent, 0.35f);
        c[ImGuiCol_Tab] = T.rail; c[ImGuiCol_TabHovered] = hov; c[ImGuiCol_TabSelected] = sel;   // editor tabs: flat, the selected one filled
        c[ImGuiCol_TabDimmed] = T.rail; c[ImGuiCol_TabDimmedSelected] = sel; c[ImGuiCol_TabSelectedOverline] = T.accent; c[ImGuiCol_TabDimmedSelectedOverline] = T.edge;
        c[ImGuiCol_SliderGrab] = mixc(T.dim, T.text, 0.4f); c[ImGuiCol_SliderGrabActive] = T.accent; c[ImGuiCol_CheckMark] = T.accent;
        c[ImGuiCol_ScrollbarBg] = T.bg; c[ImGuiCol_ScrollbarGrab] = T.button; c[ImGuiCol_ScrollbarGrabHovered] = hov; c[ImGuiCol_ScrollbarGrabActive] = T.accent;
        c[ImGuiCol_SeparatorHovered] = T.accent; c[ImGuiCol_SeparatorActive] = T.accent;
        c[ImGuiCol_TextSelectedBg] = fade(T.accent, 0.35f); c[ImGuiCol_TableRowBgAlt] = ImVec4(0, 0, 0, 0); c[ImGuiCol_TableHeaderBg] = T.rail;
        c[ImGuiCol_TableBorderLight] = fade(T.edge, 0.6f); c[ImGuiCol_TableBorderStrong] = T.edge;
        pal::accentRef() = T.accent;
    }
    plt::Style& p = plt::GetStyle();
    for (int i = 0; i < 8; i++) p.AutoColors[i] = T.series[i];
    p.PlotBg = T.plotBg; p.Border = kEdge; p.Grid = ImVec4(0.72f, 0.72f, 0.70f, 0.10f); p.Text = ImVec4(T.dim.x + 0.1f, T.dim.y + 0.1f, T.dim.z + 0.1f, 1);
}
