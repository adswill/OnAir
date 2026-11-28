// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Memory and Frequencies panels as OS windows of their own (issue #664): each can sit on a second screen. Unlike the video pop-out (popout.cpp, a bare draw
// list) the panel is interactive, so the window gets an ImGui context of its own that shares the main context's font atlas; the GLFW back end
// routes this window's mouse and keys to that context, and the picture goes to the window's surface like the pop-out's. Where it was and
// whether it is out are kept in the settings (memWin and freqWin, see ui/window_place.h); a monitor unplugged since brings it back to the first screen.
#include "../app.h"
#include "window_place.h"

namespace {

// One panel in its own window: its place, its OS window and surface, and its own ImGui context (they share the main font atlas).
struct Slot {
    const char* pref;        // settings key of the saved place
    const char* title;       // OS window title
    const char* id;          // ImGui window name inside it
    int defW, defH;          // size at 1x scale
    bool App::* open;        // the panel's "is open" flag
    void (*body)(App&);      // the panel's content
    winplace::Place place;
    bool loaded = false;
    GLFWwindow* win = nullptr;
    gfx::Surface* surf = nullptr;
    ImGuiContext* ctx = nullptr;
    ImDrawData* dd = nullptr;
};

Slot gSlots[2] = {
    {"memWin", "OnAir - Memory", "##memown", 720, 460, &App::memoryOpen, memoryPanelBody},
    {"freqWin", "OnAir - Frequencies", "##freqown", 820, 480, &App::freqOpen, freqdbPanelBody},
};

void load(Slot& s) {
    if (s.loaded) return;
    s.loaded = true;
    s.place = winplace::parse(plat::prefs().getS(s.pref, ""));
}

void save(Slot& s) {
    plat::prefs().setS(s.pref, winplace::write(s.place));
    plat::prefs().flush();
}

void saveGeometry(Slot& s) {
    if (!s.win) return;
    int x = s.place.r.x, y = s.place.r.y, w = 0, h = 0;
    if (!gWayland) { glfwGetWindowPos(s.win, &x, &y); s.place.placed = true; }   // (Wayland: the compositor places windows)
    glfwGetWindowSize(s.win, &w, &h);
    if (w > 0 && h > 0) s.place.r = winplace::Rect{x, y, w, h};
    save(s);
}

void closeWindow(Slot& s) {
    if (!s.win) return;
    saveGeometry(s);
    ImGuiContext* main = ImGui::GetCurrentContext();
    if (s.ctx) { ImGui::SetCurrentContext(s.ctx); ImGui_ImplGlfw_Shutdown(); ImGui::SetCurrentContext(main); ImGui::DestroyContext(s.ctx); s.ctx = nullptr; }
    delete s.surf; s.surf = nullptr;
    glfwDestroyWindow(s.win); s.win = nullptr;
    s.dd = nullptr;
}

bool openWindow(Slot& s) {
    const float sc = gUi;
    glfwDefaultWindowHints();
    gfx::windowHints();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);   // placed first, then shown
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    std::vector<winplace::Rect> mons;
    for (GLFWmonitor* m : monitors()) {
        winplace::Rect r;
        glfwGetMonitorWorkarea(m, &r.x, &r.y, &r.w, &r.h);
        if (r.w > 0 && r.h > 0) mons.push_back(r);
    }
    if (GLFWmonitor* pm = primaryMonitor()) {   // the primary monitor first: the fallback place
        winplace::Rect r;
        glfwGetMonitorWorkarea(pm, &r.x, &r.y, &r.w, &r.h);
        auto it = std::find_if(mons.begin(), mons.end(), [&](const winplace::Rect& q) { return q.x == r.x && q.y == r.y && q.w == r.w && q.h == r.h; });
        if (it != mons.end()) std::iter_swap(mons.begin(), it);
    }
    const winplace::Rect r = winplace::restore(s.place, gWayland ? std::vector<winplace::Rect>() : mons, (int)(s.defW * sc), (int)(s.defH * sc));
    s.win = glfwCreateWindow(r.w, r.h, s.title, nullptr, gGfx->shareWindow());
    glfwDefaultWindowHints();
    if (!s.win) return false;
    glfwSetWindowSizeLimits(s.win, winplace::kMinW, winplace::kMinH, GLFW_DONT_CARE, GLFW_DONT_CARE);
    if (!gWayland) glfwSetWindowPos(s.win, r.x, r.y);
    s.surf = gGfx->createSurface(s.win);
    if (!s.surf) { glfwDestroyWindow(s.win); s.win = nullptr; return false; }

