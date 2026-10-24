// OnAir: the program entry. main() sets up the window, the graphics back end and the App, then runs the frame loop;
// the per-frame steps and the shutdown are small functions above it. The window and fonts are in main_window.cpp,
// start-up and the developer flags in main_start.cpp, the main layout in ui/draw_ui.cpp and ui/main_tabs.cpp.
#ifdef __linux__
#include <unistd.h>
#endif
#include "app.h"
#include "ui/contrast.h"
#include "transcribe/transcribe_app.h"
#include "state/tune_worker.h"
#include "ui/touch.h"
#include "dect2/source/iq_file.h"
#include "panels/gallery.h"
#include "dect2/engine/crash_report.h"
#include "platform/discord_rpc.h"
#include <chrono>
#include <cstdlib>
#if defined(__linux__)
#include <dlfcn.h>
#endif
#include "main_internal.h"
#include "gfx/render_caps.h"
#include "ui/typography.h"
#include "state/workspace.h"
#include "state/layout.h"
#include "ui/frame_pace.h"
#include "ui/perf_overlay.h"
#include "platform/power.h"
#include "dect2/power/power.h"

// code runs before the library's (the linker places the program's own objects first), so the check here comes in time.
#if defined(DECT2_REQUIRES_AVX2) && (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
namespace {
struct CpuGate {
    CpuGate() {
        __builtin_cpu_init();
        if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) return;
        plat::showFatalError("OnAir", "This version of OnAir needs a processor with AVX2 and FMA instructions (Intel from the 2013 \"Haswell\" "
                                      "generation on, AMD from about 2015). This computer's processor does not have them.");
        std::_Exit(1);
    }
} gCpuGate;
}
#endif

std::string gForceTab;
std::string gCrashNote;   // set when the last run crashed: shown once in a small window

gfx::Backend* gGfx = nullptr;

GLFWwindow* gWindow = nullptr;

int gWinX = 100, gWinY = 100, gWinW = 1500, gWinH = 900;

static discord::Rpc* gDiscord = nullptr;   // created on first use (View > Show in Discord)
static void discordTick(const App& a) {   // posts the wanted Rich Presence; the thread does the talking
    if (!a.discordOn || !*discord::appId()) { if (gDiscord) { discord::Presence off; gDiscord->set(off); } return; }
    if (!gDiscord) gDiscord = new discord::Rpc();
    discord::Presence p; p.enabled = true; p.running = a.engine.running();
    for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) { p.mode = kModes[i].name; break; }
    const int kind = a.devIdx >= 0 && a.devIdx < (int)a.devices.size() ? (int)a.devices[a.devIdx].kind : -1;
    if (kind == (int)DeviceInfo::File) p.freq = "File playback";
    else if (kind == (int)DeviceInfo::Synthetic) p.freq = "Synthetic signal";
    else if (a.discordFreq) { char b[32]; snprintf(b, sizeof b, "%.3f MHz", a.freqMhz); p.freq = b; }
    static discord::Presence prev; static long long start = 0;
    if (!start || prev.running != p.running || prev.mode != p.mode || prev.freq != p.freq)
        start = (long long)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    prev = p; p.startUnix = start;
    gDiscord->set(p);
}

