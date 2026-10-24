// Shared declarations of the OnAir window: the application state, small helpers and the panels that draw the interface.
// The panels live in widgets.cpp, app_state.cpp, toolbar*.cpp, plots.cpp, analysis_tabs.cpp, tv.cpp, scan_outputs.cpp,
// antenna.cpp, dab_ui.cpp, wizard.cpp; main.cpp has the window loop.
#pragma once
// OnAir — digital TV receiver (DVB-T2, DVB-T, ATSC) for macOS. Phase 0 shell: sources, spectrum, waterfall, status, log.
#ifdef _WIN32
#include <direct.h>
#endif
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "gfx/plot.h"
#include "gfx/tune_stat.h"
#include "gfx/autolabel_logic.h"
#include "i18n/i18n.h"
#include "state/fm_saved.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>


#include "dect2/engine/engine.h"
#include "dect2/engine/modes.h"
#include "dect2/dsp/gpu_ldpc.h"
#include "dect2/t2/t2rx.h"
#include "dect2/net/nettuner.h"
#include "dect2/net/netradio.h"
#include "dect2/source/synth_group.h"
#include "dect2/net/kiwisdr.h"
#include "dect2/source/hamlib_rig.h"
#include "dect2/source/rig_setup.h"
#include "platform/airplay.h"
#include "dect2/common/timecompat.h"
#include "dect2/common/time_fmt.h"
#include "dect2/common/platform.h"
#include "dect2/engine/scanner.h"
#include "dect2/tv/mux_change.h"
#include "dect2/engine/updater.h"
#include "dect2/source/gain.h"
#include "dect2/source/gain_opt.h"
#include "dect2/source/agc.h"
#include "dect2/tx/tx.h"
#include "dect2/channels/channel.h"
#include "dect2/dvbt/dvbt.h"
#include "dect2/engine/quality.h"
#include "dect2/dsp/direction.h"
#include "dect2/tv/teletext.h"
#include "dect2/tv/hd_pair.h"
#include <map>
#include "dect2/net/feeds.h"
#include "dect2/net/share.h"
#include "dect2/net/pskreporter.h"
#include "dect2/net/wsprnet.h"
#include "dect2/geo/passes.h"
#include "dect2/geo/maplayers.h"
#include "ui/theme.h"
#include "gfx/gfx.h"
#include "ui/icons.h"
#include "ui/scale.h"
#include "platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>
#include <unistd.h>
#include "gfx/colormap.h"
#include "gfx/wf_view.h"
#include "gfx/wf3d.h"
#include "gfx/spec_axis.h"

using namespace dect2;

#include "gfx/waterfall.h"

extern std::string gForceTab;

// ------------------------------------------------------------------ app state

struct BwChoice { const char* label; double mhz; double nativeMsps; };
inline const BwChoice kBw[] = {
    {"8 MHz", 8, 64.0 / 7}, {"7 MHz", 7, 8.0}, {"6 MHz", 6, 48.0 / 7}, {"5 MHz", 5, 40.0 / 7}, {"1.7 MHz", 1.7, 131.0 / 71},
};

extern gfx::Backend* gGfx;
extern GLFWwindow* gWindow;
extern int gWinX, gWinY, gWinW, gWinH;

// One subtitle service of the playing programme: a DVB subtitling stream (EN 300 743) or a teletext subtitle page (EN 300 706)
struct SubOpt {
    bool ttx = false;
    int pid = 0, comp = 0, anc = 0;   // DVB: composition / ancillary page id; teletext: comp = page number
    std::string lang, key, label;
    bool fallback = false;            // teletext without a subtitling entry: page 888, else 777
};

struct VideoTex {
    gfx::Video* tex = nullptr;
    double pts = 0;          // presentation time (seconds, as in the stream) of the newest picture: subtitles are timed against it
    // subtitle overlay (subs_ui.cpp): the DVB subtitle bitmap as a texture over the picture, and a hook that draws any overlay on it
    gfx::Video* sub = nullptr;
    int subW = 0, subH = 0;
    uint64_t subSeq = 0;
    bool subShow = false;
    void (*overlayFn)(void* ctx, ImDrawList* dl, ImVec2 p0, ImVec2 size) = nullptr;
    void* overlayCtx = nullptr;
    int w = 0, h = 0;
    double dar = 0;          // display aspect of the newest picture (0 = use w / h); it can change between programmes
    uint64_t seq = 0;
    bool deint = true;
    bool has() const { return tex != nullptr; }
    void update(Player& pl) {
        auto f = pl.videoFrame(seq);
        if (!f || f->w <= 0) return;
        dar = f->dar;
        pts = f->pts;
        const bool yuv = f->rgba.empty();
        if (!tex || w != f->w || h != f->h) {
            delete tex;
            tex = gGfx->createVideo(f->w, f->h);
            w = f->w; h = f->h;
        }
        if (!yuv) { tex->uploadRGBA(f->rgba.data()); return; }
        tex->uploadNV12(f->y.data(), f->uv.data(), f->bt709, f->fullRange, f->interlaced && deint);
    }
    void draw(ImVec2 box) {
        if (!tex) return;
        if (box.x < 1 || box.y < 1 || h <= 0) return;
        float ar = dar > 0.05 ? (float)dar : (float)w / h;
        ImVec2 sz = box;
        if (box.x / box.y > ar) sz.x = box.y * ar; else sz.y = box.x / ar;
        ImVec2 p = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(p.x + (box.x - sz.x) * 0.5f, p.y + (box.y - sz.y) * 0.5f));
        ImGui::Image(tex->texture(), sz);
        if (overlayFn) overlayFn(overlayCtx, ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), sz);
        ImGui::SetCursorPos(p);
    }
};

using SavedChannel = plat::Channel;


// parts of the window the tour points at
enum WizTarget { TgNone = 0, TgSwitch, TgToolbar, TgMain, TgRight, TgConst, TgCount };

constexpr int kNumFamilies = 204;   // App::family runs from 0 to kNumFamilies - 1
constexpr int kNumSubModes = 32;   // dect2::subModes(): the protocols of HF digital, mesh, pagers, marine and VOR / ILS listed as modes of their own
constexpr int kNumSlots = kNumFamilies + kNumSubModes;   // what keeps its own frequency and gains: each family, and each protocol entry

