// Docking for the new interface (View > Docking, issue #242). The vendored Dear ImGui has no docking branch, so the app docks its own
// windows: the body of the interface becomes a dock space laid out by a tree (dock_tree.h); the built-in panels (modes, spectrum,
// list, analysis, log) are windows drawn here, the floating tool windows (map, memory, passes ...) keep drawing themselves, and
// the window placement hook (the same one the touch interface uses; the two are never on together) puts each docked window into
// its box. Drag a window by its title onto a target to dock it, drag a docked window or tab out to float it again; the bars
// between boxes resize. The layout is kept per mode in that mode's layout file (state/layout.cpp) next to the floating positions;
// named layouts are in dataDir()/dock_layouts.txt. Off by default: everything floats as before.
#include "ui2_internal.h"
#include "dock_tree.h"
#include "../state/layout.h"
#include "../platform/platform.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace ui2;

namespace {

struct Builtin { const char* key; const char* title; };
const Builtin kBuiltins[] = {
    {"###dk_modes", N_("Modes")}, {"###dk_main", N_("Spectrum")}, {"###dk_list", N_("List")},
    {"###dk_analysis", N_("Analysis")}, {"###dk_log", N_("Log")},
};
const char* const kMain = "###dk_main";

dock::Node gTree;             // the current mode's layout
bool gSaved = false;          // the layout came from (or goes to) the mode's file; false: the default is used
std::set<std::string> gFloat; // built-in panels open as floating windows
bool gWasActive = false;

struct Frame {
    bool space = false;               // the dock space was laid out this frame
    dock::Rect r;
    std::vector<dock::LeafBox> leaves;
    std::vector<dock::SplitBar> bars;
    std::map<std::string, dock::Rect> place;   // docked window -> its rectangle
    std::map<std::string, ImGuiWindow*> win;   // top-level windows by key
} gF;

struct Pending { bool on = false; std::string name, target; dock::Zone z = dock::Zone::None; float share = 0.5f; } gPend;
std::string gFocus;           // the last focused dockable window (the keyboard commands act on it)
std::string gSaveName;        // View > Docking > Save layout as
std::vector<dock::Named> gNamed;
bool gNamedLoaded = false;

// a window's key: its fixed "###id" when it has one (the title may be translated), otherwise its whole name
std::string keyOf(const char* name) {
    const char* id = std::strstr(name, "###");
    return id ? std::string(id) : std::string(name);
}
std::string labelOf(const std::string& key, ImGuiWindow* w) {
    for (const Builtin& b : kBuiltins) if (key == b.key) return tr(b.title);
    const std::string n = w ? std::string(w->Name) : key;
    const size_t h = n.find("##");
    return h == std::string::npos ? n : (h == 0 ? n.substr(n.find_first_not_of('#')) : n.substr(0, h));
}
bool isBuiltin(const std::string& k) { for (const Builtin& b : kBuiltins) if (k == b.key) return true; return false; }

void dirty() { gSaved = true; if (ImGui::GetCurrentContext()) ImGui::MarkIniSettingsDirty(); }

dock::Node defaultTree() {
    dock::Node t;
    dock::dockAt(t, "", dock::Zone::Centre, kMain);
    dock::dockAt(t, kMain, dock::Zone::Right, "###dk_list", 0.27f);
    dock::dockAt(t, kMain, dock::Zone::Bottom, "###dk_analysis", 0.3f);
    dock::dockAt(t, "", dock::Zone::Left, "###dk_modes", 0.17f);
    return t;
}

// a window that may be docked: a titled, movable, resizable top-level window with saved settings (not a dialog, popup or tooltip)
bool dockable(ImGuiWindow* w) {
    if (!w) return false;
    if (w->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Modal | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) return false;
    return w->Name[0] && !(w->Name[0] == '#' && w->Name[1] == '#');
}

bool alive(const std::string& k) {
    if (isBuiltin(k)) return true;   // drawn by dockPanes() while docked
    const auto it = gF.win.find(k);
    return it != gF.win.end() && (it->second->WasActive || it->second->Active);
}

ImVec2 floatSize(ImGuiWindow* w) {
    if (ImGuiWindowSettings* s = ImGui::FindWindowSettingsByWindow(w)) if (s->Size.x > 40 && s->Size.y > 40) return ImVec2(s->Size.x, s->Size.y);
    return ImVec2(std::max(320.f * gUi, gF.r.w * 0.4f), std::max(240.f * gUi, gF.r.h * 0.45f));
}

// take a window out of the tree; it floats with its old floating size, the grab point kept under the mouse
void undock(const std::string& k, ImGuiWindow* w, bool grab) {
    if (!dock::remove(gTree, k)) return;
    if (isBuiltin(k)) gFloat.insert(k);
    dirty();
    if (!w) return;
    ImGuiContext& g = *GImGui;
    const ImVec2 sz = floatSize(w), m = ImGui::GetIO().MousePos;
    ImVec2 p;
    if (grab) {
        const float fx = std::max(0.f, std::min(1.f, (m.x - w->Pos.x) / std::max(1.f, w->Size.x)));
        p = ImVec2(std::floor(m.x - fx * sz.x), std::floor(m.y - ImGui::GetFrameHeight() * 0.5f));
    } else if (ImGuiWindowSettings* s = ImGui::FindWindowSettingsByWindow(w)) {
        p = ImVec2(s->Pos.x, s->Pos.y);
    } else {
        p = ImVec2(gF.r.x + (gF.r.w - sz.x) * 0.5f, gF.r.y + (gF.r.h - sz.y) * 0.5f);
    }
    w->Pos = p;
    w->Size = w->SizeFull = sz;
    if (grab && g.MovingWindow == w) g.ActiveIdClickOffset = ImVec2(m.x - p.x, m.y - p.y);
}

void place(ImGuiWindow* w) {
    ImGuiContext& g = *GImGui;
    const std::string k = keyOf(w->Name);
    if (!dock::contains(gTree, k)) return;
    const auto it = gF.place.find(k);
    // dragged out by its title (a drag inside the window does not undock it). ImGui has already moved the window this frame, so the
    // press is tested against the title bar where the window was docked
    if (it != gF.place.end() && g.MovingWindow == w && ImGui::IsMouseDragging(0, 10 * gUi) &&
        ImRect(it->second.x, it->second.y, it->second.x + it->second.w, it->second.y + w->TitleBarHeight).Contains(g.IO.MouseClickedPos[0])) {
        w->Pos = ImVec2(it->second.x, it->second.y);
        w->Size = ImVec2(it->second.w, it->second.h);
        undock(k, w, true);
        return;
    }
    if (!gF.space || it == gF.place.end()) { w->HiddenFramesCanSkipItems = 1; return; }   // another tab is shown, or no dock space this frame
    const dock::Rect& r = it->second;
    w->Pos = ImVec2(r.x, r.y);
    w->Size = w->SizeFull = ImVec2(std::max(1.f, r.w), std::max(1.f, r.h));
    w->Collapsed = false;
    w->Flags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;   // the floating size stays in the file
}

void collectWindows() {
    gF.win.clear();
    for (ImGuiWindow* w : GImGui->Windows) if (!(w->Flags & ImGuiWindowFlags_ChildWindow)) gF.win[keyOf(w->Name)] = w;
}

std::string modeKey(const App& a) {
    std::string sub;
    if (const auto* sm = curSub(a)) sub = sm->id;
    std::string f = layoutFileName(a.family, sub);
    return f.substr(0, f.size() - 4);
}
std::string namedPath() { return plat::dataDir() + "/dock_layouts.txt"; }
void loadNamed() {
    if (gNamedLoaded) return;
    gNamedLoaded = true;
    std::ifstream f(namedPath(), std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    gNamed = dock::parseNamed(ss.str());
}
void saveNamed() { std::ofstream f(namedPath(), std::ios::binary | std::ios::trunc); f << dock::writeNamed(gNamed); }

void applyTree(const dock::Node& t) {
    gTree = t;
    for (const Builtin& b : kBuiltins) if (dock::contains(gTree, b.key)) gFloat.erase(b.key);
    dirty();
}

// the keyboard commands (also in the menu)
void cmdEdge(dock::Zone z) { if (!gFocus.empty() && dock::dockAt(gTree, "", z, gFocus, 0.3f)) { gFloat.erase(gFocus); dirty(); } }
void cmdTab() {
    if (gFocus.empty()) return;
    std::string t = dock::contains(gTree, kMain) && gFocus != kMain ? kMain : std::string();
    if (t.empty()) for (const auto& p : dock::panels(gTree)) if (p != gFocus) { t = p; break; }
    if (dock::dockAt(gTree, t, dock::Zone::Centre, gFocus)) { gFloat.erase(gFocus); dirty(); }
}
void cmdUndock() { if (!gFocus.empty()) { auto it = gF.win.find(gFocus); undock(gFocus, it == gF.win.end() ? nullptr : it->second, false); } }
void cmdCycle(int dir) {
    if (gFocus.empty() || !dock::contains(gTree, gFocus)) return;
    const std::string n = dock::cycleTab(gTree, gFocus, dir, alive);
    if (n != gFocus && dock::setActive(gTree, n)) { gFocus = n; dirty(); auto it = gF.win.find(n); if (it != gF.win.end()) ImGui::FocusWindow(it->second); }
}

void drawTargets(const dock::Rect& r, bool outer, dock::Zone hot, float sz) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImU32 fill = u32(ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.55f)), hotFill = u32(ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.95f)), line = u32(kBright);
    for (dock::Zone z : {dock::Zone::Centre, dock::Zone::Left, dock::Zone::Right, dock::Zone::Top, dock::Zone::Bottom}) {
        if (outer && z == dock::Zone::Centre) continue;
        const dock::Rect q = dock::zoneRect(r, z, sz, outer);
        dl->AddRectFilled(ImVec2(q.x, q.y), ImVec2(q.x + q.w, q.y + q.h), z == hot ? hotFill : fill, 3 * gUi);
        dl->AddRect(ImVec2(q.x, q.y), ImVec2(q.x + q.w, q.y + q.h), line, 3 * gUi);
        const float c = q.w * 0.3f;   // a mark showing which side
        ImVec2 a(q.x + c, q.y + c), b(q.x + q.w - c, q.y + q.h - c);
        if (z == dock::Zone::Left) b.x = q.x + q.w * 0.5f;
        if (z == dock::Zone::Right) a.x = q.x + q.w * 0.5f;
        if (z == dock::Zone::Top) b.y = q.y + q.h * 0.5f;
        if (z == dock::Zone::Bottom) a.y = q.y + q.h * 0.5f;
        dl->AddRectFilled(a, b, line);
    }
}