// frame pacing: the screen is only redrawn as often as it can change (the policy and its rates: ui/frame_pace.h). Unpaced, the loop would
// follow the display (120 Hz on a ProMotion Mac) and draw the same spectrum several times over. Input wakes the loop at once; --shot keeps
// the unpaced loop its frame counts rely on. Returns the interval chosen (View > Performance overlay shows it).
struct PaceInfo { double interval = 0; bool battery = false, occluded = false; };
static PaceInfo paceFrame(GLFWwindow* window, App& app, const DevOpts& o, double& frameNext, double& lastInputT) {
    const double t = glfwGetTime();
    pace::State ps;
    ps.shot = o.shotPath != nullptr;
    ps.shown = glfwGetWindowAttrib(window, GLFW_VISIBLE) && !glfwGetWindowAttrib(window, GLFW_ICONIFIED);
    ps.occluded = ps.shown && ::power::windowOccluded(window);
    ps.popOut = popOutShown();
    ps.noOsWindows = gNoOsWindows;
    ps.reduceCpuHidden = app.reduceCpuHidden;
    ps.focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
    ps.recentInput = t - lastInputT < 1.0;
    ps.video = app.engine.player().selected() >= 0;
    ps.running = anyRunning();
    ps.liveWaterfall = ps.running && !app.wf.paused;   // a shown window with a live waterfall keeps 30 frames a second, focused or not (#246)
    ps.batterySaver = app.powerLowMode != 0;                                // View > Power > Low power mode (one setting for both)
    ps.onBattery = ps.batterySaver && dect2::power::lowPower();             // manual on, or automatic with the battery or serious heat
    ::power::setReceiving(ps.running);   // no App Nap while a receiver runs
    const double interval = pace::interval(ps);
    if (interval > 0 && frameNext > t) {
        glfwWaitEventsTimeout(frameNext - t);
        if (glfwGetTime() < frameNext - 0.002) lastInputT = glfwGetTime();                               // woken early: an event
    }
    frameNext = glfwGetTime() + interval;
    return {interval, ps.onBattery, ps.occluded};
}

// true when the main window has nothing to draw this frame (minimised, or hidden with the CPU saver); the pop-out keeps playing
static bool minimisedFrame(GLFWwindow* window, App& app, const DevOpts& o, int& w, int& h) {
    glfwGetFramebufferSize(window, &w, &h);
    const bool skipDraw = !o.shotPath && !gNoOsWindows && app.reduceCpuHidden && !glfwGetWindowAttrib(window, GLFW_VISIBLE);
                if (skipDraw || w == 0 || h == 0 || glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
                    // minimised: nothing to draw in the main window, but the pop-out video window stays and keeps playing
                    if (!popOutShown()) { glfwWaitEventsTimeout(0.1); return true; }
                    ImGui_ImplGlfw_NewFrame();
                    ImGui::NewFrame();
                    app.video.update(app.engine.player());
                    popOutFrame(app);
                    ImGui::EndFrame();
                    popOutPresent();
    return true;
                }
    return false;
}

// follow the monitor scale (and View > Text size); a style ImGui refuses is put back instead of stopping the app
static void applyDisplayScale(GLFWwindow* window, App& app, const ImGuiStyle& baseStyle) {
                    // there DECT2_UISCALE (dev) still sets it, to see how a Windows laptop at 125 or 150 % lays out
                    float s = 1;
    #ifndef __APPLE__
                    float xs = 1, ys = 1;
                    glfwGetWindowContentScale(window, &xs, &ys);
                    s = std::max(1.f, std::max(xs, ys));
    #endif
                    if (const char* e = getenv("DECT2_UISCALE")) s = std::max(0.5f, (float)atof(e));
                    touch::setDisplayScale(s);   // 48 px targets are measured at the display scale, not the text scale
                    s *= app.uiTextScale;   // View > Text size
                    touch::setOn(app.touchUi);
                    if (app.touchUi) s *= app.touchScale;   // the touch interface's larger text
                    static bool lastTouch = false;
                    if (s != gUi || lastTouch != app.touchUi) {
                        lastTouch = app.touchUi;
                        gStyleEpoch++;
                        gUi = s;
                        ImGuiStyle& st = ImGui::GetStyle();
                        st = baseStyle;
                        st.ScaleAllSizes(s);
                        st.FontScaleDpi = s;
                        plt::GetStyle().Scale = s;
                    }
    ImGuiStyle& st = ImGui::GetStyle();
    if (!(st.WindowMinSize.x >= 1)) st.WindowMinSize.x = 32 * gUi;
    if (!(st.WindowMinSize.y >= 1)) st.WindowMinSize.y = 32 * gUi;
}

// text size: Ctrl +, Ctrl - and Ctrl 0 (Cmd on a Mac), in either interface
static void textSizeKeys(App& app) {
    handleFullscreenHotkeys();   // F11 / Alt+Enter: the whole window, either interface (not while typing)
    ImGuiIO& kio = ImGui::GetIO();
    if ((kio.KeyCtrl || kio.KeySuper) && !kio.WantTextInput) {
        float t = app.uiTextScale;
        if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) t = std::min(2.f, std::round(t * 10 + 1) / 10.f);
        if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) t = std::max(0.8f, std::round(t * 10 - 1) / 10.f);
        if (ImGui::IsKeyPressed(ImGuiKey_0, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad0, false)) t = 1.f;
        if (t != app.uiTextScale) { app.uiTextScale = t; savePrefs(app); }
    }
}

