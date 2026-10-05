// OnAir: window, main loop and the layout of the panels.
#include "app.h"

std::string gForceTab;

gfx::Backend* gGfx = nullptr;

GLFWwindow* gWindow = nullptr;

int gWinX = 100, gWinY = 100, gWinW = 1500, gWinH = 900;

ImU32 ttxColour(int c, float alpha) {
    static const ImVec4 col[8] = {ImVec4(0, 0, 0, 1), ImVec4(1, 0.1f, 0.1f, 1), ImVec4(0.1f, 1, 0.1f, 1), ImVec4(1, 1, 0.1f, 1),
                                  ImVec4(0.2f, 0.3f, 1, 1), ImVec4(1, 0.2f, 1, 1), ImVec4(0.1f, 1, 1, 1), ImVec4(1, 1, 1, 1)};
    ImVec4 v = col[c & 7]; v.w = alpha;
    return ImGui::ColorConvertFloat4ToU32(v);
}

// ---- programme guide helpers
int64_t utcNowOf(const App& a) { return a.ts.utcNow ? a.ts.utcNow : (int64_t)time(nullptr); }

void overviewTab(App& a) {
    ImGui::Checkbox("peak hold", &a.peakHold);
    ImGui::SameLine(0, 16 * gUi); ImGui::SetNextItemWidth(170 * gUi);
    ImGui::DragFloatRange2("spectrum dB", &a.yMin, &a.yMax, 1, -160, 20, "%.0f", "%.0f");
    ImGui::SameLine(0, 16 * gUi); ImGui::SetNextItemWidth(170 * gUi);
    ImGui::DragFloatRange2("waterfall dB", &a.wf.minDb, &a.wf.maxDb, 1, -160, 20, "%.0f", "%.0f");
    const float h = ImGui::GetContentRegionAvail().y;
    spectrumPlot(a, ImVec2(-1, h * 0.5f - 2));
    waterfallPlot(a, ImVec2(-1, -1));
}

void receiverTab(App& a) {
    if (a.dabMode) { dabEnsembleTab(a); return; }
    if (a.atsc3Mode) { atsc3ReceiverTab(a); return; }
    if (a.isdbtMode) { isdbtReceiverTab(a); return; }
    if (a.rx.standard == 2 || a.atscMode) {
        const AtscTelemetry& at = a.rx.atsc;
        ImGui::TextDisabled("ATSC 8-VSB receiver: matched filter, pilot loop, symbol clock, field sync, per-field equaliser, trellis, Reed-Solomon");
        ImGui::Spacing();
        auto row = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); ImGui::SameLine(190 * gUi); ImGui::PushFont(a.mono, 0); ImGui::Text(fmt, v...); ImGui::PopFont(); };
        row("pilot carrier", "%s", at.pilot ? "locked" : "searching");
        row("carrier offset", "%+.1f Hz", at.cfoHz);
        row("symbol clock offset", "%+.2f ppm", at.sroPpm);
        row("segment sync", "%s  (correlation %.2f)", at.segSync ? "locked" : "searching", at.syncQuality);
        row("field sync", "%s  (field %d)", at.fieldSync ? "locked" : "searching", at.fieldParity);
        row("equaliser", "%s, SNR %.1f dB from the known sync symbols", at.eqTrained ? "trained" : "not trained", at.snrDb);
        row("data symbols", "%.1f dB from the nearest of the 8 levels", at.dataSnrDb);
        row("fields decoded", "%llu", (unsigned long long)at.fields);
        row("Reed-Solomon", "%llu clean, %llu corrected, %llu failed", (unsigned long long)at.rsClean, (unsigned long long)at.rsCorrected, (unsigned long long)at.rsFailed);
        row("lock losses", "%llu (field sync misses %llu)", (unsigned long long)at.lockLosses, (unsigned long long)at.fieldSyncMisses);
        return;
    }
    subNav("rx", a.subRx, {"Sync", "Signalling", "FEC", "Frame map", "Channel"});
    switch (a.subRx) {
    case 0: syncTab(a); break;
    case 1: signallingTab(a); break;
    case 2: fecTab(a); break;
    case 3: frameMapTab(a); break;
    default: channelDashboard(a); break;
    }
}

void streamTab(App& a) {
    subNav("st", a.subStream, {"Outputs", "Transport stream"});
    if (a.subStream == 0) outputsTab(a); else tsTab(a);
}

