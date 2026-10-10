// Shared declarations of the OnAir window: the application state, small helpers and the panels that draw the interface.
// The panels live in widgets.cpp, app_state.cpp, toolbar.cpp, plots.cpp, analysis_tabs.cpp, tv.cpp, scan_outputs.cpp,
// antenna.cpp, dab_ui.cpp, wizard.cpp; main.cpp has the window loop.
#pragma once
// OnAir — digital TV receiver (DVB-T2, DVB-T, ATSC) for macOS. Phase 0 shell: sources, spectrum, waterfall, status, log.
#ifdef _WIN32
#include <direct.h>
#endif
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "plot.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>


#include "dect2/engine.h"
#include "dect2/modes.h"
#include "dect2/gpu_ldpc.h"
#include "dect2/t2rx.h"
#include "dect2/nettuner.h"
#include "airplay.h"
#include "dect2/timecompat.h"
#include "dect2/platform.h"
#include "dect2/scanner.h"
#include "dect2/updater.h"
#include "dect2/gain.h"
#include "dect2/channel.h"
#include "dect2/dvbt.h"
#include "dect2/quality.h"
#include "dect2/direction.h"
#include "dect2/teletext.h"
#include "theme.h"
#include "gfx.h"
#include "icons.h"
#include "mascot.h"
#include "scale.h"
#include "platform.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>
#include <unistd.h>

using namespace dect2;

// ------------------------------------------------------------------ waterfall texture

struct Waterfall {
    static constexpr int W = 1024, H = 512;
    gfx::Image* img = nullptr;
    std::vector<uint32_t> lut;
    int writeRow = 0; // newest row; older rows follow at writeRow+1 ... wrapping
    int filled = 0;
    double pushT = 0; // when the newest row came in (glfwGetTime)
    double rowDt = 1.0 / 30; // steady seconds per row, for the time labels and the scrolling
    double stamps[128] = {};
    int nStamps = 0;

    // average over the last 128 rows, and move only when that drifts by more than 2%: the labels stay put
    void stamp(double t) {
        const int N = 128;
        if (nStamps >= N) {
            const double dt = (t - stamps[nStamps % N]) / N;
            if (dt > 0 && std::fabs(dt - rowDt) > 0.02 * rowDt) rowDt = dt;
        } else if (nStamps >= 8) rowDt = (t - stamps[0]) / nStamps;   // starting up: whatever we have
        stamps[nStamps % N] = t;
        nStamps++;
        pushT = t;
    }
    float minDb = -100, maxDb = -32;

    void palette() { if (lut.size() == 256) for (int i = 0; i < 256; i++) lut[i] = jet(i / 255.0f); }   // after the interface palette changed
    void init(gfx::Backend* gfx) {
        img = gfx->createImage(W, H, 0xFF000000u);
        lut.resize(256);
        for (int i = 0; i < 256; i++) lut[i] = jet(i / 255.0f);
    }
    static uint32_t jet(float v) {
        v = std::min(1.f, std::max(0.f, v));
        if (pal::wfMode() == 2) {   // Turbo (Google's colour map), with the noise floor fading to black
            const float t = 0.12f + 0.84f * v;
            auto poly = [&](float a0, float a1, float a2, float a3, float a4, float a5) { return std::min(1.f, std::max(0.f, a0 + t * (a1 + t * (a2 + t * (a3 + t * (a4 + t * a5)))))); };
            const float r = poly(0.13572138f, 4.61539260f, -42.66032258f, 132.13108234f, -152.94239396f, 59.28637943f);
            const float g = poly(0.09140261f, 2.19418839f, 4.84296658f, -14.18503333f, 4.27729857f, 2.82956604f);
            const float b = poly(0.10667330f, 12.64194608f, -60.58204836f, 110.36276771f, -89.90310912f, 27.34824973f);
            const float f = std::min(1.f, v / 0.22f), k = f * f * (3 - 2 * f);   // smoothstep: black at the bottom
            return 0xFF000000u | ((uint32_t)(b * k * 255 + 0.5f) << 16) | ((uint32_t)(g * k * 255 + 0.5f) << 8) | (uint32_t)(r * k * 255 + 0.5f);
        }
        static const float cool[5][3] = {{6, 10, 20}, {16, 38, 70}, {30, 100, 140}, {110, 190, 205}, {240, 250, 250}};
        static const float grey[5][3] = {{0, 0, 0}, {34, 34, 33}, {96, 95, 92}, {170, 168, 162}, {244, 242, 236}};
        const float (*stops)[3] = pal::wfMode() == 1 ? grey : cool;
        v *= 4.f;
        const int i = std::min(3, (int)v);
        const float f = v - i;
        auto ch = [&](int k) { return (uint32_t)(stops[i][k] + (stops[i + 1][k] - stops[i][k]) * f + 0.5f); };
        return 0xFF000000u | (ch(2) << 16) | (ch(1) << 8) | ch(0);
    }
    void push(const std::vector<float>& dbfs) {
        std::vector<uint32_t> row(W);
        size_t n = dbfs.size();
        for (int x = 0; x < W; x++) {
            size_t a = (size_t)x * n / W, b = std::max(a + 1, (size_t)(x + 1) * n / W);
            float m = -200;
            for (size_t k = a; k < b; k++) m = std::max(m, dbfs[k]);
            float v = (m - minDb) / (maxDb - minDb);
            row[x] = lut[(int)(std::min(1.f, std::max(0.f, v)) * 255)];
        }
        writeRow = (writeRow + H - 1) % H;
        img->update(0, writeRow, W, 1, row.data());
        filled = std::min(H, filled + 1);
    }
};