// the developer flags that act once the receiver runs: --autostart, --fmscan, --stress
static void devAutomation(App& app, DevOpts& o) {
    {   // dev/test only: DECT2_CLICKS="frame:x:y,frame:x:y" injects a left click at (x,y) on those frames (hidden --shot runs of popups and buttons)
        static const char* env = getenv("DECT2_CLICKS");
        if (env) {
            const int fr = ImGui::GetFrameCount();
            int f; float x, y; const char* c = env;
            while (sscanf(c, "%d:%f:%f", &f, &x, &y) == 3) {
                ImGuiIO& io = ImGui::GetIO();
                if (fr == f - 1) io.AddMousePosEvent(x, y);
                if (fr == f) io.AddMouseButtonEvent(0, true);
                if (fr == f + 2) io.AddMouseButtonEvent(0, false);
                c = strchr(c, ','); if (!c) break; c++;
            }
        }
    }
    if (o.memSelfTest && ++o.memFrame == 3) { fflush(stdout); std::exit(memorySelfTest(app)); }   // the Memory recall check (tests/app/run_memory_recall.cmake)
    if (o.autostart) {
        o.autostart = false;
        app.devIdx = 0;
        if (o.hackrfStart) for (int i = 0; i < (int)app.devices.size(); i++) if (app.devices[i].kind == (o.fileStart ? DeviceInfo::File : DeviceInfo::HackRF) || (!o.fileStart && app.devices[i].isRadio())) app.devIdx = i;
        bool found = o.startDevice.empty();
        if (!o.startDevice.empty()) {
            for (int i = 0; i < (int)app.devices.size(); i++) if (app.devices[i].name == o.startDevice) { app.devIdx = i; found = true; break; }
            if (!found) {
                const std::string msg = "Requested source not found: " + o.startDevice + ". Receiver was not started.";
                app.engine.log(msg); fprintf(stderr, "%s\n", msg.c_str());
            }
        }
        if (found) {
            app.tune.centerHz = app.freqMhz * 1e6; fileStartPrep(app); applyBandwidth(app);
            app.engine.setComputeMode(app.computeMode); app.engine.setStandard(engineStd(app));
            app.engine.start(app.devices[app.devIdx], app.tune, app.file);
        }
    }
    if (o.fileVfoMhz > 0 && fileVfoActive(app) && glfwGetTime() > 2.0) { fileSetVfo(app, o.fileVfoMhz); o.fileVfoMhz = 0; }
    if (o.fmScanAuto && app.fmMode && app.engine.running() && glfwGetTime() > 3.0) {
        o.fmScanAuto = false;
        app.fmScan.running = true; app.fmScan.phase = 0; app.fmScan.idx = -1; app.fmScan.t0 = 0; app.fmScan.results.clear(); app.fmScan.cand.clear(); app.fmScan.savedFreq = app.freqMhz;
    }
    if (o.stress && app.engine.running() && ++o.stressFrame % 100 == 0) {
        o.stressStep++;
        app.freqMhz = 522.0 + 0.5 * (o.stressStep % 4);
        if (app.devices[app.devIdx].isGeneric()) app.tune.gainDb = 20 + 5 * (o.stressStep % 5);
        else { app.tune.lnaDb = 16 + 8 * (o.stressStep % 3); app.tune.vgaDb = 20 + 4 * (o.stressStep % 4); app.tune.ampOn = o.stressStep % 2; }
        app.tune.centerHz = app.freqMhz * 1e6;
        app.engine.retune(app.tune);
        app.peak.clear();   // what the toolbar does after a retune
        fprintf(stderr, "stress: retune %d -> %.1f MHz\n", o.stressStep, app.freqMhz);
    }
}

