// small reusable widgets: pill buttons, lamps, gauges, scatter and history plots, formatting helpers
#include "../app.h"
#include "typography.h"
#include "touch.h"
#include "command_palette.h"
#include "imgui_internal.h"
#include "window_fit.h"

// Frequency step arrows: one step on press, then after 0.40 s one more step every 0.08 s.
// At most one step per frame so a hitch does not jump many channels.
bool freqStepArrow(const char* id, ImGuiDir dir) {
    constexpr float kDelay = 0.40f, kRate = 0.08f;
    ImGui::ArrowButton(id, dir);
    const ImGuiID item = ImGui::GetItemID();
    struct Hold { ImGuiID id = 0; float t0 = 0, last = 0; };
    static Hold h;
    const float now = (float)ImGui::GetTime();
    if (ImGui::IsItemClicked()) {
        h.id = item;
        h.t0 = h.last = now;
        return true;
    }
    if (ImGui::IsItemActive() && h.id == item && now - h.t0 >= kDelay && now - h.last >= kRate) {
        h.last = now;
        return true;
    }
    return false;
}

void restoreCursor(ImVec2 screenPos) {
    ImGui::SetCursorScreenPos(screenPos);
    // the spot came from the layout (GetCursorScreenPos after the last item): nothing is extended, so the check at End()/EndChild() is not wanted
    ImGui::GetCurrentWindow()->DC.IsSetPos = false;
}

void keepWindowsReachable() {
    ImGuiContext& g = *GImGui;
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    if (d.x < 64 || d.y < 64) return;   // minimised
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) return;   // not while the user moves one: it would fight the drag
    const float grab = 4 * ImGui::GetFrameHeight();
    for (ImGuiWindow* w : g.Windows) {
        if (!w->WasActive || w->Hidden) continue;
        if (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) continue;
        const winfit::V2 p0{w->Pos.x, w->Pos.y};
        const winfit::V2 p = winfit::reachablePos(p0, {w->Size.x, w->Size.y}, {d.x, d.y}, grab, w->TitleBarHeight);
        if (winfit::moved(p, p0)) ImGui::SetWindowPos(w, ImVec2(p.x, p.y), ImGuiCond_Always);
    }
}

bool comboTr(const char* label, int* v, std::initializer_list<const char*> items) {
    const char* t[32];
    int n = 0;
    for (const char* s : items) if (n < 32) t[n++] = tr(s);
    return ImGui::Combo(label, v, t, n);
}

ImVec2 fitButton(const char* label, float minW) {
    return ImVec2(std::max(minW, ImGui::CalcTextSize(label, nullptr, true).x + 2 * ImGui::GetStyle().FramePadding.x), 0);
}