extern std::string gForceTab;

// ------------------------------------------------------------------ app state

struct BwChoice { const char* label; double mhz; double nativeMsps; };
inline const BwChoice kBw[] = {
    {"8 MHz", 8, 64.0 / 7}, {"7 MHz", 7, 8.0}, {"6 MHz", 6, 48.0 / 7}, {"5 MHz", 5, 40.0 / 7}, {"1.7 MHz", 1.7, 131.0 / 71},
};

extern gfx::Backend* gGfx;
extern GLFWwindow* gWindow;
extern int gWinX, gWinY, gWinW, gWinH;

struct VideoTex {
    gfx::Video* tex = nullptr;
    int w = 0, h = 0;
    double dar = 0;          // display aspect of the newest picture (0 = use w / h); it can change between programmes
    uint64_t seq = 0;
    bool deint = true;
    bool has() const { return tex != nullptr; }
    void update(Player& pl) {
        auto f = pl.videoFrame(seq);
        if (!f || f->w <= 0) return;
        dar = f->dar;
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
        ImGui::SetCursorPos(p);
    }
};

using SavedChannel = plat::Channel;


// parts of the window the tour points at
enum WizTarget { TgNone = 0, TgSwitch, TgToolbar, TgMain, TgRight, TgConst, TgCount };

constexpr int kNumFamilies = 26;   // App::family runs from 0 to kNumFamilies - 1