// the old per-view names (for --tab in screenshots) map onto the new tabs and sub-views
void routeTab(App& a, const std::string& name) {
    struct R { const char* old; const char* tab; int tv, rx, st; };
    static const R map[] = {
        {"Player", "TV", 0, -1, -1}, {"Guide", "TV", 1, -1, -1}, {"Teletext", "TV", 2, -1, -1},
        {"Overview", "Overview", -1, -1, -1}, {"Spectrum", "Overview", -1, -1, -1}, {"Waterfall", "Overview", -1, -1, -1}, {"Constellations", "Overview", -1, -1, -1},
        {"Sync", "Receiver", -1, 0, -1}, {"Signalling", "Receiver", -1, 1, -1}, {"FEC", "Receiver", -1, 2, -1}, {"Frame map", "Receiver", -1, 3, -1},
        {"Channel", "Receiver", -1, 4, -1}, {"Impulse response", "Receiver", -1, 4, -1}, {"SNR per carrier", "Receiver", -1, 4, -1},
        {"Outputs", "Stream", -1, -1, 0}, {"TS", "Stream", -1, -1, 1}, {"Scan", "Scan", -1, -1, -1}, {"History", "History", -1, -1, -1}, {"Log", "History", -1, -1, -1}};
    for (auto& r : map) if (name == r.old) {
        gForceTab = r.tab;
        if (r.tv >= 0) a.subTv = r.tv;
        if (r.rx >= 0) a.subRx = r.rx;
        if (r.st >= 0) a.subStream = r.st;
        return;
    }
    gForceTab = name;
}

ImU32 scoreColour(double sc) {
    const float t = (float)std::min(1.0, std::max(0.0, sc / 100.0));
    return IM_COL32((int)(230 * (1 - t) + 40 * t), (int)(70 + 150 * t), (int)(60 + 30 * t), 255);
}