bool runningButton(const App& a, const char* label, bool small) {
    const bool run = a.engine.running();
    ImGui::BeginDisabled(!run);
    const bool hit = small ? ImGui::SmallButton(label) : ImGui::Button(label);
    ImGui::EndDisabled();
    if (!run && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Start the receiver first"));
    return hit && run;
}

void firstWindowSize(float w, float h) {
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    const winfit::V2 s = d.x > 0 && d.y > 0 ? winfit::firstSize({w, h}, {d.x, d.y}, 24 * gUi) : winfit::V2{w, h};
    ImGui::SetNextWindowSize(ImVec2(s.x, s.y), ImGuiCond_FirstUseEver);
}

int gSpaceClaimFrame = -10;
bool gTightTabs = false;
void setUiLanguage(App& a, const std::string& code) {
    a.uiLang = code;
    i18n::set(code.empty() ? plat::uiLanguage() : code);
    savePrefs(a);
}

void languageMenuItems(App& a) {
    if (ImGui::MenuItem(tr("Same as the system"), nullptr, a.uiLang.empty())) setUiLanguage(a, "");
    ImGui::Separator();
    int n = 0;
    const i18n::Language* L = i18n::languages(n);
    for (int i = 0; i < n; i++) if (ImGui::MenuItem(L[i].name, nullptr, !a.uiLang.empty() && a.uiLang == L[i].code)) setUiLanguage(a, L[i].code);
}

void languageButton(App& a) {
    int n = 0;
    const i18n::Language* L = i18n::languages(n);
    const char* cur = L[0].name;
    for (int i = 0; i < n; i++) if (!strcmp(i18n::current(), L[i].code)) cur = L[i].name;
    char lbl[64];
    snprintf(lbl, sizeof lbl, "%s##lang", cur);
    if (ImGui::SmallButton(lbl)) ImGui::OpenPopup("##langpop");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Interface language"));
    if (ImGui::BeginPopup("##langpop")) { languageMenuItems(a); ImGui::EndPopup(); }
}

App* gTabOwner = nullptr;            // the receiver whose main tab bar is being drawn (mainTabs)
void* gMainTabBar = nullptr;         // that bar (ImGuiTabBar*), so that tabs of bars inside a tab are not mistaken for main tabs

bool tabItem(const char* name, Ic icon) {
    ImGuiTabItemFlags fl = 0;
    if (!gForceTab.empty() && gForceTab == name) { fl = ImGuiTabItemFlags_SetSelected; gForceTab.clear(); }
    // the label is padded with spaces to leave room for the icon, which is drawn over that gap
    const float gap = pal::dev() ? 0.f : iconSize() + 5.f;
    const int nSp = (int)std::ceil(gap / ImGui::CalcTextSize(" ").x);
    const std::string label = std::string(nSp, ' ') + tr(name) + "###" + name;
    // a narrow pane (gTightTabs, set by mainTabs() for its tab bar): less padding, so that every tab keeps its whole name in view
    if (gTightTabs) ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3 * gUi, ImGui::GetStyle().FramePadding.y));
    const bool open = typo::beginTabItem(label.c_str(), nullptr, fl);
    if (touch::on()) touch::noteTab(name, open);   // swipe between tabs
    if (open && gTabOwner && gMainTabBar && (void*)ImGui::GetCurrentTabBar() == gMainTabBar) gTabOwner->mainTabName = name;   // per receiver (#342)
    paletteNoteTab(name);                          // the command palette lists the tabs of this frame
    if (gTightTabs) ImGui::PopStyleVar();
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    if (!pal::dev()) icons::draw(icon, ImVec2(r0.x + ImGui::GetStyle().FramePadding.x + iconSize() * 0.5f + 1.f, (r0.y + r1.y) * 0.5f), iconSize() * 0.92f,
                open ? IM_COL32(255, 255, 255, 255) : IM_COL32(140, 154, 170, 255));
    return open;
}

namespace {
std::vector<GLFWmonitor*> gMons;
GLFWmonitor* gPrimaryMon = nullptr;
bool gMonsStale = true;
GLFWmonitorfun gPrevMonitorCb = nullptr;
void onMonitor(GLFWmonitor* m, int event) {
    gMonsStale = true;   // read again at the next use
    if (gPrevMonitorCb) gPrevMonitorCb(m, event);
}
}   // namespace

void watchMonitors() {
    gPrevMonitorCb = glfwSetMonitorCallback(onMonitor);
    gMonsStale = true;
}
const std::vector<GLFWmonitor*>& monitors() {
    if (gMonsStale) {
        gMonsStale = false;
        gMons.clear();
        int count = 0;
        if (GLFWmonitor** m = glfwGetMonitors(&count)) gMons.assign(m, m + std::max(count, 0));
        gPrimaryMon = gMons.empty() ? nullptr : glfwGetPrimaryMonitor();
    }
    return gMons;
}
GLFWmonitor* primaryMonitor() { monitors(); return gPrimaryMon; }

// The monitor holding most of the rectangle (screen coordinates), the primary one when it is on none. On Wayland a window's position is not
// known (glfwGetWindowPos does not work there): the primary one.
GLFWmonitor* monitorFor(int x, int y, int w, int h) {
    const auto& mons = monitors();
    GLFWmonitor* m = primaryMonitor();
    if (gWayland) return m;
    long best = 0;
    for (size_t i = 0; i < mons.size(); i++) {
        int mx = 0, my = 0;
        glfwGetMonitorPos(mons[i], &mx, &my);
        const GLFWvidmode* mv = glfwGetVideoMode(mons[i]);
        if (!mv) continue;
        const long ox = std::max(0, std::min(x + w, mx + mv->width) - std::max(x, mx));
        const long oy = std::max(0, std::min(y + h, my + mv->height) - std::max(y, my));
        if (ox * oy > best) { best = ox * oy; m = mons[i]; }
    }
    return m;
}

