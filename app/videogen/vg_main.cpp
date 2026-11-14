// onair-videogen: the Digital video generator as its own program (#458, docs/VIDEO-GENERATOR.md, docs/VIDEOGEN-LINK.md).
//
//   onair-videogen [--onair host:port] [--token t] [--slot n] [--easy | --advanced]
//   OnAir's Tools > Digital video generator starts it with --onair 127.0.0.1:<port> --token <t>; started by hand it looks for OnAir
//   at 127.0.0.1:5460 and links when OnAir runs. Screenshots (the OnAir flags): --hidden --shot <png> [--shotdelay s], --tab <name>,
//   --sources n, --preview; the window size from ONAIR_WINSIZE (e.g. 1400x900).
//
// The window, graphics, fonts, settings and translations are OnAir's own (the onair_uikit library); the engine is dect2core.
#include "vg_state.h"
#include "vg_window.h"
#include "platform.h"
#include "render_caps.h"
#include "theme.h"
#include "touch.h"
#include "typography.h"
#include "cjk_fonts.h"
#include "imgui_impl_glfw.h"
#include "dect2/engine/crash_report.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

gfx::Backend* gGfx = nullptr;
bool gWayland = false;   // read by the OpenGL back end (window positions)

// The shared plots (plot.cpp) ask OnAir's touch interface for gestures: the generator has none
namespace touch {
bool on() { return false; }
const GestureFrame& gesture() { static const GestureFrame none{}; return none; }
bool longPressHeld() { return false; }
} // namespace touch

namespace {

struct Args {
    vgui::Options o;
    bool hidden = false, preview = false;
    std::string shot, tab;
    double shotDelay = 0;
    int sources = 0;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        const std::string k = argv[i];
        const bool more = i + 1 < argc;
        if (k == "--onair" && more) {
            const std::string v = argv[++i];
            const size_t c = v.rfind(':');
            a.o.host = c == std::string::npos ? v : v.substr(0, c);
            if (c != std::string::npos) a.o.port = std::clamp(atoi(v.c_str() + c + 1), 0, 65534);
        } else if (k == "--token" && more) a.o.token = std::string(argv[++i]).substr(0, 80);
        else if (k == "--slot" && more) a.o.slot = std::clamp(atoi(argv[++i]), 1, dect2::vgen::kMaxInstances);
        else if (k == "--easy") a.o.easy = true;
        else if (k == "--advanced") a.o.advanced = true;
        else if (k == "--hidden") a.hidden = true;
        else if (k == "--shot" && more) a.shot = argv[++i];
        else if (k == "--shotdelay" && more) a.shotDelay = std::max(0.0, atof(argv[++i]));
        else if (k == "--tab" && more) a.tab = argv[++i];
        else if (k == "--sources" && more) a.sources = std::clamp(atoi(argv[++i]), 1, dect2::vgen::kMaxSources);
        else if (k == "--preview") a.preview = true;
        else if (k == "--lang" && more) i++;
        // --mute, --no-first-run and others of OnAir's screenshot runs: nothing to do here
    }
    return a;
}

void mergeCjk(ImGuiIO& io, ImFont* dst) {
    for (const auto& f : plat::cjkFontCandidates(i18n::current())) {
        FILE* t = fopen(f.c_str(), "rb");
        if (!t) continue;
        fclose(t);
        ImFontConfig mc;
        mc.MergeMode = true;
        mc.DstFont = dst;
        io.Fonts->AddFontFromFileTTF(f.c_str(), dst->Sources[0]->SizePixels, &mc);
    }
}

void loadFonts(ImGuiIO& io) {
    if (typo::loadFonts(io, mergeCjk)) return;   // Inter (the Clean / Compact look)
    if (typo::loadEmbeddedMono(io, 13.0f, mergeCjk)) return;
    ImFont* f = io.Fonts->AddFontDefault();
    cjk::merge(io, f);
}

float displayScale(GLFWwindow* w) {
    float s = 1;
#ifndef __APPLE__
    float xs = 1, ys = 1;
    glfwGetWindowContentScale(w, &xs, &ys);
    s = std::max(1.f, std::max(xs, ys));
#endif
    if (const char* e = getenv("DECT2_UISCALE")) s = std::max(0.5f, (float)atof(e));
    return s;
}

} // namespace