// the settings handler: [OnAirDock][Layout] in the per-mode layout file
void* iniOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name) { return std::strcmp(name, "Layout") == 0 ? (void*)1 : nullptr; }
void iniLine(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line) {
    if (!std::strncmp(line, "tree=", 5)) { dock::Node n; if (dock::parse(line + 5, n)) { gTree = n; gSaved = true; } }
    else if (!std::strncmp(line, "float=", 6)) {
        gFloat.clear();
        std::string s = line + 6;
        size_t p = 0;
        while (p <= s.size()) { size_t e = s.find('|', p); if (e == std::string::npos) e = s.size(); std::string k = s.substr(p, e - p); if (isBuiltin(k)) gFloat.insert(k); p = e + 1; }
    }
}
void iniClear(ImGuiContext*, ImGuiSettingsHandler*) { gTree = dock::Node(); gFloat.clear(); gSaved = false; }
void iniWrite(ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* buf) {
    if (!gSaved) return;
    buf->appendf("[%s][Layout]\ntree=%s\nfloat=", h->TypeName, dock::serialise(gTree).c_str());
    bool first = true;
    for (const auto& k : gFloat) { buf->appendf("%s%s", first ? "" : "|", k.c_str()); first = false; }
    buf->append("\n\n");
}

}  // namespace