struct App {
    Engine engine;
    int rxId = 1;                 // receiver number: RX 1 is the first (receivers.h)
    bool rxForcedMute = false, rxSavedMuted = false;   // only the receiver that plays gets sound: the others are muted
    NetTuner net{engine};      // network tuner (declared after the engine so that it stops first)
    bool netOn = false, netLan = false;
    int netPort = 8089;
    char netKey[32] = "";
    // first-run tour (wizard.inc)
    bool wizOpen = false, wizStart = false, wizAcked = false;
    int wizTour = -1;           // the guided tour on screen (index in tour::tours), -1 for the first-run tour
    int creditsTab = 0;         // 0 Contributors, 1 Projects and licences (the tab the Credits window opens on)
    bool voiceKeysOpen = false;     // the Voice keys window (Tools menu, the "encrypted: add your key" buttons; app/keys)
    bool voiceCodecsOpen = false;   // the Voice codecs overview window (Help menu)
    bool creditsOpen = false;   // the Credits window (Help menu, the "by: ..." button)
    bool backupOpen = false;    // Tools > Backup and restore (app/panels/backup_ui.cpp)
    bool reportOpen = false;    // Help > Report a problem (app/panels/report_problem.cpp)
    bool galleryOpen = false;           // the picture gallery (View > Images)
    bool dfOpen = false;                // the direction finder window (View > Direction finder; app/df)
    bool rfiOpen = false;               // the interference hunter window (View > Interference hunter; app/rfi)
    bool passiveRadarOpen = false;      // the passive radar window (View > Passive radar; app/radar)
    bool satnogsOpen = false;           // the SatNOGS DB transmitter list (View > SatNOGS transmitters)
    bool passesOpen = false;            // the satellite pass planner window (View > Passes)
    bool stationHealthOpen = false;     // the station health report window (View > Station health; app/panels/station_health_ui.cpp)
    bool sweepOpen = false;             // the sweep scanner window (View > Sweep scanner)
    bool scanViewOpen = false;          // the merged Scanner view: memory table + scan list + tones (View > Scanner)
    bool scannerOpen = false;           // the scanner window (View > Scanner, highly experimental)
    bool schedulerOpen = false;         // Tools > Scheduler: timed and satellite-pass jobs that tune, record and restore (app/panels/scheduler_ui.cpp)
    bool recmgrOpen = false;            // Tools > Recordings: the recordings manager (app/panels/recmgr_ui.cpp)
    bool memoryOpen = false;            // the Memory window (View > Memory, Ctrl+M; app/memory)
    bool datasetsOpen = false;          // the Datasets window (Tools > Datasets; app/packs/datasets_ui.cpp): reference data, user overrides, opt-in updates
    bool packsOpen = false;             // the Data packs window (View > Data packs; app/packs): offline map tiles, satellite elements, airports
    bool freqOpen = false;              // the Frequencies window (View > Frequencies, Ctrl+Shift+F; app/freqdb)
    bool freqTab = false;               // the Frequencies panel as a tab of the main screen (saved as freqTab)
    bool memoryTab = false;             // the Memory panel as a tab of the main screen (View > Memory as a tab; saved as memTab)
    bool propOpen = false;              // Tools > Propagation (app/prop): predicted coverage of a transmitter on the map
    bool mapOpen = false;               // the map window (View > Map): positions and Maidenhead locators
    bool antdOpen = false;              // Tools > Antenna designer (app/antd): wire antenna modeller, method of moments (docs/ANTENNA.md)
    bool rotatorOpen = false;           // View > Antenna rotator (rotator_ui.cpp)
    int wizStep = 0;
    bool setupOpen = false;             // the setup wizard (language, radio, location, goals); Help > Setup wizard
    int setupStep = 0, setupRegion = 0;
    unsigned setupGoals = 0;            // bit per learn::Goal (app/ui/learn_logic.h)
    int setupAntenna = 0;               // learn::Antenna
    char setupLoc[48] = "";             // a Maidenhead locator or coordinates
    bool lessonsOpen = false, glossaryOpen = false, checkupOpen = false;   // Help > Setup and lessons (learn.cpp)
    double wizStepT = 0;
    float wizX = -1;
    ImVec2 tgMin[8], tgMax[8];   // screen rectangles of the parts of the window the tour points at
    VideoTex video;
    float volume = 1.f;
    int tetraSingle = 0;   // TETRA carrier choice: 0 search the span, 1 single carrier (one 25 kHz channel at the dial frequency); saved
    int tetraSlotMode = 0, tetraSlotNum = 1, tetraSlotId = 0;   // TETRA 'Listen to': 0 auto, 1 slot (tetraSlotNum 1..4), 2 talkgroup, 3 ISSI (tetraSlotId); saved
    bool muted = false, subsOn = true;
    int playReq = -1;
    int dabStation = -1;      // --station: DAB sub-channel to play on start (testing)
    std::vector<DeviceInfo> devices; // [0]=synthetic, [1]=file, then sound cards, then radios
    // radios on the network the user added (prefs "netRadios"; listed by refreshDevices) and the "Add network radio" window
    std::vector<NetRadio> netRadios;
    std::vector<std::string> hiddenRadios;   // prefs "hiddenRadios": radios the user hid from the list (by name)
    bool devicesDirty = false;               // the radio list must be rescanned (a radio was hidden or shown)
    std::vector<HamlibRig> hamlibRigs;   // prefs "hamlibRigs"; serial-CAT rigs listed with the other radios
    bool hamlibAddOpen = false;
    int hamlibAddModel = 3073;            // the model picked last (prefs "hamlibAddModel"); 3073 = IC-7300 until the user picks another
    int hamlibAddBaud = 0;                // 0 = the model's default
    char hamlibModelQuery[64] = "";       // the search box of the model picker
    std::vector<RigModelInfo> hamlibModels;   // Hamlib's model list, loaded the first time the window opens
    std::vector<SerialPortInfo> hamlibPorts;  // serial ports found when the window opened or Refresh was pressed
    bool hamlibWasOpen = false;
    uint64_t hamlibRigSeq = 0;            // the last rig-side change applied to the frequency and mode boxes
    bool hamlibLinkWas = true;            // the CAT link state last logged
    char hamlibAddPath[256] = "";
    char hamlibAddLabel[128] = "";
    char hamlibAddAudio[128] = "";         // AudioIn id (empty = CAT only, silence IQ)
    struct NetProbe { std::atomic<bool> done{false}; bool ok = false; std::string err; NetRadio radio; };
    std::shared_ptr<NetProbe> netAddProbe;   // a connection test running on a worker thread
    bool netAddOpen = false;
    // radio groups (synthetic wideband, synth_group.h): prefs "synthGroups", listed by refreshDevices; the "Group radios" window
    std::vector<SynthGroupDef> synthGroups;
    bool groupOpen = false;
    int groupEdit = -1;                   // the saved group being edited, -1 = a new one
    char groupName[64] = "";
    double groupCenterMhz = 100.0, groupSpanMhz = 8.0, groupRateMsps = 0;   // rate 0 = automatic
    std::vector<int> groupPick;           // indices into devices, in frequency order (lowest slice first)
    int netAddType = NetRadio::RtlTcp;
    char netAddHost[128] = "";
    int netAddPort = 1234;
    char netAddRemote[256] = "";   // SoapyRemote: the server radio to ask for ("mirisdr", "driver=sdrplay,serial=1")
    char netAddPass[128] = "";     // KiwiSDR: the receiver's password (blank: none); wiped after the radio is added, never logged
    // "Find KiwiSDR": the public list, downloaded only when the user clicks (kiwiFindWindow, toolbar_rig.cpp)
    struct KiwiFetch { std::atomic<bool> done{false}; bool ok = false; std::string err; std::vector<KiwiServer> list; };
    std::shared_ptr<KiwiFetch> kiwiFetch;
    std::vector<KiwiServer> kiwiList;
    bool kiwiFindOpen = false;
    char kiwiFilter[64] = "";
    char kiwiListUrl[256] = "";
    std::string netAddErr;
    // a search for Hermes Lite 2 radios (a discovery broadcast) running on a worker thread, and what the last one found
    struct NetDiscovery { std::atomic<bool> done{false}; std::vector<FoundNetRadio> found; };
    std::shared_ptr<NetDiscovery> netDiscovery;
    std::vector<FoundNetRadio> netFound;
    bool netSearched = false;
    int devIdx = 0;
    TuneSettings tune;
    FileOptions file;
    int audioIqMode = 0;          // AudioIqMode: I/Q stereo, USB audio, LSB audio, real (mono) (sound-card source)
    int audioChan = 0;            // 0 left, 1 right, 2 left + right (the real modes)
    double audioIfHz = 0;         // the audio frequency the dial sits at (a receiver's IF): shifted down to 0 Hz
    bool audioInvert = false;          // swap the sideband
    bool audioDc = true;              // remove the card's DC offset
    double audioGainDb = 0;       // input gain
    bool audioAfView = true;      // a real (mono) feed: the spectrum shows the card's audio range 0..Nyquist across the screen (display only)
    int audioRate = 48000;        // the sound card's sample rate (8 kHz to 192 kHz); applied at start
    int bwIdx = 0;
    int constView = 0;        // data constellation: 0 cells, 1 density, 2 clusters
    struct ConstStats {       // statistics of the decoded cells, collected over the last few seconds
        static constexpr int G = 96;
        uint64_t seq = 0; int mod = -1, plp = -1;
        std::vector<float> grid;                         // G x G density, row 0 at the top
        struct Pt { double n = 0, ei = 0, eq = 0, e2 = 0; };
        std::vector<Pt> pts;                             // per transmitted point: count, mean error, summed squared error
    } cst;
    bool atscMode = false;    // 6 MHz channel settings: families 1 (ATSC 1.0), 3 (ATSC 3.0) and 4 (ISDB-T)
    bool atsc3Mode = false;   // ATSC 3.0 (family 3)
    bool isdbtMode = false;   // ISDB-T (family 4)
    bool dabMode = false;     // DAB / DAB+ (family 2)
    bool fmMode = false;      // FM radio (family 5)
    bool newUi = true;        // the new interface (ui2.cpp) instead of the classic one (View > Classic interface)
    bool dockOn = false;      // View > Docking: the new interface's panels and floating windows dock into a layout (ui2_dock.cpp); off = floating as before
    struct FamGain { int lna = 32, vga = 34; bool amp = false, known = false; } famGain[kNumSlots];   // the radio gains remembered for each mode
    double famFreq[kNumSlots] = {};  // the frequency each of the modes added after FM was last tuned to (0 = never)
    std::string modeHidden, modeHiddenGroups, modeFavs;   // Mode > Modes shown...: the modes and groups left out of the lists, and the favourites, by name, separated by |
    bool modeFavsOnly = false;   // list only the favourites (and the active mode)
    bool modeVisOpen = false;    // the Modes shown window
    std::string modeGroupsOpen;   // the mode list groups left unfolded, by name, separated by |
    int uiVariant = 0;        // layout of the new interface: 0 sidebar (the default), 1 scope, 2 tiles, 3 faceplate, 4-6 scope children, 7 panel
    bool touchUi = false;     // the touch interface (ui/touch.h): big targets, a bottom dock, full-screen picker and sheets; Settings > Touch interface, --touch
    float touchScale = 1.35f; // the touch interface's own text scale, on top of uiTextScale: 1.0 .. 2.0
    float uiTextScale = 1.f;  // View > Text size: 0.8 .. 2.0, on top of the display scale (Ctrl +, Ctrl -, Ctrl 0)
    float splitSide = 0, splitRight = 0, splitBottom = 0, splitDock = 0, splitBot2 = 0;   // dragged panel sizes in unscaled points (0 = automatic): sidebar, instruments, lower row; scope layout: instruments, lower row
    int uiTheme = 1;          // its palette: 0 terminal, 1 instrument, 2 mono
    std::string uiLang;       // the interface language chosen in View > Language ("de", "lt" ...), empty: the system's
    int dtmbBwMhz = 8;        // DTMB channel width: 8 MHz (China, Hong Kong), 6 MHz (Cuba)
    int isdbtBwMhz = 6;       // ISDB-T channel width: 6 MHz (Japan, the Americas), 7, 8 MHz (the Maldives); 0 = automatic
    bool atvFm = false;       // analog TV: FM video as analog FPV links send it (20 Msps), not the amplitude modulated broadcast signal
    int atvNbtv = 0;          // analog TV: 0 television, 1 NBTV / mechanical TV on AM, 2 the same on narrow FM (the audio is the picture)
    int atvNbtvLines = 32;    // NBTV / mechanical: lines (24 .. 240) and pictures a second; 32 at 12.5 is the NBTVA standard
    float atvNbtvFps = 12.5f;
    bool identifyTool = false;   // the Identify tool (key I): a click or a drag on the spectrum identifies the signal instead of tuning (never saved)
    bool wideDecode = false;  // "Whole span": decode every carrier in the visible span, not only the tuned one (only for modes that opt in: modeSupportsWide)
    uint64_t wideFamMask = 0; // the switch per mode family (bit = family); wideDecode is the current mode's bit
    uint64_t wideFamMaskHi[2] = {0, 0};   // the same for families 64-127 and 128-191 (wideFamOn / setWideFam read and write all three)
    dect2::WideTuning wideTuning;         // the whole-span detector's settings (the carriers table's settings button)
    bool wideTuningDirty = true;          // sent to the engine at the next frame
    int wideSel = 0;          // the carrier the sound and the detail follow (0: the best locked one)
    bool mtvOn = false;       // "Multi-channel": decode two or three adjacent TV channels of a wide capture (DVB-T2 / DVB-T)
    int mtvMax = 0;           // receivers at once (0: 2 on a laptop, 3 on a desktop)
    int mtvGrid = 0;          // 0 find out, 1 the capture is centred on a channel, 2 between two channels
    std::vector<MultiService> mtvSvc;   // every service of every channel (refreshed with ts)
    bool atvWide = false;     // analog TV: system E chosen (14 MHz channel, 10 MHz of video): the radio runs at 20 Msps
    // View > Power (one menu for both power features of 3.0: dect2/power/power.h and the frame pacing of app/ui/frame_pace.h)
    int powerLowMode = 1;     // Low power mode: 0 off, 1 automatic (on battery or when the machine is hot), 2 always. While it applies: slower
                              // spectrum / telemetry, input redraws at 30 fps, a receiver in the background at 10 (video keeps its rate)
    int powerHidden = 1;      // Save power when hidden: 0 off, 1 on battery, 2 always (slower spectrum updates, scanners wait)
    bool reduceCpuHidden = true;   // Reduce CPU when hidden: no drawing while the window is minimised or hidden (decoding goes on)
    bool perfOverlay = false;      // View > Performance overlay: frame rate, CPU per thread, wake-ups (ui/perf_overlay.cpp)
    bool discordOn = false, discordFreq = true;   // View > Show in Discord (opt-in Rich Presence, discord_rpc.cpp) and its "Show the frequency"
    bool mapSend = false, mapConsent = false;     // Server > Send data to map server (opt-in; consent once)
    bool mapVoice = false;                        // Server > also share transcripts of airband radio calls (public text; own choice, off until chosen)
    bool mapJam = false;                          // Server > also share GPS interference cells (counts per 0.5 degree cell, no aircraft ids); off until chosen
    bool mapHam = false, mapHamConsent = false, mapHam6 = false;   // Server > also share ham contacts (FT8/FT4/WSPR/JS8/logged QSOs: callsigns, locator squares, mode, band, time); own consent; 6-character squares only when chosen; all off until chosen
    bool mapMsgs = false;                         // Server > also share interesting ACARS messages (free text becomes public); off until chosen
    std::string mapDeclinedVersion;               // ONAIR_VERSION when "No, do not ask again" was chosen; another version asks again
    bool mapDeclined = false;                     // the start-up question was answered "No, do not ask again" (the Server menu still works)
    std::string mapStationId;                     // client id for /ingest, generated once
    std::string mapServer;                        // empty: https://map.on-air.dev (a local worker URL for developers)
    bool feedsOpen = false;   // View > Data feeds: the window (feeds_ui.cpp)
    bool swarmOpen = false;   // View > LAN swarm: the window (swarm_ui.cpp)
    bool watchlistOpen = false;    // View > Watchlists: alerts when listed aircraft, ships, call signs, frequencies or words are heard (watchlist_ui.cpp)
    bool msgdbOpen = false;        // View > Messages: the decoded-message database (msgdb_ui.cpp)
    bool protolabOpen = false;     // View > Protocol lab: reverse-engineering workbench for unknown signals (protolab_ui.cpp)
    bool signallabOpen = false;    // Tools > Signal Lab: blind analysis of an unknown digital signal, stage by stage, with a bit viewer (signallab_ui.cpp)
    bool modeBuilderOpen = false;  // Tools > Mode Builder: decoders built from a JSON definition (modebuilder_ui.cpp)
    bool txStudioOpen = false;     // Tools > TX studio: make, measure, decode-check and save a transmit signal without a radio (tx_studio.cpp)
    bool pluginsOpen = false;      // View > Plugins: the plugin manager (plugin/plugin_app.cpp)
    bool automationOpen = false;   // View > Automations: rules and scripts (automation_ui.cpp)
    dect2::FeedConfig feeds;  // the data feeds out (all off by default), saved with the preferences
    dect2::ShareConfig share; // "Share my radio": the rtl_tcp server (off by default; feeds_ui.cpp), saved with the preferences
    dect2::PskReporterConfig pskr;   // PSK Reporter spots (off by default; feeds_ui.cpp), saved with the preferences
    dect2::WsprNetConfig wsprNet;    // WSPRnet spots (off by default; feeds_ui.cpp), saved with the preferences
    bool highContrast = false; // View > Theme > High contrast: black, white and yellow, thick outlines, dim text lifted to 7:1 (ui2.cpp, main.cpp); wins over lightUi
    bool lightUi = false;     // View > Light: every colour drawn with its lightness turned over (dark on white), hues kept (main.cpp)
    int family = 68;           // 68 DVB, 1 ATSC, 2 DAB, 3 ATSC 3.68, 4 ISDB-T, 5 FM, 6 DVB-S/S2, 7 DTMB, 8 analog TV, 9 DMR, 168 DRM, 11 ADS-B, 12 GNSS, 13 radiosonde, 14 AIS, 15 marine, 16 ACARS, 17 Inmarsat-C, 18 Inmarsat Aero, 19 Iridium, 268 mesh, 21 HD Radio, 22 CDR, 23 pagers, 24 APRS / packet, 25 HF digital, 26 airband, 27 VDL Mode 2, 28 ISM sensors, 368 VOR / ILS, 31 M17, 33 D-STAR, 34 CW, 35 HFDL, 36 PSK31, 37 JS8Call, 42 spectrum analyser, 38 NXDN, 468 DVB-C, 45 dPMR, 44 Orbcomm, 43 Mode S uplink, 46 TETRA, 49 HF ALE, 47 MAC, 48 radio astronomy, 568 P25, 51 Bluetooth LE, 52 ionospheric sounders, 53 QO-16868, 54 Wi-Fi networks, 55 general receiver, 61 CubeSat telemetry, 64 HRPT / AHRPT (6 and up: see ModeUi; engine standard code = family + 2)
    int sub = 0;              // 1 .. kNumSubModes: the protocol entry (dect2::subModes() index + 1) of HF digital, mesh, pagers, marine or VOR / ILS; 0 none
    std::deque<float> dabSnrH, dabFicH, dabCfoH, dabCirH;
    std::deque<float> dabBufH, dabRsH, dabAuH;   // of the selected service: they restart when another one is selected
    int dabHistSub = -2;
    // announcements (EN 300 401 8.1.6): switch to the sub-channel that carries one while it lasts
    bool dabAnnOn = true; unsigned dabAnnMask = 0x3F;   // types 0..5: alarm, road traffic, transport, warning, news, weather
    int dabUserSub = -1;                                // the sub-channel the user chose (what is played when there is no announcement)
    bool dabAnnActive = false, dabAnnSuspended = false; // switched to an announcement; the user went back to the service until it ends
    int dabAnnSub = -1, dabAnnType = -1;
    double dabAnnLastSwitch = -1e9;                     // time of the last switch to or from an announcement: at most one every 5 s
    bool dabPrefsLoaded = false;
    int dabRecFmt = 0; bool dabRecLog = true;           // recording: 0 WAV, 1 as broadcast
    std::string dabRecErr;
    // following: when the signal is lost, the service is looked for on the other frequencies the FIC lists for its ensemble (FIG 0/21) and for the
    // other ensembles that carry it (FIG 0/24)
    bool dabFollow = false;
    std::vector<std::pair<double, uint16_t>> dabAlts;   // MHz, ensemble id
    double dabAltStamp = 0, dabLostSince = 0, dabFollowT0 = 0;
    int dabFollowIdx = 0;
    uint32_t dabFollowSid = 0;
    bool dabFollowing = false;
    uint64_t dabRsLast = 0, dabAuLast = 0;
    std::deque<float> fmSnrH, fmPilotH, fmRdsH;
    bool fmMono = false;      // FM: force mono (a weak station is quieter in mono)
    int fmDeemph = 50;        // FM de-emphasis in microseconds: 50 (Europe, Middle East, most of the world) or 75 (Americas, South Korea)
    int fmBass = 0, fmTreble = 0;   // FM tone control, -6 .. +6 dB each; both 0 with fmWarm off = audio untouched (saved as fmBass, fmTreble)
    bool fmWarm = false;      // FM Warm preset: a gentle -3 dB high shelf above 6 kHz (saved as fmWarm)
    int fmStereo = 0;         // FM stereo choice: 0 auto, 1 mono (fmMono kept in step), 2 stereo whenever a pilot is there (issue #213)
    int fmAudioBw = 0;        // FM audio bandwidth in Hz: 0 full (15 kHz), 12000, 8000, 5000
    int fmSquelch = 0;        // FM squelch in dB SNR: 0 off
    bool pagerNarrow = false;  // pagers: 12.5 kHz channel filter instead of 25 kHz (saved as pagerNarrow)
    bool packetNarrow = false; // APRS / packet: 12.5 kHz channel filter instead of 25 kHz (saved as packetNarrow)
    int fmRegion = 0;         // RDS meanings: 0 Auto (odd tenths of a MHz = RBDS), 1 RDS (Europe), 2 RBDS (US); saved as fmRegion
    int fmChanKhz = 0;        // FM channel filter: 0 = automatic (the receiver's standard 220 kHz), else a manual width of 100 to 300 kHz
    struct DabScan {
        bool running = false; int idx = -1; double t0 = 0, lockT = 0, savedFreq = 218.64;
        struct Res { std::string name, label, stations; double mhz = 0; bool found = false; float snr = 0; };
        std::vector<Res> results;
    } dabScan;
    struct FmScan {
        bool running = false; int phase = 0; int idx = -1; double t0 = 0, savedFreq = 100.0; uint64_t seq0 = 0, rxSeq0 = 0;   // seq0: spectrum frame, rxSeq0: receiver report at the moment of tuning; phase 0 surveys the band, phase 1 checks each candidate
        double scanLoMhz = 87.0, scanHiMhz = 108.0; int surveyHops = 11;   // scan range (MHz), fixed for the run from preferences
        std::vector<double> cand;
        std::vector<std::string> psSeq; double psT = 0;   // the names the station sent on this channel, in order, and when the last new one came
        // rt: radio text; psText: the names of a station whose name changes (a scrolling name), in order; pi: programme identification
        struct Res { std::string name, pty; double mhz = 0; bool found = false, stereo = false, rds = false; float snr = 0; std::string rt, psText; int pi = 0; bool tp = false; };
        std::vector<Res> results;
    } fmScan;
    std::vector<FmSavedStation> fmSaved;   // stations the user saved by hand, kept in the preferences between sessions
    FmSavedEdit fmSavedEdit;               // rename box, search and delete confirmation for this receiver only
    bool bwAuto = true;       // the engine measures the channel width and switches by itself
    double freqMhz = 522.0;
    bool autoScroll = true;
    int tab = 0;
    SpectrumFrame spec;
    std::vector<float> smooth, peak;
    std::vector<float> smoothPow, specView;   // the trace averaged in power; the frame as the plots show it (dB per Hz with specPerHz)
    bool specPerHz = true;              // levels in dB per Hz: the noise floor stays where it is when the FFT size changes (else per bin)
    uint64_t lastSeq = 0;
    Waterfall wf;
    float yMin = -110, yMax = -30;
    bool specAutoDb = true;             // the spectrum's dB range follows the noise floor of the view (the look says how high it sits); a drag of Min/Max turns it off
    spipe::FloorTracker specFloor;      // that floor (the same estimate as the waterfall's)
    float specZoom = 0.f;   // 0..1 frequency zoom for the vertical slider (0 = full bandwidth, 1 = ~1 kHz span)
    int tuningMode = 0;                 // 0 retune the radio LO on spectrum click, 1 decode offset only (sticky LO)
    bool dragMovesTuned = false;        // dragging past the band: true = the tuned frequency travels with the radio centre
    bool centreTuning = false;          // spectrum click: true = centre the radio on the click (target button), false = normal (marker inside the band)
    int fileEdgeCode = 0;               // IQ file: 0 fine, 1 the passband touches the edge of the recording, 2 the tune box was held back at the edge, 3 the bandwidth is wider than the recording
    bool fileInfoOpen = false;          // the File info dialog
    bool peakHold = false;             // the max-hold line (off by default: one trace; the orange max-hold line is for those who switch it on)
    float holdDecay = 4.5f;             // dB a second the max-hold line falls (0: it stays until it is reset)
    float specAvgSec = 0.08f;           // time constant of the averaged trace in seconds (0: the raw frames)
    bool specLabels = true; int specLabelMask = 63;   // names drawn on the spectrum and the waterfall (gfx/autolabel.cpp); the mask: which sources (band plan, frequency database, FM, DAB, DVB, memories)
    int specFft = 0;           // spectrum FFT size: 0 = automatic from the sample rate (8192..32768 bins), else 4096..262144 (#243)
    int traceColor = 0;        // the trace colour (cmap::kTracePresets; 0: soft white, dark grey on a light theme)
    int holdColor = -1;        // the max-hold line: -1 the usual grey, else a trace preset
    bool minHold = false;      // a min-hold line too (the floor under the signals; it rises back at the hold decay)
    bool showLive = false;     // with averaging on: the live (unaveraged) line faintly under the average
    std::vector<float> minPk;  // the min-hold line
    bool nbOn = false;         // the impulse noise blanker (#264): off by default
    float nbThr = 8.f, nbWidthUs = 20.f;   // its threshold (times the mean level) and blank width
    bool wfPerMode = false;    // each mode remembers its own waterfall colour map (saved as wfMapMode<family>)
    int wfMapFamily = -1;      // the mode the map was last loaded for
    std::string wfCustom;      // the custom gradient's stops (cmap::Gradient text)
    std::string specMarkers;   // the frequency markers on the spectrum and the waterfall (specaxis::parseMarkers text)
    float specLine = 1.2f;     // thickness of the spectrum trace
    bool specFill = true, specGrid = true, specPeaks = false, specReadout = true, specDbm = false;   // fill under the line, grid, peak markers, level at the cursor, dBm instead of dBFS
    // The spectrum look (gfx/spec_look.h: 0 OnAir, 1 SDR++ look, 2 SDRoxide look) and the choices around it
    int specLook = 1;
    int specAxisLabels = -1;   // the frequency labels: -1 as the look has them, 0 frequencies, 1 offsets from the tuned frequency
    float specLabelSize = 1.f; // the axis label size, times the look's
    int specSmoothTrace = -1;  // mild smoothing passes over the trace: -1 as the look has it, else 0..4
    bool specNfMarker = false; // the noise-floor line with its level on the spectrum
    bool specSmoothZoom = true;// the wheel zoom eases in over a few frames
    int specSnapHz = 0;        // a click tunes to a multiple of this (0: the mode's own step)
    bool wfLowPass = false;    // the waterfall's 2D noise reduction (off by default)
    bool specDemo = false;     // --specdemo: a synthetic spectrum (FM carriers, an OFDM block, an SSB voice) for screenshots
    double specLastT = 0;               // when the last spectrum frame came in
    ImFont* mono = nullptr;
    ImFont* ui = nullptr;
    std::string hackrfErr;
    bool hackrfErrHidden = false;       // the user dismissed the listing error; a new listing shows it again
    std::string startErr, startErrKey;  // last failed start/retune of the radio (from the engine log); key = the log line it came from, so a dismissed one stays away
    std::vector<std::string> usbHints;  // why a plugged-in radio is not listed (Linux), refreshed with the radio list
    double diagCopiedAt = -10, diagPollAt = -10;
    bool diagWasRunning = false;
    bool recSigmf = true;               // write SigMF metadata beside IQ recordings (pref "recSigmf", default on)
    int audioRecFmt = 0;                // audio recording format: 0 = WAV, 1 = FLAC, 2 = MP3 (pref "audioRecFmt")
    bool audioRecGate = false, audioRecSplit = false, audioRecKeep = false;   // only while audio is present / one file per transmission / keep going across retunes (prefs "audioRec...")
    // "Monitor audio" (prefs "mon...") and the audio output devices
    bool monOn = false; int monDemod = 4; float monVol = 1.f; int monLatency = 100; bool monYield = true;   // demod 4 = Auto (USB below 30 MHz, NFM above)
    std::string monDev, monDev2; bool monOut2 = false; float monVol2 = 1.f; double monOffset = 0;
    bool monNet = false; int monPort = 7355; bool monBindAll = false, monWav = true;
    bool audioRecWasActive = false; int audioRecMode = 0; double audioRecFreq = 0; std::string audioRecDonePath;
    int recFormat = 0;                  // IQ recording format: 0 = 8-bit, 1 = float, 2 = X-Midas BLUE .tmp with float samples (pref "recFormat")
    int tvBufMb = 256;                  // TV live buffer for Pause (pref "tvBufMb")
    // HD / SD simulcast (core/src/tv/hd_pair.h): the services of the scanned multiplexes (prefs "tvCatalog"), the user's pairing table ("tvHdPairs"), the per-programme
    // choice ("tvHdChoice": programme key -> 1 HD, 0 SD) and "always prefer HD" ("tvHdAlways")
    hd::Catalog hdCat; std::vector<hd::Override> hdTable; std::map<std::string, int> hdChoice; bool hdAlways = false;
    int hdRev = 1;                      // counts changes of the catalogue and the table (for the cached pairing)
    std::string hdCatText;              // the catalogue as last saved
    double hdCatT = 0;
    int hdPendSid = -1, hdPendTsid = 0; double hdPendFreq = 0, hdPendT = 0;   // a switch that waits for the retune to the other multiplex
    std::string hdLang; int hdLangSid = -1;   // audio language to keep on the service just switched to
    int hdLastSid = -1; double hdLastFreq = 0;   // the service the HD preference was last applied to
    int hdKeySid = -1, hdKeyRev = 0; double hdKeyFreq = 0; int hdCacheHd = -1, hdCacheSd = -1; std::string hdCacheWhyHd, hdCacheWhySd;   // cached pairing of the playing service
    int tvAudioIdx = 0;                 // the audio track chosen in the player tab
    int tvRecMin = 0, tvRecAtH = 0, tvRecAtM = 0;   // TV recording timer: stop after N minutes (0 = off), at a clock time
    bool tvRecAtOn = false, tvRecWasActive = false;
    std::string tvRecDonePath, tvRecErr;   // the last finished TV recording; why one could not start
    bool recWasActive = false;          // a recording was running at the last frame (to log it once when it ends)
    std::string recDonePath;            // the last finished recording, for "Show recording"
    RxTelemetry rx;
    uint64_t rxSeq = 0;
    // Second mode (Engine::setSecondary): a second receiver on the same capture. Its screens are the mode's own, drawn with a.rx swapped
    // for its report (secondScope, modeui.cpp); its sound stays off unless asked for.
    struct SecondMode {
        int modeIdx = -1;     // the kModes row chosen, -1 = none
        double mhz = 0;       // its frequency
        bool audio = false;   // play its sound (off: muted whatever the first mode does)
        RxTelemetry rx;
        uint64_t seq = 0;
        bool drawing = false; // its screens are being drawn: tuneFreq() tunes it instead of the radio
    } second;
    TsSnapshot ts;
    BbStats bb;
    double tsT = 0;
    OutputConfig out;
    int selService = -1;     // service id used for outputs (-1 = whole multiplex)
    char udpHost[64] = "127.0.0.1";
    char filePath[512] = "";
    std::deque<float> hCfo, hSnr, hTiming;
    struct HistSample { float t, snr, mer, loss, cfo, sro, level, clip, quality, aux; };   // aux: FM, the share of RDS blocks received intact
    std::deque<HistSample> hist;      // 4 samples per second, last 15 minutes
    double histT = 0;
    uint64_t histOk = 0, histBad = 0;
    bool rxSeen = false;
    int computeMode = 2; // 0 CPU, 1 GPU, 2 auto
    int stdMode = 0;     // 0 auto, 1 DVB-T2, 2 DVB-T
    bool popOut = false, videoOnly = false;
    bool popTop = true;      // the pop-out video window stays above other windows
    int histWindow = 60;
    int ttxPage = 100;
    int ttxSub = -1;                // teletext subpage shown in the tab: -1 = the newest
    int ttxEntry = 0, ttxEntryVal = 0;   // digits typed so far for a page number
    bool ttxHold = false, ttxReveal = false;
    TtxPage ttxHeld;                // the page frozen by "hold"
    std::vector<SubOpt> subOpts;    // subtitle services of the playing programme (DVB subtitles, teletext subtitle pages)
    int subIdx = -1;                // the one chosen
    std::string subsPick;           // its key, remembered between runs
    std::string tvLang;             // the language to prefer for the guide texts, the audio track and the subtitles of a TV service (ISO 639-2, "fin"); empty: what the broadcaster puts first
    int plpSel = -1; // -1 = automatic
    int subTv = 0, subRx = 0, subStream = 0; // sub-view of the TV / Receiver / Stream tabs
    std::map<int, std::vector<EpgEvent>> epg;
    double epgT = -10;
    int guideSid = -1, guideEvent = -1, guidePlaySid = -1; // guide follows playing service until user picks another in the list
    bool fakeEpg = false; // --fakeepg: made-up programmes, for checking the layout when the mux sends none
    char favName[64] = "";
    MultipathDetector mpd;
    QualityMeter quality;
    std::vector<SavedChannel> channels; // DVB-T2 muxes found by the scanner (remembered)
    bool scanWas = false;
    double scanHarvestT = 0;
    bool agcOn = false;
    std::string gainDev;   // the radio a.tune.gainDb was set for (board:serial); another radio starts at its own sensible gain
    double autoRateHz = 0;      // the mode's own radio rate ("Auto" of the sample-rate control), set by applyBandwidth()
    double chosenRateHz = 0;    // the rate chosen for this mode on this radio and applied by applyBandwidth() (0 = Auto)
    std::string biasCtx;        // the mode / radio the antenna power was last loaded for (biasContext)
    std::string rateCtx;        // mode / radio / channel width applyBandwidth() last ran for (the control refreshes Auto when it changes)
    std::string radioKey;  // the radio a.tune.radio (its own settings, DeviceInfo::settings) was loaded for (radioKeyOf); "" = none
    SmartAgc agc;          // the auto-gain loop (agc_ui.cpp, core/src/source/agc.cpp); configured per radio and mode when it starts
    std::string agcCtx;    // the radio / mode / width / rate it was configured for ("" = start it afresh)
    double agcFreqMhz = 0; // the frequency it last ran at: a retune starts a new acquisition
    bool agcAmpAllow = false;   // "Let AGC turn the amp on/off by itself" for this radio (prefs agcamp.<radio>), default off
    std::string agcAmpCtx;      // the prefs key agcAmpAllow was loaded for
    bool agcGearOpen = false;
    GainSweep sweep;
    GainOptSession gopt;   // "Optimise gain" (gain_opt_ui.cpp): a sweep of the radio's gain range for the best quality that does not overload
    bool sweepRetune = false;
    Scanner scanner;
    ScanConfig scanCfg;
    int scanPreset = 0;
    std::string bandPlan;     // the active band plan (bandplan_ui.cpp): empty = automatic from the language and region, "none" = the built-in ranges
    bool scanWasRunning = false;
    bool scanShowAll = true;            // the TV scan list: the services of every scanned transponder (off: only the tuned one)
    MuxId liveMux; bool liveMuxValid = false;   // what the live service list belongs to (a change empties it)
    std::unique_ptr<Updater> upd;      // update check and installer
    bool updCheck = true, updAuto = true, updPre = false, updStarted = false;   // official updates on; betas off until asked
    double updLast = 0;                // when the last check was made (seconds since 1970)
    int updSrc = 0;                    // update source: 0 automatic (GitHub), 1 GitHub only, 2 mirror only
    bool updMirrorOn = false;          // automatic: also try the mirror when GitHub fails (off until the user turns it on)
    std::string updMirror = "https://mirror.on-air.dev/{url}";   // the mirror address (the project's own mirror until the user types another); it is only contacted when the user chose the mirror
    bool updConfirm = false;           // the user ticked "download it anyway" for a package only the mirror vouches for
    std::string updSkip;               // a version the user chose to skip
    DirectionFinder dir;
    int antKind = 0;          // 0 directional, 1 dipole / indoor, 2 omnidirectional
    bool dirAgcWas = false;
};