    ImGuiContext* main = ImGui::GetCurrentContext();
    s.ctx = ImGui::CreateContext(ImGui::GetIO().Fonts);
    ImGuiIO& m = ImGui::GetIO();
    ImGui::SetCurrentContext(s.ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags = m.ConfigFlags;
    io.BackendFlags |= m.BackendFlags & (ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset);
    io.FontDefault = m.FontDefault;
    ImGui_ImplGlfw_InitForOther(s.win, true);
    ImGui::SetCurrentContext(main);
    glfwShowWindow(s.win);
    return true;
}

bool inOwn(Slot& s) { load(s); return s.place.out && !gNoOsWindows; }
void setOut(Slot& s, bool out) { load(s); if (s.place.out == out) return; s.place.out = out; save(s); }
bool shown(const Slot& s) { return s.win && glfwGetWindowAttrib(s.win, GLFW_VISIBLE) && !glfwGetWindowAttrib(s.win, GLFW_ICONIFIED); }

void frame(Slot& s, App& a) {
    load(s);
    if (s.win && glfwWindowShouldClose(s.win)) { s.place.out = false; closeWindow(s); save(s); return; }   // its close button: back into the main window
    const bool want = s.place.out && a.*s.open;
    if (!want) { closeWindow(s); return; }
    if (!s.win && !openWindow(s)) { s.place.out = false; save(s); a.engine.log(std::string("could not open the window ") + s.title); return; }

    int ww = 0, wh = 0, fw = 0, fh = 0;
    glfwGetWindowSize(s.win, &ww, &wh);
    glfwGetFramebufferSize(s.win, &fw, &fh);
    s.dd = nullptr;
    if (ww <= 0 || wh <= 0 || fw <= 0 || fh <= 0 || glfwGetWindowAttrib(s.win, GLFW_ICONIFIED)) return;

    ImGuiContext* main = ImGui::GetCurrentContext();
    ImGuiPlatformIO& mainPio = ImGui::GetPlatformIO();
    const ImGuiStyle mainStyle = ImGui::GetStyle();
    ImGui::SetCurrentContext(s.ctx);
    ImGui::GetStyle() = mainStyle;   // the main window's scale and colours, every frame
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    if (ImGui::Begin(s.id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        if (ImGui::SmallButton(tr("Put back"))) { s.place.out = false; save(s); }
        ImGui::Separator();
        s.body(a);
    }
    ImGui::End();
    ImGui::Render();
    s.dd = ImGui::GetDrawData();
    if (s.dd) s.dd->Textures = &mainPio.Textures;   // the textures are the main context's (one shared atlas)
    ImGui::SetCurrentContext(main);
}

void present(Slot& s) {
    if (!s.win || !s.surf || !s.dd) return;
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(s.win, &fw, &fh);
    s.surf->present(s.dd, fw, fh);
}

}  // namespace

bool memoryInOwnWindow() { return inOwn(gSlots[0]); }
void memorySetOwnWindow(bool out) { setOut(gSlots[0], out); }
bool freqdbInOwnWindow() { return inOwn(gSlots[1]); }
void freqdbSetOwnWindow(bool out) { setOut(gSlots[1], out); }

bool ownWindowShown() { return shown(gSlots[0]) || shown(gSlots[1]); }

void ownWindowFrame(App& a) {
    if (gNoOsWindows) return;
    for (Slot& s : gSlots) frame(s, a);
}

void ownWindowPresent() { for (Slot& s : gSlots) present(s); }

void ownWindowShutdown() { for (Slot& s : gSlots) closeWindow(s); }