void dockDevTree(const char* text) { dock::Node t; if (dock::parse(text, t)) { gTree = t; gSaved = true; } }

bool dockActive(const App& a) { return a.dockOn && a.newUi && !a.touchUi; }

void dockRegisterIni() {
    static bool done = false;
    if (done || !ImGui::GetCurrentContext()) return;
    done = true;
    ImGuiSettingsHandler h;
    h.TypeName = "OnAirDock";
    h.TypeHash = ImHashStr("OnAirDock");
    h.ClearAllFn = iniClear;
    h.ReadOpenFn = iniOpen;
    h.ReadLineFn = iniLine;
    h.WriteAllFn = iniWrite;
    ImGui::AddSettingsHandler(&h);
}

void dockFrameBegin(App& a) {
    const bool on = dockActive(a);
    gF.space = false;
    gF.place.clear();
    if (on) ImGuiTouchPlaceWindow = place;
    else if (ImGuiTouchPlaceWindow == place) ImGuiTouchPlaceWindow = nullptr;
    if (gWasActive && !on) {   // docking turned off: the docked windows go back to where they floated
        collectWindows();
        for (const auto& k : dock::panels(gTree)) {
            const auto it = gF.win.find(k);
            if (it == gF.win.end()) continue;
            ImGuiWindow* w = it->second;
            if (ImGuiWindowSettings* s = ImGui::FindWindowSettingsByWindow(w)) { w->Pos = ImVec2(s->Pos.x, s->Pos.y); if (s->Size.x > 40) w->Size = w->SizeFull = ImVec2(s->Size.x, s->Size.y); }
        }
    }
    gWasActive = on;
}

