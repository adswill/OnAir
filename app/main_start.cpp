// Start-up of the main program: the startup guard and first-run dialogs, and the developer / test command-line flags.
// Split from main.cpp.
// The flags are listed where they are parsed (parseDevFlags); none of them is saved.
#include "dect2/packs/datasets.h"
#include "dect2/session/session_store.h"
#include "ui/input_map.h"
#include "dect2/source/device_pick.h"
#ifdef __linux__
#include <unistd.h>
#endif
#include "app.h"
#include "ui/contrast.h"
#include "ui/touch.h"
#include "ui/tour_logic.h"
#include "ui/setup_logic.h"
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
#include "state/workspace.h"
#include "gfx/render_caps.h"

// the startup guard (a start that keeps crashing), the first-start tour and setup, the radio list and the results of the last run
void startApp(App& app, int argc, char** argv) {
    // startup guard: a start that keeps crashing (a saved setting that breaks it, #59) must not repeat forever. After one unfinished
    // start the other interface is tried (the classic and the new one share little drawing code); after two in a row the saved
    // settings are set aside and OnAir starts with the defaults
    // a start with arguments (the dev and test runs: --shot, --hidden ...) is left out: it neither changes the settings nor counts
    const int failedStarts = argc > 1 ? 0 : dect2::crash::failedStarts();
    if (argc > 1) dect2::crash::startupDone();
    std::string guardNote;
    if (failedStarts >= 2) {
        const std::string kept = plat::prefs().setAside();
        guardNote = "OnAir did not finish starting " + std::to_string(failedStarts) + " times in a row, so it starts with the default settings this time. "
                  + (kept.empty() ? std::string("The old settings could not be kept.") : "The old settings are kept in " + kept + ".");
    }
    loadPrefs(app);
    if (failedStarts == 1) {
        app.newUi = !app.newUi;
        guardNote = std::string("OnAir did not finish starting last time, so it opens with the ") + (app.newUi ? "new" : "classic")
                  + " interface this time. The other one is under View (new interface) or the \"New interface\" button (classic).";
    }
    if (!guardNote.empty()) {
        fprintf(stderr, "%s\n", guardNote.c_str());
        gCrashNote = gCrashNote.empty() ? guardNote : gCrashNote + "\n\n" + guardNote;
    }
    // the setup wizard: only for a start with no settings of any earlier use (an update never shows it to an existing user), or one left unfinished
    bool setupPending = false;
    {
        std::vector<std::string> keys;
        for (const auto& it : plat::basePrefs().dumpAll()) keys.push_back(it.key);
        setupPending = setup::wizardPending(argc == 1, plat::prefs().getB("setupDone", false), keys, failedStarts >= 2, plat::prefs().has("setupStep"));
    }
    app.wizOpen = tour::autoStart(argc == 1, plat::prefs().getB(tour::prefKey(), false), setupPending);   // the tour runs on the very first start only
    if (setupPending) { app.wizOpen = false; setupWizardOpen(app); }   // the setup comes first; the tour follows it
    if (getenv("DECT2_WIZARD")) app.wizOpen = true;   // test hook: open the tour at start-up (DECT2_WIZSTEP picks the step)
    if (getenv("DECT2_SETUP")) { app.wizOpen = false; setupWizardOpen(app); if (const char* ss = getenv("DECT2_SETUPSTEP")) app.setupStep = atoi(ss);
        if (const char* sl = getenv("DECT2_SETUPLOC")) snprintf(app.setupLoc, sizeof app.setupLoc, "%s", sl);   // test hooks: a position and the ticked interests (bits)
        if (const char* si = getenv("DECT2_SETUPINT")) app.setupInterests = (unsigned)atoi(si); }
    app.wizStepT = ImGui::GetTime();
    app.engine.player().setHardwareDecode(plat::prefs().getB("hwVideo", true));
    app.tune.synth.demoTv = true;   // the synthetic DVB-T / ATSC signal carries a test-card programme
    if (const char* ws = getenv("DECT2_WIZSTEP")) wizEnter(app, atoi(ws));
    app.engine.log("OnAir started");
    if (!gCrashNote.empty()) app.engine.log(gCrashNote);
    refreshDevices(app);
    bool safeStart = false;   // --safe-start: do not pick or open a radio by itself (a saved network radio that hangs the start-up)
    for (int i = 1; i < argc; i++) if (std::string(argv[i]) == "--safe-start") safeStart = true;
    if (safeStart) app.engine.log("safe start: no radio opened by itself; saved network radios are in the settings file, see docs/NETWORK_RADIOS.md");
    for (int i = 0; i < (int)app.devices.size() && !safeStart; i++)
        if (app.devices[i].isRadio()) { app.devIdx = i; break; }
    if (!safeStart) {   // the radio RX 1 had at the last exit, when it is in the list (before, RX 1 always took the first radio, #314)
        const std::string had = plat::prefs().getS("devName", "");
        for (int i = 2; i < (int)app.devices.size() && !had.empty(); i++)
            if (app.devices[i].isRadio() && app.devices[i].name == had) { app.devIdx = i; break; }
    }
    {   // "Get me listening": a start with no source configured (no radio, none remembered), unless the user said not to show it
        bool sourceSet = !plat::prefs().getS("devName", "").empty();
        for (const DeviceInfo& d : app.devices) if (d.isRadio()) sourceSet = true;
        if (argc == 1 && !setupPending && !sourceSet && !plat::prefs().getB("listenWizardHide", false)) listenWizardOpen(app);
    }
    if (app.devices.size() == 2) {
#ifdef _WIN32
        app.engine.log("no radio found - plug it in and press the refresh button next to the source. A HackRF needs the WinUSB driver on Windows: install it once with Zadig (zadig.akeo.ie), choosing the HackRF and the driver WinUSB. Until then use the synthetic signal or an IQ file.");
#else
        app.engine.log("no radio connected - use the synthetic signal or an IQ file");
#endif
    }
    workspaceSessionStart(app, argc == 1);   // the last-session file: a session that did not end is offered for restore
    if (getenv("DECT2_DEMO")) { wizAction(app, 2); }   // test: start the demo programme straight away
    // the decoded results of the last run (each mode's lists, until its Clear), then saved every 30 s and at exit
    {   // the session store (docs/MODE-EXPERIENCE.md): logging is on by default; it never blocks the start
        std::string err;
        if (!dect2::sess::global().open(plat::dataDir(), dect2::sess::StoreConfig(), &err)) app.engine.log("session history is off: " + err);
    }
    app.engine.setResultsDir(plat::dataDir() + "/results");
    dect2::datasets::setUserDir(plat::dataDir() + "/datasets");   // before the first frequency / airport lookup: the user's layers over the bundled data
    appResultsLoad(app);
    receiversInit(app);   // RX 2 ... that were open at the last exit
}