void toggleFullscreen() {
    if (!gWindow) return;
    if (glfwGetWindowMonitor(gWindow)) { glfwSetWindowMonitor(gWindow, nullptr, gWinX, gWinY, gWinW, gWinH, 0); return; }
    if (!gWayland) glfwGetWindowPos(gWindow, &gWinX, &gWinY);
    glfwGetWindowSize(gWindow, &gWinW, &gWinH);
    // Full screen on the monitor the window is on (the one holding most of it), not always the primary one: someone who drags OnAir to a
    // TV on the second output wants the picture there (issue #29).
    GLFWmonitor* m = monitorFor(gWinX, gWinY, gWinW, gWinH);
    if (!m) return;
    const GLFWvidmode* vm = glfwGetVideoMode(m);
    if (!vm) return;
    glfwSetWindowAttrib(gWindow, GLFW_AUTO_ICONIFY, GLFW_FALSE);  // stay up when focus moves (#71)
    glfwSetWindowMonitor(gWindow, m, 0, 0, vm->width, vm->height, vm->refreshRate);
}

void handleFullscreenHotkeys() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;   // F11 / Alt+Enter stay with the field while typing
    const bool f11 = ImGui::IsKeyPressed(ImGuiKey_F11, false);
    const bool altEnter = io.KeyAlt && !io.KeyCtrl && !io.KeySuper
        && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
    if (f11 || altEnter) toggleFullscreen();
}

bool pillButton(const char* label, bool selected, float padX) {
    ImGui::PushID(label);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    padX *= gUi;   // the padding is given in unscaled pixels
    const ImVec2 sz(ts.x + padX * 2, ImGui::GetFrameHeight() - 3 * gUi);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##pill", sz);
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = selected ? pal::remap(IM_COL32(52, 92, 108, 255)) : hov ? IM_COL32(46, 56, 68, 255) : IM_COL32(30, 35, 42, 255);
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), bg, pal::rnd(3.f * gUi));
    dl->AddText(ImVec2(p.x + padX, p.y + (sz.y - ts.y) * 0.5f), selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(176, 184, 194, 255), label);
    ImGui::PopID();
    return clicked;
}

// A row of pills that switches between sub-views of a tab. Returns the selected index.
int subNav(const char* id, int& cur, std::initializer_list<const char*> names) {
    {   // test aid (hidden GUI checks): DECT2_SUBNAV="adsbv=5" opens that pill the first time the row is drawn
        static std::map<std::string, bool> done;
        const char* e = getenv("DECT2_SUBNAV");
        const std::string key = id;
        if (e && !done[key]) { done[key] = true; const std::string v = e; const size_t q = v.find('='); if (q != std::string::npos && v.substr(0, q) == key) cur = atoi(v.c_str() + q + 1); }
    }
    ImGui::PushID(id);
    int i = 0;
    for (const char* n : names) { if (i) sameLineIf(ImGui::CalcTextSize(n).x + 22 * gUi, 6 * gUi); if (pillButton(n, cur == i)) cur = i; i++; }   // wraps in a narrow tab
    ImGui::PopID();
    ImGui::Spacing();
    return cur;
}

// Rows that wrap. The groups of a row are counted per window and frame; each one's width is kept in the window's storage for the next frame.
namespace {
struct FlowKeys { ImGuiID frame, n, x, y, same; };
FlowKeys flowKeys() { return {ImGui::GetID("##flowF"), ImGui::GetID("##flowN"), ImGui::GetID("##flowX"), ImGui::GetID("##flowY"), ImGui::GetID("##flowS")}; }
ImGuiID flowWidthKey(int k) { ImGui::PushID(k); const ImGuiID id = ImGui::GetID("##flowW"); ImGui::PopID(); return id; }
// keeps the width of the group that has just ended; returns its number
int flowMeasure(ImGuiStorage* st, const FlowKeys& k) {
    const int frame = ImGui::GetFrameCount();
    if (st->GetInt(k.frame, -1) != frame) { st->SetInt(k.frame, frame); st->SetInt(k.n, 0); st->SetFloat(k.y, -FLT_MAX); }
    const ImVec2 i0 = ImGui::GetItemRectMin(), i1 = ImGui::GetItemRectMax();
    const int n = st->GetInt(k.n, 0);
    const float gy = st->GetFloat(k.y, -FLT_MAX);
    const float gx = gy <= i0.y + 1 && gy >= i0.y - ImGui::GetFrameHeight() ? st->GetFloat(k.x, i0.x) : ImGui::GetCursorScreenPos().x;   // else a new row: from the line start
    st->SetFloat(flowWidthKey(n), i1.x - gx);
    return n;
}
bool flowFresh(ImGuiStorage* st, const FlowKeys& k) {   // nothing drawn since the last flowNext()
    return st->GetInt(k.frame, -1) == ImGui::GetFrameCount() && st->GetFloat(k.x, 0) == ImGui::GetCursorScreenPos().x && st->GetFloat(k.y, 0) == ImGui::GetCursorScreenPos().y;
}
}