namespace ui2 {

// the body of the interface with docking on: the status line, then the dock space
void dockBody(App& a, float bodyH) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * gUi, 4 * gUi));
    ImGui::BeginChild("##dockbody", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float gap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    if (sourceOptionsOpen(a)) { beginPane("##srcopt", ImVec2(0, 0), true); sourceOptions(a); endPane(); }
    beginPane("##status", ImVec2(0, 0), true);
    statusBar(a);
    endPane();
    ImGui::PopStyleVar();
    if (!gSaved && gTree.empty()) gTree = defaultTree();
    collectWindows();
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    gF.r = dock::Rect{p0.x, p0.y, std::max(1.f, av.x), std::max(1.f, av.y)};
    gF.space = true;
    dock::layout(gTree, gF.r, gap, alive, gF.leaves, gF.bars);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float th = ImGui::GetFrameHeight();
    for (const dock::LeafBox& b : gF.leaves) {
        dock::Rect wr = b.r;
        if (b.tabs.size() > 1) {   // a tab strip above the shown window
            wr.y += th; wr.h = std::max(1.f, wr.h - th);
            dl->AddRectFilled(ImVec2(b.r.x, b.r.y), ImVec2(b.r.x + b.r.w, b.r.y + th), u32(kRail));
            ImGui::PushClipRect(ImVec2(b.r.x, b.r.y), ImVec2(b.r.x + b.r.w, b.r.y + th), true);
            ImGui::SetCursorScreenPos(ImVec2(b.r.x, b.r.y));
            for (int i = 0; i < (int)b.tabs.size(); i++) {
                const std::string& k = b.tabs[i];
                const auto wit = gF.win.find(k);
                ImGuiWindow* w = wit == gF.win.end() ? nullptr : wit->second;
                const std::string lab = labelOf(k, w);
                ImGui::PushID(k.c_str());
                if (i) ImGui::SameLine(0, 2 * gUi);
                const float tw = ImGui::CalcTextSize(lab.c_str()).x + 16 * gUi;
                if (ImGui::Selectable(lab.c_str(), i == b.active, 0, ImVec2(tw, th - 2 * ImGui::GetStyle().FramePadding.y))) {
                    if (dock::setActive(gTree, k)) dirty();
                    gFocus = k;
                    if (w) ImGui::FocusWindow(w);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Click to show; drag out to float it. Ctrl+Alt+PageUp / PageDown: the next tab"));
                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 10 * gUi) && w) {   // a tab dragged out: it floats under the mouse and keeps moving
                    undock(k, w, true);
                    ImGui::StartMouseMovingWindow(w);
                    GImGui->ActiveIdClickOffset = ImVec2(ImGui::GetIO().MousePos.x - w->Pos.x, ImGui::GetIO().MousePos.y - w->Pos.y);
                }
                ImGui::PopID();
            }
            ImGui::PopClipRect();
        }
        if (!b.tabs.empty()) gF.place[b.tabs[b.active]] = wr;
    }
    for (const dock::SplitBar& s : gF.bars) {   // the bars between boxes: drag to resize
        std::string id = "##dkbar";
        for (int k : s.path) id += char('0' + k);
        ImGui::SetCursorScreenPos(ImVec2(s.r.x, s.r.y));
        ImGui::InvisibleButton(id.c_str(), ImVec2(std::max(1.f, s.r.w), std::max(1.f, s.r.h)));
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hot) ImGui::SetMouseCursor(s.vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive()) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            dock::setRatio(gTree, s.path, dock::ratioAt((s.vertical ? m.x : m.y) - gap * 0.5f, s.origin, s.len - gap, 80 * gUi));
        }
        if (ImGui::IsItemDeactivated()) dirty();
        if (hot) dl->AddRectFilled(ImVec2(s.r.x, s.r.y), ImVec2(s.r.x + s.r.w, s.r.y + s.r.h), u32(kAccent));
    }
    if (gF.leaves.empty()) {   // nothing docked
        const char* t = tr("Nothing is docked. Drag a window by its title onto a target here, or bring the panels back: View > Docking > Panels.");
        ImGui::SetCursorScreenPos(ImVec2(gF.r.x + 12 * gUi, gF.r.y + 12 * gUi));
        ImGui::PushTextWrapPos(gF.r.x + gF.r.w - 12 * gUi);
        ImGui::TextDisabled("%s", t);
        ImGui::PopTextWrapPos();
    }
    ImGui::SetCursorScreenPos(ImVec2(gF.r.x, gF.r.y));
    ImGui::Dummy(ImVec2(gF.r.w, gF.r.h));
    ImGui::EndChild();
}

// the built-in panels as windows (docked or floating); after the root window has ended
void dockPanes(App& a) {
    for (const Builtin& b : kBuiltins) {
        const bool docked = dock::contains(gTree, b.key);
        if (!docked && !gFloat.count(b.key)) continue;
        const std::string name = std::string(tr(b.title)) + b.key;
        if (!docked) ImGui::SetNextWindowSize(ImVec2(520 * gUi, 360 * gUi), ImGuiCond_FirstUseEver);
        bool open = true;
        const bool scrollable = !std::strcmp(b.key, "###dk_modes") || !std::strcmp(b.key, "###dk_log");
        if (ImGui::Begin(name.c_str(), &open, scrollable ? 0 : (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))) {
            if (!std::strcmp(b.key, "###dk_modes")) sidebar(a);
            else if (!std::strcmp(b.key, kMain)) { tgRect(a, TgMain); mainTabs(a); }
            else if (!std::strcmp(b.key, "###dk_list")) { tgRect(a, TgRight); rightPanel(a); }
            else if (!std::strcmp(b.key, "###dk_analysis")) {
                if (a.atsc3Mode) { a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0); ImGui::TextDisabled("%s", tr("No analysis plots in this mode.")); }
                else { tgRect(a, TgConst); constellationsTab(a); }
            } else logConsole(a, 0);
            ImGui::Dummy(ImVec2(0, 0));   // the panels end with SetCursorPos (they were children): an item so the window may grow to it
        }
        ImGui::End();
        if (!open) { dock::remove(gTree, b.key); gFloat.erase(b.key); dirty(); }
    }
}