// DECT2_FPS=1: frame statistics on stderr
static void perfLog(double tUi0) {
    static const bool perf = getenv("DECT2_FPS") != nullptr;   // frame statistics on stderr: DECT2_FPS=1
    static double pAcc = 0, pMax = 0, pT0 = glfwGetTime(); static int pN = 0;
    if (perf) {
        const double d = glfwGetTime() - tUi0;
        pAcc += d; pMax = std::max(pMax, d); pN++;
        if (glfwGetTime() - pT0 > 2.0) { fprintf(stderr, "ui: %.1f fps, build %.2f ms avg / %.1f ms max\n", pN / (glfwGetTime() - pT0), 1e3 * pAcc / pN, 1e3 * pMax); pAcc = pMax = 0; pN = 0; pT0 = glfwGetTime(); }
    }
}

// DECT2_STATLOG=1: a status line every 10 s on stderr (the log file on Windows)
static void statusLog(App& app) {
    static const bool statlog = getenv("DECT2_STATLOG") != nullptr;   // a status line every 10 s on stderr (the log file on Windows): DECT2_STATLOG=1
    static double sT0 = glfwGetTime();
    if (statlog && app.engine.running() && glfwGetTime() - sT0 >= 10.0) {
        sT0 = glfwGetTime();
        const PlayerStats ps = app.engine.player().stats();
        fprintf(stderr, "stat: dropped %llu, PLP frames dropped %llu, decode %.0f ms on %s (mode %d), blocks ok %llu bad %llu, player %s hw=%d decoded %llu shown %llu late %llu buf %.0f ms underruns %d A/V %+.0f ms\n",
                (unsigned long long)app.engine.droppedSamples(), (unsigned long long)app.rx.plpFramesDropped, app.rx.plpDecodeMs, app.rx.plpOnGpu ? "GPU" : "CPU", app.rx.computeMode,
                (unsigned long long)app.rx.blocksOk, (unsigned long long)app.rx.blocksBad, ps.status.c_str(), ps.hardware, (unsigned long long)ps.decoded, (unsigned long long)ps.shown, (unsigned long long)ps.late, ps.audioBufferMs, ps.underruns, ps.avOffsetMs);
        fprintf(stderr, "stat: %s\n", t2rxProfile().c_str());
        fprintf(stderr, "stat: %s\n", engineWaitProfile().c_str());
        fprintf(stderr, "stat: %s\n", app.engine.loadProfile().c_str());
    }
}

// after the --shot picture: the stream and player state on stderr
static void shotReport(App& app) {
    tx::shotReport();
    { for (int i = 0; i < 20; i++) savePrefs(app);   // the cost of the save that every tuning event used to cost on the UI thread, 20 runs
      const tunestat::Stats& ts = tuneStats(); fprintf(stderr, "tune: settings save mean %.2f ms max %.2f ms (n=%u); retunes %u mean %.2f ms max %.2f ms; saves deferred by the debounce %u\n", ts.saveMsMean(), ts.saveMsMax, ts.saves, ts.retunes, ts.retuneMsMean(), ts.retuneMsMax, ts.savesDeferred);
      const tunework::Stats tw = tunework::stats(); fprintf(stderr, "tune thread: posted %llu, radio calls %llu, coalesced %llu, UI post max %.3f ms, driver call mean %.2f ms max %.2f ms\n", (unsigned long long)tw.posted, (unsigned long long)tw.done, (unsigned long long)tw.coalesced, tw.postMsMax, tw.callMsMean, tw.callMsMax); }
    { PlayerStats ps = app.engine.player().stats(); { BbStats bb = app.engine.bbStats(); fprintf(stderr, "stream: BB frames %llu lost %llu, PLP frames dropped by busy decoder %llu, FEC blocks ok %llu bad %llu, samples dropped %llu, CPU decode %.0f ms\n", (unsigned long long)bb.frames, (unsigned long long)bb.framesLost, (unsigned long long)app.rx.plpFramesDropped, (unsigned long long)app.rx.blocksOk, (unsigned long long)app.rx.blocksBad, (unsigned long long)app.engine.droppedSamples(), app.rx.plpDecodeMs); }
    fprintf(stderr, "player: %s hw=%d %s %dx%d decoded %llu shown %llu late %llu errors %llu audio %s ch %d buf %.0f ms underruns %d A/V %+.0f ms\n", ps.status.c_str(), ps.hardware, ps.videoCodec.c_str(), ps.width, ps.height, (unsigned long long)ps.decoded, (unsigned long long)ps.shown, (unsigned long long)ps.late, (unsigned long long)ps.errors, ps.audioCodec.c_str(), ps.audioChannels, ps.audioBufferMs, ps.underruns, ps.avOffsetMs); }
}