bool flowNext(float spacing) {
    const float sp = spacing < 0 ? ImGui::GetStyle().ItemSpacing.x : spacing;
    ImGuiStorage* st = ImGui::GetStateStorage();
    const FlowKeys k = flowKeys();
    const int n = flowMeasure(st, k);
    const float need = st->GetFloat(flowWidthKey(n + 1), 0.f);
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    // a few pixels to spare: the width is last frame's, and a live value can grow by a digit since (the FM status line ran 9 px over)
    const float slack = 0.6f * ImGui::GetFontSize();
    const bool same = ImGui::GetItemRectMax().x + sp + need + slack <= right + 0.5f || need <= 0.f;
    if (same) ImGui::SameLine(0, sp);
    st->SetInt(k.n, n + 1);
    st->SetFloat(k.x, ImGui::GetCursorScreenPos().x);
    st->SetFloat(k.y, ImGui::GetCursorScreenPos().y);
    st->SetBool(k.same, same);
    return same;
}

void flowEnd() {
    ImGuiStorage* st = ImGui::GetStateStorage();
    const FlowKeys k = flowKeys();
    if (st->GetInt(k.frame, -1) != ImGui::GetFrameCount() || flowFresh(st, k)) return;
    // the next row in this window numbers its groups on from here: else its first group took this one's number and width
    // (the DVB-S lamps: "FEC" stood in for "Framing", which then did not wrap and ran past the edge)
    st->SetInt(k.n, flowMeasure(st, k) + 1);
}

void flowBreak() {
    ImGuiStorage* st = ImGui::GetStateStorage();
    const FlowKeys k = flowKeys();
    if (!flowFresh(st, k)) { flowEnd(); ImGui::NewLine(); }
    else if (st->GetBool(k.same, false)) ImGui::NewLine();
}

bool sameLineIf(float w, float spacing) {
    const float sp = spacing < 0 ? ImGui::GetStyle().ItemSpacing.x : spacing;
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (ImGui::GetItemRectMax().x + sp + w > right + 0.5f) return false;
    ImGui::SameLine(0, sp);
    return true;
}

StatusPanel::StatusPanel() {
    p = ImGui::GetCursorScreenPos();
    id = ImGui::GetID("##statuspanel");
    const float h = std::max(ImGui::GetFrameHeight() * 2.f + ImGui::GetStyle().ItemSpacing.y * 2.f, ImGui::GetStateStorage()->GetFloat(id, 0.f));
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - 4, p.y - 2), ImVec2(p.x + ImGui::GetContentRegionAvail().x + 4, p.y + h), IM_COL32(22, 23, 25, 255), 3.f);
}
StatusPanel::~StatusPanel() { ImGui::GetStateStorage()->SetFloat(id, ImGui::GetItemRectMax().y - p.y + ImGui::GetStyle().ItemSpacing.y); }

std::string ellipsize(std::string s, float w, float size) {
    ImFont* f = ImGui::GetFont();
    if (size <= 0) size = ImGui::GetFontSize();
    if (f->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()).x <= w) return s;
    while (!s.empty() && f->CalcTextSizeA(size, FLT_MAX, 0, (s + "...").c_str()).x > w) {
        s.pop_back();
        while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();   // not in the middle of a UTF-8 character
        if (!s.empty() && (unsigned char)s.back() >= 0xC0) s.pop_back();
    }
    return s.empty() ? s : s + "...";
}

std::string fitCaption(const std::string& s, float w) {
    if (ImGui::CalcTextSize(s.c_str()).x <= w) return s;
    const size_t k = s.find(" (");
    if (k != std::string::npos && ImGui::CalcTextSize(s.substr(0, k).c_str()).x <= w) return s.substr(0, k);
    return ellipsize(s, w);
}

void captionFit(float w, const char* fmt, ...) {
    char b[256];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    const std::string c = fitCaption(b, w);
    ImGui::TextDisabled("%s", c.c_str());
    if (c != b && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", b);
}

void kvColumn(float col) {
    // in screen space: after the key the cursor is at the start of the next line (the left of the window, the indent or the group)
    const float lineStart = ImGui::GetCursorScreenPos().x, keyEnd = ImGui::GetItemRectMax().x;
    const float want = lineStart + col - ImGui::GetCursorStartPos().x;   // where SameLine(col) put it
    const float x = std::max(keyEnd + 8 * gUi, std::min(want, lineStart + ImGui::GetContentRegionAvail().x * 0.5f));
    ImGui::SameLine(0, x - keyEnd);
}

// Small rounded label drawn at an absolute position; returns its width.
float tagAt(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 bg, ImU32 fg) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float w = ts.x + 10, h = ts.y + 2;
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), bg, pal::rnd(3.f));
    dl->AddText(ImVec2(pos.x + 5, pos.y + 1), fg, text);
    return w;
}

