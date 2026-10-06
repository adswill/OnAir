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

    void init(gfx::Backend* gfx) {
        img = gfx->createImage(W, H, 0xFF000000u);
        lut.resize(256);
        for (int i = 0; i < 256; i++) lut[i] = jet(i / 255.0f);
    }
    static uint32_t jet(float v) {
        // calm single-hue ramp: near-black navy, deep blue, teal, pale ice
        static const float stops[5][3] = {{6, 10, 20}, {16, 38, 70}, {30, 100, 140}, {110, 190, 205}, {240, 250, 250}};
        v = std::min(1.f, std::max(0.f, v)) * 4.f;
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
    uint64_t seq = 0;
    bool deint = true;
    bool has() const { return tex != nullptr; }
    void update(Player& pl) {
        auto f = pl.videoFrame(seq);
        if (!f || f->w <= 0) return;
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
        float ar = (float)w / h;
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
    int family = 0;           // 0 DVB, 1 ATSC, 2 DAB, 3 ATSC 3.0, 4 ISDB-T, 5 FM
    std::deque<float> dabSnrH, dabFicH;
    std::deque<float> fmSnrH, fmPilotH, fmRdsH;
    struct DabScan {
        bool running = false; int idx = -1; double t0 = 0, lockT = 0, savedFreq = 218.64;
        struct Res { std::string name, label, stations; double mhz = 0; bool found = false; float snr = 0; };
        std::vector<Res> results;
    } dabScan;
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
    struct HistSample { float t, snr, mer, loss, cfo, sro, level, clip, quality; };
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
void toggleFullscreen();
bool pillButton(const char* label, bool selected, float padX = 11);
int subNav(const char* id, int& cur, std::initializer_list<const char*> names);
float tagAt(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 bg, ImU32 fg = IM_COL32(225, 232, 240, 255));
void gaugePill(float width, float frac, ImU32 fill, const char* text);
void lamp(const char* label, int state /*0 grey 1 green 2 amber 3 red*/, int icon = -1);
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
std::string openFileDialog();
std::string saveFileDialog(const char* name);
void loadPrefs(App& a);
void savePrefs(const App& a);
void ingestSpectrum(App& a);
void ingestRx(App& a);
std::vector<double> xs(const App& a);
void applyOutputs(App& a);
std::string channelLabel(const SavedChannel& c);
void harvestScan(App& a);
void tuneToChannel(App& a, const SavedChannel& c);
void followBandwidth(App& a);
// toolbar.cpp
void toolbar(App& a);
void sourceOptions(App& a);
void statusBar(App& a);
void gainControl(App& a);
void standardSwitch(App& a);
// plots.cpp
void spectrumPlot(App& a, ImVec2 size);
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
void dabScanTab(App& a);
void dabScanStep(App& a);
// fm_ui.cpp
bool fmFrequencyCombo(App& a);
void fmStatus(App& a);
void fmHistory(App& a);
void fmPanels(App& a);
void fmRdsDisplay(App& a);
// wizard.cpp
void wizEnter(App& a, int step);
void wizAction(App& a, int step);
void wizFinish(App& a);
void wizard(App& a, ImVec2 disp);