// ------------------------------------------------------------------ UI pieces


// ------------------------------------------------------------------ small widgets (pills, tags, gauges)





// label : value readout in the status area (label dim, value in the mono font)











































// ------------------------------------------------------------------ the six top-level views










// ------------------------------------------------------------------ antenna direction finder












// ------------------------------------------------------------------ main

// ------------------------------------------------------------------ the panels and helpers (defined in the .cpp files)
// widgets.cpp
bool tabItem(const char* name, Ic icon);
// puts the layout cursor back where it was before an overlay (buttons drawn over a plot) without "extending" the window: the saved spot is
// below the last item, so a plain SetCursorScreenPos() there followed by the end of the window is the ImGui error "Code uses SetCursorPos()
// to extend window/parent boundaries" (seen in ##main2)
void restoreCursor(ImVec2 screenPos);
// once a frame: a floating window whose title bar went past the edge of the display (a saved position from a larger or a second monitor,
// a smaller window after a DPI change) is moved back so it can be grabbed again
void keepWindowsReachable();
// SetNextWindowSize(..., ImGuiCond_FirstUseEver) with the size made to fit the display (a large window at 150 % on a laptop screen)
void firstWindowSize(float w, float h);
// a button that only acts while the receiver runs: greyed out (with the reason as its tooltip) while it is stopped, instead of doing nothing
bool runningButton(const App& a, const char* label, bool small = false);
// a panel that answers Space itself (the TV picture, the transmitter) sets this to the frame number; the new interface's Space = start /
// stop the receiver is then left out, so one key press does not do two things (widgets.cpp)
extern int gSpaceClaimFrame;
// the size of a button that is at least minW wide and always wide enough for its (translated) label
ImVec2 fitButton(const char* label, float minW);
// a combo box of translated items: each item is marked with N_() where it is written and passed through tr() here
bool comboTr(const char* label, int* v, std::initializer_list<const char*> items);
// the interface language: menu items (View > Language) or a small button with a list (the classic interface); both save the choice
void languageMenuItems(App& a);
void languageButton(App& a);
void setUiLanguage(App& a, const std::string& code);   // empty: the system's language
extern bool gTightTabs;   // the tabs of mainTabs() get less padding (a narrow pane)
void toggleFullscreen();
void handleFullscreenHotkeys();          // F11 and Alt+Enter; ignored while a text field has the keyboard
std::string adsbJamBody(const std::string& station);   // adsb_ui.cpp: the POST /jam body of the GPS interference cells ("" when there is nothing to share)
bool adsbFullMap(App& a, ImVec2 disp);           // adsb_ui.cpp: the ADS-B map or radar over the whole window when it is popped out
bool modeacFullMap(App& a, ImVec2 disp);         // modeac_ui.cpp: the same in Mode A/C (its aircraft, #235)
// The radio centre (Hz) for a retune that only changes the gain or a radio setting: a.freqMhz, unless a click decodes off the centre
// (tuneDecode, "Decode offset only"): then the radio stays where it is (#236)
double settingsRetuneCentreHz(const App& a);
void receiverLocation(double& lat, double& lon);  // adsb_ui.cpp: the one receiver position (the ADS-B antenna position)
bool receiverLocationSet();                        // the user has set it (else it is a placeholder)
void receiverPositionFields(App& a);               // its fields (latitude, longitude, altitude) and the Locate button
double receiverAltitudeM();                         // its height above sea level, metres (0 until set)
void setReceiverLocation(App& a, double lat, double lon);   // move it (saved), e.g. View > Map's "Use as my location"
void syncAdsbReference(App& a);                              // push antenna position into the ADS-B tracker while that mode is running
struct MapSatRef { std::string name; int norad = 0; const dect2::Sgp4* sgp = nullptr; };
void passesMapSatellites(std::vector<MapSatRef>& out);   // passes_ui.cpp: the listed satellites with a TLE (for the live map)
struct MapCellTower { double lat = 0, lon = 0; int rangeM = 0; std::string label; unsigned colour = 0; };
void cellMapTowers(const dect2::CellTelemetry& t, std::vector<MapCellTower>& out);   // cell_ui.cpp: the towers placed (OpenCelliD / CSV)
void mapWindow(App& a);                             // map_ui.cpp: View > Map (a.mapOpen): coordinates, Maidenhead locators, distance and bearing
void propagationWindow(App& a);                     // prop_ui.cpp: Tools > Propagation (a.propOpen): Simple / Advanced coverage prediction (prop_adv.cpp)
void mapSendServerMenu(App& a);                     // map_send.cpp: Server menu (opt-in public map)
void antennaDesignerWindow(App& a);                 // antd_ui.cpp: Tools > Antenna designer (a.antdOpen): templates, wires, SWR, pattern, optimiser
void mapSendButton(App& a);                         // classic interface: the same choice as a button
void mapSendTick(App& a);                           // POST current decoded positions when opted in
bool mapPauseForTransmit(App& a);                   // turn the map connection off before the radio is keyed; true if it was on
void mapTransmitAbort(App& a);                      // the radio did not start: put the connection back if it was on
void mapTransmitEnded();                            // the radio is quiet: the Server control works again; the next send skips what is already on screen
void aeroToolsTick(App& a);                         // aero_ui.cpp: SNR-sound tone while Inmarsat Aero is the mode
void mapHamRemember(const std::string& call, const std::string& grid);   // map_send.cpp: a callsign's locator, for the partner position of a contact (own station included)
void mapHamHeard(const char* mode, double rfHz, const std::string& from, const std::string& fromGrid, const std::string& to, double tUtc);   // a decoded or logged contact; ignored unless Server > share ham contacts is on and consented
void mapSendShutdown();                             // join the ingest thread before exit
bool mapFamilySends(int family);                    // this mode can put decoded positions on the public map
std::string mapSendStatusLine();                    // short "connected / N sent" line for the mode header (empty when off)
std::string mapSendDiagnosticsBlock(const App& a);  // map.on-air.dev ingest state + recent log for Copy diagnostics
void passesDevTle(const std::string& path);       // dev (--tle): TLEs from a file, as if pasted
std::vector<std::pair<std::string, double>> passesArgosDoppler(double unixSec, double freqHz);       // passes_ui.cpp: MetOp (Argos downlink) satellites above the horizon and their Doppler
std::vector<std::pair<std::string, double>> passesGlobalstarDoppler(double unixSec, double freqHz);   // passes_ui.cpp: Globalstar satellites above the horizon and their Doppler (TLEs from the planner)
struct DfMapLine { double lat = 0, lon = 0, bearing = 0, sigma = 5, conf = 0, freqHz = 0, ageSec = 0; };
void rfiWindow(App& a);                             // rfi_ui.cpp: the interference hunter window (a.rfiOpen), every frame
void passiveRadarWindow(App& a);                    // passive_ui.cpp: the passive radar window (a.passiveRadarOpen), every frame
void dfWindow(App& a);                              // df_ui.cpp: the direction finder window (a.dfOpen), every frame
bool dfMapLine(const App& a, DfMapLine& out);       // df_ui.cpp: the current bearing as a map line, only when the user chose to share it and it is fresh and confident
void satnogsWindow(App& a);                        // satnogs_ui.cpp: the SatNOGS DB transmitter browser window (a.satnogsOpen), every frame
void passesWindow(App& a);                         // passes_ui.cpp: the satellite pass planner window (a.passesOpen) and its tracking, every frame
void memoryWindow(App& a);                         // memory_ui.cpp: the Memory window (a.memoryOpen), the Ctrl+M key, the scan of the flagged memories, every frame
void datasetsWindow(App& a);                        // datasets_ui.cpp: the Datasets window (a.datasetsOpen), every frame
void packsWindow(App& a);                           // packs_ui.cpp: the Data packs window (a.packsOpen), every frame
void freqdbWindow(App& a);                          // freqdb_ui.cpp: the Frequencies window (a.freqOpen), the Ctrl+Shift+F key, every frame
void freqdbTabItem(App& a);                         // freqdb_ui.cpp: the "Frequencies" tab when it is switched on (inside the main tab bar)
void freqdbTabToggle(App& a);
struct ModeDef;
bool freqdbPickMhz(const ModeDef& m, float width, double& mhz);   // freqdb_ui.cpp: the transmitter's band presets of one mode-list row; true when one is picked
void freqdbKnownCombo(App& a, float width);         // freqdb_ui.cpp: the Tuner panel's "Known frequencies" drop-down for the mode on screen
void memoryTabItem(App& a);                        // memory_ui.cpp: the "Memory" tab when View > Memory as a tab is on (inside the main tab bar)
int memorySelfTest(App& a);                        // memory_selftest.cpp: --memory-selftest, the recall checked on a hidden run (exit code = failures)
void memoryDev(int what);                          // memory_ui.cpp: --memdev (screenshots)
void memoryQuickBar(App& a);                       // memory_lists_ui.cpp: the starred memories of the switched-on lists as one row of buttons (every mode)
void memoryToolbarButton(App& a);                  // memory_ui.cpp: the same quick list as a small "M" button with a pop-up (the classic toolbar)
void memoryMenu(App& a);                           // memory_ui.cpp: the "M" menu of the menu bar: open, store, the favourites and the ten most recent
double generalListenHz(const App& a);
void generalSetListenHz(App& a, double hz);       // general_ui.cpp: move the listening frequency (SDR++: the VFO inside the captured band, else the radio)
int generalApiDemod();                              // general_ui.cpp, for the remote API: the demodulator (GeneralDemod), set it, the squelch
bool generalApiSetDemod(int demod);
void generalApiSquelch(bool on, bool autoLevel, double db);
bool generalApiRecord(App& a, bool on, std::string& err);
std::string tvRecJobsJson();                        // tvrec_ui.cpp, for the remote API: the recorder's scheduled jobs (read only)
void generalRecallHz(App& a, double hz);          // general_ui.cpp: a memory's frequency: the dial on hz, the channel marker back on the dial              // general_ui.cpp: the general receiver's listening frequency (the dial plus its offset)
void stationHealthWindow(App& a);                  // station_health_ui.cpp: the station and antenna health report (a.stationHealthOpen)
void sweepWindow(App& a);                          // sweep_ui.cpp: the fast sweep scanner window (a.sweepOpen)
void scannerWindow(App& a);                        // scanner_ui.cpp: the scanner window (a.scannerOpen) and its scan loop, every frame (HIGHLY EXPERIMENTAL)
void scannerButton(App& a);                        // a button that opens it (the classic interface's Scan tab)
GLFWmonitor* monitorFor(int x, int y, int w, int h);   // widgets.cpp: the monitor holding most of the rectangle
// The monitors, kept from the last change GLFW reported (glfwSetMonitorCallback) instead of asked for on each use: a monitor that goes away
// (display asleep, cable out) is dropped from the list before anything asks for its size or position again.
void watchMonitors();                                  // after ImGui_ImplGlfw_Init (it installs a monitor callback of its own; this one chains it)
const std::vector<GLFWmonitor*>& monitors();
GLFWmonitor* primaryMonitor();                         // nullptr when there is none (all asleep)
extern bool gWayland;   // main.cpp: GLFW runs on Wayland (window positions cannot be read or set there)
// popout.cpp: the video pop-out as a window of its own
extern bool gNoOsWindows;            // --hidden: no extra OS windows (the pop-out stays a panel inside the main window)
void popOutFrame(App& a);            // inside the ImGui frame: opens/closes the window and builds its picture
void popOutPresent();                // after the main frame (or instead of it while the main window is minimised): draws the window
bool popOutShown();                  // the pop-out window is open and not minimised
void popOutShutdown();               // before the graphics back end shuts down
bool pillButton(const char* label, bool selected, float padX = 11);
int subNav(const char* id, int& cur, std::initializer_list<const char*> names);
float tagAt(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 bg, ImU32 fg = IM_COL32(225, 232, 240, 255));
void gaugePill(float width, float frac, ImU32 fill, const char* text);
void lamp(const char* label, int state /*0 grey 1 green 2 amber 3 red*/, int icon = -1);
// Lost samples (Engine::sampleLoss()) for the "dropped" read-outs: "0", or where they went (callback gaps, ring overflow) and, when the
// radio is under about 97% of its set rate, that rate. Red while that is happening or a loss is recent, amber after; the tooltip says
// which problem it is (the radio's deliveries, the input ring, or receiver load).
std::string lossText(const SampleLoss& l);
ImVec4 lossColour(const SampleLoss& l, ImVec4 normal = ImVec4(0.93f, 0.95f, 0.97f, 1));
void lossTooltip(const SampleLoss& l);
// The "?" button next to the sound of a mode whose voice needs a codec OnAir does not ship (codec_help.cpp, docs/CODECS.md): found or not,
// what it is, how to install it here, where to put it, a link to the full page and "Look again"
enum class CodecHelp { Codec2, DmrAmbe, Hdc, Dra, AeroAmbe, IridiumAmbe, DstarAmbe, NxdnAmbe, DpmrAmbe, P25Imbe, Avs, DraTv, Ac4, AvsPlus };
void codecHelpButton(CodecHelp c, const char* label = "?");   // label: the button's text ("?" by default)
// Audio cleanup (audio_cleanup_ui.cpp): the button next to the volume that opens the pop-up (true when a setting changed), and the per-frame
// hand-over of the settings of the mode's kind to the cleanup chain
bool audioCleanupButton(App& a);
void audioCleanupTick(App& a);
void audioCleanupDevOpen(bool advanced);
int audioCleanupProfileOf(const App& a);   // the kind of audio of the mode (AudioProfile): 0 data / off, 1 generic, 2 AM, 3 NFM, 4 WFM, 5 SSB, 6 CW, 7 digital voice
// Audio control (audio_control_ui.cpp, docs/AUDIO.md): the "Audio" button next to the volume opens one pop-up with volume, squelch, filters, EQ, notch,
// AGC, stereo, clarifier, output device, recorder and the AF spectrum. Settings are kept per mode (prefs "acl.<family>"), including its own volume.
bool audioControlButton(App& a);
void audioControlTick(App& a);   // every frame: hands the settings of the heard receiver's mode to the audio chain, per-mode volume, keyboard shortcuts
// Voice inversion (voice_inversion_ui.cpp): the button next to the audio cleanup button (analog voice only), the per-frame hand-over of its settings
bool voiceInversionButton(App& a);
void voiceInversionTick(App& a);
void voiceInversionDevOpen();   // dev flag --voiceinv: the pop-up opens at the start   // dev flag --audioclean [adv]: the pop-up opens at the start
#include "keys/keys_ui.h"   // the Voice keys window and the per-call key status (keys_ui.cpp)
// A new file path in the "audio-streams" folder of the user data folder, without extension: "<stem>_<date>_<time>" (the stem cleaned for a file name)
std::string streamSavePath(const std::string& stem);
// "voice: needs mbelib (libmbe.dll, ...)": the exact file names tried, for the status line of the mbelib modes
const char* codecNeedMbe();
// "Listen" for a decoder mode that is silent by default (HF digital, PSK, JS8Call): the switch, remembered under prefKey, then the shared
// mute and volume, in a flow row (widgets.cpp). True when the switch changed.
bool listenControls(App& a, bool& on, const char* prefKey, const char* id, const char* tip);
int hfDemodLoad(const char* prefKey);                                         // the remembered HfDemod choice (default Auto)
bool hfDemodControl(App& a, int& choice, const char* prefKey, const char* id);   // the ham front end's demodulator combo; saves on change
// rows that wrap in a narrow window: flowNext() between the groups of a row (a label and its control, a lamp, a read-out) instead of
// ImGui::SameLine(); the next group stays on the line if it fits (as wide as in the last frame). flowBreak() ends the line, like NewLine().
bool flowNext(float spacing = -1);   // true: on the same line
void flowBreak();
void flowEnd();   // after the last group of a row that ends without flowNext(): its width is measured too
bool sameLineIf(float w, float spacing = -1);   // SameLine() when something w wide still fits after the last item; true if it did
std::string ellipsize(std::string s, float w, float size = 0);   // s cut short with "..." to fit w (at this font size, default the current one)
std::string fitCaption(const std::string& s, float w);           // a caption that fits w: without its "(...)" part, else cut short
void captionFit(float w, const char* fmt, ...);
void kvColumn(float col);   // after a key: SameLine() at the value column col, further left in a narrow pane (never over the key); wrap the value                  // TextDisabled() of such a caption, the whole of it on hover (above a plot w wide)
struct StatusPanel { ImVec2 p; ImGuiID id; StatusPanel(); ~StatusPanel(); };   // the tinted panel behind a status bar, as tall as its (wrapped) lines
void scatter(const char* id, const std::vector<cf32>& pts, ImVec2 size, double lim, ImVec4 col);
void historyPlot(const char* id, const char* ylabel, const std::deque<float>& h, ImVec2 size);
std::string fmtLocal(int64_t utc, const char* f);
const char* genreName(int g);
const char* fmtKbps(char* b, size_t n, double k);
void qualityBar(App& a, float width);
// app_state.cpp
void setFamily(App& a, int f);                 // a family with protocol entries opens on its first one
void setMode(App& a, int f, int sub);          // a mode-list entry: family and protocol entry (0 none, or the family's first)
const dect2::SubMode* curSub(const App& a);    // the protocol entry selected, nullptr for a mode without them
int subIndexOf(const char* id);                // 1 .. kNumSubModes, 0 when not one
int modeSlot(const App& a);                    // index of famFreq / famGain for the current entry
int engineStd(const App& a);
void refreshDevices(App& a);
void kiwiFindWindow(App& a);    // the "Find KiwiSDR" window (toolbar_rig.cpp)
void netRadioWindow(App& a);
void synthGroupWindow(App& a);   // the "Group radios" window: define, save and watch synthetic wideband groups (synth_group_ui.cpp)   // the "Add network radio" window (toolbar_rig.cpp)
void hamlibRigWindow(App& a);  // the "Add Hamlib rig" window (toolbar_rig.cpp)
void applyBandwidth(App& a);
double halowWantedRateHz();       // Wi-Fi HaLow (halow/halow_ui.cpp): the radio rate the chosen receive width needs (4 / 20 Msps for Auto, else the width's own)
double halowWantedWidthMhz();     // and the widest channel it means, for the spectrum's channel marker
// The sample rate chosen per mode and per radio (prefs "rate.<mode id>.<radioKeyOf>", in Hz; absent or 0 = Auto, the mode's own rate)
std::string rateModeId(const App& a);
std::string rateContext(const App& a);
double savedSampleRate(const App& a);
void saveSampleRate(App& a, double hz);
double modeMinSampleRate(const App& a);
void sampleRateControl(App& a);
bool lowerSampleRate(App& a, double& prevChoiceHz, double& newHz);   // toolbar_source.cpp: the next lower rate the radio offers for this mode (restarts the receiver); false when none
void restoreSampleRate(App& a, double choiceHz);                     // and back to the earlier choice (0 = Auto)
// meter_ui.cpp: the one signal indicator (No signal / Weak / Locked / Good) and the CPU meter's relief
void signalBars(ImDrawList* dl, ImVec2 p, float h, int quality);   // 4 rising segments + the word is drawn by the caller
void signalRow(App& a);                             // the indicator with its numbers, at the top of the Receiver tab
float signalStripWidth(App& a);                     // the width the strip cell needs
void signalStripCell(App& a, float x, float y, float h);   // drawn into the strip at x
void cpuReliefTick(App& a, float cpuPct, int cores);   // once a second from the status strip
bool cpuReliefStrip(App& a, float limitX);          // "Too slow" + Fix, or the choice made + Undo; false when nothing was drawn
void cpuSparkDraw(ImDrawList* dl, ImVec2 p, ImVec2 size, ImU32 col, ImU32 edge, int cores);
std::string biasContext(const App& a);   // the prefs key of the antenna power for this mode on this radio ("" = none)
void loadBiasTee(App& a);
void saveBiasTee(const App& a);   // toolbar_source.cpp: the "Sample rate" control under the frequency
// The radio's own settings (DeviceInfo::settings, TuneSettings::radio), saved per radio: the key a radio is saved under, loading the
// selected radio's values into a.tune.radio when another radio was chosen, saving them after a change
std::string radioKeyOf(const DeviceInfo& d);
void syncRadioSettings(App& a);
void saveRadioSettings(const App& a);
std::string openFileDialog();
std::string saveFileDialog(const char* name);
void wfExportRow(App& a, double now);   // gfx/wf_export.cpp: a row the waterfall took, for a running PNG export
void wfExportPopup(App& a);             // gfx/wf_export.cpp: the Start / Stop block of the waterfall popup
void loadPrefs(App& a);
void loadFeedPrefs(App& a);         // feeds_ui.cpp: the data feed settings, and starts the ones that are on
void saveFeedPrefs(const App& a);
void gainOptButtons(App& a, bool running);   // gain_opt_ui.cpp: "Optimise gain" / "Cancel" and "Undo gain" on the gain row
void gainOptTick(App& a);                      // every frame (gainControl): runs the sweep
// agc_ui.cpp: the auto-gain loop's glue to the app
void agcReset(App& a);                         // the AGC starts afresh (switched on, or the gain was changed by something else)
bool agcStep(App& a, GainSetting& g);          // one tick: true when `g` (HackRF stages, or a generic radio's gain in .vga) must be applied
void agcGear(App& a);                          // the gear button beside "AGC" and its popup
void agcAmpWarning(const App& a);              // the "amp can fry itself" note, shown beside a manual Amp switch that is on
bool agcAmpSwitchable(const App& a);           // this radio has an amplifier the user can switch
bool agcAmpAllowed(App& a);                    // the user let the AGC switch it (default no)
GainModel agcModel(const App& a);              // the selected radio's gain stages
int agcMaxTotal(App& a);                       // the highest overall gain the AGC / the optimiser may use on this radio (without the amp unless allowed)
GainSetting stagesFor(App& a, int totalDb);    // an overall gain as the radio's stages, the amp as the user set it (it joins only above its range when allowed)
bool gainOptStatus(App& a);                    // the progress / result line; true when it drew something
// autogain_ui.cpp: auto gain for every radio and mode (docs/AUTOGAIN.md). Auto is a.agcOn; Optimise once / Safe start run once with Undo.
bool autogainStep(App& a);                     // every frame (gainControl): true when it changed a.tune (the caller retunes)
void autogainMeterOnly(App& a);                // every frame while it is off: the meters keep running
void autogainControl(App& a, bool vertical, bool running);   // the "Auto gain" control (Off / Auto / Optimise once / Safe start), gear popup, Stop / Undo
void autogainMeters(App& a);                   // the meter strip: ADC headroom bar + clip lamp, noise floor, warnings with advice
bool autogainStatus(App& a);                   // the progress / result line of a one-shot; true when it drew something
// The integration API (first-run wizard, automations): how 1 = Auto, 2 = Optimise once, 3 = Safe start. False when no radio is running.
bool autogainRun(App& a, int how);
bool autogainBusy(const App& a);               // a one-shot is running
std::string autogainResult(const App& a);      // the last one-shot's result text
std::string autogainStatusJson(App& a);        // read-only state for the Web server / remote API (dect2::ag::autogainJson)
void remoteApiLoadPrefs(App& a);    // remote_api_app.cpp: the remote-control API (HTTP + WebSocket), off by default: its settings, started when on
void remoteApiSavePrefs(const App& a);
void remoteApiTick(App& a);          // every frame: runs the commands the API queued for the main thread, publishes the telemetry
void remoteApiSection(App& a);       // the settings panel (in the Data feeds window)
void shareTick(App& a);             // feeds_ui.cpp: carries out the frequency / gain commands of the clients of "Share my radio" (when allowed)
void watchlistWindow(App& a);   // app/watchlist/watchlist_ui.cpp: the Watchlists window (View > Watchlists); also starts the alerts
void msgdbSearchFor(App& a, const std::string& text);   // opens the Messages window filtered to the text (the palette's search results)
std::vector<std::pair<std::string, std::string>> msgdbQuickSearch(const std::string& text, size_t n);   // best n matches: {label, text}
void msgdbWindow(App& a);   // msgdb_ui.cpp: the Messages window (View > Messages); also starts the database and its feed
void protolabWindow(App& a);   // protolab_ui.cpp: the Protocol lab window (View > Protocol lab)
void signallabWindow(App& a);  // signallab_ui.cpp: the Signal Lab window (Tools > Signal Lab)
void modeBuilderWindow(App& a);   // modebuilder_ui.cpp: the Mode Builder window (Tools > Mode Builder)
void modeBuilderFrame(App& a);    // modebuilder_ui.cpp, every frame: lists the registered custom modes, runs the selected one on receiver 1
void modeBuilderOpenText(const std::string& json);   // modebuilder_ui.cpp: opens the Mode Builder with this definition (Protocol lab > Export)
bool customModeSink(const float* iq, size_t nComplex, double rate);   // modebuilder_ui.cpp: the samples of a running custom mode (true: taken)
void txStudioWindow(App& a);   // tx_studio.cpp: Tools > TX studio (a.txStudioOpen); no radio, nothing is transmitted
void pluginFrame(App& a);          // plugin/plugin_app.cpp, every frame: discovery at the first call, starts and stops the running plugin mode, ticks the host
void pluginShutdown();             // at exit: stops the plugins and removes the crash marker
void pluginManagerWindow(App& a);  // View > Plugins
void automationWindow(App& a);   // automation_ui.cpp: the Automations window (View > Automations) and, every frame, the engine's actions that touch the receiver
extern std::string gAutomationShotPath;   // automation_sink.cpp: a screenshot of the program's own window is saved here after the next frame (then cleared)
void feedsWindow(App& a);          // feeds_ui.cpp: the "Data feeds" window (View > Data feeds)
void swarmWindow(App& a);          // swarm_ui.cpp: the "LAN swarm" window (View > LAN swarm)
void rotatorWindow(App& a);        // rotator_ui.cpp: GS-232 rotator connect / status
void rotatorTick(App& a);          // rotator_ui.cpp: signal-peak azimuth calibration sweep
void rotatorLoadPrefs();           // rotator_ui.cpp: prefs and optional auto-connect
void rotatorSavePrefs();
void rotatorPointBearing(double bearingDeg, double elDeg);
// The transmitter (tx_ui.cpp): independent of the receivers, with its own radio. The state is not an App's; transmitting is never saved.
struct ModeDef;
bool& txViewFlag();                // the RX / TX switch: true = TX view (never saved: the program opens in RX view)
void txViewSwitch(App& a);         // the slider at the top left of both interfaces
bool& hamViewFlag();               // the Ham view (third choice of the switch, exclusive with TX)
void hamPanel(App& a);             // app/ham/ham_ui.cpp
void txDevOpen(App& a, const char* modeId, bool dryRun);   // dev flags --tx / --txdry <mode id>: open the TX view on a signal, optionally with the test (no RF) running
void txTopBar(App& a, ImVec2 disp);   // tx_panel.cpp: the top bar of the TX view (run button, frequency digits, link to RX, presets, output meter, level)
void txPanel(App& a);              // the guided transmit panel (tx_panel.cpp): status header, 1 Signal, 2 Settings, 3 Preview and send; fills the main area in TX view
void txSelectMode(App& a, int family, const char* subId);
bool txModeHighlighted(const ModeDef& m);
void txTick(App& a);               // every frame: gives the radio back when a transmission ended; the on-screen TX indicator
void txShutdown();                 // stops a running transmission (the radio goes quiet before it returns); the program's end
bool txActive();
void savePrefs(const App& a);
void savePrefsSoon(const App& a);   // a tuning event: the save follows 0.6 s after the last one, not on the UI thread's mouse path
void savePrefsTick();               // each frame
void savePrefsFlush();              // at exit
tunestat::Stats& tuneStats();
int adcBitsFor(const DeviceInfo& d, double rateHz);
void ingestSpectrum(App& a);
void ingestRx(App& a);
std::vector<double> xs(const App& a);
float dbmOffset(const App& a);                 // dB to add to a level in dBFS to get dBm, set for the selected radio (0 until it is)
void setDbmOffset(const App& a, float db);
// the spectrum looks (plots.cpp, gfx/spec_look.h): set one with its trace colour, weight, fill and colour map (save: into the settings);
// the look in use (null: OnAir); its picker (Settings: the Spectrum menu, the Display section)
void specLookSet(App& a, int look, bool save = true);
const slook::LookSet* specLookActive(const App& a);
void specLookCombo(App& a, float width);
void specLookBand(const App& a, double x0, double x1, double centre, bool spectrum, bool dragging);   // the passband in the look's colours
void wfAutoButton(App& a);                     // plots.cpp: "Auto", on while the waterfall range follows the noise floor
void wfDisplayPopup(App& a);                   // plots.cpp: colour map, intensity range, speed and pause of the waterfall
void specDisplayPopup(App& a);                 // plots.cpp: the spectrum's averaging, max hold, markers, grid, fill and unit
float& specWaterfallSplit();                   // plots.cpp: spectrum share of the combined plot (default 0.5)
void resetSpectrum(App& a);                    // plots.cpp: default dB ranges, waterfall auto, 50/50 split (#142)
// Whole-span decoding (modeui.cpp). A mode opts in with registerWideMode(family); the switch is drawn by the spectrum (plots.cpp) for those only.
bool modeSupportsWide(int family);
void registerWideMode(int family);
bool wideFamOn(const App& a, int family);                 // the whole-span switch of a mode family (kept per family)
void setWideFam(App& a, int family, bool on);
bool wideActive(const App& a);                          // the switch is on for this mode and the engine runs it
void wideSync(App& a);                                  // once a frame: pushes the switch, the selected carrier and the mute to the engine
void wideCarrierCell(const App& a, int carrierId, double relHz);   // a table cell with the carrier's frequency (MHz); "-" for a single channel
void wideCarriersTable(App& a, float h);                // the carriers table: click selects (sound, detail), double click tunes
double radioCenterMhz(const App& a);   // where the radio is tuned: the frequency plus the mode's tuneOffsetHz (a file source: the frequency as typed)
// IQ recordings: the spectrum stays on the recording's centre; the tune box only moves the demodulator offset (app/gfx/file_vfo.h)
bool fileSourceSelected(const App& a);        // an IQ file is the chosen source
bool fileVfoActive(const App& a);             // ... and it runs
void fileSetVfo(App& a, double absMhz);       // click / drag / typed frequency: clamp to the recorded span and move the offset only
double fileCentreMhz(const App& a);           // the recording's centre (running: the engine's; else the file centre or the frequency)
void fileStartPrep(App& a);                    // before engine.start(): keeps the recording's centre, the frequency box is the VFO
void fileSetCentre(App& a, double centreMhz); // the File info dialog: a centre the header lacked
void applyOutputs(App& a);
std::string channelLabel(const SavedChannel& c, const char* unnamed = "DVB-T2 mux");   // 'unnamed' is shown for a channel without a name: the standard of the current mode (muxName)
const char* muxName(const App& a);                                                        // what an unnamed channel is called in the current mode: 'DTMB', 'DVB-T mux', 'ATSC mux' ...
void harvestScan(App& a);
// The result lists the screens keep themselves (results_ui.cpp; the receivers' own are Engine::setResultsDir): load at start, save every
// 30 s (now: at once, at exit), each into <data folder>/results/app-<name>.bin
void appResultsLoad(App& a);
void appResultsSave(App& a, bool now = false);
// The Clear of a mode's decoded lists, at the top of its list (results_ui.cpp): true when pressed
bool resultsClearButton(const char* id);
void scanDbTick(App& a);   // scan_db_ui.cpp: asks to share a finished scan
void tuneToChannel(App& a, const SavedChannel& c);
void followBandwidth(App& a);
void setDvbBandwidth(App& a, int idx);   // toolbar_source.cpp: the DVB-T2 / DVB-T width kBw[idx] (manual), or idx < 0 for Automatic; a running receiver follows at once
void setIsdbtBandwidth(App& a, int mhz);  // isdbt_ui.cpp: 6, 7, 8 or 0 (automatic); a running receiver starts again with it
const char* isdbtWidthLabel(int mhz);      // isdbt_ui.cpp: "6 MHz", "8 MHz (Maldives)", "Automatic"
void setDtmbBandwidth(App& a, int mhz);  // dtmb_ui.cpp: 8 or 6 MHz; a running receiver starts again with it
// the top bar: toolbar.cpp, toolbar_source.cpp, toolbar_rig.cpp, statusbar.cpp, mode_select.cpp
void toolbar(App& a);
void sourceOptions(App& a);
bool sourceOptionsOpen(const App& a);   // synthetic, a file not playing, or a sound card
void statusBar(App& a);
void gainControl(App& a);
void standardSwitch(App& a);
extern float gSwitchWidth;
// The receiver modes (mode_table.cpp). Adding a mode is one row there.
// subId: one protocol of a shared receiver (dect2::subModes()), a mode of its own with its own settings, decoder and views
struct ModeDef { int family; const char* name; int group; ImU32 col; const char* blurb; const char* tip; const char* sub; ImVec4 accent; const char* subId = nullptr; const char* keys = nullptr; };
constexpr int kModeCapacity = 400;   // the built-in rows and the plugins' (mode_table.cpp)
extern ModeDef kModes[];
extern int kNumModes;
bool addPluginModeRow(int family, const std::string& name, const std::string& blurb, const std::string& sub);   // appends a row in the PLUGINS group; false when the table is full
// Custom modes (Mode Builder): family numbers from 20000, stored in <data folder>/modes/registered.txt and never reused; the engine standard is family + 2
constexpr int kCustomFamilyBase = 20000;
inline bool isCustomFamily(int f) { return f >= kCustomFamilyBase && f < kCustomFamilyBase + 10000; }
bool addCustomModeRow(int family, const std::string& name, const std::string& blurb, const std::string& sub);   // a row in the CUSTOM MODES group (updates the texts of an existing one)
struct ModeRange { const ModeDef* b; const ModeDef* e; const ModeDef* begin() const { return b; } const ModeDef* end() const { return e; } };
inline ModeRange allModes() { return {kModes, kModes + kNumModes}; }   // for (const auto& m : allModes())
constexpr int kNumGroups = 17;   // the headings of the mode lists (mode_table.cpp); UI only, never saved by number
extern const char* const kGroupNames[kNumGroups];
extern const ImU32 kGroupCols[kNumGroups];
// The screens of a mode added after FM (family 6 and up). Each mode has one in its app/<mode>_ui.cpp and modeui.cpp lists them; the shell calls whichever
// entry is set where it would draw the DVB version. A null entry means: nothing of this kind for the mode (or the default noted).
struct ModeMeter { const char* label; const char* fmt; double v, lo, hi; int level; };   // one bar of the meter bank; level 0 neutral, 1 good, 2 marginal, 3 bad
struct ModeUi {
    const char* sideTitle = "SERVICES";                 // title of the list on the right: SERVICES, STATIONS, AIRCRAFT, CALLS ...
    const char* tabName = nullptr;                      // the mode's own main tab, drawn instead of the TV tab (null: the TV tab, for the TV modes)
    Ic tabIcon = Ic::Tv;
    void (*tab)(App&) = nullptr;
    void (*receiver)(App&) = nullptr;                   // the Receiver tab: what the receiver is doing (null: no tab)
    bool stream = false;                                // show the Stream tab (transport stream, outputs): the modes that make a transport stream
    void (*list)(App&) = nullptr;                       // the list on the right (null: the service cards of the TV modes)
    void (*panels)(App&) = nullptr;                     // the analysis row at the bottom, where the TV modes have the constellations
    void (*status)(App&) = nullptr;                     // the status bar
    void (*summary)(const App&, std::string& line1, std::string& line2) = nullptr;   // the two lines in the top bar
    void (*tuner)(App&, bool& retune) = nullptr;        // the mode's own tuning controls (symbol rate, standard, ...); set retune to apply
    void (*decoder)(App&, bool& retune) = nullptr;      // its decoder options
    void (*synth)(App&, bool& changed) = nullptr;       // options of the built-in test signal (SynthConfig::mode, modeOpt, modeVal); set changed to restart it
    void (*scan)(App&) = nullptr;                       // the Scan tab (null: a note)
    void (*tick)(App&) = nullptr;                       // every frame while the mode is selected: push volume and mute to the receiver, run scans, collect histories
    void (*meters)(const App&, std::vector<ModeMeter>& out) = nullptr;   // the meter bank of the Scope: Meters layout
};
bool modeSupportsWideHf(int family);                    // modeui.cpp: the HF families that decode the visible span inside the receiver (CW, PSK, HF digital)
bool modeSupportsWideChannels(int family);               // modeui.cpp: VDL2, HFDL (Whole span = every channel of the capture, set on the receiver)
bool modeSupportsWide(int family);                      // modeui.cpp: the families that can decode the whole visible span (CW, PSK, HF digital)
const ModeUi* modeUi(int family);                       // modeui.cpp: nullptr for the original families (0 to 5)
const char* listTitle(const App& a, bool upper);        // "STATIONS", "SERVICES" or the mode's own, for the list on the right
// Second mode (modeui.cpp): run kModes[modeIdx] at mhz beside the first mode (-1: off); false and the engine's note when refused
bool setSecondMode(App& a, int modeIdx, double mhz);
void secondModeTick(App& a);                 // every frame: its report, its tick (sound pushed muted unless asked for)
void secondModeTabs(App& a);                 // in mainTabs(): the tab "Second: <mode>" with the mode's own main screen
void secondModeControl(App& a, bool vertical);   // the tuner panel: the button and its pop-up (pick a mode and its frequency)
std::string scanModeKey(const ModeDef& m);         // scanner_ui.cpp: the mode key the scan list and the memories store (the protocol id, the mode's id, or dvb / atsc / ... / fm)
int scanModeIndex(const std::string& key);         // its index in kModes, -1 when this build has no such mode
struct ScanSeed { double mhz = 0; std::string mode, name; double dwellSec = 2; bool priority = false, lockout = false; };
void scannerLoadList(App& a, const std::vector<ScanSeed>& list, bool openClassic = true);   // scanner_ui.cpp: replaces the scan list (the memory scan), opens the window and starts it
// the merged Scanner view (scan_view_ui.cpp) drives the scanner's machine through these (scanner_ui.cpp)
struct ScanLive { bool running = false; int state = 0 /* 0 idle 1 listening 2 active 3 hang */; double mhz = 0; std::string mode, name, what; bool priority = false; int entry = -1; float levelDb = 0; uint64_t hitSeq = 0; };
ScanLive scannerLive();
void scannerControl(App& a, int what);                                  // 0 stop, 1 skip, 2 lockout the current channel
void scannerTuning(float squelchDb, double hangSec, double holdMaxSec, double prioritySec);
void scanViewWindow(App& a);                                            // scan_view_ui.cpp: the merged Scanner view (a.scanViewOpen)
void selectMode(App& a, int family, const char* subId = nullptr);
// app/autodet/autodet_ui.cpp: the "TV (auto)" / "Digital voice (auto)" rows (subId "auto:tv" / "auto:voice")
bool autodetRow(const char* subId, int* kind);   // true for such a row; kind: dect2::autodet::Capture::KindTv / KindVoice
bool autodetActive(const App& a, int kind);
void autodetSelect(App& a, int kind);            // what a click on the row does
void autodetCancel(App& a);                      // another mode was picked
void autodetTick(App& a);                        // every frame
void autodetStatus(App& a);                      // its line at the top of the status bar
void marineVhfJump(App& a, int channel, char suffix = 0);   // marinevhf_ui.cpp: open the Marine VHF mode on this channel (the DSC screen names one in a call)
bool modeSelected(const App& a, const ModeDef& m);    // the row of the current mode
const char* modeSearchBox();                          // a search box at the top of a mode drop-down (mode_select.cpp); returns the query
bool modeMatches(const ModeDef& m, const char* query); // the search box: name, subtitle, description and keywords
// Modes shown (mode_visibility.cpp, rules in mode_visibility.h): what the lists leave out. --mode, --pick, deep links and the remote API ignore it.
bool modeListed(const App& a, const ModeDef& m, const char* query = nullptr);   // in a list: matches the search, and is not hidden (unless searching or active)
bool modeHiddenNow(const App& a, const ModeDef& m);                           // hidden by the user (a search still finds it: tag it "hidden")
bool modeGroupListed(const App& a, int group, const char* query = nullptr);   // at least one mode of the group is listed
void modeVisibilityWindow(App& a);
void startReceiver(App& a);     // what the Start button does
double audioViewOffsetHz(const App& a);   // a sound card in a Real mode with the audio-range view: where the spectrum's centre lies from the dial (else 0)
void applyAudioSettings(App& a);   // the sound-card options into a.tune.radio (toolbar_source.cpp)
enum TbPart { TbSource = 1, TbFreq = 2, TbTuner = 4, TbGain = 8, TbDecoder = 16, TbAll = 31 };
void bbfSyncPrefs();   // toolbar.cpp: once, the saved baseband filters into the registry the receivers read (dsp/baseband_filter.h)
void toolbarParts(App& a, int mask, bool vertical);   // pieces of the top bar, for the sidebar of the new interface
void receiverGlance(App& a, float w, float h);       // analysis_tabs.cpp: a few key/value lines about the receiver
void tuneFreq(App& a, double mhz);                     // set the frequency now (resets the FM receiver)
void tuneDecode(App& a, double absMhz);                // sticky LO: decode absMhz without moving the radio centre
void spectrumTune(App& a, double absMhz);              // spectrum / waterfall click (respects tuningMode)
void uplinkNoteAdsb(const dect2::AdsbTelemetry& t);   // uplink_ui.cpp: callsigns heard by the ADS-B mode in this session
double meshSampleRate(App& a);                  // mesh_ui.cpp: the radio rate the selected LoRa bandwidth needs (2 Msps; 4 above 500 kHz)
void dvbsStatusRows(App& a, const DvbsTelemetry& t, bool on, bool run);   // dvbs_ui.cpp: the lock lamps and readouts of a report (QO-100's decoder panel)
double dvbsSymbolRateMsym();   // dvbs_ui.cpp: the manual DVB-S/S2 symbol rate in Msym/s (0 = from the spectrum)
void dvbsSetSymbolRate(App& a, double msym);   // dvbs_ui.cpp: the DVB-S/S2 symbol rate, set from elsewhere (the spectrum explorer)
// wb_ui.cpp: the Signal workbench. A file dropped on the window opens in it (the mode is selected); the dev flags open and drive it for --shot runs
void wbQueueDrop(const char* path);
void wbDropTick(App& a);
void wbDevOpen(const std::string& path, double rate, int tab);
void wbDevSelect(double t0Sec, double t1Sec, double f0Hz, double f1Hz);
void wbDevDemod(int kind, double baud, bool runMeasure, bool runDemod);
void mainTabs(App& a);          // the tab bar with the tabs that fit the current mode (main.cpp)
void drawShell2(App& a, ImVec2 disp);   // ui2.cpp: the new interface (its parts: ui2_theme.cpp, ui2_widgets.cpp, ui2_shell.cpp, ui2_faces.cpp)
void applyUiTheme(App& a);              // ui2.cpp: classic or new colours and shapes
// ui2_dock.cpp: docking of the new interface (View > Docking; issue #242)
bool dockActive(const App& a);          // docking is on and usable (new interface, touch interface off)
void dockFrameBegin(App& a);            // before the interface is drawn: installs the window placement hook
void dockFrameEnd(App& a);              // after every window: drop targets, docking on release, window order, shortcuts
void dockDevSelfTest();                 // --dockselftest: scripted drag / keyboard / layout checks, then exit
void dockDevTree(const char* text);     // --docktree: start with this layout (dock_tree.h text form)
void dockRegisterIni();                 // once, before the first per-mode layout file is read: the dock layout is kept in that file
// plots.cpp
// The width of the channel the mode's receiver works with: the light band on the spectrum, its edges on the waterfall. DVB-T2 / DVB-T: the
// measured or chosen raster width; FM: the channel filter; DTMB: 8 or 6 MHz; DVB-S/S2: symbol rate x (1 + roll-off) once it is known; the
// others: the width their standard (and receiver) fixes. settable: the user can choose it (channelWidthPopup), hasAuto: and leave it automatic.
struct ChanWidth { double mhz = 0; bool settable = false, hasAuto = false, manual = false; };
ChanWidth channelWidth(const App& a);
std::string widthText(double mhz);   // "7 MHz", "1.536 MHz", "12.5 kHz"
void channelWidthPopup(App& a);      // automatic or manual, where the mode allows it; otherwise what fixes the width
void spectrumWaterfall(App& a, float& frac, bool drag);   // the spectrum on top of the waterfall, one panel
void specFreqViewUpdate(const App& a);                                     // plots.cpp: keep linked frequency pan/zoom in sync with the radio
void specFreqViewGet(double& loMhz, double& hiMhz);
void specFreqViewWheel(App& a, float mouseNorm, float wheel, double& loMhz, double& hiMhz);
bool specDbRangeKeys(App& a);                                              // Ctrl+arrows dB range (3D view has no spectrum plot hover)
float specPlotBwBtnW();                                                    // plots.cpp: layout of corner controls (same frame)
float specPlotWideBtnW();
void channelWidthButton(App& a, ImVec2 plotMin, ImVec2 plotSize, const char* yLabel);
void wideSwitch(App& a, ImVec2 plotMin, ImVec2 plotSize);
void multiSwitch(App& a, ImVec2 plotMin, ImVec2 plotSize);
float specCornerPopup(App& a, const char* label, const char* popId, ImVec2 plotMin, ImVec2 plotSize, float rightUsed, float reserveLeft, float dropY, bool on, const char* tip, void (*body)(App&));
double clickTuneMhz(const App& a, double mhz);   // plots.cpp: where a click on the spectrum at mhz tunes (the mode's limits and raster); NAN: the mode is not tuned by a click
void waterfall3dPlot(App& a, ImVec2 size);       // gfx/wf3d_plot.cpp: the 3D waterfall in place of the flat one (a.wf.s3d.on)
void waterfall3dPopup(App& a);                   // gfx/wf3d_plot.cpp: its settings (in the waterfall popup)
float bandPlanStripHeight();         // plots.cpp: one text line plus padding; follows the font and resize
void drawBandPlanStrip(ImDrawList* dl, float x0, float x1, float y, float h, double loMhz, double hiMhz);   // saAllocations in view
// gfx/autolabel.cpp: the names of the stations in view, drawn on the spectrum (plotId 0) and the waterfall (1); a click on a name tunes and opens its mode
void autoLabelsDraw(App& a, int plotId, ImVec2 plotMin, ImVec2 plotSize, double loMhz, double hiMhz);
bool autoLabelsHovered();                 // the mouse is on a name: the plot's own click-to-tune stands back
float autoLabelsButton(App& a, ImVec2 plotMin, ImVec2 plotSize, float rightUsed);   // the "Labels" corner button with the source toggles; returns its width
// bandplan_ui.cpp and freqdb_ui.cpp: what they offer to the labels, and the click on a database entry
void bandPlanLabelSource(const App& a, std::vector<autolabel::Label>& out);
void freqdbLabelSource(std::vector<autolabel::Label>& out);
void specxDrawBookmarks(ImDrawList* dl, double loHz, double hiHz, float x0, float x1, float y0, float y1);   // specx_ui.cpp: named point / range marks
// specx_ident.cpp: "Identify this signal" (dect2/engine/sig_ident.h). The region is [offHz -+ widthHz / 2] from the samples' centre, which the radio
// has at sampleCentreRadioHz (rfOfCentreHz: its RF, for the bands); identifyAt: the signal under a click on the spectrum (MHz)
void identifyStart(App& a, double sampleCentreRadioHz, double offHz, double widthHz, double rfOfCentreHz);
void identifyAt(App& a, double clickMhz);
void identifyRegion(App& a, double loMhz, double hiMhz);   // a dragged region (MHz)
void identifyAll(App& a);                                  // every signal in the capture, in a table
void identifyTuned(App& a, double freqHz, double bwHz);    // tunes the radio to a signal found by the sweep and identifies it there
bool identifyRegionMhz(double& loMhz, double& hiMhz);      // the region of the open card, to draw it on the plots
void identifyWindow(App& a);   // every frame: the window while it is open
// general_ui.cpp: the general receiver's channel marker on the spectrum (plotId 0) and the waterfall (1): a click puts the channel
// there (inside the band, without retuning the radio), its filter edges can be dragged
void generalPlotMouse(App& a, int plotId);
void generalChannelBand(const App& a, bool fill);
bool generalDragging(int plotId);
double generalChannelWidthMhz(const App& a);
const char* generalDemodLabel();      // the general receiver's demodulator as drawn in the spectrum readout ("NFM", "AM"...)
void generalDemodPicker();            // its demodulator radio buttons (the spectrum's BW popup)
void histogramPlot(App& a, ImVec2 size);
void constDensityPlot(App& a, ImVec2 sz);
void constClusterPlot(App& a, ImVec2 sz);
// analysis_tabs.cpp
void syncTab(App& a);
void historyTab(App& a);
void frameMapTab(App& a);
void signallingTab(App& a);
void atscPanels(App& a);
void atsc3Status(App& a);
bool atsc3Quality(App& a, QualityReport& q);   // the signal-quality bar for ATSC 3.0 (share of decoded blocks); false when there is nothing to show
void atsc3ReceiverTab(App& a);
void isdbtStatus(App& a);
void isdbtReceiverTab(App& a);
void updateTick(App& a);
void updateButton(App& a);
void supportButton();
std::string modeCreditsLine(const char* modeName);   // "\n\nCredits: a, b" for a mode's tooltip, empty when nobody is credited for it (credits_ui.cpp)
void creditsWindow(App& a);   // Help, Credits and licences: the projects OnAir uses (app/panels/credits_ui.cpp)
void clearAllButton(App& a);   // top bar, next to the version: empties every decoded list, after an "are you sure"
void marineFaxClearUi();     // marine_ui.cpp: empties the weather fax picture on screen and in the saved copy (engine side: MarineReceiver::clearFax)
bool updateOnExit(App& a);
void constellationsTab(App& a);
void channelTab(App& a);
void impulseTab(App& a);
void snrTab(App& a);
void fecTab(App& a);
void tsTab(App& a);
void logPanel(App& a);
void historyLogTab(App& a);
// diag_ui.cpp: radio problems shown under the radio picker, and the diagnostics text
struct RadioMsg { int kind; std::string text; };   // kind: 0 radio listing error, 1 failed start/retune, 2 frequency outside the radio's range, 3 USB hint
std::string radioRangeWarning(const DeviceInfo& d, double freqMhz, const std::map<std::string, std::string>* radio = nullptr);   // empty when the frequency is fine or the range is unknown
namespace fixit { struct Entry; }
void fixitUi(App& a, const fixit::Entry* e, const char* idKey);   // diag_ui.cpp: the "How to fix" button + popup of a fixit.h entry (nothing for nullptr)
std::vector<RadioMsg> radioMessages(const App& a);                    // what is shown right now (dismissed ones left out)
struct DiagInput {
    std::string version, os, cpu, gpu;
    std::vector<DeviceInfo> radios;
    std::vector<std::string> messages, log;   // log: the whole engine log, filtered inside
    std::string mapServer;                    // map ingest connection log (map_send.cpp)
};
std::string buildDiagnostics(const DiagInput& in);   // plain text, no ImGui
std::string diagnosticsText(App& a);
void systemInfoText(std::string& os, std::string& cpu, std::string& gpu);   // diag_ui.cpp: this computer's OS, CPU and graphics names
bool writeZipFile(const std::string& path, const std::vector<std::pair<std::string, std::string>>& files);   // diag_ui.cpp: a zip, entries stored
void radioMessagesUi(App& a, bool vertical);
// radio_ui.cpp: the "Radio settings" of the selected radio (only the ones it has): a collapsible section in a side panel (vertical), a
// button with a popup in a one-line bar. A change reaches a running radio at once (a retune, or a restart for one that takes effect at open)
void radioSettingsUi(App& a, bool vertical);
void calibrateFrequencyOpen(App& a);   // radio_ui.cpp: Tools > Calibrate frequency (ppm wizard: FM / ATSC / time carrier / manual, quick check, drift track)
void calibrateFrequencyDraw(App& a);   // once per frame (draw_ui.cpp)
// subs_ui.cpp: DVB subtitles and teletext subtitle pages over the picture, the language choice, the teletext page drawing
void subsUpdate(App& a);                  // every frame: the subtitle services of the playing programme, decoder PIDs, the overlay bitmap
void subsControls(App& a);                // on/off checkbox and language choice (the TV panel)
void subsKey(App& a, int what);           // 1 = on/off, 2 = next language (the pop-out window's keys)
void drawTtxGrid(ImDrawList* dl, ImFont* f, ImVec2 o, float cw, float ch, const TtxGrid& g, bool overlay, bool reveal);
// tv.cpp
void teletextTab(App& a);
const EpgEvent* epgCurrent(const App& a, int sid, const EpgEvent** next = nullptr);
void guideTab(App& a);
void tvGuideTab(App& a);            // tvrec_ui.cpp: the programme guide grid, search, record / remind
void tvRecordingsTab(App& a);       // scheduled and finished recordings, recorder settings
void tvHdIngest(App& a);            // each 0.25 s with the multiplex's services: catalogue, switch waiting for a retune, HD preference
void tvHdCheckbox(App& a);          // the 'HD' checkbox of the player tab (greyed out unless an HD or SD counterpart of the playing service exists)
hd::Service tvHdService(const App& a, const TsService& sv);   // a service of the multiplex being received as the pairing sees it
void tvHdLoad(App& a);
void tvHdSave(const App& a);
void tvPlaybackBar(App& a);         // timeshift pause / rewind / live and the controls of a recording being played
void timeMachineLoad(App& a);       // timemachine_ui.cpp: the "Rewind" memory's settings from the preferences (before the first start)
void timeMachineUi(App& a);         // the button in the diagnostics row and its window
void schedulerWindow(App& a);          // scheduler_ui.cpp: the Scheduler window (a.schedulerOpen) and, every frame, the clock check that starts and ends the jobs
void schedulerTick(App& a);
void recmgrWindow(App& a);             // recmgr_ui.cpp: the Recordings manager (a.recmgrOpen); its automatic cleanup runs every frame check
void recmgrTick(App& a);
bool recordingOpenPath(App& a, const std::string& path, uint64_t firstSample, uint64_t endSample);   // recording_library_ui.cpp: a recording as the IQ file source, optionally only the sample range
void recordingLibraryUi(App& a);    // recording_library_ui.cpp: the "Library" button in the diagnostics row and its window (the recordings folder)
void timeMachineTick(App& a);       // every frame: the squelch trigger, the end of a save
bool timeMachineSaveNow(App& a, double postSeconds, std::string& note);   // an automation rule's "Save the rewind"
void tvRecTick(App& a);             // every frame: the recorder's clock, the schedule, the guide cache
void tvRecOverrideTs(App& a);       // while a recording plays, a.ts is that stream's service list
void tvRecShutdown(App& a);         // before the engine goes: finishes the recordings that run
void playerTab(App& a);
void rightPanel(App& a);
void tvTab(App& a);
void channelDashboard(App& a);
// scan_outputs.cpp
void scanTab(App& a);
void outputsTab(App& a);
// antenna.cpp
void feedDirection(App& a);
void compassRose(App& a, ImVec2 size);
void antennaTab(App& a);
// main.cpp
ImU32 ttxColour(int c, float alpha = 1.f);
int64_t utcNowOf(const App& a);
void overviewTab(App& a);
void receiverTab(App& a);
void streamTab(App& a);
void routeTab(App& a, const std::string& name);
ImU32 scoreColour(double sc);
void drawUI(App& a, ImVec2 disp, bool draw = true);   // draw = false: only the work that must go on for a receiver whose tab is not shown
// dab_ui.cpp
const char* dabChannelName(double mhz);
bool dabChannelCombo(App& a);
// bandplan_ui.cpp: band plans (channel rasters of a country). The picker with import / export / delete returns true when the active plan changed;
// the channel list of the TV modes sets a.freqMhz; the scan ranges are the TV bands of the active plan for the mode (empty: use the built-in lists)
struct TvRange { std::string label; double startMhz = 0, stopMhz = 0, stepMhz = 0, bwMhz = 0; };
bool bandPlanPicker(App& a);
std::vector<double> bandPlanChannels(const App& a, double loHz, double hiHz);   // the plan's channels of the current mode's bands in the range (whole-span priors)
bool tvChannelCombo(App& a);
std::vector<TvRange> tvBandRanges(App& a);
void dabStatus(App& a);
void dabHistory(App& a);
void dabPanels(App& a);
std::string dabSubText(const DabEnsemble& e, int sub);
void dabSelectStation(App& a, int sub, bool play);
void dabStations(App& a);
void dabRadioTab(App& a);
void dabEnsembleTab(App& a);
void dabTuneMhz(App& a, double mhz);   // dab_ui.cpp: tunes the running receiver to a DAB frequency (any click shift dropped, the old ensemble forgotten)
bool dabLogoImage(App& a, const DabEnsemble& ens, uint32_t sid, float h);   // dab_ui.cpp: the station logo (SPI / slide show) of a service
void dabSlidePanel(App& a, const DabEnsemble& ens, float maxH);   // dab_ui.cpp: the latest slide show picture with its name
void dabAnalyzerTab(App& a);       // dab_analyzer_ui.cpp: the ensemble as a tree with details and the MSC allocation
void dabGuideTab(App& a);          // dab_guide_ui.cpp: the programme guide (SPI, ETSI TS 102 818 / 371)
bool dabHasSpi(const DabEnsemble& ens);
void dabSpiTick(App& a);           // dab_guide_ui.cpp: every frame in DAB mode: starts the collection, the cache, the logos
std::string dabSpiNowText(App& a, uint32_t sid);   // "title (until 21:00)   Next: title (21:00)" from the guide, "" when unknown
void dabLogoSet(uint16_t eid, uint32_t sid, int w, int h, const uint32_t* rgba);   // dab_ui.cpp: gives the station logo cache a picture
void dabJournalineTab(App& a);     // dab_ui.cpp: Journaline pages (ETSI TS 102 979)
bool dabHasJournaline(const DabEnsemble& ens);
void dabFicTab(App& a);            // dab_fic_ui.cpp: the FIC, FIG by FIG, as a tree
void dabTransmittersTab(App& a);   // dab_tii_ui.cpp: the TII transmitters, on a map with a transmitter list
void dabTiiCollect(App& a);        // dab_tii_ui.cpp: notes the transmitters heard (every frame, also during a band scan)
void dabScanTab(App& a);
void dabScanStep(App& a);
void fmScanTab(App& a);
void fmScanStep(App& a);
// fm_ui.cpp
bool fmFrequencyCombo(App& a);
void fmStatus(App& a);
void fmHistory(App& a);
void fmPanels(App& a);
void fmRadioTab(App& a);
void fmRadioPanel(App& a);
void fmTune(App& a, double mhz);
// wizard.cpp
void wizEnter(App& a, int step);
void wizAction(App& a, int step);   // tour.cpp: step 2 starts the synthetic demo signal (the DECT2_DEMO test hook)
void wizard(App& a, ImVec2 disp);                      // tour.cpp: the first-run tour, each frame
void tourStart(App& a);                                // tour.cpp: (re)start the tour from the first step
void tourStartTour(App& a, int which);                 // tour.cpp: start one of the Help > Guided tours (resumes where it was left)
void tourMark(const char* id, ImVec2 lo, ImVec2 hi);   // tour.cpp: report where a part of the window is this frame (the tour points at it)
void wizEnter(App& a, int step);
void setupWizardOpen(App& a);                        // wizard.cpp: (re)start the setup wizard
void setupWizard(App& a, ImVec2 disp);               // wizard.cpp: the window, each frame
void lessonsStart(App& a, int planPos = -1);         // learn.cpp: the hands-on lessons for the setup's goals (resumes where they were left when planPos < 0)
void lessonsUi(App& a, ImVec2 disp);                 // learn.cpp: the running lesson (spotlight + card), the Lessons list, the glossary and the checkup windows, each frame
void selectModeById(App& a, const std::string& id);  // learn.cpp: "fm", "tv" (the region's TV standard), a protocol entry or a mode id; restarts a running receiver
bool tourAnchor(const char* id, ImVec2& lo, ImVec2& hi);   // tour.cpp: where a part of the window was reported this frame or the last; false when it is not on screen
void listenWizardOpen(App& a);                       // listen_wizard.cpp: "Get me listening" (Help menu, or a start with no source configured)
void listenWizard(App& a, ImVec2 disp);              // listen_wizard.cpp: the window, each frame
#include "dect2/engine/device_claims.h"
#include "state/receivers.h"