void gaugePill(float width, float frac, ImU32 fill, const char* text) {
    width = std::max(width, ImGui::CalcTextSize(text).x + 10 * gUi);   // never narrower than its text (a large display scale)
    const float h = ImGui::GetFrameHeight() - 2;
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.y += 1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), pal::dev() ? IM_COL32(8, 8, 8, 255) : IM_COL32(14, 16, 20, 255), pal::rnd(3.f));
    frac = std::min(1.f, std::max(0.f, frac));
    if (frac > 0.02f) dl->AddRectFilled(p, ImVec2(p.x + std::max(pal::dev() ? 1.f : h, width * frac), p.y + h), pal::remap(fill), pal::rnd(3.f));
    dl->AddRect(p, ImVec2(p.x + width, p.y + h), pal::dev() ? IM_COL32(44, 44, 42, 255) : IM_COL32(52, 58, 66, 255), pal::rnd(3.f));
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(p.x + (width - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), IM_COL32(240, 244, 248, 255), text);
    ImGui::Dummy(ImVec2(width, h));
}

void lamp(const char* label, int state /*0 grey 1 green 2 amber 3 red*/, int icon) {
    ImVec4 c = state == 1 ? pal::okGreen() : state == 2 ? pal::warnAmber()
             : state == 3 ? pal::badRed() : ImVec4(0.26f, 0.29f, 0.33f, 1);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const ImU32 cu = ImGui::ColorConvertFloat4ToU32(c);
    (void)icon;
    const float sq = 7.f, cy = p.y + ImGui::GetTextLineHeight() * 0.5f;
    if (pal::panel()) {   // a round LED in a dark bezel, with a small highlight when lit
        const ImVec2 c(p.x + 6.f * gUi, cy);
        dl->AddCircleFilled(c, 6.f * gUi, IM_COL32(8, 8, 8, 255));
        dl->AddCircle(c, 6.f * gUi, IM_COL32(70, 70, 68, 255), 0, 1.f);
        if (state) {
            dl->AddCircleFilled(c, 4.4f * gUi, cu);
            dl->AddCircleFilled(ImVec2(c.x - 1.4f * gUi, c.y - 1.6f * gUi), 1.5f * gUi, IM_COL32(255, 255, 255, 120));
        } else dl->AddCircleFilled(c, 4.4f * gUi, IM_COL32(30, 31, 31, 255));
        ImGui::Dummy(ImVec2(14.f * gUi, ImGui::GetTextLineHeight()));
    } else {
    if (state) dl->AddRectFilled(ImVec2(p.x + 1, cy - sq * 0.5f), ImVec2(p.x + 1 + sq, cy + sq * 0.5f), cu);
    else dl->AddRect(ImVec2(p.x + 1, cy - sq * 0.5f), ImVec2(p.x + 1 + sq, cy + sq * 0.5f), cu);
    ImGui::Dummy(ImVec2(sq + 5, ImGui::GetTextLineHeight()));
    }
    ImGui::SameLine(0, 0);
    ImGui::TextDisabled("%s", tr(label));
    static const struct { const char* k; const char* tip; } kTips[] = {
        {"IQ", N_("Sample stream. Green: the ADC level is in a healthy range. Amber: too low or high. Red: clipping.")},
        {"P1", N_("DVB-T2: the P1 preamble symbol (frame start and mode) is being found.")},
        {"GI", N_("Guard interval and symbol timing are locked.")},
        {"L1-pre", N_("DVB-T2: the L1 pre-signalling block (frame structure) decodes with a good CRC.")},
        {"L1-post", N_("DVB-T2: the L1 post-signalling block (PLP list, modulation) decodes with a good CRC.")},
        {"Frame", N_("A full T2 frame is being received and equalised.")},
        {"LDPC", N_("Forward error correction, inner code: share of blocks that decode.")},
        {"BCH", N_("Forward error correction, outer code: share of blocks that decode.")},
        {"TS", N_("Transport stream: services found, continuity errors counted.")},
        {"Video", N_("The player is decoding and showing pictures.")},
        {"Audio", N_("The player has audio buffered and playing.")},
        {"Sync", N_("DVB-T: OFDM symbol sync from the cyclic prefix.")},
        {"TPS", N_("DVB-T: the transmission parameter signalling bits are decoded.")},
        {"Chan", N_("Channel estimate from the pilots is valid.")},
        {"Viterbi", N_("DVB-T: the convolutional decoder found the packet sync.")},
        {"RS", N_("Reed-Solomon outer code: share of packets that are clean.")},
        {"Pilot", N_("ATSC: the 8-VSB pilot carrier is locked.")},
        {"Seg", N_("ATSC: data segment sync found.")},
        {"Field", N_("ATSC: field sync found.")},
        {"Eq", N_("ATSC: the equaliser is trained.")},
        {"Trellis", N_("ATSC: the trellis decoder produces a valid transport stream.")}};
    {
        const ImVec2 mx = ImGui::GetItemRectMax();
        if (ImGui::IsMouseHoveringRect(p, ImVec2(mx.x, p.y + ImGui::GetTextLineHeight()))) {
            for (auto& e : kTips) if (!strcmp(e.k, label)) { ImGui::SetTooltip("%s", tr(e.tip)); break; }
        }
    }
}