void dockMenu(App& a) {
    // the shortcut text: ImGui swaps Cmd and Ctrl on macOS, so there the keys are Cmd+Option
    static std::string scBuf[12];
    static int scN = 0;
    auto sc = [](const char* key) { std::string& b = scBuf[scN++ % 12]; b = std::string(ImGui::GetIO().ConfigMacOSXBehaviors ? "Cmd+Opt+" : "Ctrl+Alt+") + key; return b.c_str(); };
    if (!ImGui::BeginMenu(tr("Docking"))) return;
    ImGui::BeginDisabled(a.touchUi);
    if (ImGui::MenuItem(tr("Use docking"), sc("D"), &a.dockOn)) savePrefs(a);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tr(a.touchUi ? "Not with the touch interface." : "Arrange the panels and tool windows into a docked layout: drag a window by its title onto a target. Off: everything floats as before."));
    ImGui::Separator();
    ImGui::BeginDisabled(!dockActive(a));
    if (ImGui::BeginMenu(tr("Panels"))) {
        for (const Builtin& b : kBuiltins) {
            const bool shown = dock::contains(gTree, b.key) || gFloat.count(b.key);
            if (ImGui::MenuItem(tr(b.title), nullptr, shown)) {
                if (shown) { dock::remove(gTree, b.key); gFloat.erase(b.key); }
                else if (!dock::dockAt(gTree, dock::contains(gTree, kMain) ? kMain : "", dock::Zone::Bottom, b.key, 0.3f)) dock::dockAt(gTree, "", dock::Zone::Bottom, b.key, 0.3f);
                dirty();
            }
        }
        ImGui::EndMenu();
    }
    {
        const auto it = gF.win.find(gFocus);
        const std::string lab = gFocus.empty() ? std::string(tr("(click a window first)")) : labelOf(gFocus, it == gF.win.end() ? nullptr : it->second);
        ImGui::BeginDisabled(gFocus.empty());
        if (ImGui::BeginMenu((std::string(tr("Window")) + ": " + lab + "###dkwin").c_str())) {
            if (ImGui::MenuItem(tr("Dock left"), sc("Left"))) cmdEdge(dock::Zone::Left);
            if (ImGui::MenuItem(tr("Dock right"), sc("Right"))) cmdEdge(dock::Zone::Right);
            if (ImGui::MenuItem(tr("Dock top"), sc("Up"))) cmdEdge(dock::Zone::Top);
            if (ImGui::MenuItem(tr("Dock bottom"), sc("Down"))) cmdEdge(dock::Zone::Bottom);
            if (ImGui::MenuItem(tr("Dock as a tab of the spectrum"), sc("Enter"))) cmdTab();
            const bool docked = dock::contains(gTree, gFocus);
            if (ImGui::MenuItem(tr("Undock"), sc("U"), false, docked)) cmdUndock();
            if (ImGui::MenuItem(tr("Next tab"), sc("PageDown"), false, docked)) cmdCycle(1);
            if (ImGui::MenuItem(tr("Previous tab"), sc("PageUp"), false, docked)) cmdCycle(-1);
            ImGui::EndMenu();
        }
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    loadNamed();
    const std::string mk = modeKey(a);
    if (ImGui::BeginMenu(tr("Save layout as"))) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s", gSaveName.c_str());
        ImGui::SetNextItemWidth(180 * gUi);
        const bool enter = ImGui::InputTextWithHint("##dkname", tr("name"), buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue);
        gSaveName = buf;
        ImGui::SameLine();
        if ((ImGui::Button(tr("Save")) || enter) && !gSaveName.empty()) {
            dock::putNamed(gNamed, mk, gSaveName, dock::serialise(gTree));
            saveNamed();
            gSaveName.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::TextDisabled("%s", tr("Kept for this mode; Load layout lists them."));
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Load layout"))) {
        int n = 0;
        for (const dock::Named& l : gNamed) if (l.mode == mk) { n++; if (ImGui::MenuItem(l.name.c_str())) { dock::Node t; if (dock::parse(l.tree, t)) applyTree(t); } }
        if (!n) ImGui::TextDisabled("%s", tr("None saved for this mode"));
        bool other = false;
        for (const dock::Named& l : gNamed) if (l.mode != mk) other = true;
        if (other && ImGui::BeginMenu(tr("From other modes"))) {
            for (const dock::Named& l : gNamed) if (l.mode != mk && ImGui::MenuItem((l.name + "  (" + l.mode + ")").c_str())) { dock::Node t; if (dock::parse(l.tree, t)) applyTree(t); }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Delete layout"))) {
        int n = 0;
        std::string drop;
        for (const dock::Named& l : gNamed) if (l.mode == mk) { n++; if (ImGui::MenuItem(l.name.c_str())) drop = l.name; }
        if (!drop.empty() && dock::dropNamed(gNamed, mk, drop)) saveNamed();
        if (!n) ImGui::TextDisabled("%s", tr("None saved for this mode"));
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem(tr("Undock all"))) {
        for (const auto& k : dock::panels(gTree)) { const auto it = gF.win.find(k); undock(k, it == gF.win.end() ? nullptr : it->second, false); }
    }
    if (ImGui::MenuItem(tr("Reset docked layout"))) { gFloat.clear(); applyTree(defaultTree()); }
    ImGui::EndDisabled();
    ImGui::EndMenu();
}

}  // namespace ui2

