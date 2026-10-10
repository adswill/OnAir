// The video pop-out as an OS window of its own (issues #20, #29): it can sit on another monitor, above other windows, and stays when the
// main window is minimised. It is drawn with the main window's graphics back end (the same Metal device, or an OpenGL context sharing the
// main one), from the main loop, with one ImGui context: its picture is a separate draw list handed to the renderer, so no ImGui window
// or viewport is involved and the main window's keyboard shortcuts are untouched (the window has no ImGui input callbacks).
#include "app.h"

bool gNoOsWindows = false;

namespace {

GLFWwindow* gPop = nullptr;
gfx::Surface* gSurf = nullptr;
ImDrawList* gDl = nullptr;
ImDrawData gDd;
bool gTop = true;                        // the floating state the window has now
int gWx = 0, gWy = 0, gWw = 0, gWh = 0;  // the windowed place while it is full screen
double gLastClick = -1;
bool gToggleFs = false, gLeaveFs = false;

void onMouse(GLFWwindow*, int button, int action, int) {
    if (button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS) return;
    const double t = glfwGetTime();
    if (gLastClick >= 0 && t - gLastClick < ImGui::GetIO().MouseDoubleClickTime + 0.1) { gToggleFs = true; gLastClick = -1; }
    else gLastClick = t;
}

void onKey(GLFWwindow*, int key, int, int action, int) {
    if (action != GLFW_PRESS) return;
    if (key == GLFW_KEY_ESCAPE) gLeaveFs = true;
    if (key == GLFW_KEY_F) gToggleFs = true;   // as F in the main window
}

void saveGeometry() {
    if (!gPop) return;
    int x = gWx, y = gWy, w = gWw, h = gWh;
    if (!glfwGetWindowMonitor(gPop)) { glfwGetWindowPos(gPop, &x, &y); glfwGetWindowSize(gPop, &w, &h); }
    if (w <= 0 || h <= 0) return;
    plat::Prefs& d = plat::prefs();
    d.setI("popX", x); d.setI("popY", y); d.setI("popW", w); d.setI("popH", h);
    d.flush();
}

void setFullscreen(bool on) {
    GLFWmonitor* cur = glfwGetWindowMonitor(gPop);
    if (on == (cur != nullptr)) return;
    if (!on) {
        glfwSetWindowMonitor(gPop, nullptr, gWx, gWy, gWw, gWh, 0);
        glfwSetWindowAttrib(gPop, GLFW_FLOATING, gTop ? GLFW_TRUE : GLFW_FALSE);
        return;
    }
    glfwGetWindowPos(gPop, &gWx, &gWy); glfwGetWindowSize(gPop, &gWw, &gWh);
    GLFWmonitor* m = monitorFor(gWx, gWy, gWw, gWh);   // the monitor the window is on
    const GLFWvidmode* vm = m ? glfwGetVideoMode(m) : nullptr;
    if (vm) glfwSetWindowMonitor(gPop, m, 0, 0, vm->width, vm->height, vm->refreshRate);
}

void closeWindow() {
    if (!gPop) return;
    saveGeometry();
    delete gSurf; gSurf = nullptr;
    glfwDestroyWindow(gPop); gPop = nullptr;
    gDd.Clear();
    gToggleFs = gLeaveFs = false; gLastClick = -1;
}

bool openWindow(const App& a) {
    plat::Prefs& d = plat::prefs();
    int w = (int)d.getI("popW", (long)(640 * gUi)), h = (int)d.getI("popH", (long)(380 * gUi));
    w = std::max(160, std::min(w, 8192)); h = std::max(90, std::min(h, 8192));
    glfwDefaultWindowHints();
    gfx::windowHints();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);       // placed first, then shown
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);  // the main window keeps the keyboard
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_FLOATING, a.popTop ? GLFW_TRUE : GLFW_FALSE);
    gPop = glfwCreateWindow(w, h, "OnAir video", nullptr, gGfx->shareWindow());
    glfwDefaultWindowHints();
    if (!gPop) return false;
    gTop = a.popTop;
    glfwSetWindowSizeLimits(gPop, 160, 90, GLFW_DONT_CARE, GLFW_DONT_CARE);
    if (d.has("popX") && d.has("popY")) {
        // back where it was, if that is still on a screen: a monitor unplugged since would leave it out of reach. At least a good part of
        // the window's top (its title bar) must lie in some monitor's work area, otherwise it goes to the middle of the primary one.
        const int x = (int)d.getI("popX", 0), y = (int)d.getI("popY", 0);
        int count = 0;
        GLFWmonitor** mons = glfwGetMonitors(&count);
        bool onScreen = false;
        for (int i = 0; i < count && !onScreen; i++) {
            int mx = 0, my = 0, mw = 0, mh = 0;
            glfwGetMonitorWorkarea(mons[i], &mx, &my, &mw, &mh);
            const int ox = std::min(x + w, mx + mw) - std::max(x, mx);
            onScreen = ox >= std::min(w, 100) && y >= my && y < my + mh - 40;
        }
        if (onScreen) glfwSetWindowPos(gPop, x, y);
        else if (GLFWmonitor* pm = glfwGetPrimaryMonitor()) {
            int mx = 0, my = 0, mw = 0, mh = 0;
            glfwGetMonitorWorkarea(pm, &mx, &my, &mw, &mh);
            if (mw > 0 && mh > 0) {
                w = std::min(w, mw); h = std::min(h, mh);
                glfwSetWindowSize(gPop, w, h);
                glfwSetWindowPos(gPop, mx + (mw - w) / 2, my + (mh - h) / 2);
            }
        }
    }
    glfwSetMouseButtonCallback(gPop, onMouse);
    glfwSetKeyCallback(gPop, onKey);
    gSurf = gGfx->createSurface(gPop);
    if (!gSurf) { glfwDestroyWindow(gPop); gPop = nullptr; return false; }
    glfwShowWindow(gPop);
    return true;
}

} // namespace