void scatter(const char* id, const std::vector<cf32>& pts, ImVec2 size, double lim, ImVec4 col) {
    if (plt::BeginPlot(id, size, plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_Equal)) {
        plt::SetupAxes(nullptr, nullptr, plt::AxisFlags_NoTickLabels, plt::AxisFlags_NoTickLabels);
        plt::SetupAxisLimits(plt::X1, -lim, lim, plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, -lim, lim, plt::Cond_Always);
        if (!pts.empty()) {
            plt::Spec sp;
            sp.Marker = plt::Marker_Circle; sp.MarkerSize = 1.6f; sp.Stride = sizeof(cf32);
            sp.MarkerFillColor = col; sp.MarkerLineColor = col; sp.LineColor = col;
            const float* d = reinterpret_cast<const float*>(pts.data());
            plt::PlotScatter("pts", d, d + 1, (int)pts.size(), sp);
        }
        plt::EndPlot();
    }
}

void historyPlot(const char* id, const char* ylabel, const std::deque<float>& h, ImVec2 size) {
    if (plt::BeginPlot(id, size, plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("samples (~30/s)", ylabel, 0, plt::AxisFlags_AutoFit);
        plt::SetupAxisLimits(plt::X1, 0, 600, plt::Cond_Always);
        if (!h.empty()) {
            std::vector<float> v(h.begin(), h.end());
            plt::Spec hs; hs.LineColor = pal::accent(); hs.FillFade = 0.9f;
            plt::PlotLine("h", v.data(), (int)v.size(), hs);
        }
        plt::EndPlot();
    }
}

std::string fmtLocal(int64_t utc, const char* f) {
    time_t t = (time_t)utc; struct tm m{}; dect2::localTime(t, &m);
    // a time that cannot be converted (zero or out-of-range from a damaged table) leaves negative fields: strftime() with those is undefined
    // and a debug runtime asserts on it (#74)
    if (m.tm_sec < 0 || m.tm_min < 0 || m.tm_hour < 0 || m.tm_mday < 1 || m.tm_mon < 0 || m.tm_year < 0 || m.tm_wday < 0 || m.tm_yday < 0) return "-";
    char b[48]; if (!strftime(b, sizeof b, f, &m)) return "-";
    return b;
}

const char* genreName(int g) {
    static const char* n[] = {"", "Movie / drama", "News / current affairs", "Show / game show", "Sports", "Children's / youth", "Music / ballet / dance", "Arts / culture", "Social / political / economics", "Education / science", "Leisure / hobbies"};
    return g >= 1 && g <= 10 ? n[g] : "";
}

const char* fmtKbps(char* b, size_t n, double k) { if (k >= 1000) snprintf(b, n, "%.2f Mbit/s", k / 1000); else snprintf(b, n, "%.0f kbit/s", k); return b; }

void qualityBar(App& a, float width) {
    QualityReport a3q;
    const bool a3 = a.atsc3Mode && atsc3Quality(a, a3q);
    const QualityReport& q = a3 ? a3q : a.quality.report();
    const bool run = a.engine.running();
    const float pct = run && q.valid ? (float)q.percent : 0.f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), pal::dev() ? IM_COL32(8, 8, 8, 255) : IM_COL32(120, 28, 28, 255), pal::rnd(3.f)); // red = nothing yet
    const float t = pct / 100.f;
    const ImVec4 lo(0.95f, 0.55f, 0.15f, 1), hi(0.25f, 0.85f, 0.35f, 1);
    const ImVec4 col = pal::dev() ? ImVec4(0.65f, 0.48f, 0.18f, 1) : ImVec4(lo.x + (hi.x - lo.x) * t, lo.y + (hi.y - lo.y) * t, lo.z + (hi.z - lo.z) * t, 1);   // the dev palette has no red-to-green blend
    if (pct > 0) dl->AddRectFilled(p, ImVec2(p.x + width * t, p.y + h), ImGui::ColorConvertFloat4ToU32(col), pal::rnd(3.f));
    dl->AddRect(p, ImVec2(p.x + width, p.y + h), pal::dev() ? IM_COL32(44, 44, 42, 255) : IM_COL32(70, 76, 84, 255), pal::rnd(3.f));
    char txt[96];
    if (!run || !q.valid) snprintf(txt, sizeof txt, tr("signal quality: %s"), run ? tr("no lock") : "-");
    else snprintf(txt, sizeof txt, tr("signal quality %.0f%%  %s"), q.percent, q.label.c_str());
    ImVec2 ts = ImGui::CalcTextSize(txt);
    dl->AddText(ImVec2(p.x + (width - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), IM_COL32(235, 238, 242, 255), txt);
    ImGui::Dummy(ImVec2(width, h));
    if (ImGui::IsItemHovered() && run && q.valid) {
        ImGui::BeginTooltip();
        if (a3) {
            ImGui::Text(tr("FEC blocks decoded in the last frame: %.1f%%"), q.fecOk * 100);
            ImGui::TextDisabled("%s", tr("ATSC 3.0 does not report a signal-to-noise ratio yet; this bar shows how much of the data decodes."));
        } else if (q.requiredDb <= 0) {   // the modes with no table of required SNR
            ImGui::Text(tr("data SNR %.1f dB, data decoded %.1f%% (last frames)"), q.snrDb, q.fecOk * 100);
            ImGui::TextDisabled("%s", tr("This bar shows how much of the recent data decodes."));
        } else {
            ImGui::Text(tr("data SNR %.1f dB, needed for this modulation/code rate about %.1f dB"), q.snrDb, q.requiredDb);
            ImGui::Text(tr("margin %+.1f dB, FEC blocks decoded %.1f%% (last frames)"), q.marginDb, q.fecOk * 100);
            ImGui::TextDisabled("%s", tr("0 dB margin is the edge of reception (25%); +6 dB or more is comfortable."));
        }
        ImGui::EndTooltip();
    }
}