// --dockselftest: drives the mouse and keyboard through ImGui's input queue in a hidden run (drag to dock, drag out, the keyboard
// commands, named layouts, the per-mode file), prints one line per check and exits with the number of failures
namespace {
struct SelfTest { bool on = false; int f = 0, step = 0, t0 = 0, fails = 0; ImVec2 from, to; std::string saved; } gST;
void stCheck(bool ok, const char* m) { std::printf("dock selftest: %s %s\n", ok ? "ok  " : "FAIL", m); std::fflush(stdout); if (!ok) gST.fails++; }
bool stDrag(int rel) {   // press at `from`, move to `to` in steps, let go
    ImGuiIO& io = ImGui::GetIO();
    if (rel == 0) io.AddMousePosEvent(gST.from.x, gST.from.y);
    else if (rel == 1) io.AddMouseButtonEvent(0, true);
    else if (rel <= 13) { const float t = (rel - 1) / 12.f; io.AddMousePosEvent(gST.from.x + (gST.to.x - gST.from.x) * t, gST.from.y + (gST.to.y - gST.from.y) * t); }
    else if (rel == 15) io.AddMouseButtonEvent(0, false);
    return rel >= 19;
}
bool stChord(int rel, ImGuiKey k) {   // Ctrl+Alt+k
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiKey ctrl = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;   // ImGui swaps Cmd and Ctrl on macOS: io.KeyCtrl is Cmd there
    if (rel == 0) { io.AddKeyEvent(ctrl, true); io.AddKeyEvent(ImGuiMod_Alt, true); }
    else if (rel == 1) io.AddKeyEvent(k, true);
    else if (rel == 2) io.AddKeyEvent(k, false);
    else if (rel == 3) { io.AddKeyEvent(ctrl, false); io.AddKeyEvent(ImGuiMod_Alt, false); }
    return rel >= 5;
}
ImGuiWindow* stWin(const char* k) { const auto it = gF.win.find(k); return it == gF.win.end() ? nullptr : it->second; }
const dock::LeafBox* stLeaf(const char* k) { for (const auto& b : gF.leaves) for (const auto& t : b.tabs) if (t == k) return &b; return nullptr; }
ImVec2 stTitle(ImGuiWindow* w) { return ImVec2(w->Pos.x + 40 * gUi, w->Pos.y + ImGui::GetFrameHeight() * 0.5f); }
void stNext() { gST.step++; gST.t0 = gST.f; }

void selfTestFrame(App& a) {
    ImGuiContext& g = *GImGui;
    const int rel = gST.f++ - gST.t0;   // 0 on the first frame of a step
    ImGuiWindow* map = stWin("Map");
    switch (gST.step) {
    case 0:   // settle: the map open and floating, the default layout, no popup in the way
        a.mapOpen = true;
        if (g.OpenPopupStack.Size) ImGui::ClosePopupToLevel(0, false);
        if (gST.f < 240 || !map || !gF.space || !stLeaf(kMain)) return;
        gFloat.clear(); applyTree(defaultTree());
        map->Pos = ImVec2(gF.r.x + gF.r.w * 0.35f, gF.r.y + gF.r.h * 0.3f);
        stNext();
        return;
    case 1:   // drag the map's title onto the right target of the spectrum's box
        if (rel == 0) {
            const dock::LeafBox* b = stLeaf(kMain);
            const dock::Rect q = dock::zoneRect(b->r, dock::Zone::Right, 32 * gUi, false);
            gST.from = stTitle(map); gST.to = ImVec2(q.x + q.w * 0.5f, q.y + q.h * 0.5f);
        }
        if (!stDrag(rel)) return;
        stCheck(dock::contains(gTree, "Map") && dock::leafOf(gTree, "Map") != dock::leafOf(gTree, kMain), "dragged onto a target: docked beside the spectrum");
        stNext();
        return;
    case 2:   // let the layout settle, then drag it out by its title to a place without a target
        if (rel < 4) return;
        if (rel == 4) { const dock::LeafBox* b = stLeaf(kMain); gST.from = stTitle(map); gST.to = ImVec2(b->r.x + b->r.w * 0.25f, b->r.y + b->r.h * 0.3f); }   // away from every target
        if (!stDrag(rel - 4)) return;
        stCheck(!dock::contains(gTree, "Map"), "a docked window dragged by its title floats");
        stNext();
        return;
    case 3:   // now floating: drag it onto the list's centre target
        if (rel < 4) return;
        if (rel == 4) { const dock::LeafBox* b = stLeaf("###dk_list"); const dock::Rect q = dock::zoneRect(b->r, dock::Zone::Centre, 32 * gUi, false);
                        gST.from = stTitle(map); gST.to = ImVec2(q.x + q.w * 0.5f, q.y + q.h * 0.5f); }
        if (!stDrag(rel - 4)) return;
        stCheck(dock::leafOf(gTree, "Map") == dock::leafOf(gTree, "###dk_list") && dock::leafOf(gTree, "Map")->tabs.size() == 2, "centre target: a tab of the list");
        stNext();
        return;
    case 4:   // Ctrl+Alt+PageDown: the next tab
        if (rel < 4) return;
        if (rel == 4 && map) ImGui::FocusWindow(map);
        if (!stChord(rel - 4, ImGuiKey_PageDown)) return;
        { const dock::Node* l = dock::leafOf(gTree, "Map"); stCheck(l && l->tabs[l->active] == "###dk_list", "Ctrl+Alt+PageDown: the next tab"); }
        stNext();
        return;
    case 5:   // Ctrl+Alt+U, Ctrl+Alt+Left, Ctrl+Alt+Enter
        if (rel < 4) return;
        if (rel == 4 && map) ImGui::FocusWindow(map);
        if (rel < 10) { stChord(rel - 4, ImGuiKey_U); return; }
        if (rel == 10) stCheck(!dock::contains(gTree, "Map"), "Ctrl+Alt+U: undocked");
        if (rel == 12 && map) ImGui::FocusWindow(map);
        if (rel >= 12 && rel < 18) { stChord(rel - 12, ImGuiKey_LeftArrow); return; }
        if (rel == 18) stCheck(gTree.kind == dock::Node::SplitH && gTree.kids[0].kind == dock::Node::Leaf && gTree.kids[0].tabs[0] == "Map", "Ctrl+Alt+Left: docked along the left edge");
        if (rel == 20 && map) ImGui::FocusWindow(map);
        if (rel >= 20 && rel < 26) { stChord(rel - 20, ImGuiKey_Enter); return; }
        if (rel == 26) { stCheck(dock::leafOf(gTree, "Map") == dock::leafOf(gTree, kMain), "Ctrl+Alt+Enter: a tab of the spectrum"); stNext(); }
        return;
    case 6: {   // a splitter dragged
        if (rel < 4) return;
        if (rel == 4) {
            if (gF.bars.empty()) { stCheck(false, "a splitter to drag"); stNext(); return; }
            const dock::SplitBar& s = gF.bars[0];
            gST.saved = dock::serialise(gTree);
            gST.from = ImVec2(s.r.x + s.r.w * 0.5f, s.r.y + s.r.h * 0.5f);
            gST.to = s.vertical ? ImVec2(gST.from.x + 60 * gUi, gST.from.y) : ImVec2(gST.from.x, gST.from.y + 60 * gUi);
        }
        if (!stDrag(rel - 4)) return;
        stCheck(dock::serialise(gTree) != gST.saved, "a bar dragged: the split moved");
        stNext();
        return;
    }
    case 7: {   // named layouts and the per-mode file
        const std::string mk = modeKey(a), mine = dock::serialise(gTree);
        loadNamed();
        dock::putNamed(gNamed, mk, "selftest", mine);
        saveNamed();
        gNamedLoaded = false; gNamed.clear(); loadNamed();
        applyTree(defaultTree());
        bool found = false;
        for (const auto& l : gNamed) if (l.mode == mk && l.name == "selftest") { dock::Node t; found = dock::parse(l.tree, t); if (found) applyTree(t); }
        stCheck(found && dock::serialise(gTree) == mine, "named layout saved, read back from the file and loaded");
        const std::string ini = ImGui::SaveIniSettingsToMemory();
        ImGui::ClearIniSettings();
        const bool cleared = gTree.empty() && !gSaved;
        ImGui::LoadIniSettingsFromMemory(ini.c_str(), ini.size());
        stCheck(cleared && dock::serialise(gTree) == mine, "layout written to and read from the mode's layout file");
        if (dock::dropNamed(gNamed, mk, "selftest")) saveNamed();
        stNext();
        return;
    }
    case 8:   // docking off: the windows float again
        if (rel == 0) { a.dockOn = false; return; }
        if (rel < 4) return;
        stCheck(map && !(map->Flags & ImGuiWindowFlags_NoResize), "docking off: the map is an ordinary window again");
        std::printf("dock selftest: %d failed\n", gST.fails);
        std::fflush(stdout);
        std::exit(gST.fails ? 1 : 0);
    }
}
}  // namespace