// the developer and test flags (--shot, --hidden, --fm, --variant, ...): see the list in docs/ for what each does
void parseDevFlags(App& app, int argc, char** argv, DevOpts& o) {
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--autostart") o.autostart = true;
        if (std::string(argv[i]) == "--no-first-run") {
#ifdef _WIN32
            _putenv_s("DECT2_SKIP_FIRSTRUN", "1");
#else
            setenv("DECT2_SKIP_FIRSTRUN", "1", 1);
#endif
        }   // dev: no first-start question (public map), nothing covers the window; the same as DECT2_SKIP_FIRSTRUN=1
        if (std::string(argv[i]) == "--device" && i + 1 < argc) { o.startDevice = argv[++i]; o.autostart = true; }
        if (std::string(argv[i]) == "--antenna" && i + 1 < argc) { o.startAntenna = argv[++i]; o.autostart = true; }
        if (std::string(argv[i]) == "--list-devices") {   // the radios found: index, driver, name, serial, antenna (the same list onair-cli prints), then exit
            std::string lerr;
            const auto radios = dect2::listRadios(lerr);
            fputs(dect2::deviceListText(radios).c_str(), stdout);
            if (radios.empty()) fprintf(stderr, "no radio found %s\n", lerr.c_str());
            fflush(stdout);
            std::exit(0);
        }
        if (std::string(argv[i]) == "--tx") txDevOpen(app, i + 1 < argc && argv[i + 1][0] != '-' ? argv[i + 1] : nullptr, false);   // dev: TX view, optionally on a signal id
        if (std::string(argv[i]) == "--txdry" && i + 1 < argc) txDevOpen(app, argv[++i], true);   // dev: TX view with the test (no RF) running on a signal id
        if (std::string(argv[i]) == "--stress") o.stress = true;   // dev: retune frequency and gain every second, like moving the controls
        if (std::string(argv[i]) == "--shot" && i + 1 < argc) { o.shotPath = argv[++i]; rcaps::setUnpaced(true); }
        if (std::string(argv[i]) == "--low-power-draw") rcaps::forceLowPower(true);   // as ONAIR_LOW_POWER_DRAW=1: lower frame rate, simple ADS-B map
        if (std::string(argv[i]) == "--shotdelay" && i + 1 < argc) o.shotAfter = atof(argv[++i]);
        if (std::string(argv[i]) == "--voiceinv") voiceInversionDevOpen();   // dev: the Voice inversion pop-up open
        if (std::string(argv[i]) == "--audioclean") audioCleanupDevOpen(i + 1 < argc && std::string(argv[i + 1]) == "adv");   // dev: the Audio cleanup pop-up open (adv: Advanced)
        if (std::string(argv[i]) == "--fmscan") o.fmScanAuto = true;
        if (std::string(argv[i]) == "--vfo" && i + 1 < argc) o.fileVfoMhz = atof(argv[++i]);
        if (std::string(argv[i]) == "--passes") app.passesOpen = true;
        if (std::string(argv[i]) == "--df") app.dfOpen = true;
        if (std::string(argv[i]) == "--passive-radar") app.passiveRadarOpen = true;
        if (std::string(argv[i]) == "--rfi") app.rfiOpen = true;
        if (std::string(argv[i]) == "--images") app.galleryOpen = true;   // dev: open View > Images
        if (std::string(argv[i]) == "--images-dev" && i + 1 < argc) { app.galleryOpen = true; galleryDev(atoi(argv[++i])); }   // dev: Images with a studio view (1 map + histogram, 2 mosaic)
        if (std::string(argv[i]) == "--hamlib") app.hamlibAddOpen = true;   // dev: open the Add Hamlib rig window
        if (std::string(argv[i]) == "--packs") app.packsOpen = true;   // dev: open View > Data packs
        if (std::string(argv[i]) == "--map") app.mapOpen = true;   // dev: open View > Map
        if (std::string(argv[i]) == "--memory-selftest") o.memSelfTest = true;
        if (std::string(argv[i]) == "--memdev" && i + 1 < argc) memoryDev(atoi(argv[++i]));   // dev: 1 the Store current dialog, 2 the quick list as a window
        if (std::string(argv[i]) == "--memory") app.memoryOpen = true;   // dev: open View > Memory
        if (std::string(argv[i]) == "--memtab") app.memoryTab = true;    // dev: the Memory tab of the main screen
        if (std::string(argv[i]) == "--scanner") app.scanViewOpen = true; else if (std::string(argv[i]) == "--scanner-classic") app.scannerOpen = true;   // dev: open View > Scanner   // dev: open View > Passes
        if (std::string(argv[i]) == "--tle" && i + 1 < argc) passesDevTle(argv[++i]);   // dev: TLEs from a file for the pass planner (no download)
        if (std::string(argv[i]) == "--rx2") addReceiver();   // dev: a second receiver at once (no warning dialog)
        if (std::string(argv[i]) == "--tour") { app.wizOpen = true; app.wizStep = i + 1 < argc && argv[i + 1][0] != '-' ? atoi(argv[++i]) : -1; }   // dev: open the tour (at this step)
        if (std::string(argv[i]) == "--controls") inputmap::windowOpen() = true;   // dev: the View > Controls window open (screenshots)
        if (std::string(argv[i]) == "--credits") app.creditsOpen = true;   // dev: the Credits window open (screenshots)
        if (std::string(argv[i]) == "--mute") { app.muted = true; app.volume = 0; }
        if (std::string(argv[i]) == "--specdemo") app.specDemo = true;   // dev: a synthetic spectrum (FM, OFDM, SSB) without a radio, for screenshots of the looks
        if (std::string(argv[i]) == "--tunerstyle" && i + 1 < argc) app.tunerStyle = std::max(0, std::min(1, atoi(argv[++i])));   // dev: the tuner look (0 Classic, 1 Simple), not saved
        if (std::string(argv[i]) == "--look" && i + 1 < argc) specLookSet(app, atoi(argv[++i]), false);   // dev: the spectrum look (0 OnAir, 1 SDR++, 2 SDRoxide), not saved
        if (std::string(argv[i]) == "--wbfile" && i + 1 < argc) {   // dev: the Signal workbench opens this recording (--wbrate for a raw file, --wbtab, --wbsel, --wbkind, --wbbaud, --wbrun)
            double rate = 0; int tab = 0;
            for (int k = 1; k + 1 < argc; k++) { if (std::string(argv[k]) == "--wbrate") rate = atof(argv[k + 1]); if (std::string(argv[k]) == "--wbtab") tab = atoi(argv[k + 1]); }
            wbDevOpen(argv[i + 1], rate, tab);
            setFamily(app, 73);
            routeTab(app, "Workbench");
            i++;
        }
        if (std::string(argv[i]) == "--wbsel" && i + 4 < argc) { wbDevSelect(atof(argv[i + 1]), atof(argv[i + 2]), atof(argv[i + 3]), atof(argv[i + 4])); i += 4; }   // dev: time range (s) and band (Hz from the centre; 0 0: none)
        if (std::string(argv[i]) == "--wbdemod" && i + 3 < argc) { const std::string r = argv[i + 3]; wbDevDemod(atoi(argv[i + 1]), atof(argv[i + 2]), r.find('m') != std::string::npos, r.find('d') != std::string::npos); i += 3; }   // dev: demodulator number, baud, "m" measure and / or "d" demodulate
        if (std::string(argv[i]) == "--identify") gIdentifyDev = true;   // dev: identify the strongest signal once the receiver runs (with --shot)
        if (std::string(argv[i]) == "--tab" && i + 1 < argc) routeTab(app, argv[++i]);
        if (std::string(argv[i]) == "--second" && i + 2 < argc) {   // dev: a second mode (the mode-list row named so) at a frequency in MHz
            const std::string nm = argv[++i];
            const double mhz = atof(argv[++i]);
            for (int k = 0; k < kNumModes; k++) if (nm == kModes[k].name) { setSecondMode(app, k, mhz); break; }
        }
        if (std::string(argv[i]) == "--dvb") setFamily(app, 0);
        if (std::string(argv[i]) == "--atsc") setFamily(app, 1);
        if (std::string(argv[i]) == "--atsc3") setFamily(app, 3);
        if (std::string(argv[i]) == "--isdbt") setFamily(app, 4);
        if (std::string(argv[i]) == "--dab") setFamily(app, 2);
        if (std::string(argv[i]) == "--fm") setFamily(app, 5);
        if (std::string(argv[i]) == "--mode" && i + 1 < argc) { const char* id = argv[++i]; if (const SubMode* sm = subModeById(id)) setMode(app, sm->stdMode - 2, subIndexOf(id)); else if (const ModeTuning* mt = modeTuningById(id)) setFamily(app, mt->stdMode - 2); }   // dev: rtty, ft8, meshtastic, pocsag, navtex ... (the protocol entries), dvbs, dtmb, atv, dmr, drm, adsb, gnss, sonde, ais, marine, acars, inmc, aero, iridium, mesh, hdr, cdr, pager, packet, hfdig, airband, vdl2, ism, vor, ils, m17, dstar, cw, hfdl, nxdn, dpmr, tetra, psk, js8 or ale, psk or js8
        if (std::string(argv[i]) == "--sopt" && i + 2 < argc) { const int k = atoi(argv[i + 1]); if (k >= 0 && k < 8) app.tune.synth.modeOpt[k] = atoi(argv[i + 2]); i += 2; }       // dev: option k of the mode's test signal
        if (std::string(argv[i]) == "--sval" && i + 2 < argc) { const int k = atoi(argv[i + 1]); if (k >= 0 && k < 4) app.tune.synth.modeVal[k] = atof(argv[i + 2]); i += 2; }   // dev: value k of the mode's test signal
        if (std::string(argv[i]) == "--pick" && i + 1 < argc) {   // dev: pick the mode-list row named so, else the first row the search finds, as a click would
            const char* q = argv[++i];
            int pick = -1;
            auto same = [](const char* x, const char* y) { for (; *x && *y; x++, y++) if (tolower((unsigned char)*x) != tolower((unsigned char)*y)) return false; return *x == *y; };
            for (int m = 0; m < kNumModes && pick < 0; m++) if (same(kModes[m].name, q)) pick = m;
            for (int m = 0; m < kNumModes && pick < 0; m++) if (modeMatches(kModes[m], q)) pick = m;
            if (pick >= 0) selectMode(app, kModes[pick].family, kModes[pick].subId);
        }
        if (std::string(argv[i]) == "--ui2") app.newUi = true;
        if (std::string(argv[i]) == "--lang" && i + 1 < argc) i18n::set(argv[++i]);   // dev: the interface in this language (not saved)
        if (std::string(argv[i]) == "--layout" && i + 1 < argc) app.layoutUnified = std::string(argv[++i]) == "unified";   // dev: classic | unified, not saved
        if (std::string(argv[i]) == "--viewmenu") app.devViewMenu = 1;
        if (std::string(argv[i]) == "--viewmenu2") app.devViewMenu = 2;
        if (std::string(argv[i]) == "--kitgallery") app.kitGallery = true;   // dev: the UI kit gallery page instead of the main window
        if (std::string(argv[i]) == "--theme" && i + 1 < argc) app.uiTheme = atoi(argv[++i]);
        if (std::string(argv[i]) == "--light") app.lightUi = true;   // dev: View > Theme > Light
        if (std::string(argv[i]) == "--touch") app.touchUi = true;   // the touch interface (not saved)
        if (std::string(argv[i]) == "--touchsheet" && i + 1 < argc) { app.touchUi = true; touch::devOpen(argv[++i]); }   // dev: a touch sheet open at the start (keypad[:7.1], step, modes, menu, gain, vol, tuner, more[1-3])
        if (std::string(argv[i]) == "--touchscale" && i + 1 < argc) app.touchScale = std::max(1.f, std::min(2.f, (float)atof(argv[++i])));
        if (std::string(argv[i]) == "--hc") app.highContrast = true;   // dev: View > Theme > High contrast
        if (std::string(argv[i]) == "--variant" && i + 1 < argc) app.uiVariant = std::max(0, std::min(7, atoi(argv[++i])));
        if (std::string(argv[i]) == "--classic") app.newUi = false;
        if (std::string(argv[i]) == "--dock") app.dockOn = true;   // dev: View > Docking on
        if (std::string(argv[i]) == "--dockselftest") { app.dockOn = true; dockDevSelfTest(); }   // dev: drives docking with scripted input and exits (ui2_dock.cpp)
        if (std::string(argv[i]) == "--docktree" && i + 1 < argc) { app.dockOn = true; dockDevTree(argv[++i]); }   // dev: a dock layout in its text form (ui/dock_tree.h)
        if (std::string(argv[i]) == "--rate" && i + 1 < argc) app.file.sampleRate = atof(argv[++i]) * 1e6;
        if (std::string(argv[i]) == "--freq" && i + 1 < argc) app.freqMhz = atof(argv[++i]);
        if (std::string(argv[i]) == "--station" && i + 1 < argc) app.dabStation = atoi(argv[++i]);
        if (std::string(argv[i]) == "--constview" && i + 1 < argc) app.constView = atoi(argv[++i]);
        if (std::string(argv[i]) == "--wf3d" && i + 1 < argc) {   // dev: the 3D waterfall at angle n (0-4, wf3d.h Preset) for a screenshot
            app.wf.s3d.on = true; app.wf.s3d.preset = std::max(0, std::min((int)w3d::NumPresets - 1, atoi(argv[++i]))); app.wf.s3d.cam = w3d::preset(app.wf.s3d.preset);
        }
        if (std::string(argv[i]) == "--synthgain") app.tune.synth.gainModel = true;
        if (std::string(argv[i]) == "--dvbt") { app.tune.synth.dvbt = true; app.tune.synth.snrDb = 28; }
        if (std::string(argv[i]) == "--t2only") app.stdMode = 1;
        if (std::string(argv[i]) == "--dvbtonly") app.stdMode = 2;
        if (std::string(argv[i]) == "--nodeint") app.video.deint = false;
        if (std::string(argv[i]) == "--fakeepg") app.fakeEpg = true;
        if (std::string(argv[i]) == "--agc") app.agcOn = true;
        if (std::string(argv[i]) == "--agcgear") app.agcGearOpen = true;   // dev: open the AGC settings popup (screenshots)
        if (std::string(argv[i]) == "--fakehackrf") {   // dev: list a HackRF that is not there and select it, to see the gain controls of a radio in a screenshot
            DeviceInfo d; d.kind = DeviceInfo::HackRF; d.board = "HackRF One"; d.name = "HackRF One (screenshot)"; d.serial = "screenshot";
            app.devices.push_back(d); app.devIdx = (int)app.devices.size() - 1;
        }
        if (std::string(argv[i]) == "--fft" && i + 1 < argc) app.tune.synth.tx.s2field1 = atoi(argv[++i]);
        if (std::string(argv[i]) == "--gi" && i + 1 < argc) app.tune.synth.tx.giIdx = atoi(argv[++i]);
        if (std::string(argv[i]) == "--cfo" && i + 1 < argc) app.tune.synth.cfoHz = atof(argv[++i]);
        if (std::string(argv[i]) == "--snr" && i + 1 < argc) app.tune.synth.snrDb = atof(argv[++i]);
        if (std::string(argv[i]) == "--echo" && i + 1 < argc) app.tune.synth.echoDb = atof(argv[++i]);
        if (std::string(argv[i]) == "--echodelay" && i + 1 < argc) app.tune.synth.echoDelay = atoi(argv[++i]);
        if (std::string(argv[i]) == "--pp" && i + 1 < argc) app.tune.synth.tx.pp = atoi(argv[++i]);
        if (std::string(argv[i]) == "--ext") app.tune.synth.tx.ext = true;
        if (std::string(argv[i]) == "--hackrf") o.hackrfStart = true;
        if (std::string(argv[i]) == "--file" && i + 1 < argc) { app.file.path = argv[++i]; app.file.centreHz = 0; app.file.sampleRate = 10e6; app.file.format = FileFormat::CS8;
            { IqFileInfo info; std::string perr;
              if (probeIqFile(app.file.path, info, perr)) { app.file.path = info.dataPath; app.file.format = info.format; app.file.bigEndian = info.bigEndian; app.file.offsetBytes = info.offsetBytes; app.file.endBytes = info.endBytes; if (info.sampleRate > 0) app.file.sampleRate = info.sampleRate; if (info.centerHz > 0) app.freqMhz = info.centerHz / 1e6; }
              else if (!perr.empty()) fprintf(stderr, "%s\n", perr.c_str()); }
            o.fileStart = true; o.hackrfStart = true; o.autostart = true; }
        if (std::string(argv[i]) == "--play" && i + 1 < argc) app.playReq = atoi(argv[++i]);
        if (std::string(argv[i]) == "--lna" && i + 1 < argc) app.tune.lnaDb = atoi(argv[++i]);
        if (std::string(argv[i]) == "--vga" && i + 1 < argc) app.tune.vgaDb = atoi(argv[++i]);
        if (std::string(argv[i]) == "--amp") app.tune.ampOn = true;
    }
}