int main(int argc, char** argv) {
    dect2::crash::install();
    const Args a = parseArgs(argc, argv);
    {   // OnAir's interface language (View > Language), else the system's; --lang wins (parsed again below)
        const std::string saved = plat::prefs().getS("uiLang", std::string());
        i18n::set(saved.empty() ? plat::uiLanguage() : saved);
        for (int i = 1; i + 1 < argc; i++) if (std::string(argv[i]) == "--lang") i18n::set(argv[i + 1]);
    }
    glfwSetErrorCallback([](int code, const char* text) { fprintf(stderr, "GLFW error %d: %s\n", code, text ? text : ""); });
#if defined(__linux__)
    if (!getenv("WAYLAND_DISPLAY")) glfwInitHint(0x00050003, 0x00060004);   // X11 when there is no Wayland (as OnAir)
#endif
    if (!glfwInit()) { plat::showFatalError("OnAir video generator", "No display: the generator needs a window."); return 1; }
    GLFWwindow* window = nullptr;
    int winW = 1240, winH = 860;
    if (const char* ws = getenv("ONAIR_WINSIZE")) sscanf(ws, "%dx%d", &winW, &winH);
    for (int lvl = 0; lvl < gfx::levelCount() && !window; lvl++) {
        gfx::setLevel(lvl);
        glfwDefaultWindowHints();
        gfx::windowHints();
        if (a.hidden) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        window = glfwCreateWindow(winW, winH, tr("OnAir - Digital video generator"), nullptr, nullptr);
    }
    if (!window) { plat::showFatalError("OnAir video generator", "Could not open a window with OpenGL 3.0 or newer."); return 1; }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    gGfx = gfx::create(window);
    if (!gGfx) { plat::showFatalError("OnAir video generator", "Could not start the graphics (OpenGL)."); return 1; }
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    {
        const long v = plat::prefs().getI("uiTypography", 0);   // OnAir's View > Font look
        typo::setVariant(v == 1 ? typo::Clean : v == 2 ? typo::Compact : typo::Classic);
    }
    applyTheme();
    typo::applyStyle(ImGui::GetStyle());
    const ImGuiStyle baseStyle = ImGui::GetStyle();
    ImGui_ImplGlfw_InitForOther(window, true);
    glfwSetDropCallback(window, [](GLFWwindow*, int n, const char** paths) { vgQueueDrop(n, paths); });
    loadFonts(io);
    if (!a.shot.empty()) rcaps::setUnpaced(true);

    vgui::Options o = a.o;
    vgui::init(o);
    if (a.sources > 0) vgui::ensureSources(a.sources);   // screenshots: at least this many sources
    if (a.preview) vgui::setPreview(true);
    if (!a.tab.empty()) vgui::selectTab(a.tab);

    bool userClosed = false, quit = false;
    int frameNo = 0;
    const double t0 = glfwGetTime();
    while (!quit) {
        if (glfwWindowShouldClose(window)) { userClosed = true; break; }
        if (a.shot.empty()) glfwWaitEventsTimeout(1.0 / 30);   // about 30 pictures a second: the preview needs no more
        else glfwPollEvents();
        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        if (w <= 0 || h <= 0) { if (vgui::frame().quit) quit = true; continue; }   // minimised: the link is still served
        const float s = displayScale(window);
        if (s != gUi) {
            gUi = s;
            ImGuiStyle& st = ImGui::GetStyle();
            st = baseStyle;
            st.ScaleAllSizes(s);
            st.FontScaleDpi = s;
        }
        static const float kClear[4] = {0.04f, 0.045f, 0.05f, 1.f};
        gGfx->newFrame(w, h, kClear);
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        const vgui::FrameResult r = vgui::frame();
        if (r.raise) { glfwShowWindow(window); glfwRestoreWindow(window); glfwFocusWindow(window); glfwRequestWindowAttention(window); }
        if (r.quit) quit = true;
        ImGui::Render();
        frameNo++;
        const bool shotNow = !a.shot.empty() && (a.shotDelay > 0 ? glfwGetTime() - t0 > a.shotDelay : frameNo == 400);
        gGfx->endFrame(ImGui::GetDrawData(), shotNow ? a.shot.c_str() : nullptr);
        if (shotNow) quit = true;
    }
    vgui::shutdown(userClosed);
    gGfx->shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    dect2::crash::startupDone();
    return 0;
}