// orderly shutdown: transmitter quiet, receivers stopped, results saved, windows and back end closed
static void shutdownApp(App& app, gfx::Backend* gfxBackend, GLFWwindow* window) {
    savePrefsFlush();
    layoutSaveNow();
    workspaceSessionEnd(app);   // the last-session file is marked clean first: the state is the one at exit
    delete gDiscord;   // clears the status and joins its thread
    txShutdown();      // the transmitter goes quiet before anything else is closed
    pluginShutdown();  // plugin threads and helper processes end, the crash marker goes
    receiversShutdown(app);
    tvRecShutdown(app);   // the recordings that run are finished and closed before the receiver stops
    app.engine.stop();
    app.engine.flushResults();
    appResultsSave(app, true);
    updateOnExit(app);   // a downloaded update replaces this program once it has ended
    popOutShutdown();   // its window and surface go before the back end
    gfxBackend->shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    dect2::crash::startupDone();   // a quick clean exit (the --shot runs) is not a failed start
}

int main(int argc, char** argv) {
    dect2::crash::install();   // first: a crash from here on leaves a report
#ifdef __linux__
    if (geteuid() == 0) fprintf(stderr, "OnAir: running as root is not needed; see docs for udev rules\n");
#endif
#ifdef _WIN32
    dect2::crash::startLog();   // the program has no console window: keep its messages in a log file next to the settings (the old one is kept as onair-prev.log)
#endif
    {
        std::string report;
        if (dect2::crash::lastRunCrashed(report)) {
#ifdef _WIN32
            gCrashNote = "The last run of OnAir crashed; the report is in " + report + " - please send it with onair-prev.log";
#else
            gCrashNote = "The last run of OnAir crashed; the report is in " + report + " - please send it";
#endif
            fprintf(stderr, "%s\n", gCrashNote.c_str());
        }
    }
    if (!dect2::cpuSupportsBuild()) {
        plat::showFatalError("OnAir", "This version of OnAir needs a processor with AVX2 and FMA instructions (any Intel or AMD processor from about 2013 on).");
        return 1;
    }
    GLFWwindow* window = createMainWindow(argc, argv);
    if (!window) return 1;   // createMainWindow has already said why

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
#ifdef DECT2_UI_LAYOUT_CHECK
    void layoutCheckInstall();   // layout_check.cpp (developer build)
    layoutCheckInstall();
#endif
    gfx::Backend* gfxBackend = gfx::create(window);
    if (!gfxBackend) {
        fprintf(stderr, "could not start the graphics back end (%s)\n", gfx::levelName());
        plat::showFatalError("OnAir", "OnAir could not start its graphics (the OpenGL functions it needs are missing or the shaders failed).\n\n"
            "On a Raspberry Pi or another board with a Mesa driver, try:\nMESA_GL_VERSION_OVERRIDE=3.3 MESA_GLSL_VERSION_OVERRIDE=330 onair");
        return 1;
    }
    gGfx = gfxBackend;

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    {   // the font look saved in View > Font look (Current when never changed); read here, before the fonts load
        const long v = plat::prefs().getI("uiTypography", 0);
        typo::setVariant(v == 1 ? typo::Clean : v == 2 ? typo::Compact : typo::Classic);
    }
    {   // View > Time display: UTC (default) or the computer's local time, with an optional (UTC)/(local) label
        dect2::timefmt::setZone(plat::prefs().getB("timeLocal", false) ? dect2::timefmt::Zone::Local : dect2::timefmt::Zone::Utc);
        dect2::timefmt::setLabel(plat::prefs().getB("timeLabel", false));
    }
    applyTheme();
    typo::applyStyle(ImGui::GetStyle());
    static ImGuiStyle baseStyle = ImGui::GetStyle();   // the style at scale 1

    App app;
    loadFonts(app, io);
    // A layout slip (an item missing after SetCursorPos, for example) is recovered by ImGui; the red 'MESSAGE FROM DEAR IMGUI' box and the assert
    // are for developers, so users do not get a popup over the window. The recovery message still goes to the debug log.
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = false;


    ImGui_ImplGlfw_InitForOther(window, true);
    glfwSetDropCallback(window, [](GLFWwindow*, int n, const char** paths) { if (n > 0) wbQueueDrop(paths[0]); });   // a recording dropped on the window: the Signal workbench opens it
    watchMonitors();
    if (gWayland) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;   // see chooseGlfwPlatform()

    app.wf.init(gfxBackend);
    startApp(app, argc, argv);
    DevOpts o;
    parseDevFlags(app, argc, argv, o);
    double last = glfwGetTime();
    double frameNext = 0, lastInputT = 0;
    while (!glfwWindowShouldClose(window)) {
        appResultsSave(app);   // every 30 s: the lists the screens keep themselves (the engine saves its own on a thread)
        layoutTick(app, !app.touchUi && !o.shotPath);   // per-mode window positions (desktop; not in --shot runs)
        workspaceSessionTick(app);   // every few seconds: the last-session file (crash restore)
        discordTick(app);
        savePrefsTick();   // a deferred tuning save (savePrefsSoon), once the tuning has been still
        {
            const PaceInfo pi = paceFrame(window, app, o, frameNext, lastInputT);
            glfwPollEvents();
            wbDropTick(app);
            updateTick(app);
            rcaps::frameGate(anyRunning());   // Linux / software OpenGL: input never draws faster than 60 fps; low-power drawing caps lower
            int w, h;
            if (minimisedFrame(window, app, o, w, h)) continue;
            static const float kClear[4] = {0.04f, 0.045f, 0.05f, 1.f}, kClearLight[4] = {0.955f, 0.95f, 0.96f, 1.f};
            static const float kClearHc[4] = {0.f, 0.f, 0.f, 1.f};
            gfxBackend->newFrame(w, h, app.highContrast ? kClearHc : app.lightUi ? kClearLight : kClear);
            ImGui_ImplGlfw_NewFrame();
            applyDisplayScale(window, app, baseStyle);
            ImGui::NewFrame();
            textSizeKeys(app);
            if (static bool up = false; !up && ImGui::GetFrameCount() > 120 && glfwGetTime() > 5) { up = true; dect2::crash::startupDone(); }   // started: see the startup guard
            devAutomation(app, o);
            rcaps::hintWindow(app);   // software OpenGL found: offer low-power drawing once
            double now = glfwGetTime();
            last = now;
            const double tUi0 = glfwGetTime();
            receiversFrame(app, io.DisplaySize);
            perfOverlayDraw(app);
            popOutFrame(app);
            keepWindowsReachable();
            ImGui::Render();
            if (app.highContrast) lightenDrawData(ImGui::GetDrawData(), false, true);   // High contrast wins over Light
            else if (app.lightUi) lightenDrawData(ImGui::GetDrawData());
            perfLog(tUi0);
            perfFrame(glfwGetTime(), glfwGetTime() - tUi0, pi.interval, pi.battery, pi.occluded, app.perfOverlay);
            const bool shotNow = o.shotPath && (o.shotAfter > 0 ? glfwGetTime() > o.shotAfter : ++o.shotFrame == (o.hackrfStart ? (app.playReq >= 0 || app.engine.player().selected() >= 0 ? 2400 : 900) : 400));
            statusLog(app);
            const bool autoShot = !shotNow && !gAutomationShotPath.empty();   // the Automations' screenshot action: the program's own window
            const std::string autoPath = autoShot ? gAutomationShotPath : std::string();
            if (autoShot) gAutomationShotPath.clear();
            if (pi.occluded && !shotNow && !autoShot && gfxBackend->canSkipFrame()) gfxBackend->skipFrame();   // nobody can see it: no GPU work
            else gfxBackend->endFrame(ImGui::GetDrawData(), shotNow ? o.shotPath : autoShot ? autoPath.c_str() : nullptr);
            popOutPresent();
            if (shotNow) {
                shotReport(app);
                glfwSetWindowShouldClose(window, 1);
            }
        }
    }
    shutdownApp(app, gfxBackend, window);
    return 0;
}