void drawUI(App& a, ImVec2 disp) {
    ingestSpectrum(a);
    ingestRx(a);
    dabHistory(a);
    dabScanStep(a);
    harvestScan(a);
    if (a.engine.running() && glfwGetTime() - a.epgT > 1.0) {
        a.epg = a.engine.epg();
        if (a.fakeEpg) {
            const int64_t base = (int64_t)time(nullptr) / 1800 * 1800 - 1800;
            static const char* titles[] = {"Morning News", "The Garden Show", "Football: Derby", "Documentary: Oceans", "Quiz Night", "Late Film"};
            for (auto& sv : a.ts.services) for (int k = 0; k < 8; k++) {
                EpgEvent e; e.eventId = k + 1; e.start = base + k * 3600; e.duration = 3600; e.running = k == 1 ? 4 : 0; e.genre = 1 + k % 5;
                e.title = std::string(titles[(k + sv.id) % 6]); e.text = "Short synopsis of " + e.title; e.extended = "A longer description of the programme, with details about the presenters, guests and what to expect during the hour.";
                a.epg[sv.id].push_back(e);
            }
        }
        a.epgT = glfwGetTime();
    }
    if (!a.engine.running()) { a.epg.clear(); }
    gainControl(a);
    followBandwidth(a);
    feedDirection(a);
    {
        int pid = -1;
        const int sid = a.engine.player().selected();
        if (sid >= 0) for (auto& sv : a.ts.services) if (sv.id == sid) for (auto& st : sv.streams) if (st.kind == "teletext") { pid = st.pid; break; }
        a.engine.teletext().setPid(pid);
    }
    a.video.update(a.engine.player());
    if (a.playReq >= 0) for (auto& sv : a.ts.services) if (sv.id == a.playReq && sv.havePmt) { a.engine.player().select(sv.id); a.engine.player().setVolume(a.volume); a.playReq = -1; break; }
    {
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            if (a.videoOnly) { a.videoOnly = false; if (glfwGetWindowMonitor(gWindow)) toggleFullscreen(); }
            else if (a.video.has()) { a.videoOnly = true; if (!glfwGetWindowMonitor(gWindow)) toggleFullscreen(); }
        }
        if (a.videoOnly && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { a.videoOnly = false; if (glfwGetWindowMonitor(gWindow)) toggleFullscreen(); }
    }
    if (a.videoOnly) {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(disp);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
        ImGui::Begin("##vonly", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        if (a.video.has()) a.video.draw(disp); else ImGui::TextDisabled("no picture - press Esc");
        if (ImGui::IsWindowHovered() && ImGui::IsMouseDoubleClicked(0)) { a.videoOnly = false; if (glfwGetWindowMonitor(gWindow)) toggleFullscreen(); }
        ImGui::End();
        ImGui::PopStyleColor();
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(disp);
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

    const ImVec2 tb0 = ImGui::GetCursorScreenPos();
    toolbar(a);
    if (a.devices[a.devIdx].kind == DeviceInfo::Synthetic || (a.devices[a.devIdx].kind == DeviceInfo::File && !a.engine.running())) sourceOptions(a);
    a.tgMin[TgToolbar] = ImVec2(tb0.x - 4, tb0.y - 3); a.tgMax[TgToolbar] = ImVec2(tb0.x + ImGui::GetContentRegionAvail().x + 4, ImGui::GetCursorScreenPos().y);
    const ImVec2 sw0 = ImGui::GetCursorScreenPos();
    standardSwitch(a);
    ImGui::SameLine(disp.x - 64 - 150 * gUi);
    updateButton(a);
    ImGui::SameLine(disp.x - 64);
    if (ImGui::SmallButton("Tour")) { a.wizOpen = true; a.wizX = -1; a.wizStep = 0; a.wizStepT = ImGui::GetTime(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Take the guided tour with Onny");
    a.tgMin[TgSwitch] = ImVec2(sw0.x - 2, sw0.y - 2); a.tgMax[TgSwitch] = ImVec2(sw0.x + 420, ImGui::GetCursorScreenPos().y);
    ImGui::Separator();
    statusBar(a);
    ImGui::Separator();

    float logH = (a.atsc3Mode ? 0 : 250) * gUi, rightW = 350 * gUi;   // ATSC 3.0 has no constellation panels yet
    float mainH = ImGui::GetContentRegionAvail().y - logH - 6;
    ImGui::BeginChild("main", ImVec2(disp.x - rightW - 16 * gUi, mainH));
    a.tgMin[TgMain] = ImGui::GetWindowPos(); a.tgMax[TgMain] = ImVec2(a.tgMin[TgMain].x + ImGui::GetWindowSize().x, a.tgMin[TgMain].y + ImGui::GetWindowSize().y);
    if (ImGui::BeginTabBar("tabs")) {
        if (tabItem("Overview", Ic::Grid)) { overviewTab(a); ImGui::EndTabItem(); }
        if (a.dabMode) { if (tabItem("Radio", Ic::Radio)) { dabRadioTab(a); ImGui::EndTabItem(); } }
        else if (tabItem("TV", Ic::Tv)) { tvTab(a); ImGui::EndTabItem(); }
        if (tabItem(a.dabMode ? "Ensemble" : "Receiver", Ic::Antenna)) { receiverTab(a); ImGui::EndTabItem(); }
        if (!a.dabMode && tabItem("Stream", Ic::Layers)) { streamTab(a); ImGui::EndTabItem(); }
        if (tabItem("Scan", Ic::Scan)) { scanTab(a); ImGui::EndTabItem(); }
        if (tabItem("Antenna", Ic::Compass)) { antennaTab(a); ImGui::EndTabItem(); }
        if (tabItem("History", Ic::Chart)) { historyLogTab(a); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("right", ImVec2(0, mainH));
    a.tgMin[TgRight] = ImGui::GetWindowPos(); a.tgMax[TgRight] = ImVec2(a.tgMin[TgRight].x + ImGui::GetWindowSize().x, a.tgMin[TgRight].y + ImGui::GetWindowSize().y);
    rightPanel(a);
    ImGui::EndChild();

    if (!a.atsc3Mode) {
        ImGui::Separator();
        ImGui::BeginChild("constellations", ImVec2(0, 0));
        a.tgMin[TgConst] = ImGui::GetWindowPos(); a.tgMax[TgConst] = ImVec2(a.tgMin[TgConst].x + ImGui::GetWindowSize().x, a.tgMin[TgConst].y + ImGui::GetWindowSize().y);
        constellationsTab(a);
        ImGui::EndChild();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    ImGui::End();
    wizard(a, disp);
    if (a.popOut) {
        ImGui::SetNextWindowSize(ImVec2(640 * gUi, 380 * gUi), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(200, 160), ImGuiCond_FirstUseEver);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
        if (ImGui::Begin("Video", &a.popOut)) {
            if (a.video.has()) a.video.draw(ImGui::GetContentRegionAvail()); else ImGui::TextDisabled("no picture");
            if (ImGui::IsWindowHovered() && ImGui::IsMouseDoubleClicked(0)) { a.videoOnly = true; if (!glfwGetWindowMonitor(gWindow)) toggleFullscreen(); }
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }
}

int main(int argc, char** argv) {
#ifdef _WIN32
    {   // the program has no console window: keep its messages in a log file next to the settings
        if (const char* ap = getenv("APPDATA")) {
            const std::string dir = std::string(ap) + "\\OnAir";
            _mkdir(dir.c_str());
            FILE* unused = freopen((dir + "\\onair.log").c_str(), "w", stderr);
            (void)unused;
        }
    }
#endif
    if (!dect2::cpuSupportsBuild()) {
        plat::showFatalError("OnAir", "This version of OnAir needs a processor with AVX2 and FMA instructions (any Intel or AMD processor from about 2013 on).");
        return 1;
    }
    glfwInit();
    gfx::windowHints();
    GLFWwindow* window = glfwCreateWindow(1500, 900, "OnAir", nullptr, nullptr);
    if (!window) return 1;
#ifndef __APPLE__
    {   // the default size is in pixels: make the window as large (in points) as on a 96 dpi display, but never larger than the screen
        float xs = 1, ys = 1;
        glfwGetWindowContentScale(window, &xs, &ys);
        const float s = std::max(1.f, std::max(xs, ys));
        int wx = 0, wy = 0, ww = 0, wh = 0;
        if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) glfwGetMonitorWorkarea(mon, &wx, &wy, &ww, &wh);
        if (s > 1.01f) glfwSetWindowSize(window, ww > 0 ? std::min((int)(1500 * s), ww) : (int)(1500 * s), wh > 0 ? std::min((int)(900 * s), wh) : (int)(900 * s));
    }
#endif
    gWindow = window;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    gfx::Backend* gfxBackend = gfx::create(window);
    if (!gfxBackend) { fprintf(stderr, "could not start the graphics back end\n"); return 1; }
    gGfx = gfxBackend;

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    applyTheme();
    static ImGuiStyle baseStyle = ImGui::GetStyle();   // the style at scale 1

    App app;
    ImFont* ui = nullptr;
    app.mono = nullptr;
    for (const auto& f : plat::monoFontCandidates()) if (access(f.c_str(), R_OK) == 0 && (ui = io.Fonts->AddFontFromFileTTF(f.c_str(), 13.0f))) break; // the whole UI is monospaced
    for (const auto& f : plat::monoFontCandidates()) if (access(f.c_str(), R_OK) == 0 && (app.mono = io.Fonts->AddFontFromFileTTF(f.c_str(), 13.0f))) break;
    if (!ui) ui = io.Fonts->AddFontDefault();
    app.ui = ui;
    if (!app.mono) app.mono = ui;

    ImGui_ImplGlfw_InitForOther(window, true);

    app.wf.init(gfxBackend);
    loadPrefs(app);
    app.wizOpen = argc == 1 && !plat::prefs().getB("wizardDone", false);   // the tour runs on the very first start only
    if (getenv("DECT2_WIZARD")) app.wizOpen = true;
    app.wizStepT = ImGui::GetTime();
    app.engine.player().setHardwareDecode(plat::prefs().getB("hwVideo", true));
    app.tune.synth.demoTv = true;   // the synthetic DVB-T / ATSC signal carries a test-card programme
    if (const char* ws = getenv("DECT2_WIZSTEP")) wizEnter(app, atoi(ws));
    app.engine.log("OnAir started");
    refreshDevices(app);
    for (int i = 0; i < (int)app.devices.size(); i++)
        if (app.devices[i].isRadio()) { app.devIdx = i; break; }
    if (app.devices.size() == 2) {
#ifdef _WIN32
        app.engine.log("no radio found - plug it in and press the refresh button next to the source. A HackRF needs the WinUSB driver on Windows: install it once with Zadig (zadig.akeo.ie), choosing the HackRF and the driver WinUSB. Until then use the synthetic signal or an IQ file.");
#else
        app.engine.log("no radio connected - use the synthetic signal or an IQ file");
#endif
    }
    if (getenv("DECT2_DEMO")) { wizAction(app, 2); }   // test: start the demo programme straight away

    bool autostart = false, hackrfStart = false, fileStart = false, stress = false;
    int stressFrame = 0, stressStep = 0;
    const char* shotPath = nullptr;
    int shotFrame = 0;
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--autostart") autostart = true;
        if (std::string(argv[i]) == "--stress") stress = true;   // dev: retune frequency and gain every second, like moving the controls
        if (std::string(argv[i]) == "--shot" && i + 1 < argc) shotPath = argv[++i];
        if (std::string(argv[i]) == "--tab" && i + 1 < argc) routeTab(app, argv[++i]);
        if (std::string(argv[i]) == "--atsc") setFamily(app, 1);
        if (std::string(argv[i]) == "--atsc3") setFamily(app, 3);
        if (std::string(argv[i]) == "--isdbt") setFamily(app, 4);
        if (std::string(argv[i]) == "--dab") setFamily(app, 2);
        if (std::string(argv[i]) == "--rate" && i + 1 < argc) app.file.sampleRate = atof(argv[++i]) * 1e6;
        if (std::string(argv[i]) == "--freq" && i + 1 < argc) app.freqMhz = atof(argv[++i]);
        if (std::string(argv[i]) == "--station" && i + 1 < argc) app.dabStation = atoi(argv[++i]);
        if (std::string(argv[i]) == "--constview" && i + 1 < argc) app.constView = atoi(argv[++i]);
        if (std::string(argv[i]) == "--synthgain") app.tune.synth.gainModel = true;
        if (std::string(argv[i]) == "--dvbt") { app.tune.synth.dvbt = true; app.tune.synth.snrDb = 28; }
        if (std::string(argv[i]) == "--t2only") app.stdMode = 1;
        if (std::string(argv[i]) == "--dvbtonly") app.stdMode = 2;
        if (std::string(argv[i]) == "--nodeint") app.video.deint = false;
        if (std::string(argv[i]) == "--fakeepg") app.fakeEpg = true;
        if (std::string(argv[i]) == "--agc") app.agcOn = true;
        if (std::string(argv[i]) == "--fft" && i + 1 < argc) app.tune.synth.tx.s2field1 = atoi(argv[++i]);
        if (std::string(argv[i]) == "--gi" && i + 1 < argc) app.tune.synth.tx.giIdx = atoi(argv[++i]);
        if (std::string(argv[i]) == "--cfo" && i + 1 < argc) app.tune.synth.cfoHz = atof(argv[++i]);
        if (std::string(argv[i]) == "--snr" && i + 1 < argc) app.tune.synth.snrDb = atof(argv[++i]);
        if (std::string(argv[i]) == "--echo" && i + 1 < argc) app.tune.synth.echoDb = atof(argv[++i]);
        if (std::string(argv[i]) == "--echodelay" && i + 1 < argc) app.tune.synth.echoDelay = atoi(argv[++i]);
        if (std::string(argv[i]) == "--pp" && i + 1 < argc) app.tune.synth.tx.pp = atoi(argv[++i]);
        if (std::string(argv[i]) == "--ext") app.tune.synth.tx.ext = true;
        if (std::string(argv[i]) == "--hackrf") hackrfStart = true;
        if (std::string(argv[i]) == "--file" && i + 1 < argc) { app.file.path = argv[++i]; app.file.sampleRate = 10e6; app.file.format = FileFormat::CS8; fileStart = true; hackrfStart = true; autostart = true; }
        if (std::string(argv[i]) == "--play" && i + 1 < argc) app.playReq = atoi(argv[++i]);
        if (std::string(argv[i]) == "--lna" && i + 1 < argc) app.tune.lnaDb = atoi(argv[++i]);
        if (std::string(argv[i]) == "--vga" && i + 1 < argc) app.tune.vgaDb = atoi(argv[++i]);
        if (std::string(argv[i]) == "--amp") app.tune.ampOn = true;
    }
    double last = glfwGetTime();
    while (!glfwWindowShouldClose(window)) {
        {
            glfwPollEvents();
            updateTick(app);
            int w, h;
            glfwGetFramebufferSize(window, &w, &h);
            if (w == 0 || h == 0) { glfwWaitEventsTimeout(0.1); continue; }
            static const float kClear[4] = {0.04f, 0.045f, 0.05f, 1.f};
            gfxBackend->newFrame(w, h, kClear);
            ImGui_ImplGlfw_NewFrame();
#ifndef __APPLE__
            {   // display scaling (Windows / Linux): follow the scale factor of the monitor the window is on
                float xs = 1, ys = 1;
                glfwGetWindowContentScale(window, &xs, &ys);
                float s = std::max(1.f, std::max(xs, ys));
                if (const char* e = getenv("DECT2_UISCALE")) s = std::max(0.5f, (float)atof(e));
                if (s != gUi) {
                    gUi = s;
                    ImGuiStyle& st = ImGui::GetStyle();
                    st = baseStyle;
                    st.ScaleAllSizes(s);
                    st.FontScaleDpi = s;
                    plt::GetStyle().Scale = s;
                }
            }
#endif
            ImGui::NewFrame();
            if (autostart) {
                autostart = false;
                app.devIdx = 0;
                if (hackrfStart) for (int i = 0; i < (int)app.devices.size(); i++) if (app.devices[i].kind == (fileStart ? DeviceInfo::File : DeviceInfo::HackRF) || (!fileStart && app.devices[i].isRadio())) app.devIdx = i;
                app.tune.centerHz = app.freqMhz * 1e6; applyBandwidth(app);
                app.engine.setComputeMode(app.computeMode); app.engine.setStandard(engineStd(app));
                app.engine.start(app.devices[app.devIdx], app.tune, app.file);
            }
            if (stress && app.engine.running() && ++stressFrame % 100 == 0) {
                stressStep++;
                app.freqMhz = 522.0 + 0.5 * (stressStep % 4);
                if (app.devices[app.devIdx].isGeneric()) app.tune.gainDb = 20 + 5 * (stressStep % 5);
                else { app.tune.lnaDb = 16 + 8 * (stressStep % 3); app.tune.vgaDb = 20 + 4 * (stressStep % 4); app.tune.ampOn = stressStep % 2; }
                app.tune.centerHz = app.freqMhz * 1e6;
                app.engine.retune(app.tune);
                app.peak.clear();   // what the toolbar does after a retune
                fprintf(stderr, "stress: retune %d -> %.1f MHz\n", stressStep, app.freqMhz);
            }
            double now = glfwGetTime();
            last = now;
            const double tUi0 = glfwGetTime();
            drawUI(app, io.DisplaySize);
            ImGui::Render();
            static const bool perf = getenv("DECT2_FPS") != nullptr;   // frame statistics on stderr: DECT2_FPS=1
            static double pAcc = 0, pMax = 0, pT0 = glfwGetTime(); static int pN = 0;
            if (perf) {
                const double d = glfwGetTime() - tUi0;
                pAcc += d; pMax = std::max(pMax, d); pN++;
                if (glfwGetTime() - pT0 > 2.0) { fprintf(stderr, "ui: %.1f fps, build %.2f ms avg / %.1f ms max\n", pN / (glfwGetTime() - pT0), 1e3 * pAcc / pN, 1e3 * pMax); pAcc = pMax = 0; pN = 0; pT0 = glfwGetTime(); }
            }
            const bool shotNow = shotPath && ++shotFrame == (hackrfStart ? (app.playReq >= 0 || app.engine.player().selected() >= 0 ? 2400 : 900) : 400);
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
            gfxBackend->endFrame(ImGui::GetDrawData(), shotNow ? shotPath : nullptr);
            if (shotNow) {
                { PlayerStats ps = app.engine.player().stats(); { BbStats bb = app.engine.bbStats(); fprintf(stderr, "stream: BB frames %llu lost %llu, PLP frames dropped by busy decoder %llu, FEC blocks ok %llu bad %llu, samples dropped %llu, CPU decode %.0f ms\n", (unsigned long long)bb.frames, (unsigned long long)bb.framesLost, (unsigned long long)app.rx.plpFramesDropped, (unsigned long long)app.rx.blocksOk, (unsigned long long)app.rx.blocksBad, (unsigned long long)app.engine.droppedSamples(), app.rx.plpDecodeMs); }
                fprintf(stderr, "player: %s hw=%d %s %dx%d decoded %llu shown %llu late %llu errors %llu audio %s ch %d buf %.0f ms underruns %d A/V %+.0f ms\n", ps.status.c_str(), ps.hardware, ps.videoCodec.c_str(), ps.width, ps.height, (unsigned long long)ps.decoded, (unsigned long long)ps.shown, (unsigned long long)ps.late, (unsigned long long)ps.errors, ps.audioCodec.c_str(), ps.audioChannels, ps.audioBufferMs, ps.underruns, ps.avOffsetMs); }
                glfwSetWindowShouldClose(window, 1);
            }
        }
    }
    app.engine.stop();
    updateOnExit(app);   // a downloaded update replaces this program once it has ended
    gfxBackend->shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