struct App {
    Engine engine;
    NetTuner net{engine};      // network tuner (declared after the engine so that it stops first)
    bool netOn = false, netLan = false;
    int netPort = 8089;
    char netKey[32] = "";
    // first-run tour (wizard.inc)
    bool wizOpen = false, wizStart = false, wizAcked = false;
    int wizStep = 0;
    double wizStepT = 0;
    float wizX = -1;
    ImVec2 tgMin[8], tgMax[8];   // screen rectangles of the parts of the window the tour points at
    VideoTex video;
    float volume = 1.f;
    bool muted = false, subsOn = true;
    int playReq = -1;
    int dabStation = -1;      // --station: DAB sub-channel to play on start (testing)
    std::vector<DeviceInfo> devices; // [0]=synthetic, [1]=file, then HackRFs
    int devIdx = 0;
    TuneSettings tune;
    FileOptions file;
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
    struct FamGain { int lna = 32, vga = 20; bool amp = true, known = false; } famGain[kNumFamilies];   // the radio gains remembered for each mode
    bool famBias[kNumFamilies] = {};    // antenna power per mode while the app runs (never saved: it starts off)
    double famFreq[kNumFamilies] = {};  // the frequency each of the modes added after FM was last tuned to (0 = never)
    int uiVariant = 0;        // layout of the new interface: 0 sidebar (the default), 1 scope, 2 tiles, 3 faceplate, 4-6 scope children, 7 panel
    int uiTheme = 1;          // its palette: 0 terminal, 1 instrument, 2 mono
    int dtmbBwMhz = 8;        // DTMB channel width: 8 MHz (China, Hong Kong), 6 MHz (Cuba)
    bool lightUi = false;     // View > Light: every colour drawn with its lightness turned over (dark on white), hues kept (main.cpp)
    int family = 0;           // 0 DVB, 1 ATSC, 2 DAB, 3 ATSC 3.0, 4 ISDB-T, 5 FM, 6 DVB-S/S2, 7 DTMB, 8 analog TV, 9 DMR, 10 DRM, 11 ADS-B, 12 GNSS, 13 radiosonde, 14 AIS, 15 marine, 16 ACARS, 17 Inmarsat-C, 18 Inmarsat Aero, 19 Iridium, 20 mesh, 21 HD Radio, 22 CDR, 23 pagers, 24 APRS / packet, 25 HF digital (6 and up: see ModeUi; engine standard code = family + 2)
    std::deque<float> dabSnrH, dabFicH;
    std::deque<float> fmSnrH, fmPilotH, fmRdsH;
    int fmDeemph = 50;        // FM de-emphasis in microseconds: 50 (Europe, Middle East, most of the world) or 75 (Americas, South Korea)
    int fmChanKhz = 0;        // FM channel filter: 0 = automatic (the receiver's standard 220 kHz), else a manual width of 100 to 300 kHz
    struct DabScan {
        bool running = false; int idx = -1; double t0 = 0, lockT = 0, savedFreq = 218.64;
        struct Res { std::string name, label, stations; double mhz = 0; bool found = false; float snr = 0; };
        std::vector<Res> results;
    } dabScan;
    struct FmScan {
        bool running = false; int phase = 0; int idx = -1; double t0 = 0, savedFreq = 100.0; uint64_t seq0 = 0, rxSeq0 = 0;   // seq0: spectrum frame, rxSeq0: receiver report at the moment of tuning; phase 0 surveys the band, phase 1 checks each candidate
        std::vector<double> cand;
        struct Res { std::string name, pty; double mhz = 0; bool found = false, stereo = false, rds = false; float snr = 0; };
        std::vector<Res> results;
    } fmScan;
    bool bwAuto = true;       // the engine measures the channel width and switches by itself
    double freqMhz = 522.0;
    bool autoScroll = true;
    int tab = 0;
    SpectrumFrame spec;
    std::vector<float> smooth, peak;
    uint64_t lastSeq = 0;
    Waterfall wf;
    float yMin = -110, yMax = -30;
    bool peakHold = true;
    ImFont* mono = nullptr;
    ImFont* ui = nullptr;
    std::string hackrfErr;
    bool hackrfErrHidden = false;       // the user dismissed the listing error; a new listing shows it again
    std::string startErr, startErrKey;  // last failed start/retune of the radio (from the engine log); key = the log line it came from, so a dismissed one stays away
    std::vector<std::string> usbHints;  // why a plugged-in radio is not listed (Linux), refreshed with the radio list
    double diagCopiedAt = -10, diagPollAt = -10;
    bool diagWasRunning = false;
    int recFormat = 0;                  // IQ recording format: 0 = 8-bit, 1 = float (pref "recFormat")
    bool recWasActive = false;          // a recording was running at the last frame (to log it once when it ends)
    std::string recDonePath;            // the last finished recording, for "Show recording"
    RxTelemetry rx;
    uint64_t rxSeq = 0;
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
    int histWindow = 60;
    int ttxPage = 100;
    int plpSel = -1; // -1 = automatic
    int subTv = 0, subRx = 0, subStream = 0; // sub-view of the TV / Receiver / Stream tabs
    std::map<int, std::vector<EpgEvent>> epg;
    double epgT = -10;
    int guideSid = -1, guideEvent = -1;
    bool fakeEpg = false; // --fakeepg: made-up programmes, for checking the layout when the mux sends none
    char favName[64] = "";
    MultipathDetector mpd;
    QualityMeter quality;
    std::vector<SavedChannel> channels; // DVB-T2 muxes found by the scanner (remembered)
    bool scanWas = false;
    double scanHarvestT = 0;
    bool agcOn = false;
    std::string gainDev;   // the radio a.tune.gainDb was set for (board:serial); another radio starts at its own sensible gain
    std::string radioKey;  // the radio a.tune.radio (its own settings, DeviceInfo::settings) was loaded for (radioKeyOf); "" = none
    AutoGain agc;
    GainSweep sweep;
    bool sweepRetune = false;
    Scanner scanner;
    ScanConfig scanCfg;
    int scanPreset = 0;
    bool scanWasRunning = false;
    std::unique_ptr<Updater> upd;      // update check and installer
    bool updCheck = true, updAuto = true, updPre = true, updStarted = false;
    double updLast = 0;                // when the last check was made (seconds since 1970)
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
extern bool gTightTabs;   // the tabs of mainTabs() get less padding (a narrow pane)
void toggleFullscreen();
bool pillButton(const char* label, bool selected, float padX = 11);
int subNav(const char* id, int& cur, std::initializer_list<const char*> names);
float tagAt(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 bg, ImU32 fg = IM_COL32(225, 232, 240, 255));
void gaugePill(float width, float frac, ImU32 fill, const char* text);
void lamp(const char* label, int state /*0 grey 1 green 2 amber 3 red*/, int icon = -1);
// Lost samples (Engine::sampleLoss()) for the "dropped" read-outs: "0", or the seconds lost on the radio side and on the OnAir (CPU) side;
// red while a loss is recent, amber after it; the tooltip on the item before says what each side means and what helps
std::string lossText(const SampleLoss& l);
ImVec4 lossColour(const SampleLoss& l, ImVec4 normal = ImVec4(0.93f, 0.95f, 0.97f, 1));
void lossTooltip(const SampleLoss& l);
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
void setFamily(App& a, int f);
int engineStd(const App& a);
void refreshDevices(App& a);
void applyBandwidth(App& a);
// The radio's own settings (DeviceInfo::settings, TuneSettings::radio), saved per radio: the key a radio is saved under, loading the
// selected radio's values into a.tune.radio when another radio was chosen, saving them after a change
std::string radioKeyOf(const DeviceInfo& d);
void syncRadioSettings(App& a);
void saveRadioSettings(const App& a);
std::string openFileDialog();
std::string saveFileDialog(const char* name);
void loadPrefs(App& a);
void savePrefs(const App& a);
int adcBitsFor(const DeviceInfo& d, double rateHz);
void ingestSpectrum(App& a);
void ingestRx(App& a);
std::vector<double> xs(const App& a);
double radioCenterMhz(const App& a);   // where the radio is tuned: the frequency plus the mode's tuneOffsetHz (a file source: the frequency as typed)
void applyOutputs(App& a);
std::string channelLabel(const SavedChannel& c);
void harvestScan(App& a);
void scanDbTick(App& a);   // scan_db_ui.cpp: asks to share a finished scan
void tuneToChannel(App& a, const SavedChannel& c);
void followBandwidth(App& a);
void setDvbBandwidth(App& a, int idx);   // toolbar.cpp: the DVB-T2 / DVB-T width kBw[idx] (manual), or idx < 0 for Automatic; a running receiver follows at once
void setDtmbBandwidth(App& a, int mhz);  // dtmb_ui.cpp: 8 or 6 MHz; a running receiver starts again with it
// toolbar.cpp
void toolbar(App& a);
void sourceOptions(App& a);
void statusBar(App& a);
void gainControl(App& a);
void standardSwitch(App& a);
extern float gSwitchWidth;
// The receiver modes (toolbar.cpp). Adding a mode is one row there.
struct ModeDef { int family; const char* name; int group; ImU32 col; const char* blurb; const char* tip; const char* sub; ImVec4 accent; };
extern const ModeDef kModes[];
extern const int kNumModes;
constexpr int kNumGroups = 7;   // TV, radio, aviation, maritime, satellite, utility, amateur
extern const char* const kGroupNames[kNumGroups];
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
const ModeUi* modeUi(int family);                       // modeui.cpp: nullptr for the original families (0 to 5)
const char* listTitle(const App& a, bool upper);        // "STATIONS", "SERVICES" or the mode's own, for the list on the right
void selectMode(App& a, int family);
void startReceiver(App& a);     // what the Start button does
enum TbPart { TbSource = 1, TbFreq = 2, TbTuner = 4, TbGain = 8, TbDecoder = 16, TbAll = 31 };
void toolbarParts(App& a, int mask, bool vertical);   // pieces of the top bar, for the sidebar of the new interface
void receiverGlance(App& a, float w, float h);       // analysis_tabs.cpp: a few key/value lines about the receiver
void tuneFreq(App& a, double mhz);                     // set the frequency now (resets the FM receiver)
void mainTabs(App& a);          // the tab bar with the tabs that fit the current mode (main.cpp)
void drawShell2(App& a, ImVec2 disp);   // ui2.cpp: the new interface
void applyUiTheme(App& a);              // ui2.cpp: classic or new colours and shapes
// plots.cpp
// The width of the channel the mode's receiver works with: the light band on the spectrum, its edges on the waterfall. DVB-T2 / DVB-T: the
// measured or chosen raster width; FM: the channel filter; DTMB: 8 or 6 MHz; DVB-S/S2: symbol rate x (1 + roll-off) once it is known; the
// others: the width their standard (and receiver) fixes. settable: the user can choose it (channelWidthPopup), hasAuto: and leave it automatic.
struct ChanWidth { double mhz = 0; bool settable = false, hasAuto = false, manual = false; };
ChanWidth channelWidth(const App& a);
std::string widthText(double mhz);   // "7 MHz", "1.536 MHz", "12.5 kHz"
void channelWidthPopup(App& a);      // automatic or manual, where the mode allows it; otherwise what fixes the width
void spectrumPlot(App& a, ImVec2 size, bool noFreqAxis = false);
void waterfallPlot(App& a, ImVec2 size);
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
std::string radioRangeWarning(const DeviceInfo& d, double freqMhz);   // empty when the frequency is fine or the range is unknown
std::vector<RadioMsg> radioMessages(const App& a);                    // what is shown right now (dismissed ones left out)
struct DiagInput {
    std::string version, os, cpu, gpu;
    std::vector<DeviceInfo> radios;
    std::vector<std::string> messages, log;   // log: the whole engine log, filtered inside
};
std::string buildDiagnostics(const DiagInput& in);   // plain text, no ImGui
std::string diagnosticsText(App& a);
void radioMessagesUi(App& a, bool vertical);
// radio_ui.cpp: the "Radio settings" of the selected radio (only the ones it has): a collapsible section in a side panel (vertical), a
// button with a popup in a one-line bar. A change reaches a running radio at once (a retune, or a restart for one that takes effect at open)
void radioSettingsUi(App& a, bool vertical);
// tv.cpp
void teletextTab(App& a);
const EpgEvent* epgCurrent(const App& a, int sid, const EpgEvent** next = nullptr);
void guideTab(App& a);
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
void drawUI(App& a, ImVec2 disp);
// dab_ui.cpp
const char* dabChannelName(double mhz);
bool dabChannelCombo(App& a);
void dabStatus(App& a);
void dabHistory(App& a);
void dabPanels(App& a);
std::string dabSubText(const DabEnsemble& e, int sub);
void dabSelectStation(App& a, int sub, bool play);
void dabStations(App& a);
void dabRadioTab(App& a);
void dabEnsembleTab(App& a);
void dabTransmittersTab(App& a);   // dab_tii_ui.cpp: the TII transmitters, on a map with a transmitter list
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
void wizAction(App& a, int step);
void wizFinish(App& a);
void wizard(App& a, ImVec2 disp);