void popOutFrame(App& a) {
    if (gNoOsWindows) return;   // the old panel inside the main window (drawUI)
    if (gPop && glfwWindowShouldClose(gPop)) a.popOut = false;   // its close button
    if (!a.popOut) { closeWindow(); return; }
    if (!gPop && !openWindow(a)) { a.popOut = false; a.engine.log("could not open the video window"); return; }
    const bool fs = glfwGetWindowMonitor(gPop) != nullptr;
    if (gTop != a.popTop) { gTop = a.popTop; if (!fs) glfwSetWindowAttrib(gPop, GLFW_FLOATING, gTop ? GLFW_TRUE : GLFW_FALSE); }
    if (gToggleFs) setFullscreen(!fs);
    else if (gLeaveFs) setFullscreen(false);
    gToggleFs = gLeaveFs = false;

    int ww = 0, wh = 0, fw = 0, fh = 0;
    glfwGetWindowSize(gPop, &ww, &wh);
    glfwGetFramebufferSize(gPop, &fw, &fh);
    gDd.Clear();
    if (ww <= 0 || wh <= 0 || fw <= 0 || fh <= 0) return;   // minimised
    if (!gDl) gDl = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData());
    gDl->_ResetForNewFrame();
    gDl->PushClipRect(ImVec2(0, 0), ImVec2((float)ww, (float)wh));
    gDl->PushTexture(ImGui::GetIO().Fonts->TexRef);
    const VideoTex& v = a.video;
    if (v.has() && v.h > 0) {   // the whole picture at its display aspect, black bars around it
        const float ar = v.dar > 0.05 ? (float)v.dar : (float)v.w / v.h;
        float sw = (float)ww, sh = (float)wh;
        if (sw / sh > ar) sw = sh * ar; else sh = sw / ar;
        const ImVec2 p0(((float)ww - sw) * 0.5f, ((float)wh - sh) * 0.5f);
        gDl->AddImage(v.tex->texture(), p0, ImVec2(p0.x + sw, p0.y + sh));
    } else {
        const char* t = "no picture";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        gDl->AddText(ImVec2(((float)ww - ts.x) * 0.5f, ((float)wh - ts.y) * 0.5f), IM_COL32(128, 128, 128, 255), t);
    }
    gDl->PopTexture();
    gDl->PopClipRect();
    gDd.Valid = true;
    gDd.DisplayPos = ImVec2(0, 0);
    gDd.DisplaySize = ImVec2((float)ww, (float)wh);
    gDd.FramebufferScale = ImVec2((float)fw / ww, (float)fh / wh);   // per window: the monitor it is on may have another scale
    gDd.Textures = &ImGui::GetPlatformIO().Textures;
    gDd.AddDrawList(gDl);
}

void popOutPresent() {
    if (!gPop || !gSurf || !gDd.Valid) return;
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(gPop, &fw, &fh);
    gSurf->present(&gDd, fw, fh);
}

bool popOutShown() { return gPop && glfwGetWindowAttrib(gPop, GLFW_VISIBLE) && !glfwGetWindowAttrib(gPop, GLFW_ICONIFIED); }

void popOutShutdown() {
    closeWindow();
    if (gDl) { IM_DELETE(gDl); gDl = nullptr; }
}