void dockDevSelfTest() { gST.on = true; }

void dockFrameEnd(App& a) {
    struct Run { App& a; ~Run() { if (gST.on) selfTestFrame(a); } } run{a};
    ImGuiIO& io = ImGui::GetIO();
    if (!a.touchUi && a.newUi && !io.WantTextInput && io.KeyCtrl && io.KeyAlt && !io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_D, false)) { a.dockOn = !a.dockOn; savePrefs(a); }
    if (!dockActive(a)) { gPend.on = false; return; }
    ImGuiContext& g = *GImGui;
    if (!gF.space) collectWindows();
    {   // the window the keyboard commands act on: the focused dockable or docked window
        ImGuiWindow* nw = g.NavWindow ? g.NavWindow->RootWindow : nullptr;
        if (nw) { const std::string k = keyOf(nw->Name); if (dock::contains(gTree, k) || dockable(nw)) gFocus = k; }
    }
    if (gF.space && !io.WantTextInput && io.KeyCtrl && io.KeyAlt && !io.KeySuper) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) cmdEdge(dock::Zone::Left);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) cmdEdge(dock::Zone::Right);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) cmdEdge(dock::Zone::Top);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) cmdEdge(dock::Zone::Bottom);
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) cmdTab();
        if (ImGui::IsKeyPressed(ImGuiKey_U, false)) cmdUndock();
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) cmdCycle(1);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) cmdCycle(-1);
    }
    // a floating window being dragged: drop targets; it docks where the mouse is let go
    ImGuiWindow* mv = g.MovingWindow ? g.MovingWindow->RootWindow : nullptr;
    if (mv && gF.space && dockable(mv) && !dock::contains(gTree, keyOf(mv->Name))) {
        const float sz = 32 * gUi;
        const ImVec2 m = io.MousePos;
        Pending p;
        p.name = keyOf(mv->Name);
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        dock::Rect box = gF.r;
        if (gF.leaves.empty()) {   // an empty dock space takes it whole
            if (gF.r.contains(m.x, m.y)) { p.z = dock::Zone::Centre; }
            fg->AddRect(ImVec2(gF.r.x, gF.r.y), ImVec2(gF.r.x + gF.r.w, gF.r.y + gF.r.h), u32(kAccent), 0, 0, 2 * gUi);
        } else {
            p.z = dock::hitTest(gF.r, m.x, m.y, sz, true);
            p.share = 0.3f;
            for (const dock::LeafBox& b : gF.leaves) {
                if (!b.r.contains(m.x, m.y)) continue;
                const dock::Zone z = dock::hitTest(b.r, m.x, m.y, sz, false);
                if (p.z == dock::Zone::None && z != dock::Zone::None) { p.z = z; p.target = b.tabs[b.active]; p.share = 0.5f; box = b.r; }
                drawTargets(b.r, false, p.target.empty() ? dock::Zone::None : p.z, sz);
            }
            drawTargets(gF.r, true, p.target.empty() ? p.z : dock::Zone::None, sz);
        }
        if (p.z != dock::Zone::None) {
            const dock::Rect pr = dock::previewRect(box, p.z, p.share);
            fg->AddRectFilled(ImVec2(pr.x, pr.y), ImVec2(pr.x + pr.w, pr.y + pr.h), u32(ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.22f)));
            p.on = true;
        }
        gPend = p;
    } else if (!g.MovingWindow && gPend.on) {   // let go over a target
        if (dock::dockAt(gTree, gPend.target, gPend.z, gPend.name, gPend.share)) { gFloat.erase(gPend.name); gFocus = gPend.name; dirty(); }
        gPend.on = false;
    } else if (!g.MovingWindow) gPend.on = false;
    // the docked windows stay behind the floating ones, just above the interface's root window
    if (gF.space) {
        std::vector<ImGuiWindow*> docked;
        for (ImGuiWindow* w : g.Windows) if (!(w->Flags & ImGuiWindowFlags_ChildWindow) && dock::contains(gTree, keyOf(w->Name))) docked.push_back(w);
        for (auto it = docked.rbegin(); it != docked.rend(); ++it) ImGui::BringWindowToDisplayBack(*it);
        if (ImGuiWindow* root = ImGui::FindWindowByName("##root")) ImGui::BringWindowToDisplayBack(root);
    }
}