std::string lossText(const SampleLoss& l) {
    auto dur = [](double s) { char b[32]; if (s < 1) snprintf(b, sizeof b, "%.0f ms", s * 1000); else snprintf(b, sizeof b, "%.1f s", s); return std::string(b); };
    std::string t;
    if (l.rateLow) {
        char b[96];
        snprintf(b, sizeof b, tr("%.1f%% of set rate"), l.deliveredPct);
        t = b;
    }
    if (l.radioEvents) t += (t.empty() ? "" : ", ") + dur(l.radioSec) + " " + tr("callback gaps");
    if (l.cpuEvents) t += (t.empty() ? "" : ", ") + dur(l.cpuSec) + " " + tr("ring overflow");
    return t.empty() ? "0" : t;
}

ImVec4 lossColour(const SampleLoss& l, ImVec4 normal) {
    if (l.rateLow) return pal::badRed();
    double since = -1;
    for (double s : {l.sinceRadioSec, l.sinceCpuSec}) if (s >= 0 && (since < 0 || s < since)) since = s;
    if (since < 0) return normal;
    return since < 30 ? pal::badRed() : pal::warnAmber();
}

void lossTooltip(const SampleLoss& l) {
    if (!ImGui::IsItemHovered()) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32);
    auto ago = [](double s) { char b[48]; if (s < 0) return std::string(tr("never")); if (s < 120) snprintf(b, sizeof b, tr("%.0f s ago"), s); else snprintf(b, sizeof b, tr("%.0f min ago"), s / 60); return std::string(b); };
    if (!l.live) ImGui::TextUnformatted(tr("Files and the test signal never lose samples: they wait for the receiver."));
    else {
        if (l.deliveredPct >= 0)
            ImGui::Text(tr("Arriving from the radio: %.1f%% of the set rate"), l.deliveredPct);
        else ImGui::TextUnformatted(tr("Arriving from the radio: not measured yet"));
        ImGui::TextDisabled("%s", l.rateLow
            ? (l.network ? tr("Below 97% of the rate the radio was set to: the network link or the radio's server is handing over less than asked. This is not the receiver's CPU load. Try a wired connection or a lower sample rate.")
                         : tr("Below 97% of the rate the radio was set to: the radio or its driver is handing over less than asked. This is not the receiver's CPU load. Try a USB port on the computer (no hub), a shorter cable, or a lower sample rate."))
            : tr("The sample rate actually handed over by the radio, over the last seconds. A warning appears below 97% of the set rate."));
        ImGui::Spacing();
        ImGui::Text(tr("Callback gaps: %.2f s in %llu gaps, last %s"), l.radioSec, (unsigned long long)l.radioEvents, ago(l.sinceRadioSec).c_str());
        ImGui::TextDisabled("%s", tr("Gaps between the library's callbacks: the radio or its driver delivered less than the set rate, so those samples never entered OnAir."));
        ImGui::Spacing();
        ImGui::Text(tr("Ring overflow: %.2f s discarded %llu times, last %s"), l.cpuSec, (unsigned long long)l.cpuEvents, ago(l.sinceCpuSec).c_str());
        ImGui::TextDisabled("%s", tr("OnAir did not read the input ring fast enough, so samples the radio had already delivered were thrown away. Close other programs, or pick a lower sample rate or a narrower channel."));
        ImGui::Spacing();
        ImGui::Text(tr("Receiver load: %.0f%% of real time"), l.loadPct);
        ImGui::TextDisabled("%s", tr("Processing time of a block versus its duration, on the sample thread. Near 100% the receiver is just keeping up. High load and lost samples are separate: load can be high with every sample kept, and samples can be lost while load is low."));
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

int hfDemodLoad(const char* prefKey) {
    const long v = plat::prefs().getI(prefKey, dect2::HfDemodAuto);
    return v >= 0 && v < dect2::HfDemodCount ? (int)v : dect2::HfDemodAuto;
}

bool hfDemodControl(App& a, int& choice, const char* prefKey, const char* id) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Demod"));
    sameLineIf(100 * gUi, 5 * gUi);
    char cur[32];
    if (choice == dect2::HfDemodAuto) snprintf(cur, sizeof cur, tr("Auto (%s)"), dect2::hfDemodName(dect2::hfDemodResolve(choice, a.freqMhz)));
    else snprintf(cur, sizeof cur, "%s", dect2::hfDemodName(choice));
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(std::min(110 * gUi, ImGui::GetContentRegionAvail().x));
    bool changed = false;
    if (ImGui::BeginCombo("##demod", cur)) {
        static const int kOrder[] = {dect2::HfDemodAuto, dect2::HfDemodUsb, dect2::HfDemodLsb, dect2::HfDemodNfm, dect2::HfDemodAm};
        for (const int d : kOrder)
            if (ImGui::Selectable(dect2::hfDemodName(d), choice == d) && choice != d) { choice = d; changed = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("How the signal becomes the audio the decoders hear.\nAuto: USB below 30 MHz, NFM at and above.\n"
                          "NFM: 12.5 kHz channel, no de-emphasis, 300 - 3000 Hz audio.\nISS SSTV on 145.800 MHz is FM."));
    ImGui::PopID();
    if (changed) plat::prefs().setI(prefKey, choice);
    return changed;
}

bool listenControls(App& a, bool& on, const char* prefKey, const char* id, const char* tip) {
    char lbl[48];
    snprintf(lbl, sizeof lbl, "%s##%s", tr("Listen"), id);
    const bool changed = ImGui::Checkbox(lbl, &on);
    if (changed) plat::prefs().setB(prefKey, on);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    if (!on) return changed;
    flowNext(6 * gUi);
    ImGui::PushID(id);
    if (iconFlat(a.muted ? Ic::Mute : Ic::Speaker, tr(a.muted ? "Unmute" : "Mute"))) a.muted = !a.muted;
    ImGui::SameLine(0, 4 * gUi);
    ImGui::SetNextItemWidth(90 * gUi);
    float vol = a.volume * 100.f;
    if (ImGui::SliderFloat("##vol", &vol, 0, 100, "%.0f %%")) a.volume = vol / 100.f;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Volume (shared with the other modes)"));
    ImGui::SameLine(0, 4 * gUi);
    audioControlButton(a);
    ImGui::SameLine(0, 4 * gUi);
    audioCleanupButton(a);
    ImGui::SameLine(0, 4 * gUi);
    voiceInversionButton(a);
    ImGui::PopID();
    return changed;
}
