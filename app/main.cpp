// OnAir — digital TV receiver (DVB-T2, DVB-T, ATSC) for macOS. Phase 0 shell: sources, spectrum, waterfall, status, log.
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "implot.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>


#include "dect2/engine.h"
#include "dect2/nettuner.h"
#include "dect2/timecompat.h"
#include "dect2/platform.h"
#include "dect2/scanner.h"
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

static std::string gForceTab; // dev: --tab <name> selects a tab for screenshots
static bool tabItem(const char* name, Ic icon) {
    ImGuiTabItemFlags fl = 0;
    if (!gForceTab.empty() && gForceTab == name) { fl = ImGuiTabItemFlags_SetSelected; gForceTab.clear(); }
    // the label is padded with spaces to leave room for the icon, which is drawn over that gap
    const float gap = iconSize() + 5.f;
    const int nSp = (int)std::ceil(gap / ImGui::CalcTextSize(" ").x);
    const std::string label = std::string(nSp, ' ') + name + "###" + name;
    const bool open = ImGui::BeginTabItem(label.c_str(), nullptr, fl);
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    icons::draw(icon, ImVec2(r0.x + ImGui::GetStyle().FramePadding.x + iconSize() * 0.5f + 1.f, (r0.y + r1.y) * 0.5f), iconSize() * 0.92f,
                open ? IM_COL32(255, 255, 255, 255) : IM_COL32(140, 154, 170, 255));
    return open;
}

// ------------------------------------------------------------------ app state

struct BwChoice { const char* label; double mhz; double nativeMsps; };
static const BwChoice kBw[] = {
    {"8 MHz", 8, 64.0 / 7}, {"7 MHz", 7, 8.0}, {"6 MHz", 6, 48.0 / 7}, {"5 MHz", 5, 40.0 / 7}, {"1.7 MHz", 1.7, 131.0 / 71},
};

static gfx::Backend* gGfx = nullptr;
static GLFWwindow* gWindow = nullptr;
static int gWinX = 100, gWinY = 100, gWinW = 1500, gWinH = 900;
static void toggleFullscreen() {
    if (!gWindow) return;
    if (glfwGetWindowMonitor(gWindow)) { glfwSetWindowMonitor(gWindow, nullptr, gWinX, gWinY, gWinW, gWinH, 0); return; }
    glfwGetWindowPos(gWindow, &gWinX, &gWinY); glfwGetWindowSize(gWindow, &gWinW, &gWinH);
    GLFWmonitor* m = glfwGetPrimaryMonitor();
    const GLFWvidmode* vm = glfwGetVideoMode(m);
    glfwSetWindowMonitor(gWindow, m, 0, 0, vm->width, vm->height, vm->refreshRate);
}

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
    bool atscMode = false;    // standard family: false = DVB (T2/T), true = ATSC
    bool dabMode = false;     // DAB / DAB+ (family 2)
    int family = 0;           // 0 DVB, 1 ATSC, 2 DAB
    std::deque<float> dabSnrH, dabFicH;
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
    double lastFrameT = 0, frameDt = 1.0 / 30;
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
    DirectionFinder dir;
    int antKind = 0;          // 0 directional, 1 dipole / indoor, 2 omnidirectional
    bool dirAgcWas = false;
};

// DAB screens (app/dab_ui.inc)
static void dabStatus(App& a);
static void dabStations(App& a);
static void dabPanels(App& a);
static void dabScanTab(App& a);
static void dabScanStep(App& a);
static void dabHistory(App& a);
static void dabRadioTab(App& a);
static void dabEnsembleTab(App& a);
static bool dabChannelCombo(App& a);
static void setFamily(App& a, int f) {
    a.family = f; a.atscMode = f == 1; a.dabMode = f == 2;
    if (f == 2 && !(a.freqMhz >= 174 && a.freqMhz <= 240)) a.freqMhz = 218.640;
}
// what to tell the engine: 0 auto, 1 DVB-T2, 2 DVB-T, 3 ATSC, 4 DAB
static int engineStd(const App& a) { return a.family == 1 ? 3 : a.family == 2 ? 4 : a.stdMode; }

static void refreshDevices(App& a) {
    a.devices.clear();
    DeviceInfo s; s.kind = DeviceInfo::Synthetic; s.name = "Synthetic test signal (DVB-T2 8K, 8 MHz)"; a.devices.push_back(s);
    DeviceInfo f; f.kind = DeviceInfo::File; f.name = "IQ recording file…"; a.devices.push_back(f);
    std::string err;
    for (auto& d : listHackrfDevices(err)) a.devices.push_back(d);
    a.hackrfErr = err;
    const size_t nHack = a.devices.size() - 2;
    std::string serr;
    for (auto& d : listSoapyDevices(serr)) a.devices.push_back(d);
    a.engine.log("device scan: " + std::to_string(nHack) + " HackRF, " + std::to_string(a.devices.size() - 2 - nHack) + " other radio(s)" + (soapySupported() ? "" : " (built without SoapySDR)") + (serr.empty() ? "" : "  " + serr));
}

static void applyBandwidth(App& a) {
    // HackRF Pro: the tuned centre is only exact at <= 10 Msps and at 20 Msps, so use 10 Msps (8 for narrow channels)
    a.tune.sampleRate = kBw[a.bwIdx].mhz >= 7 ? 10e6 : 8e6;
    {   // a radio that cannot reach that rate runs as fast as it can (the source picks the nearest rate it offers)
        const DeviceInfo& dv = a.devices[a.devIdx];
        if (dv.kind == DeviceInfo::Soapy && dv.maxRateHz > 0) a.tune.sampleRate = std::min(a.tune.sampleRate, dv.maxRateHz);
        if (dv.kind == DeviceInfo::Soapy && a.tune.gainDb > dv.gainMaxDb && dv.gainMaxDb > 0) a.tune.gainDb = dv.gainMaxDb;
    }
    a.tune.basebandFilterHz = 0;
    a.tune.bandwidthMhz = kBw[a.bwIdx].mhz;
    a.tune.synth.atsc = a.atscMode;
    a.tune.synth.dab = a.dabMode;
    if (a.dabMode) {   // a DAB ensemble is 1.536 MHz wide: 2.048 Msps is the natural rate (RTL-SDR dongles do it too)
        a.tune.bandwidthMhz = 1.7; a.tune.sampleRate = 2.048e6; a.tune.basebandFilterHz = 1.75e6;
        const DeviceInfo& dv = a.devices[a.devIdx];
        if (dv.kind == DeviceInfo::Soapy && dv.maxRateHz > 0) a.tune.sampleRate = std::min(a.tune.sampleRate, dv.maxRateHz);
    } else if (a.atscMode) { a.tune.bandwidthMhz = 6; a.tune.sampleRate = 8e6; if (a.devices[a.devIdx].kind == DeviceInfo::Soapy && a.devices[a.devIdx].maxRateHz > 0) a.tune.sampleRate = std::min(a.tune.sampleRate, a.devices[a.devIdx].maxRateHz); }   // an ATSC channel is always 6 MHz wide
}

static std::string openFileDialog() { return plat::openFileDialog(); }
static std::string saveFileDialog(const char* name) { return plat::saveFileDialog(name); }

static void loadPrefs(App& a) {
    plat::Prefs& d = plat::prefs();
    if (d.has("freqMhz")) a.freqMhz = d.getD("freqMhz", a.freqMhz);
    if (d.has("lna")) a.tune.lnaDb = (int)d.getI("lna", a.tune.lnaDb);
    if (d.has("vga")) a.tune.vgaDb = (int)d.getI("vga", a.tune.vgaDb);
    if (d.has("gain")) a.tune.gainDb = d.getD("gain", a.tune.gainDb);
    a.tune.ampOn = d.getB("amp", false);
    if (d.has("family")) { const int f = (int)d.getI("family", 0); a.family = f; a.atscMode = f == 1; a.dabMode = f == 2; }
    if (d.has("compute")) a.computeMode = (int)d.getI("compute", a.computeMode);
    if (d.has("standard")) a.stdMode = (int)d.getI("standard", a.stdMode);
    a.bwIdx = std::max(0, std::min((int)(sizeof kBw / sizeof *kBw) - 1, (int)d.getI("bw", 0)));
    if (d.has("bwAuto")) a.bwAuto = d.getB("bwAuto", a.bwAuto);
    if (d.has("filePath")) a.file.path = d.getS("filePath", "");
    if (d.has("outPath")) snprintf(a.filePath, sizeof a.filePath, "%s", d.getS("outPath", "").c_str());
    if (d.has("udpHost")) snprintf(a.udpHost, sizeof a.udpHost, "%s", d.getS("udpHost", "").c_str());
    if (d.has("udpPort")) a.out.port = (int)d.getI("udpPort", a.out.port);
    a.out.rtp = d.getB("udpRtp", false);
    a.out.dropNull = d.getB("dropNull", false);
    a.channels = d.getChannels();
}

static void savePrefs(const App& a) {
    plat::Prefs& d = plat::prefs();
    d.setD("freqMhz", a.freqMhz);
    d.setI("lna", a.tune.lnaDb);
    d.setI("vga", a.tune.vgaDb);
    d.setD("gain", a.tune.gainDb);
    d.setB("amp", a.tune.ampOn);
    d.setI("family", a.family);
    d.setChannels(a.channels);
    d.setI("compute", a.computeMode);
    d.setI("standard", a.stdMode);
    d.setI("bw", a.bwIdx);
    d.setB("bwAuto", a.bwAuto);
    d.setS("filePath", a.file.path);
    d.setS("outPath", a.filePath);
    d.setS("udpHost", a.udpHost);
    d.setI("udpPort", a.out.port);
    d.setB("udpRtp", a.out.rtp);
    d.setB("dropNull", a.out.dropNull);
    d.flush();
}

// ------------------------------------------------------------------ UI pieces


// ------------------------------------------------------------------ small widgets (pills, tags, gauges)

static bool pillButton(const char* label, bool selected, float padX = 11) {
    ImGui::PushID(label);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 sz(ts.x + padX * 2, ImGui::GetFrameHeight() - 3);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##pill", sz);
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = selected ? IM_COL32(52, 92, 108, 255) : hov ? IM_COL32(46, 56, 68, 255) : IM_COL32(30, 35, 42, 255);
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), bg, 3.f);
    dl->AddText(ImVec2(p.x + padX, p.y + (sz.y - ts.y) * 0.5f), selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(176, 184, 194, 255), label);
    ImGui::PopID();
    return clicked;
}

// A row of pills that switches between sub-views of a tab. Returns the selected index.
static int subNav(const char* id, int& cur, std::initializer_list<const char*> names) {
    ImGui::PushID(id);
    int i = 0;
    for (const char* n : names) { if (i) ImGui::SameLine(0, 6 * gUi); if (pillButton(n, cur == i)) cur = i; i++; }
    ImGui::PopID();
    ImGui::Spacing();
    return cur;
}

// Small rounded label drawn at an absolute position; returns its width.
static float tagAt(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 bg, ImU32 fg = IM_COL32(225, 232, 240, 255)) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float w = ts.x + 10, h = ts.y + 2;
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), bg, 3.f);
    dl->AddText(ImVec2(pos.x + 5, pos.y + 1), fg, text);
    return w;
}

static void gaugePill(float width, float frac, ImU32 fill, const char* text) {
    const float h = ImGui::GetFrameHeight() - 2;
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.y += 1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), IM_COL32(14, 16, 20, 255), 3.f);
    frac = std::min(1.f, std::max(0.f, frac));
    if (frac > 0.02f) dl->AddRectFilled(p, ImVec2(p.x + std::max(h, width * frac), p.y + h), fill, 3.f);
    dl->AddRect(p, ImVec2(p.x + width, p.y + h), IM_COL32(52, 58, 66, 255), 3.f);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(p.x + (width - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), IM_COL32(240, 244, 248, 255), text);
    ImGui::Dummy(ImVec2(width, h));
}

// label : value readout in the status area (label dim, value in the mono font)

static void lamp(const char* label, int state /*0 grey 1 green 2 amber 3 red*/, int icon = -1) {
    ImVec4 c = state == 1 ? pal::okGreen() : state == 2 ? pal::warnAmber()
             : state == 3 ? pal::badRed() : ImVec4(0.26f, 0.29f, 0.33f, 1);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const ImU32 cu = ImGui::ColorConvertFloat4ToU32(c);
    (void)icon;
    const float sq = 7.f, cy = p.y + ImGui::GetTextLineHeight() * 0.5f;
    if (state) dl->AddRectFilled(ImVec2(p.x + 1, cy - sq * 0.5f), ImVec2(p.x + 1 + sq, cy + sq * 0.5f), cu);
    else dl->AddRect(ImVec2(p.x + 1, cy - sq * 0.5f), ImVec2(p.x + 1 + sq, cy + sq * 0.5f), cu);
    ImGui::Dummy(ImVec2(sq + 5, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0, 0);
    ImGui::TextDisabled("%s", label);
    static const struct { const char* k; const char* tip; } kTips[] = {
        {"IQ", "Sample stream. Green: the ADC level is in a healthy range. Amber: too low or high. Red: clipping."},
        {"P1", "DVB-T2: the P1 preamble symbol (frame start and mode) is being found."},
        {"GI", "Guard interval and symbol timing are locked."},
        {"L1-pre", "DVB-T2: the L1 pre-signalling block (frame structure) decodes with a good CRC."},
        {"L1-post", "DVB-T2: the L1 post-signalling block (PLP list, modulation) decodes with a good CRC."},
        {"Frame", "A full T2 frame is being received and equalised."},
        {"LDPC", "Forward error correction, inner code: share of blocks that decode."},
        {"BCH", "Forward error correction, outer code: share of blocks that decode."},
        {"TS", "Transport stream: services found, continuity errors counted."},
        {"Video", "The player is decoding and showing pictures."},
        {"Audio", "The player has audio buffered and playing."},
        {"Sync", "DVB-T: OFDM symbol sync from the cyclic prefix."},
        {"TPS", "DVB-T: the transmission parameter signalling bits are decoded."},
        {"Chan", "Channel estimate from the pilots is valid."},
        {"Viterbi", "DVB-T: the convolutional decoder found the packet sync."},
        {"RS", "Reed-Solomon outer code: share of packets that are clean."},
        {"Pilot", "ATSC: the 8-VSB pilot carrier is locked."},
        {"Seg", "ATSC: data segment sync found."},
        {"Field", "ATSC: field sync found."},
        {"Eq", "ATSC: the equaliser is trained."},
        {"Trellis", "ATSC: the trellis decoder produces a valid transport stream."}};
    {
        const ImVec2 mx = ImGui::GetItemRectMax();
        if (ImGui::IsMouseHoveringRect(p, ImVec2(mx.x, p.y + ImGui::GetTextLineHeight()))) {
            for (auto& e : kTips) if (!strcmp(e.k, label)) { ImGui::SetTooltip("%s", e.tip); break; }
        }
    }
}

static void toolbar(App& a) {
    bool running = a.engine.running();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(pal::accent(), "ONAIR");
    ImGui::SameLine(0, 16 * gUi);
    ImGui::TextDisabled("SRC"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(190 * gUi);
    ImGui::BeginDisabled(running);
    if (ImGui::BeginCombo("##src", a.devices[a.devIdx].name.c_str())) {
        for (int i = 0; i < (int)a.devices.size(); i++)
            if (ImGui::Selectable(a.devices[i].name.c_str(), i == a.devIdx)) a.devIdx = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (iconFlat(Ic::Refresh, "Rescan for devices")) refreshDevices(a), a.devIdx = std::min(a.devIdx, (int)a.devices.size() - 1);
    ImGui::EndDisabled();

    bool isHw = a.devices[a.devIdx].isRadio();
    const DeviceInfo& curDev = a.devices[a.devIdx];
    const bool generic = curDev.kind == DeviceInfo::Soapy;
    bool isFile = a.devices[a.devIdx].kind == DeviceInfo::File;

    vSeparator();
    ImGui::TextDisabled("FREQ");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Centre frequency");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(110 * gUi);
    bool retune = false;
    ImGui::InputDouble("##freq", &a.freqMhz, 0, 0, "%.3f MHz");
    if (ImGui::IsItemDeactivatedAfterEdit()) retune = true;
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled(a.dabMode ? "CH" : "BW");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(a.dabMode ? "DAB channel" : "Channel bandwidth");
    ImGui::SameLine(0, 5 * gUi);
    if (a.dabMode) { if (dabChannelCombo(a)) retune = true; }
    else {
    ImGui::SetNextItemWidth((a.bwAuto ? 125 : 80) * gUi);
    {
        char bl[32];
        snprintf(bl, sizeof bl, a.bwAuto ? "%s (auto)" : "%s", kBw[a.bwIdx].label);
        if (a.atscMode) snprintf(bl, sizeof bl, "6 MHz");
        ImGui::BeginDisabled(a.atscMode || (running && !a.bwAuto));
        if (ImGui::BeginCombo("##bw", bl)) {
            for (int i = 0; i < (int)(sizeof kBw / sizeof *kBw); i++)
                if (ImGui::Selectable(kBw[i].label, i == a.bwIdx && !a.bwAuto)) { a.bwIdx = i; a.bwAuto = false; savePrefs(a); }
            ImGui::Separator();
            if (ImGui::Selectable("Automatic", a.bwAuto)) { a.bwAuto = true; savePrefs(a); }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Automatic: the width of the signal is measured in the spectrum and the channel bandwidth (5, 6, 7 or 8 MHz) is set for you.");
        if (generic && curDev.maxRateHz > 0 && curDev.maxRateHz < 7.9e6 * kBw[a.bwIdx].mhz / 8.0 - 1) {
            ImGui::SameLine(0, 6 * gUi);
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.28f, 1), "(!)");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("This radio tops out at %.2f Msps; a %g MHz channel needs about %.1f Msps.\nChoose a narrower channel (for example 1.7 MHz DVB-T2-Lite) or a faster radio.", curDev.maxRateHz / 1e6, kBw[a.bwIdx].mhz, 7.9 * kBw[a.bwIdx].mhz / 8.0);
        }
    }
    }

    vSeparator();
    ImGui::BeginDisabled(!isHw);
    ImGui::TextDisabled("GAIN"); ImGui::SameLine(0, 5 * gUi);
    if (generic) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150 * gUi);
        float gdb = (float)a.tune.gainDb;
        ImGui::SliderFloat("##gain", &gdb, (float)curDev.gainMinDb, (float)std::max(curDev.gainMaxDb, curDev.gainMinDb + 1.0), "%.0f dB");
        a.tune.gainDb = std::round(gdb);
        if (ImGui::IsItemDeactivatedAfterEdit()) retune = true, a.agcOn = false;
    } else {
    ImGui::TextDisabled("LNA");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(78 * gUi);
    int lna = a.tune.lnaDb;
    ImGui::SliderInt("##lna", &lna, 0, 40, "%d dB");
    a.tune.lnaDb = (lna + 4) / 8 * 8; // hardware steps are 8 dB
    if (ImGui::IsItemDeactivatedAfterEdit()) retune = true, a.agcOn = false;
    ImGui::SameLine();
    ImGui::TextDisabled("VGA");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(78 * gUi);
    int vga = a.tune.vgaDb;
    ImGui::SliderInt("##vga", &vga, 0, 62, "%d dB");
    a.tune.vgaDb = (vga + 1) / 2 * 2; // 2 dB steps
    if (ImGui::IsItemDeactivatedAfterEdit()) retune = true, a.agcOn = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Amp", &a.tune.ampOn)) retune = true, a.agcOn = false;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("AGC", &a.agcOn)) { a.agc.reset(); }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Keep the ADC level in a healthy window (about -16 dBFS rms, no clipping) by adjusting LNA, VGA and amp.\nTouching a gain control turns it off.");
    ImGui::SameLine();
    if (a.sweep.active()) {
        if (ImGui::Button("Stop tune")) { a.sweep = GainSweep(); }
    } else if (ImGui::Button("Auto-tune") && running) {
        a.sweep.start(ImGui::GetTime(), generic ? (int)curDev.gainMaxDb : 0);
        a.agcOn = false;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Gain helper: tries LNA/VGA/amp combinations for about 40 s and keeps the one with the best SNR that does not clip.\nNeeds a signal the receiver can lock to.");
    ImGui::EndDisabled();
    if (!isHw && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Gain controls apply to radios only (not to a recording or the synthetic signal)");

    vSeparator();
    if (!running) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.42f, 0.28f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.14f, 0.56f, 0.36f, 1));
        const bool startClicked = iconButton(Ic::Play, "Start", IM_COL32(40, 70, 82, 255), IM_COL32(56, 94, 110, 255)) || a.wizStart;
        a.wizStart = false;
        ImGui::PopStyleColor(2);
        if (startClicked) {
            a.tune.centerHz = a.freqMhz * 1e6;
            applyBandwidth(a);
            if (isFile) { const double r = guessSampleRate(a.file.path); if (r > 0) a.file.sampleRate = r; } // the name says the rate
            if (isFile && a.file.path.empty()) a.engine.log("choose an IQ file first");
            else {
                a.engine.setComputeMode(a.computeMode); a.engine.setStandard(engineStd(a));
                a.engine.start(a.devices[a.devIdx], a.tune, a.file);
                savePrefs(a);
                a.smooth.clear(); a.peak.clear(); a.lastSeq = 0;
            }
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.16f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.22f, 0.22f, 1));
        const bool stopClicked = iconButton(Ic::Stop, "Stop", IM_COL32(112, 48, 48, 255), IM_COL32(146, 62, 62, 255));
        ImGui::PopStyleColor(2);
        if (stopClicked) a.engine.stop();
    }

    if (a.family != 2) {
    vSeparator();
    {
        static const char* modes[] = {"CPU", "GPU", "Auto"};
        char lbl[48];
        const bool gpuNow = a.rx.plpOnGpu;
        snprintf(lbl, sizeof lbl, "%s", a.computeMode == 2 ? (gpuNow ? "Auto (GPU)" : "Auto (CPU)") : modes[a.computeMode]);
        ImGui::TextDisabled("COMPUTE"); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(104 * gUi);
        if (ImGui::BeginCombo("##compute", lbl)) {
            for (int i = 0; i < 3; i++) {
                const bool dis = i == 1 && !a.rx.gpuAvailable && running;
                if (dis) ImGui::BeginDisabled();
                if (ImGui::Selectable(modes[i], a.computeMode == i)) { a.computeMode = i; a.engine.setComputeMode(i); savePrefs(a); }
                if (dis) ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("LDPC decoding backend.\nCPU: NEON on all cores. GPU: Metal compute.\nAuto: CPU until it falls behind real time, then GPU.");
    }
    ImGui::SameLine(0, 14 * gUi);
    {
        static const char* names[] = {"Auto", "DVB-T2", "DVB-T"};
        char lbl[48];
        const int act = a.engine.activeStandard();
        snprintf(lbl, sizeof lbl, "%s", a.stdMode == 0 ? (a.rx.standard == 1 ? "Auto (DVB-T)" : "Auto (DVB-T2)") : names[a.stdMode]);
        (void)act;
        ImGui::TextDisabled("STD"); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(112 * gUi);
        if (ImGui::BeginCombo("##std", lbl)) {
            for (int i = 0; i < 3; i++) if (a.family == 0 && ImGui::Selectable(names[i], a.stdMode == i)) { a.stdMode = i; a.engine.setStandard(i); savePrefs(a); }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Which broadcast standard to decode.\nAuto alternates between DVB-T2 and DVB-T until one locks.");
    }

    }

    if (retune && running) {
        a.tune.centerHz = a.freqMhz * 1e6;
        a.engine.log("retune " + std::to_string(a.freqMhz) + " MHz");
        a.engine.retune(a.tune);
        a.peak.clear();
        savePrefs(a);
    }

}

// Options of the selected source that are not gain/frequency: the synthetic generator's parameters, or the IQ file.
static void sourceOptions(App& a) {
    bool running = a.engine.running();
    bool isFile = a.devices[a.devIdx].kind == DeviceInfo::File;
    if (a.devices[a.devIdx].kind == DeviceInfo::Synthetic) {
        SynthConfig& sc = a.tune.synth;
        bool ch = false;
        ImGui::TextDisabled("synthetic");
        ImGui::SameLine();
        if (!a.atscMode && ImGui::Checkbox("DVB-T", &sc.dvbt)) ch = true;
        ImGui::SameLine();
        if (a.atscMode) {
            ImGui::TextDisabled("ATSC 8-VSB, 6 MHz");
        } else if (sc.dvbt) {
            static const char* fftN[] = {"2K", "8K"}; static const char* gis[] = {"1/32", "1/16", "1/8", "1/4"};
            static const char* mods[] = {"QPSK", "16-QAM", "64-QAM"}; static const char* rates[] = {"1/2", "2/3", "3/4", "5/6", "7/8"};
            ImGui::SetNextItemWidth(60 * gUi);
            if (ImGui::BeginCombo("##tfft", fftN[sc.dvbtMode & 1])) { for (int i = 0; i < 2; i++) if (ImGui::Selectable(fftN[i], i == sc.dvbtMode)) { sc.dvbtMode = i; ch = true; } ImGui::EndCombo(); }
            ImGui::SameLine(); ImGui::TextDisabled("GI"); ImGui::SameLine(); ImGui::SetNextItemWidth(66 * gUi);
            if (ImGui::BeginCombo("##tgi", gis[sc.dvbtGuard & 3])) { for (int i = 0; i < 4; i++) if (ImGui::Selectable(gis[i], i == sc.dvbtGuard)) { sc.dvbtGuard = i; ch = true; } ImGui::EndCombo(); }
            ImGui::SameLine(); ImGui::SetNextItemWidth(84 * gUi);
            if (ImGui::BeginCombo("##tmod", mods[sc.dvbtMod % 3])) { for (int i = 0; i < 3; i++) if (ImGui::Selectable(mods[i], i == sc.dvbtMod)) { sc.dvbtMod = i; ch = true; } ImGui::EndCombo(); }
            ImGui::SameLine(); ImGui::SetNextItemWidth(60 * gUi);
            if (ImGui::BeginCombo("##trate", rates[sc.dvbtRate % 5])) { for (int i = 0; i < 5; i++) if (ImGui::Selectable(rates[i], i == sc.dvbtRate)) { sc.dvbtRate = i; ch = true; } ImGui::EndCombo(); }
        } else {
        static const struct { const char* n; int code; } fm[] = {{"2K", 0}, {"8K", 1}, {"4K", 2}, {"1K", 3}, {"16K", 4}, {"32K", 5}};
        const char* cur = "?";
        for (auto& f : fm) if (f.code == sc.tx.s2field1) cur = f.n;
        ImGui::SetNextItemWidth(60 * gUi);
        if (ImGui::BeginCombo("##sfft", cur)) {
            for (auto& f : fm) if (ImGui::Selectable(f.n, f.code == sc.tx.s2field1)) { sc.tx.s2field1 = f.code; ch = true; }
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::TextDisabled("GI"); ImGui::SameLine();
        ImGui::SetNextItemWidth(70 * gUi);
        if (ImGui::BeginCombo("##sgi", guardName(sc.tx.giIdx))) {
            for (int g = 0; g < kNumGi; g++) if (ImGui::Selectable(guardName(g), g == sc.tx.giIdx)) { sc.tx.giIdx = g; ch = true; }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("ext", &sc.tx.ext)) ch = true;
        ImGui::SameLine(); ImGui::SetNextItemWidth(62 * gUi);
        {
            char ppl[8]; snprintf(ppl, sizeof ppl, "PP%d", sc.tx.pp + 1);
            if (ImGui::BeginCombo("##spp", ppl)) {
                for (int q = 0; q < 8; q++) { snprintf(ppl, sizeof ppl, "PP%d", q + 1); if (ImGui::Selectable(ppl, q == sc.tx.pp)) { sc.tx.pp = q; ch = true; } }
                ImGui::EndCombo();
            }
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("TR", &sc.tx.tr)) ch = true;
        }
        ImGui::SameLine(); ImGui::TextDisabled("SNR"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
        float snr = (float)sc.snrDb; if (ImGui::SliderFloat("##ssnr", &snr, 0, 40, "%.0f dB")) { sc.snrDb = snr; ch = true; }
        ImGui::SameLine(); ImGui::TextDisabled("CFO"); ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
        float cfo = (float)(sc.cfoHz / 1e3); if (ImGui::SliderFloat("##scfo", &cfo, -40, 40, "%.2f kHz")) { sc.cfoHz = cfo * 1e3; ch = true; }
        ImGui::SameLine(); ImGui::TextDisabled("SRO"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
        float sro = (float)sc.sroPpm; if (ImGui::SliderFloat("##ssro", &sro, -50, 50, "%.0f ppm")) { sc.sroPpm = sro; ch = true; }
        ImGui::SameLine(); ImGui::TextDisabled("echo"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
        float ec = (float)sc.echoDb; if (ImGui::SliderFloat("##sec", &ec, 0, 20, ec == 0 ? "off" : "-%.0f dB")) { sc.echoDb = ec; ch = true; }
        if (ch && running) a.engine.retune(a.tune);
    }
    if (isFile && !running) {
        ImGui::TextDisabled("file");
        ImGui::SameLine();
        if (ImGui::SmallButton("Open…")) { auto p = openFileDialog(); if (!p.empty()) { a.file.path = p; a.file.format = guessFormat(p); const double r = guessSampleRate(p); if (r > 0) a.file.sampleRate = r; } }
        ImGui::SameLine();
        ImGui::TextUnformatted(a.file.path.empty() ? "(none)" : a.file.path.c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70 * gUi);
        const char* fm[] = {"cs8", "cu8", "cf32"};
        int fi = (int)a.file.format;
        if (ImGui::Combo("##ff", &fi, fm, 3)) a.file.format = (FileFormat)fi;
        ImGui::SameLine();
        ImGui::TextDisabled("rate");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * gUi);
        double msps = a.file.sampleRate / 1e6;
        if (ImGui::InputDouble("##frate", &msps, 0, 0, "%.4f Msps")) a.file.sampleRate = msps * 1e6;
        ImGui::SameLine();
        ImGui::Checkbox("loop", &a.file.loop);
    }
}

static void statusBar(App& a) {
    if (a.dabMode) { dabStatus(a); return; }
    const bool run = a.engine.running();
    const SignalStats& st = a.spec.stats;
    const RxTelemetry& rx = a.rx;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    {   // a tinted panel behind the two status lines
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight() * 2.f + ImGui::GetStyle().ItemSpacing.y * 2.f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - 4, p.y - 2), ImVec2(p.x + ImGui::GetContentRegionAvail().x + 4, p.y + h), IM_COL32(22, 23, 25, 255), 3.f);
    }
    // ---- line 1: lamps, lock state and signal mode
    {
        int iq = run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0;
        lamp("IQ", iq, (int)Ic::Wave); ImGui::SameLine(0, 12 * gUi);
        const double frameS = rx.frameMs > 0 ? rx.frameMs / 1e3 : 0.5;
        const bool isT = rx.standard == 1;
        const double okFrac = (rx.blocksOk + rx.blocksBad) ? (double)rx.blocksOk / (rx.blocksOk + rx.blocksBad) : 0;
        const int fec = !run || !rx.plpValid || rx.plpFrames == 0 ? 0 : okFrac > 0.995 ? 1 : okFrac > 0.5 ? 2 : 3;
        if (rx.standard == 2) {
            const AtscTelemetry& at = rx.atsc;
            const double okF = (at.rsClean + at.rsCorrected + at.rsFailed) ? (double)(at.rsClean + at.rsCorrected) / (double)(at.rsClean + at.rsCorrected + at.rsFailed) : 0;
            lamp("Pilot", !run ? 0 : at.pilot ? 1 : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("Seg", !run ? 0 : at.segSync ? 1 : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("Field", !run ? 0 : at.fieldSync ? 1 : at.segSync ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("Eq", !run ? 0 : at.eqTrained ? 1 : at.fieldSync ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("Trellis", !run ? 0 : at.tsOk ? 1 : at.eqTrained ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("RS", !run || at.fields == 0 ? 0 : okF > 0.995 ? 1 : okF > 0.5 ? 2 : 3); ImGui::SameLine(0, 12 * gUi);
        } else if (isT) {
            // DVB-T: cyclic-prefix sync, guard interval, TPS signalling, channel estimate, then Viterbi and Reed-Solomon
            lamp("Sync", !run ? 0 : rx.state >= 1 ? 1 : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("GI", !run ? 0 : rx.giIdx >= 0 ? (rx.state == 2 ? 1 : 2) : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("TPS", !run ? 0 : rx.dvbt.tpsOk ? 1 : rx.state == 1 ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("Chan", !run ? 0 : rx.chValid && rx.dataValid ? 1 : rx.dvbt.tpsOk ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("Viterbi", !run ? 0 : rx.dvbt.fecSync ? 1 : rx.dvbt.tpsOk ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("RS", fec); ImGui::SameLine(0, 12 * gUi);
        } else {
        lamp("P1", !run || !a.rxSeen ? 0 : (rx.p1.valid && rx.secSinceP1 < std::max(1.0, 3 * frameS) ? 1 : 2)); ImGui::SameLine(0, 12 * gUi);
        lamp("GI", !run ? 0 : rx.state == 2 ? 1 : rx.state == 1 ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
        lamp("L1-pre", !run ? 0 : rx.l1preOk ? 1 : rx.l1preGood > 0 ? 2 : rx.chValid ? 3 : 0); ImGui::SameLine(0, 12 * gUi);
        lamp("L1-post", !run ? 0 : rx.l1postOk ? 1 : rx.l1postGood > 0 ? 2 : rx.l1preOk ? 3 : 0); ImGui::SameLine(0, 12 * gUi);
        lamp("Frame", !run ? 0 : rx.dataValid ? 1 : rx.l1preOk ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
        lamp("LDPC", fec); ImGui::SameLine(0, 12 * gUi);
        lamp("BCH", fec); ImGui::SameLine(0, 12 * gUi);
        }
        lamp("TS", !run ? 0 : a.ts.services.empty() ? 0 : (a.ts.ccErrors > 0 && a.bb.framesLost > 0.02 * (a.bb.frames + 1)) ? 2 : 1, (int)Ic::Layers); ImGui::SameLine(0, 12 * gUi);
        const PlayerStats ps = a.engine.player().stats();
        const bool pl = run && ps.active;
        lamp("Video", !pl || !ps.hasVideo ? 0 : (ps.shown > 0 && ps.videoQueue > 2) ? 1 : 2, (int)Ic::Tv); ImGui::SameLine(0, 12 * gUi);
        lamp("Audio", !pl || !ps.hasAudio ? 0 : (ps.audioBufferMs > 150) ? 1 : 2, (int)Ic::Speaker); ImGui::SameLine(0, 10 * gUi);
        ImGui::TextDisabled("|"); ImGui::SameLine(0, 10 * gUi);
    }
    auto ro = [&](const char* label, const std::string& val, ImVec4 col = ImVec4(0.93f, 0.95f, 0.97f, 1)) {
        static const struct { const char* k; Ic ic; } kIcons[] = {
            {"State", Ic::Pulse}, {"Mode", Ic::Layers}, {"FFT", Ic::Grid}, {"GI", Ic::Echo}, {"Pilots", Ic::Target}, {"PLP", Ic::Layers},
            {"CFO", Ic::Wave}, {"SRO", Ic::Clock}, {"SNR", Ic::Signal}, {"MER", Ic::Target}, {"Delay", Ic::Echo}, {"fs", Ic::Gauge},
            {"dropped", Ic::Warning}, {"Channel", Ic::Antenna}, {"Field", Ic::Layers}, {"TPS", Ic::Info}, {"hier", Ic::Layers}, {"Pilot", Ic::Target}};
        ImGui::AlignTextToFramePadding();
        for (auto& e : kIcons) if (!strcmp(e.k, label)) {
            const bool isState = !strcmp(label, "State");
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            iconInline(e.ic, isState ? ImGui::ColorConvertFloat4ToU32(col) : iconDim(), 0.9f);
            ImGui::PopStyleVar();
            ImGui::SameLine(0, 4 * gUi);
            break;
        }
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(col, "%s", val.c_str()); ImGui::PopFont();
        ImGui::SameLine(0, 15 * gUi);
    };
    char b[96];
    if (!run) ro("State", "stopped", ImVec4(0.6f, 0.64f, 0.68f, 1));
    else if (rx.state == 2) ro("State", "Locked", ImVec4(0.35f, 0.90f, 0.45f, 1));
    else if (rx.state == 1) ro("State", "syncing", ImVec4(0.95f, 0.75f, 0.2f, 1));
    else ro("State", "searching", ImVec4(0.6f, 0.64f, 0.68f, 1));
    if (run && rx.standard == 2 && rx.state >= 1) {
        ro("Mode", "ATSC 8-VSB");
        ro("Channel", "6 MHz");
        snprintf(b, sizeof b, "%d", rx.atsc.fieldParity); ro("Field", b);
        ro("Pilot", rx.atsc.pilot ? "locked" : "-");
    } else if (run && rx.standard == 1 && rx.state >= 1) {
        ro("Mode", "DVB-T");
        ro("FFT", rx.fftN == 8192 ? "8K" : "2K");
        ro("GI", dvbt::guardName(rx.giIdx));
        if (rx.dvbt.tpsOk) {
            snprintf(b, sizeof b, "%s %s", dvbt::modName(rx.dvbt.mod), dvbt::rateName(rx.dvbt.crHp));
            ro("TPS", b);
            if (rx.dvbt.hier) ro("hier", "yes", ImVec4(0.95f, 0.7f, 0.2f, 1));
        } else ro("TPS", "searching", ImVec4(0.6f, 0.64f, 0.68f, 1));
    } else if (run && rx.state == 2) {
        ro("Mode", s1Name(rx.p1.s1));
        const FftMode* fm = fftModeFromSize(rx.fftN);
        snprintf(b, sizeof b, "%s%s", fm ? fm->name : "?", rx.extCarriers ? " ext" : ""); ro("FFT", b);
        ro("GI", guardName(rx.giIdx));
        if (rx.l1preOk) { snprintf(b, sizeof b, "PP%d", rx.l1pre.pilotPattern + 1); ro("Pilots", b); }
        if (rx.plpValid) {
            static const char* mods[] = {"QPSK", "16-QAM", "64-QAM", "256-QAM"}; static const char* rates[] = {"1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "1/3", "2/5"};
            snprintf(b, sizeof b, "%s %s%s", rx.plpFec.mod >= 0 && rx.plpFec.mod < 4 ? mods[rx.plpFec.mod] : "?", rx.plpFec.rate >= 0 && rx.plpFec.rate < 8 ? rates[rx.plpFec.rate] : "?", rx.plpFec.rotation ? " rot" : "");
            ro("PLP", b);
        }
    }
    ImGui::NewLine();
    // ---- line 2: numbers and gauges
    if (run && (rx.state == 2 || (rx.standard == 1 && rx.state >= 1))) {
        snprintf(b, sizeof b, "%+.1f Hz", rx.cfoHz); ro("CFO", b);
        if (rx.standard != 1) { snprintf(b, sizeof b, "%+.1f ppm", rx.sroPpm); ro("SRO", b); }
        snprintf(b, sizeof b, "%.1f dB", rx.dataValid ? rx.dataSnrDb : rx.cpSnrDb); ro("SNR", b);
        if (rx.plpMerDb > 0 && rx.plpMerDb < 90) { snprintf(b, sizeof b, "%.1f dB", rx.plpMerDb); ro("MER", b); }
        if (!a.mpd.report().echoes.empty()) { snprintf(b, sizeof b, "%.2f us", std::fabs(a.mpd.report().echoes[0].delayUs)); ro("Delay", b); }
    }
    if (run) { snprintf(b, sizeof b, "%.3f Msps", a.engine.sampleRate() / 1e6); ro("fs", b); }
    ImGui::AlignTextToFramePadding();
    iconInline(Ic::Gauge, iconDim(), 0.9f); ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("Level"); ImGui::SameLine(0, 5 * gUi);
    {
        const ImU32 col = adc == AdcStatus::Overload ? IM_COL32(176, 66, 58, 255) : adc == AdcStatus::Good ? IM_COL32(40, 112, 150, 255) : IM_COL32(176, 130, 48, 255);
        snprintf(b, sizeof b, run ? "%.1f dBFS" : "-", st.rmsDbfs);
        gaugePill(130, run ? (st.rmsDbfs + 60.f) / 60.f : 0.f, col, b);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("ADC level (rms). %s\npeak %.2f   clip %.3f%%   DC %+.3f / %+.3f", adcAdvice(adc).c_str(), st.peak, st.clipFraction * 100, st.dcI, st.dcQ);
    }
    ImGui::SameLine(0, 15 * gUi);
    ImGui::AlignTextToFramePadding();
    iconInline(Ic::Signal, iconDim(), 0.9f); ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("Quality"); ImGui::SameLine(0, 5 * gUi);
    {
        const QualityReport& q = a.quality.report();
        const float t = run && q.valid ? (float)q.percent / 100.f : 0.f;
        const ImU32 col = t < 0.25f ? IM_COL32(176, 66, 58, 255) : t < 0.5f ? IM_COL32(176, 130, 48, 255) : IM_COL32(40, 112, 150, 255);
        snprintf(b, sizeof b, run && q.valid ? "%.0f%%  %s" : "-", q.percent, q.label.c_str());
        gaugePill(130, t, col, b);
        if (ImGui::IsItemHovered() && run && q.valid) ImGui::SetTooltip("data SNR %.1f dB, needed about %.1f dB (margin %+.1f dB)\nFEC blocks decoded %.1f%%", q.snrDb, q.requiredDb, q.marginDb, q.fecOk * 100);
    }
    ImGui::SameLine(0, 15 * gUi);
    if (run) { snprintf(b, sizeof b, "%llu", (unsigned long long)a.engine.droppedSamples()); ro("dropped", b, a.engine.droppedSamples() ? ImVec4(0.95f, 0.45f, 0.3f, 1) : ImVec4(0.93f, 0.95f, 0.97f, 1)); }
    if (run && a.mpd.report().level != MultipathLevel::Unknown) {
        const MultipathReport& mr = a.mpd.report();
        const ImVec4 col = mr.level == MultipathLevel::None ? ImVec4(0.5f, 0.55f, 0.6f, 1) : mr.level == MultipathLevel::Mild ? ImVec4(0.95f, 0.8f, 0.3f, 1)
                         : mr.level == MultipathLevel::Likely ? ImVec4(0.95f, 0.6f, 0.2f, 1) : ImVec4(0.95f, 0.35f, 0.25f, 1);
        ImGui::AlignTextToFramePadding();
        iconInline(Ic::Echo, ImGui::ColorConvertFloat4ToU32(col), 0.9f); ImGui::SameLine(0, 4 * gUi);
        ImGui::TextColored(col, "multipath: %s", multipathName(mr.level));
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(mr.headline.c_str());
            for (auto& r : mr.reasons) ImGui::BulletText("%s", r.c_str());
            ImGui::EndTooltip();
        }
        ImGui::SameLine(0, 15 * gUi);
    }
    if (run && a.devices[a.devIdx].kind != DeviceInfo::File) {
        ImGui::AlignTextToFramePadding();
        if (a.sweep.active()) ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "gain helper %d/%d ...", std::max(0, a.sweep.current()) + 1, (int)a.sweep.entries().size());
        else if (adc == AdcStatus::Overload) ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1), "ADC OVERLOAD - %s", a.agcOn ? "AGC is lowering the gain" : "reduce the gain (or enable AGC)");
        else if (adc == AdcStatus::High) ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC level high");
        else if (adc == AdcStatus::Low) ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC level low - raise the gain");
        else if (adc == AdcStatus::NoSignal) ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC sees almost nothing - antenna / gain?");
        else ImGui::NewLine();
    } else ImGui::NewLine();
}

static void ingestSpectrum(App& a) {
    SpectrumFrame f;
    if (!a.engine.latestSpectrum(f, a.lastSeq)) return;
    a.lastSeq = f.seq;
    a.spec = f;
    size_t n = f.dbfs.size();
    if (a.smooth.size() != n || a.peak.size() != n) { a.smooth = f.dbfs; a.peak = f.dbfs; }   // both lines always have the frame size (a retune clears the peak line)
    for (size_t i = 0; i < n; i++) {
        a.smooth[i] += 0.35f * (f.dbfs[i] - a.smooth[i]);
        a.peak[i] = std::max(a.peak[i] - 0.15f, f.dbfs[i]);
    }
    double t = glfwGetTime();
    if (a.lastFrameT > 0) a.frameDt = a.frameDt * 0.9 + (t - a.lastFrameT) * 0.1;
    a.lastFrameT = t;
    a.wf.push(f.dbfs);
}

static void ingestRx(App& a) {
    double now = glfwGetTime();
    if (a.engine.running() && now - a.tsT > 0.25) { a.ts = a.engine.tsSnapshot(); a.bb = a.engine.bbStats(); a.tsT = now; }
    RxTelemetry t;
    if (!a.engine.latestRx(t, a.rxSeq)) return;
    a.rxSeq = t.seq;
    if (t.p1Count > 0) a.rxSeen = true;
    if (t.dataFrames < a.rx.dataFrames) { a.mpd.reset(); a.quality.reset(); a.cst.mod = -1; } // receiver was restarted or retuned
    a.rx = std::move(t);
    {   // statistics of the decoded constellation cells
        App::ConstStats& c = a.cst;
        const RxTelemetry& r = a.rx;
        if (r.plpConst.empty() || r.plpConstTx.size() != r.plpConst.size()) { c.seq = 0; c.mod = -1; }
        else if (r.plpConstSeq != c.seq) {
            const int nPts = 1 << (2 * (r.plpFec.mod + 1));
            if (c.mod != r.plpFec.mod || c.plp != r.plpId || (int)c.pts.size() != nPts || c.grid.empty()) {
                c.mod = r.plpFec.mod; c.plp = r.plpId;
                c.pts.assign(nPts, App::ConstStats::Pt());
                c.grid.assign(App::ConstStats::G * App::ConstStats::G, 0.f);
            }
            c.seq = r.plpConstSeq;
            for (float& v : c.grid) v *= 0.93f;
            for (auto& p : c.pts) { p.n *= 0.97; p.ei *= 0.97; p.eq *= 0.97; p.e2 *= 0.97; }
            const int G = App::ConstStats::G;
            const float lim = 1.4f;
            for (size_t i = 0; i < r.plpConst.size(); i++) {
                const cf32 o = r.plpConst[i];
                const int gx = (int)((o.real() + lim) / (2 * lim) * G), gy = (int)((lim - o.imag()) / (2 * lim) * G);
                if (gx >= 0 && gx < G && gy >= 0 && gy < G) c.grid[gy * G + gx] += 1.f;
                const unsigned l = r.plpConstTx[i];
                if (l < c.pts.size()) {
                    const cf32 e = o - qamPoint(c.mod, false, l);
                    auto& p = c.pts[l];
                    p.n += 1; p.ei += e.real(); p.eq += e.imag(); p.e2 += std::norm(e);
                }
            }
        }
    }
    a.mpd.update(a.rx);
    a.quality.update(a.rx);
    {
        const double nowT = glfwGetTime();
        if (a.engine.running() && nowT - a.histT >= 0.25) {
            App::HistSample h;
            h.t = (float)nowT;
            h.snr = a.rx.dataValid ? a.rx.dataSnrDb : NAN;
            h.mer = a.rx.plpMerDb > 0 && a.rx.plpMerDb < 90 ? (float)a.rx.plpMerDb : NAN;
            const uint64_t dOk = a.rx.blocksOk >= a.histOk ? a.rx.blocksOk - a.histOk : 0, dBad = a.rx.blocksBad >= a.histBad ? a.rx.blocksBad - a.histBad : 0;
            h.loss = (dOk + dBad) ? 100.f * (float)dBad / (float)(dOk + dBad) : NAN;
            a.histOk = a.rx.blocksOk; a.histBad = a.rx.blocksBad;
            h.cfo = a.rx.state == 2 ? (float)a.rx.cfoHz : NAN;
            h.sro = a.rx.state == 2 ? (float)a.rx.sroPpm : NAN;
            h.level = a.spec.stats.rmsDbfs; h.clip = a.spec.stats.clipFraction * 100.f;
            h.quality = a.quality.report().valid ? (float)a.quality.report().percent : NAN;
            a.hist.push_back(h);
            if (a.hist.size() > 3600) a.hist.pop_front();
            a.histT = nowT;
        }
    }
    auto push = [](std::deque<float>& d, float v) { d.push_back(v); if (d.size() > 600) d.pop_front(); };
    if (a.rx.state == 2) {
        push(a.hCfo, (float)a.rx.cfoHz);
        push(a.hSnr, a.rx.cpSnrDb);
        push(a.hTiming, a.rx.timingErr);
    }
}

static std::vector<double> xs(const App& a) {
    size_t n = a.smooth.size();
    std::vector<double> x(n);
    double fs = a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate;
    double c = a.freqMhz;
    for (size_t i = 0; i < n; i++) x[i] = c + ((double)i / n - 0.5) * fs / 1e6;
    return x;
}

static void spectrumPlot(App& a, ImVec2 size) {
    if (ImPlot::BeginPlot("##spec", size, ImPlotFlags_NoLegend | ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes("frequency (MHz)", "power (dBFS/bin)");
        double fs = (a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate) / 1e6;
        // follow a retune: when the centre or the span changes, bring the view back to the new band (otherwise it keeps the user's own zoom)
        static double lastC = 0, lastFs = 0;
        const bool moved = lastC != a.freqMhz || lastFs != fs;
        lastC = a.freqMhz; lastFs = fs;
        ImPlot::SetupAxisLimits(ImAxis_X1, a.freqMhz - fs / 2, a.freqMhz + fs / 2, moved ? ImPlotCond_Always : ImPlotCond_Once);
        ImPlot::SetupAxisLimits(ImAxis_Y1, a.yMin, a.yMax, ImPlotCond_Once);
        ImPlot::SetupAxisFormat(ImAxis_X1, "%.2f");
        if (!a.smooth.empty()) {
            auto x = xs(a);
            // channel overlay: 8 MHz occupied band around the centre
            double bw = kBw[a.bwIdx].mhz;
            double xo[2] = {a.freqMhz - bw / 2 * 0.95, a.freqMhz + bw / 2 * 0.95};
            double yo[2] = {a.yMax, a.yMax};
            ImPlotSpec band; band.FillColor = pal::accent(0.10f); band.LineColor = ImVec4(0, 0, 0, 0);
            ImPlot::PlotShaded("band", xo, yo, 2, a.yMin, band);
            if (a.peakHold) {
                ImPlotSpec ps; ps.LineColor = pal::grey(0.40f); ps.LineWeight = 1.0f;
                std::vector<double> yp(a.peak.begin(), a.peak.end());
                ImPlot::PlotLine("peak", x.data(), yp.data(), (int)std::min(x.size(), yp.size()), ps);
            }
            ImPlotSpec ss; ss.LineColor = pal::accent(); ss.LineWeight = 1.3f;
            std::vector<double> ysm(a.smooth.begin(), a.smooth.end());
            ImPlot::PlotLine("spectrum", x.data(), ysm.data(), (int)std::min(x.size(), ysm.size()), ss);
            double cx[2] = {a.freqMhz, a.freqMhz}, cy[2] = {a.yMin, a.yMax};
            ImPlotSpec cs; cs.LineColor = pal::grey(0.35f);
            ImPlot::PlotLine("centre", cx, cy, 2, cs);
        }
        ImPlot::EndPlot();
    }
}

static void waterfallPlot(App& a, ImVec2 size) {
    const int H = Waterfall::H;
    double secs = H * a.frameDt;
    if (ImPlot::BeginPlot("##wf", size, ImPlotFlags_NoLegend | ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes("frequency (MHz)", "seconds ago");
        double fs = (a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate) / 1e6;
        double x0 = a.freqMhz - fs / 2, x1 = a.freqMhz + fs / 2;
        static double lastC = 0, lastFs = 0;
        const bool moved = lastC != a.freqMhz || lastFs != fs;
        lastC = a.freqMhz; lastFs = fs;
        ImPlot::SetupAxisLimits(ImAxis_X1, x0, x1, moved ? ImPlotCond_Always : ImPlotCond_Once);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -secs, 0, ImPlotCond_Once);
        ImPlot::SetupAxisFormat(ImAxis_X1, "%.2f");
        int w = a.wf.writeRow;
        double secA = (H - w) * a.frameDt;
        ImPlot::PlotImage("a", a.wf.img->texture(), ImPlotPoint(x0, -secA), ImPlotPoint(x1, 0), ImVec2(0, (float)w / H), ImVec2(1, 1));
        if (w > 0)
            ImPlot::PlotImage("b", a.wf.img->texture(), ImPlotPoint(x0, -secs), ImPlotPoint(x1, -secA), ImVec2(0, 0), ImVec2(1, (float)w / H));
        ImPlot::EndPlot();
    }
}

static void histogramPlot(App& a, ImVec2 size) {
    if (ImPlot::BeginPlot("##hist", size, ImPlotFlags_NoLegend | ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes("|sample| (fraction of full scale)", "count");
        ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, 1, ImPlotCond_Once);
        float h[64], x[64];
        for (int i = 0; i < 64; i++) { h[i] = (float)std::max<uint32_t>(1, a.spec.stats.hist[i]); x[i] = (i + 0.5f) / 64; }
        ImPlotSpec bs; bs.FillColor = pal::accent(0.8f);
        ImPlot::PlotBars("adc", x, h, 64, 1.0 / 64, bs);
        ImPlot::EndPlot();
    }
}

static void scatter(const char* id, const std::vector<cf32>& pts, ImVec2 size, double lim, ImVec4 col) {
    if (ImPlot::BeginPlot(id, size, ImPlotFlags_NoLegend | ImPlotFlags_NoTitle | ImPlotFlags_Equal)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_NoTickLabels);
        ImPlot::SetupAxisLimits(ImAxis_X1, -lim, lim, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -lim, lim, ImPlotCond_Always);
        if (!pts.empty()) {
            ImPlotSpec sp;
            sp.Marker = ImPlotMarker_Circle; sp.MarkerSize = 1.6f; sp.Stride = sizeof(cf32);
            sp.MarkerFillColor = col; sp.MarkerLineColor = col; sp.LineColor = col;
            const float* d = reinterpret_cast<const float*>(pts.data());
            ImPlot::PlotScatter("pts", d, d + 1, (int)pts.size(), sp);
        }
        ImPlot::EndPlot();
    }
}

static void historyPlot(const char* id, const char* ylabel, const std::deque<float>& h, ImVec2 size) {
    if (ImPlot::BeginPlot(id, size, ImPlotFlags_NoLegend | ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes("samples (~30/s)", ylabel, 0, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, 600, ImPlotCond_Always);
        if (!h.empty()) {
            std::vector<float> v(h.begin(), h.end());
            ImPlot::PlotLine("h", v.data(), (int)v.size());
        }
        ImPlot::EndPlot();
    }
}

static void syncTab(App& a) {
    if (a.rx.standard == 1) {
        const RxTelemetry& rx = a.rx;
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "DVB-T synchronisation");
        if (rx.state == 0) { ImGui::TextDisabled("looking for a cyclic prefix: the FFT size (2K/8K) and guard interval are found from the correlation of each symbol's guard with its end"); return; }
        if (ImGui::BeginTable("synct", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 220);
            auto row = [&](const char* k, const char* fmt, auto... args) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k); ImGui::TableNextColumn(); char b[160]; snprintf(b, sizeof b, fmt, args...); ImGui::TextUnformatted(b); };
            row("FFT size / guard interval", "%d / %s (%d samples)", rx.fftN, dvbt::guardName(rx.giIdx), rx.guard);
            row("Carrier frequency offset", "%+.1f Hz", rx.cfoHz);
            row("Symbols processed", "%llu", (unsigned long long)rx.symbols);
            row("Frame length", "%.2f ms (68 symbols)", rx.frameMs);
            row("TPS frame sync", "%s", rx.dvbt.tpsOk ? "locked" : "searching (sync word + BCH over 68 symbols)");
            row("Noise / SNR estimate", "%.1f dB", rx.dataSnrDb);
            ImGui::EndTable();
        }
        return;
    }
    const RxTelemetry& rx = a.rx;
    float h = ImGui::GetContentRegionAvail().y;
    ImGui::TextDisabled("P1 detector - C-A-B correlation (peak > 0.30 triggers an S1/S2 decode)");
    if (ImPlot::BeginPlot("##p1", ImVec2(-1, h * 0.33f), ImPlotFlags_NoLegend | ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes("time (ms)", "correlation");
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 1.05, ImPlotCond_Always);
        if (!rx.p1Trace.empty() && rx.nativeRate > 0) {
            std::vector<float> xv(rx.p1Trace.size());
            double dt = kTraceDecim / rx.nativeRate * 1e3;
            for (size_t i = 0; i < xv.size(); i++) xv[i] = (float)((i - (double)xv.size()) * dt);
            ImPlot::SetupAxisLimits(ImAxis_X1, xv.front(), 0, ImPlotCond_Always);
            ImPlot::PlotLine("m", xv.data(), rx.p1Trace.data(), (int)xv.size());
            float tx[2] = {xv.front(), 0}, ty[2] = {0.30f, 0.30f};
            ImPlotSpec ts; ts.LineColor = ImVec4(0.9f, 0.7f, 0.2f, 0.7f);
            ImPlot::PlotLine("thr", tx, ty, 2, ts);
        }
        ImPlot::EndPlot();
    }
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::TextDisabled("guard-interval score (mean cyclic-prefix correlation over 3 symbols)");
    if (ImPlot::BeginPlot("##gi", ImVec2(w * 0.38f, -1), ImPlotFlags_NoLegend | ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes(nullptr, "score", ImPlotAxisFlags_NoGridLines, 0);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 1.05, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_X1, -0.6, kNumGi - 0.4, ImPlotCond_Always);
        double pos[kNumGi]; const char* lab[kNumGi];
        for (int i = 0; i < kNumGi; i++) { pos[i] = i; lab[i] = guardName(i); }
        ImPlot::SetupAxisTicks(ImAxis_X1, pos, kNumGi, lab);
        float sc[kNumGi], x[kNumGi];
        for (int i = 0; i < kNumGi; i++) { sc[i] = rx.giScore[i]; x[i] = (float)i; }
        ImPlotSpec bs; bs.FillColor = ImVec4(0.45f, 0.65f, 1.0f, 0.85f);
        ImPlot::PlotBars("gi", x, sc, kNumGi, 0.7, bs);
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    ImGui::BeginChild("hist", ImVec2(0, -1));
    float hh = ImGui::GetContentRegionAvail().y / 3 - 4;
    historyPlot("##hc", "CFO (Hz)", a.hCfo, ImVec2(-1, hh));
    historyPlot("##hs", "CP-SNR (dB)", a.hSnr, ImVec2(-1, hh));
    historyPlot("##ht", "timing (samples)", a.hTiming, ImVec2(-1, hh));
    ImGui::EndChild();
}

static void historyTab(App& a) {
    if (a.hist.empty()) { ImGui::TextDisabled("history fills while the receiver runs (4 samples per second, last 15 minutes)"); return; }
    const double nowT = glfwGetTime();
    {
        static const int wins[] = {30, 60, 300, 900};
        static const char* names[] = {"30 s", "1 min", "5 min", "15 min"};
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("window");
        for (int i = 0; i < 4; i++) { ImGui::SameLine(0, 6 * gUi); if (pillButton(names[i], a.histWindow == wins[i])) a.histWindow = wins[i]; }
        ImGui::SameLine(0, 14 * gUi);
        ImGui::TextDisabled("gaps in a line mean the receiver was not locked; CFO and SRO are on Receiver > Sync");
    }
    std::vector<float> xs(a.hist.size());
    for (size_t i = 0; i < xs.size(); i++) xs[i] = (float)(a.hist[i].t - nowT);
    auto series = [&](const char* id, const char* ylab, std::initializer_list<std::pair<const char*, float App::HistSample::*>> ser, double ymin, double ymax, bool fixed, bool last) {
        if (!ImPlot::BeginPlot(id, ImVec2(0, 0), ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) return;
        ImPlot::SetupAxes(last ? "seconds ago" : nullptr, nullptr, last ? 0 : ImPlotAxisFlags_NoTickLabels, fixed ? 0 : ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisLimits(ImAxis_X1, -a.histWindow, 0, ImPlotCond_Always);
        if (fixed) ImPlot::SetupAxisLimits(ImAxis_Y1, ymin, ymax, ImPlotCond_Once);
        int k = 0;
        static const ImVec4 cols[3] = {ImVec4(0.45f, 0.75f, 1, 1), ImVec4(0.95f, 0.7f, 0.2f, 1), ImVec4(0.4f, 0.85f, 0.5f, 1)};
        for (auto& s : ser) {
            std::vector<float> ys(a.hist.size());
            for (size_t i = 0; i < ys.size(); i++) ys[i] = a.hist[i].*(s.second);
            ImPlotSpec sp; sp.LineColor = cols[k++ % 3]; sp.LineWeight = 1.6f;
            ImPlot::PlotLine(s.first, xs.data(), ys.data(), (int)xs.size(), sp);
        }
        {   // caption inside the plot instead of a rotated axis label (the plots are short)
            const ImPlotRect lim = ImPlot::GetPlotLimits();
            const float tw = ImGui::CalcTextSize(ylab).x;
            ImPlot::PlotText(ylab, lim.X.Min, lim.Y.Max, ImVec2(tw * 0.5f + 8, 10));
        }
        ImPlot::EndPlot();
    };
    if (ImPlot::BeginSubplots("##hist", 4, 1, ImVec2(-1, -1), ImPlotSubplotFlags_LinkAllX | ImPlotSubplotFlags_NoTitle | ImPlotSubplotFlags_NoLegend | ImPlotSubplotFlags_NoMenus)) {
        series("quality (%)##h1", "quality %", {{"quality", &App::HistSample::quality}}, 0, 100, true, false);
        series("SNR / MER (dB)##h2", "SNR / MER dB", {{"data SNR", &App::HistSample::snr}, {"MER", &App::HistSample::mer}}, 0, 0, false, false);
        series("FEC block loss (%)##h3", "lost blocks %", {{"lost blocks", &App::HistSample::loss}}, 0, 100, true, false);
        series("ADC level (dBFS)##h4", "ADC dBFS", {{"rms", &App::HistSample::level}}, -60, 0, true, true);
        ImPlot::EndSubplots();
    }
}

static void frameMapTab(App& a) {
    if (a.rx.standard == 1) {
        const RxTelemetry& rx = a.rx;
        ImGui::TextDisabled("A DVB-T frame has 68 OFDM symbols; four frames form a superframe. Forward error correction here is a continuous stream (Viterbi + Reed-Solomon), so there is no block map as in DVB-T2.");
        if (rx.dvbt.tpsOk) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 o = ImGui::GetCursorScreenPos();
            const float W = ImGui::GetContentRegionAvail().x - 10, H = 22;
            for (int f = 0; f < 4; f++) {
                const float x0 = o.x + f * W / 4;
                dl->AddRectFilled(ImVec2(x0, o.y + f * 0), ImVec2(x0 + W / 4 - 3, o.y + H), f == rx.dvbt.frameIdx ? IM_COL32(40, 120, 70, 255) : IM_COL32(48, 50, 52, 255), 3.f);
                char t[24]; snprintf(t, sizeof t, "frame %d", f + 1);
                dl->AddText(ImVec2(x0 + 8, o.y + 4), IM_COL32(230, 235, 240, 255), t);
                if (f == rx.dvbt.frameIdx) dl->AddRectFilled(ImVec2(x0, o.y + H - 4), ImVec2(x0 + (W / 4 - 3) * (rx.dvbt.symbolIdx + 1) / 68.f, o.y + H), IM_COL32(110, 220, 140, 255), 3.f);
            }
            ImGui::Dummy(ImVec2(W, H + 8));
        }
        return;
    }
    const RxTelemetry& rx = a.rx;
    if (!rx.l1preOk) { ImGui::TextDisabled("the frame structure appears once L1-pre has been decoded"); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const L1Pre& p = rx.l1pre;
    static const int nP2tab[6] = {8, 2, 4, 16, 1, 1};
    const int fftCode = p.s2 >> 1;
    const int nP2 = fftCode >= 0 && fftCode < 6 ? nP2tab[fftCode] : 1;
    const int nData = p.numDataSyms;
    ImGui::TextDisabled("T2 frame: P1 + %d P2 symbol%s + %d data symbols (%s, GI %s)  -  %.1f ms", nP2, nP2 == 1 ? "" : "s", nData, fftModeFromSize(rx.fftN) ? fftModeFromSize(rx.fftN)->name : "?", guardName(rx.giIdx), rx.frameMs);
    ImVec2 o = ImGui::GetCursorScreenPos();
    const float W = ImGui::GetContentRegionAvail().x - 10, H = 34;
    const double symUnits = 1.0;             // widths in OFDM symbols; P1 is about half a long symbol
    const double p1u = 0.5, total = p1u + nP2 + nData;
    float x = o.x;
    auto seg = [&](double units, ImU32 col, const char* label) {
        const float w = (float)(units / total * W);
        dl->AddRectFilled(ImVec2(x, o.y), ImVec2(x + w - 1, o.y + H), col, 0.f);
        if (w > 28) { ImVec2 ts = ImGui::CalcTextSize(label); dl->AddText(ImVec2(x + (w - ts.x) / 2, o.y + (H - ts.y) / 2), IM_COL32(15, 18, 22, 255), label); }
        x += w;
    };
    (void)symUnits;
    seg(p1u, IM_COL32(240, 180, 60, 255), "P1");
    seg(nP2, IM_COL32(230, 120, 190, 255), "P2");
    seg(nData, IM_COL32(90, 200, 120, 255), "data symbols (PLP cells, pilots, edge pilots)");
    ImGui::Dummy(ImVec2(W, H + 6));
    if (rx.plpValid) ImGui::TextDisabled("PLP %d: %d FEC blocks per frame (%s, rate index %d%s), time interleaver %d", rx.plpId, rx.plpBlocks, rx.plpFec.mod == 0 ? "QPSK" : rx.plpFec.mod == 1 ? "16-QAM" : rx.plpFec.mod == 2 ? "64-QAM" : "256-QAM", rx.plpFec.rate, rx.plpFec.rotation ? ", rotated" : "", p.numDataSyms > 0 ? 0 : 0);
    ImGui::Separator();
    ImGui::TextDisabled("FEC block map: one row per decoded frame (newest at the bottom), one cell per FEC block. Green = decoded, red = failed.");
    const auto& bm = rx.blockMap;
    if (bm.empty()) { ImGui::TextDisabled("no decoded frames yet"); return; }
    size_t nb = 0;
    for (auto& f : bm) nb = std::max(nb, f.size());
    ImVec2 g = ImGui::GetCursorScreenPos();
    const float availW = ImGui::GetContentRegionAvail().x - 80, availH = ImGui::GetContentRegionAvail().y - 4;
    const float cw = nb ? std::max(1.f, availW / (float)nb) : 1.f;
    const float ch = std::max(2.f, std::min(10.f, availH / (float)bm.size()));
    for (size_t r = 0; r < bm.size(); r++) {
        const float y = g.y + r * ch;
        int ok = 0;
        for (size_t b = 0; b < bm[r].size(); b++) {
            ok += bm[r][b];
            dl->AddRectFilled(ImVec2(g.x + b * cw, y), ImVec2(g.x + (b + 1) * cw, y + ch - 1), bm[r][b] ? IM_COL32(60, 170, 90, 255) : IM_COL32(220, 60, 55, 255));
        }
        const float pct = bm[r].empty() ? 0.f : 100.f * ok / (float)bm[r].size();
        char t[16]; snprintf(t, sizeof t, "%.0f%%", pct);
        if (ch >= 8) dl->AddText(ImVec2(g.x + nb * cw + 6, y - 2), pct < 99.5f ? IM_COL32(240, 140, 120, 255) : IM_COL32(140, 150, 160, 255), t);
    }
    ImGui::Dummy(ImVec2(availW + 70, ch * bm.size()));
}

static ImU32 ttxColour(int c, float alpha = 1.f) {
    static const ImVec4 col[8] = {ImVec4(0, 0, 0, 1), ImVec4(1, 0.1f, 0.1f, 1), ImVec4(0.1f, 1, 0.1f, 1), ImVec4(1, 1, 0.1f, 1),
                                  ImVec4(0.2f, 0.3f, 1, 1), ImVec4(1, 0.2f, 1, 1), ImVec4(0.1f, 1, 1, 1), ImVec4(1, 1, 1, 1)};
    ImVec4 v = col[c & 7]; v.w = alpha;
    return ImGui::ColorConvertFloat4ToU32(v);
}

static void teletextTab(App& a) {
    TeletextDecoder& tx = a.engine.teletext();
    const Player& pl = a.engine.player();
    if (tx.pid() < 0) {
        ImGui::TextDisabled(pl.selected() < 0 ? "Play a service (Player tab) - teletext is read from the playing service." : "The playing service has no teletext stream.");
        return;
    }
    ImGui::SetNextItemWidth(80 * gUi);
    ImGui::InputInt("page", &a.ttxPage, 0, 0);
    a.ttxPage = std::max(100, std::min(899, a.ttxPage));
    ImGui::SameLine(); if (ImGui::SmallButton("<")) a.ttxPage = std::max(100, a.ttxPage - 1);
    ImGui::SameLine(); if (ImGui::SmallButton(">")) a.ttxPage = std::min(899, a.ttxPage + 1);
    ImGui::SameLine(); if (ImGui::SmallButton("100")) a.ttxPage = 100;
    ImGui::SameLine();
    {
        auto pages = tx.pages();
        ImGui::TextDisabled("%zu pages received, %llu packets", pages.size(), (unsigned long long)tx.packets());
        if (!pages.empty()) {
            ImGui::SameLine();
            std::string l = "available: ";
            int n = 0;
            for (int p : pages) { if (n++ >= 14) { l += "..."; break; } l += std::to_string(p) + " "; }
            ImGui::TextDisabled("%s", l.c_str());
        }
    }
    TtxPage pg;
    if (!tx.page(a.ttxPage, pg)) { ImGui::TextDisabled("page %d has not been received yet (pages repeat every few seconds)", a.ttxPage); return; }
    TtxGrid grid;
    ttxRender(pg, grid);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float cw = std::max(6.f, std::min(avail.x / 40.f, avail.y / 25.f * 0.55f)), ch = cw / 0.55f;
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + cw * 40, o.y + ch * 25), IM_COL32(0, 0, 0, 255));
    const bool flashOn = std::fmod(ImGui::GetTime(), 1.0) < 0.6;
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 40; c++) {
            const TtxCell& cell = grid[r][c];
            const ImVec2 p0(o.x + c * cw, o.y + r * ch), p1(p0.x + cw, p0.y + ch);
            if (cell.bg) dl->AddRectFilled(p0, p1, ttxColour(cell.bg));
            if (cell.flash && !flashOn) continue;
            if (cell.mosaic) {
                const float gx = cell.separated ? 1.f : 0.f;
                for (int k = 0; k < 6; k++) {
                    if (!(cell.sextants & (1 << k))) continue;
                    const int cx = k & 1, cy = k >> 1;
                    dl->AddRectFilled(ImVec2(p0.x + cx * cw / 2 + gx, p0.y + cy * ch / 3 + gx), ImVec2(p0.x + (cx + 1) * cw / 2 - gx, p0.y + (cy + 1) * ch / 3 - gx), ttxColour(cell.fg));
                }
            } else if (cell.ch > ' ') {
                char buf[5] = {0};
                uint32_t u = cell.ch;
                if (u < 0x80) buf[0] = (char)u;
                else if (u < 0x800) { buf[0] = (char)(0xC0 | (u >> 6)); buf[1] = (char)(0x80 | (u & 0x3F)); }
                else { buf[0] = (char)(0xE0 | (u >> 12)); buf[1] = (char)(0x80 | ((u >> 6) & 0x3F)); buf[2] = (char)(0x80 | (u & 0x3F)); }
                ImFont* f = a.mono;
                const float fs = cell.dh ? ch * 2.0f * 0.8f : ch * 0.8f;
                if (cell.dh == 1) { dl->PushClipRect(p0, p1, true); dl->AddText(f, fs, ImVec2(p0.x, p0.y), ttxColour(cell.fg), buf); dl->PopClipRect(); }
                else if (cell.dh == 2) { dl->PushClipRect(p0, p1, true); dl->AddText(f, fs, ImVec2(p0.x, p0.y - ch), ttxColour(cell.fg), buf); dl->PopClipRect(); }
                else dl->AddText(f, fs, ImVec2(p0.x, p0.y), ttxColour(cell.fg), buf);
            }
        }
    char title[64];
    snprintf(title, sizeof title, "P%d%s", pg.number, pg.subtitle ? "  (subtitle page)" : "");
    dl->AddText(ImVec2(o.x + 2, o.y + 1), IM_COL32(255, 255, 255, 255), title);
    ImGui::Dummy(ImVec2(cw * 40, ch * 25));
}

// ---- programme guide helpers
static int64_t utcNowOf(const App& a) { return a.ts.utcNow ? a.ts.utcNow : (int64_t)time(nullptr); }
static std::string fmtLocal(int64_t utc, const char* f) {
    time_t t = (time_t)utc; struct tm m; dect2::localTime(t, &m);
    char b[48]; strftime(b, sizeof b, f, &m);
    return b;
}
static const EpgEvent* epgCurrent(const App& a, int sid, const EpgEvent** next = nullptr) {
    auto it = a.epg.find(sid);
    if (it == a.epg.end()) return nullptr;
    const int64_t now = utcNowOf(a);
    const EpgEvent* cur = nullptr;
    if (next) *next = nullptr;
    for (size_t i = 0; i < it->second.size(); i++) {
        const EpgEvent& e = it->second[i];
        if (e.start <= now && now < e.end()) { cur = &e; if (next && i + 1 < it->second.size()) *next = &it->second[i + 1]; break; }
        if (e.start > now) { if (next && !*next) *next = &e; break; }
    }
    return cur;
}
static const char* genreName(int g) {
    static const char* n[] = {"", "Movie / drama", "News / current affairs", "Show / game show", "Sports", "Children's / youth", "Music / ballet / dance", "Arts / culture", "Social / political / economics", "Education / science", "Leisure / hobbies"};
    return g >= 1 && g <= 10 ? n[g] : "";
}

static void guideTab(App& a) {
    const int64_t now = utcNowOf(a);
    if (a.ts.services.empty()) { ImGui::TextDisabled("waiting for the service list..."); return; }
    size_t total = 0;
    for (auto& kv : a.epg) total += kv.second.size();
    if (total == 0) {
        ImGui::TextWrapped("No programme information received yet. Broadcasters send it in the EIT tables, which repeat every few seconds (now/next) to a few minutes (schedule). "
                           "Some multiplexes send none at all.");
        ImGui::TextDisabled("clock: %s%s", fmtLocal(now, "%a %d %b %H:%M").c_str(), a.ts.utcNow ? " (from the broadcast time signal)" : " (this computer's clock)");
        return;
    }
    if (a.guideSid < 0 || !a.epg.count(a.guideSid)) { a.guideSid = a.engine.player().selected() >= 0 && a.epg.count(a.engine.player().selected()) ? a.engine.player().selected() : a.epg.begin()->first; }
    ImGui::BeginChild("gsv", ImVec2(260 * gUi, 0), ImGuiChildFlags_Borders);
    for (auto& sv : a.ts.services) {
        auto it = a.epg.find(sv.id);
        if (it == a.epg.end()) continue;
        const EpgEvent* nx = nullptr;
        const EpgEvent* cur = epgCurrent(a, sv.id, &nx);
        ImGui::PushID(sv.id);
        if (ImGui::Selectable(sv.name.empty() ? std::to_string(sv.id).c_str() : sv.name.c_str(), a.guideSid == sv.id)) { a.guideSid = sv.id; a.guideEvent = -1; }
        ImGui::TextDisabled("  %s", cur ? (fmtLocal(cur->start, "%H:%M") + " " + cur->title).c_str() : "-");
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("gev", ImVec2(0, 0));
    const auto& evs = a.epg[a.guideSid];
    ImGui::TextDisabled("%zu events   local time %s", evs.size(), fmtLocal(now, "%a %d %b %H:%M").c_str());
    const float detailH = 150;
    ImGui::BeginChild("evlist", ImVec2(0, -detailH), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("evt", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("length", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("programme");
        std::string lastDay;
        for (size_t i = 0; i < evs.size(); i++) {
            const EpgEvent& e = evs[i];
            if (e.end() < now - 1800) continue; // hide what ended more than half an hour ago
            const bool cur = e.start <= now && now < e.end();
            const std::string day = fmtLocal(e.start, "%a %d %b");
            if (day != lastDay) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "%s", day.c_str()); lastDay = day; }
            ImGui::TableNextRow();
            if (cur) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(40, 110, 60, 110));
            ImGui::TableNextColumn();
            char lbl[64]; snprintf(lbl, sizeof lbl, "%s - %s##%d", fmtLocal(e.start, "%H:%M").c_str(), fmtLocal(e.end(), "%H:%M").c_str(), e.eventId);
            if (ImGui::Selectable(lbl, a.guideEvent == e.eventId, ImGuiSelectableFlags_SpanAllColumns)) a.guideEvent = e.eventId;
            ImGui::TableNextColumn(); ImGui::Text("%d min", (e.duration + 30) / 60);
            ImGui::TableNextColumn();
            if (cur) { const float f = (float)(now - e.start) / (float)e.duration; ImGui::Text("%s", e.title.c_str()); ImGui::SameLine(); ImGui::TextDisabled("  now, %.0f%%", f * 100); }
            else ImGui::TextUnformatted(e.title.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::BeginChild("evdet", ImVec2(0, 0), ImGuiChildFlags_Borders);
    const EpgEvent* sel = nullptr;
    for (auto& e : evs) if (e.eventId == a.guideEvent) sel = &e;
    if (!sel) sel = epgCurrent(a, a.guideSid);
    if (sel) {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.4f, 1), "%s", sel->title.c_str());
        ImGui::TextDisabled("%s - %s  (%d min)%s%s", fmtLocal(sel->start, "%a %H:%M").c_str(), fmtLocal(sel->end(), "%H:%M").c_str(), (sel->duration + 30) / 60, genreName(sel->genre)[0] ? "   " : "", genreName(sel->genre));
        if (!sel->text.empty()) ImGui::TextWrapped("%s", sel->text.c_str());
        if (!sel->extended.empty()) ImGui::TextWrapped("%s", sel->extended.c_str());
    } else ImGui::TextDisabled("select a programme");
    ImGui::EndChild();
    ImGui::EndChild();
}

static void signallingTab(App& a) {
    if (a.rx.standard == 1) {
        const RxTelemetry& rx = a.rx;
        if (!rx.dvbt.tpsOk) { ImGui::TextDisabled("TPS (transmission parameter signalling) has not been decoded yet"); return; }
        static const char* modes[] = {"2K", "8K"}; static const char* hier[] = {"non-hierarchical", "hierarchical, alpha = 1", "hierarchical, alpha = 2", "hierarchical, alpha = 4"};
        dvbt::Params q; q.mode = rx.dvbt.mode; q.guard = rx.dvbt.guard; q.mod = rx.dvbt.mod; q.hier = rx.dvbt.hier; q.crHp = rx.dvbt.crHp; q.crLp = rx.dvbt.crLp;
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "DVB-T transmission parameters (TPS, EN 300 744)");
        if (ImGui::BeginTable("tps", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 200);
            auto row = [&](const char* k, const char* fmt, auto... args) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k); ImGui::TableNextColumn(); char b[160]; snprintf(b, sizeof b, fmt, args...); ImGui::TextUnformatted(b); };
            row("FFT mode", "%s (%d carriers, %d data carriers per symbol)", modes[rx.dvbt.mode & 1], dvbt::carriersK(rx.dvbt.mode), dvbt::dataCarriers(rx.dvbt.mode));
            row("Guard interval", "%s of the useful symbol (%d samples)", dvbt::guardName(rx.dvbt.guard), dvbt::guardSamples(rx.dvbt.mode, rx.dvbt.guard));
            row("Constellation", "%s", dvbt::modName(rx.dvbt.mod));
            row("Hierarchy", "%s", hier[rx.dvbt.hier & 3]);
            row("Code rate (high priority)", "%s", dvbt::rateName(rx.dvbt.crHp));
            row("Code rate (low priority)", "%s", dvbt::rateName(rx.dvbt.crLp));
            row("Cell identifier", "%d", rx.dvbt.cellId);
            row("Frame in superframe", "%d of 4   (symbol %d of 68)", rx.dvbt.frameIdx + 1, rx.dvbt.symbolIdx + 1);
            row("Net bit rate", "%.2f Mbit/s", dvbt::netBitrate(q, rx.nativeRate) / 1e6);
            row("TPS block age", "%.2f s", rx.dvbt.secSinceTps);
            ImGui::EndTable();
        }
        if (rx.dvbt.hier) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.2f, 1), "! hierarchical modulation is announced: only the high-priority stream is decoded correctly by this receiver");
        return;
    }
    const RxTelemetry& rx = a.rx;
    ImGui::PushFont(a.mono, 0);
    auto row = [](const char* k, const char* fmt, ...) {
        char b[160];
        va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
        ImGui::TextDisabled("%-22s", k); ImGui::SameLine(); ImGui::TextUnformatted(b);
    };
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "P1 preamble");
    if (rx.p1.valid) {
        const FftMode* fm = fftModeFromS2(rx.p1.s2field1);
        row("S1", "%d  %s", rx.p1.s1, s1Name(rx.p1.s1));
        row("S2 field 1", "%d  FFT %s", rx.p1.s2field1, fm ? fm->name : "?");
        row("S2 field 2", "%d  %s", rx.p1.mixed, rx.p1.mixed ? "mixed (T2 + FEF/other frames)" : "all frames same preamble type");
        row("sequence correlation", "%.2f", rx.p1.conf);
        row("C-A-B correlation", "%.2f", rx.p1.metric);
        row("P1 carrier CFO", "%+.1f Hz", rx.p1.cfoHz);
        row("P1 detections", "%llu", (unsigned long long)rx.p1Count);
        row("last P1", "%.2f s ago", rx.secSinceP1);
    } else ImGui::TextDisabled("no P1 decoded yet");
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "OFDM / frame (blind, before L1-pre)");
    if (rx.state >= 1) {
        row("FFT size", "%d", rx.fftN);
        row("carriers (normal)", "%d", rx.carriers);
        if (rx.giIdx >= 0) {
            row("guard interval", "%s  (%d samples)  margin %.2fx", guardName(rx.giIdx), rx.guard, rx.giMargin);
            row("symbol duration", "%.1f us", (rx.fftN + rx.guard) / rx.nativeRate * 1e6);
        }
        if (rx.frameMs > 0) {
            row("frame length", "%.2f ms  =  P1 + %d symbols", rx.frameMs, rx.symbolsPerFrame);
            row("sample clock offset", "%+.1f ppm", rx.sroPpm);
        }
        row("CFO (tracked)", "%+.1f Hz", rx.cfoHz);
        row("CP correlation", "%.3f  (~%.1f dB SNR)", rx.cpCorr, rx.cpSnrDb);
        row("symbols processed", "%llu", (unsigned long long)rx.symbols);
    }
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "L1-pre   (decoded %llu, failed %llu, LDPC iterations %d)", (unsigned long long)rx.l1preGood, (unsigned long long)rx.l1preBad, rx.l1Iters);
    if (rx.l1preGood > 0) {
        const L1Pre& p = rx.l1pre;
        static const char* paprN[] = {"off", "ACE", "TR", "ACE + TR"};
        static const char* modN[] = {"BPSK", "QPSK", "16-QAM", "64-QAM"};
        static const char* verN[] = {"1.1.1", "1.2.1", "1.3.1"};
        row("TYPE", "%d  %s", p.type, p.type == 0 ? "TS" : p.type == 1 ? "GSE/GS" : "TS + GS");
        row("carrier mode", "%s", p.bwtExt ? "extended" : "normal");
        row("S1 / S2", "%d / %d (FFT %s%s)", p.s1, p.s2, fftModeFromS2(p.s2 >> 1) ? fftModeFromS2(p.s2 >> 1)->name : "?", (p.s2 & 1) ? ", FEF present" : "");
        row("guard interval", "%s", guardName(p.guardInterval));
        row("PAPR", "%s", p.papr < 4 ? paprN[p.papr] : "reserved");
        row("L1-post", "%s, rate 1/2, %s FEC, %d cells, %d info bits%s", p.l1Mod < 4 ? modN[p.l1Mod] : "?", p.l1Fec == 0 ? "16K" : "64K", p.postSize, p.postInfoSize, p.postScrambled ? ", scrambled" : "");
        row("pilot pattern", "PP%d", p.pilotPattern + 1);
        row("cell / network / system", "%d / 0x%04X / 0x%04X", p.cellId, p.networkId, p.systemId);
        row("T2 frames per super-frame", "%d", p.numFrames);
        row("data symbols per frame", "%d", p.numDataSyms);
        row("T2 version", "%s%s", p.version < 3 ? verN[p.version] : "?", p.lite ? "  (T2-Lite)" : "");
        row("RF channels", "%d (current %d)  regen %d  tx-id %d", p.numRf, p.curRf, p.regen, p.txIdAvail);
    } else ImGui::TextDisabled("not decoded yet");
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "L1-post   (decoded %llu, failed %llu)", (unsigned long long)rx.l1postGood, (unsigned long long)rx.l1postBad);
    if (rx.l1postGood > 0) {
        const L1Post& q = rx.l1post;
        static const char* cod[] = {"1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "1/3", "2/5"};
        static const char* pmod[] = {"QPSK", "16-QAM", "64-QAM", "256-QAM"};
        static const char* ptype[] = {"common", "data type 1", "data type 2"};
        row("sub-slices / PLPs / aux", "%d / %d / %d", q.subSlices, q.numPlp, q.numAux);
        for (auto& rf : q.rf) row("RF frequency", "%.3f MHz  (index %d)", rf.freq / 1e6, rf.idx);
        row("frame index", "%d   change counter %d", q.frameIdx, q.changeCounter);
        for (size_t i = 0; i < q.plps.size(); i++) {
            const L1PlpConf& c = q.plps[i];
            char hdr[32]; snprintf(hdr, sizeof hdr, "PLP %d", c.id);
            row(hdr, "%s, %s, rate %s, %s, %s FEC, rotation %s", c.type < 3 ? ptype[c.type] : "?", c.mod < 4 ? pmod[c.mod] : "?", c.cod < 8 ? cod[c.cod] : "?",
                c.payloadType == 3 ? "TS" : c.payloadType == 0 ? "GFPS" : c.payloadType == 1 ? "GCS" : "GSE", c.fecType == 1 ? "64K" : "16K", c.rotation ? "on" : "off");
            row("", "max %d FEC blocks, TI %d (%s), group %d, interval %d, mode %d", c.numBlocksMax, c.timeIlLength, c.timeIlType ? "multi-frame" : "one frame", c.groupId, c.frameInterval, c.plpMode);
            if (i < q.dyn.size()) row("", "start %d, %d blocks in this frame", q.dyn[i].start, q.dyn[i].numBlocks);
        }
    } else ImGui::TextDisabled("%s", rx.l1preGood ? "not decoded (L1-pre must be valid first)" : "not decoded yet");
    ImGui::PopFont();
}


// Decoded-cell views of the data constellation: density heat map and per-point clusters
static void constDensityPlot(App& a, ImVec2 sz) {
    const App::ConstStats& c = a.cst;
    if (ImPlot::BeginPlot("##c2d", sz, ImPlotFlags_NoLegend | ImPlotFlags_NoTitle | ImPlotFlags_Equal | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_NoTickLabels);
        ImPlot::SetupAxisLimits(ImAxis_X1, -1.4, 1.4, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -1.4, 1.4, ImPlotCond_Always);
        const int G = App::ConstStats::G;
        if ((int)c.grid.size() == G * G) {
            std::vector<float> v(c.grid.size());
            float mx = 0;
            for (size_t i = 0; i < v.size(); i++) { v[i] = std::sqrt(c.grid[i]); mx = std::max(mx, v[i]); }   // square root: faint areas stay visible
            static ImPlotColormap cm = -1;
            if (cm < 0) {   // black -> accent -> white, instead of the rainbow-like "hot" map
                const ImVec4 cols[4] = {ImVec4(0.02f, 0.03f, 0.05f, 1), ImVec4(0.10f, 0.28f, 0.45f, 1), pal::accent(), ImVec4(0.95f, 0.98f, 1.f, 1)};
                cm = ImPlot::AddColormap("onair", cols, 4);
            }
            ImPlot::PushColormap(cm);
            ImPlot::PlotHeatmap("density", v.data(), G, G, 0, std::max(1.f, mx * 0.85f), nullptr, ImPlotPoint(-1.4, -1.4), ImPlotPoint(1.4, 1.4));
            ImPlot::PopColormap();
        }
        const int M = 2 * (a.rx.plpFec.mod + 1);
        std::vector<cf32> grid;
        for (unsigned l = 0; l < (1u << M); l++) grid.push_back(qamPoint(a.rx.plpFec.mod, false, l));
        ImPlotSpec gs; gs.Marker = ImPlotMarker_Cross; gs.MarkerSize = 4.f; gs.Stride = sizeof(cf32);
        gs.MarkerFillColor = gs.MarkerLineColor = gs.LineColor = ImVec4(1, 1, 1, 0.55f);
        const float* g = reinterpret_cast<const float*>(grid.data());
        ImPlot::PlotScatter("ideal", g, g + 1, (int)grid.size(), gs);
        ImPlot::EndPlot();
    }
}

static void constClusterPlot(App& a, ImVec2 sz) {
    const App::ConstStats& c = a.cst;
    const int mod = a.rx.plpFec.mod;
    static const float kDmin[4] = {1.4142f, 0.6325f, 0.3086f, 0.1534f};
    const float half = 0.5f * kDmin[mod];
    if (ImPlot::BeginPlot("##c2k", sz, ImPlotFlags_NoLegend | ImPlotFlags_NoTitle | ImPlotFlags_Equal | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_NoTickLabels);
        ImPlot::SetupAxisLimits(ImAxis_X1, -1.4, 1.4, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -1.4, 1.4, ImPlotCond_Always);
        int hover = -1;
        double meanRatio = 0; int nr = 0;
        for (auto& p : c.pts) if (p.n >= 4) { meanRatio += std::sqrt(p.e2 / (2 * p.n)); nr++; }
        meanRatio = nr ? meanRatio / nr : 1;
        const ImPlotPoint mp = ImPlot::GetPlotMousePos();
        double bestD = 1e9;
        for (size_t l = 0; l < c.pts.size(); l++) {
            const App::ConstStats::Pt& p = c.pts[l];
            const cf32 tx = qamPoint(mod, false, (unsigned)l);
            const double d = std::hypot(mp.x - tx.real(), mp.y - tx.imag());
            if (ImPlot::IsPlotHovered() && d < bestD && d < half * 1.2) { bestD = d; hover = (int)l; }
            if (p.n < 4) continue;
            const double sg = std::sqrt(p.e2 / (2 * p.n));        // standard deviation per axis
            // colour relative to the average ring: green = tighter than average, red = looser (a coded signal is decoded at
            // noise levels where all rings overlap, so an absolute scale would show everything red)
            const float t = (float)std::min(1.0, std::max(0.0, 0.5 + (sg / meanRatio - 1.0) * 4.0));
            const ImVec4 col(0.25f + 0.7f * t, 0.85f - 0.5f * t, 0.45f - 0.15f * t, 0.9f);
            const double cx = tx.real() + p.ei / p.n, cy = tx.imag() + p.eq / p.n;
            float xs[25], ys[25];
            for (int k = 0; k < 25; k++) { const double th = k * 2 * M_PI / 24; xs[k] = (float)(cx + sg * std::cos(th)); ys[k] = (float)(cy + sg * std::sin(th)); }
            ImPlotSpec ls; ls.LineColor = col; ls.LineWeight = (int)l == hover ? 2.5f : 1.3f;
            ImPlot::PlotLine("##ring", xs, ys, 25, ls);
            ImPlotSpec ds; ds.Marker = ImPlotMarker_Circle; ds.MarkerSize = 2.f; ds.MarkerFillColor = ds.MarkerLineColor = ds.LineColor = col;
            const float px = (float)cx, py = (float)cy;
            ImPlot::PlotScatter("##centre", &px, &py, 1, ds);
        }
        {
            const int M = 2 * (mod + 1);
            std::vector<cf32> grid;
            for (unsigned l = 0; l < (1u << M); l++) grid.push_back(qamPoint(mod, false, l));
            ImPlotSpec gs; gs.Marker = ImPlotMarker_Cross; gs.MarkerSize = 4.f; gs.Stride = sizeof(cf32);
            gs.MarkerFillColor = gs.MarkerLineColor = gs.LineColor = ImVec4(1, 1, 1, 0.8f);
            const float* g = reinterpret_cast<const float*>(grid.data());
            ImPlot::PlotScatter("ideal", g, g + 1, (int)grid.size(), gs);
        }
        if (hover >= 0 && c.pts[hover].n >= 4) {
            const App::ConstStats::Pt& p = c.pts[hover];
            const int M = 2 * (mod + 1);
            char bits[16];
            for (int b = 0; b < M; b++) bits[b] = ((hover >> (M - 1 - b)) & 1) ? '1' : '0';
            bits[M] = 0;
            ImGui::BeginTooltip();
            ImGui::Text("point %s", bits);
            ImGui::Text("sigma %.3f per axis (%.0f%% of the half-distance to a neighbour)", std::sqrt(p.e2 / (2 * p.n)), 100 * std::sqrt(p.e2 / (2 * p.n)) / half);
            ImGui::Text("offset %+.3f %+.3f", p.ei / p.n, p.eq / p.n);
            ImGui::Text("EVM %.1f dB", 10 * std::log10(std::max(1e-9, p.e2 / p.n)));
            ImGui::EndTooltip();
        }
        ImPlot::EndPlot();
    }
}

// ATSC: histogram of the equalised symbols (eight peaks), equaliser response, strip chart of the levels and a text summary
static void atscPanels(App& a) {
    const AtscTelemetry& at = a.rx.atsc;
    const float availW = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
    const float side = std::max(90.f, std::min(availH - 26.f, availW / 4.f - 16.f));
    const float gap = std::max(6.f, (availW - 4 * side) / 5.f);
    const ImVec2 sz(side, side);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("Equalised levels (%zu symbols)", at.levels.size());
    if (ImPlot::BeginPlot("##ah", sz, ImPlotFlags_NoLegend | ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_NoTickLabels);
        ImPlot::SetupAxisLimits(ImAxis_X1, -9, 9, ImPlotCond_Always);
        constexpr int NB = 72;
        float cnt[NB] = {}, xs[NB];
        for (int i = 0; i < NB; i++) xs[i] = -9.f + (i + 0.5f) * 18.f / NB;
        float mx = 1;
        for (float v : at.levels) { const int k = (int)((v + 9.f) / 18.f * NB); if (k >= 0 && k < NB) mx = std::max(mx, ++cnt[k]); }
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, mx * 1.15, ImPlotCond_Always);
        ImPlotSpec sp; sp.FillColor = pal::accent(0.85f); sp.LineColor = pal::accent();
        ImPlot::PlotBars("levels", xs, cnt, NB, 18.0 / NB * 0.9, sp);
        const double lv[8] = {-7, -5, -3, -1, 1, 3, 5, 7};
        ImPlotSpec gs; gs.LineColor = ImVec4(1, 1, 1, 0.35f);
        ImPlot::PlotInfLines("ideal", lv, 8, gs);
        ImPlot::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("Equaliser response (%zu taps)", at.eqTaps.size());
    if (ImPlot::BeginPlot("##ae", sz, ImPlotFlags_NoLegend | ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxes("symbols", nullptr, 0, ImPlotAxisFlags_AutoFit);
        if (!at.eqTaps.empty()) {
            std::vector<float> xs(at.eqTaps.size());
            for (size_t i = 0; i < xs.size(); i++) xs[i] = (float)(at.eqCursor - (int)i);   // symbols relative to the main tap: positive = echoes of later symbols
            ImPlot::SetupAxisLimits(ImAxis_X1, -(double)(at.eqTaps.size() - 1 - at.eqCursor) - 1, (double)at.eqCursor + 1, ImPlotCond_Always);
            ImPlotSpec sp; sp.LineColor = pal::accent();
            ImPlot::PlotLine("taps", xs.data(), at.eqTaps.data(), (int)xs.size(), sp);
        }
        ImPlot::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("Symbol levels in sequence");
    if (ImPlot::BeginPlot("##as", sz, ImPlotFlags_NoLegend | ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_NoTickLabels);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -9, 9, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, std::max<size_t>(2, at.levels.size()), ImPlotCond_Always);
        if (!at.levels.empty()) {
            ImPlotSpec sp; sp.Marker = ImPlotMarker_Circle; sp.MarkerSize = 1.3f;
            sp.MarkerFillColor = sp.MarkerLineColor = sp.LineColor = pal::accent(0.6f);
            ImPlot::PlotScatter("l", at.levels.data(), (int)at.levels.size(), 1.0, 0.0, sp);
        }
        const double lv[8] = {-7, -5, -3, -1, 1, 3, 5, 7};
        ImPlotSpec gs; gs.LineColor = ImVec4(1, 1, 1, 0.25f); gs.Flags = ImPlotInfLinesFlags_Horizontal;
        ImPlot::PlotInfLines("ideal", lv, 8, gs);
        ImPlot::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("ATSC receiver");
    ImGui::BeginChild("atxt", sz);
    ImGui::PushFont(a.mono, 0);
    ImGui::Text("pilot       %s", at.pilot ? "locked" : "searching");
    ImGui::Text("segment     %s  sync %.2f", at.segSync ? "locked" : "searching", at.syncQuality);
    ImGui::Text("field       %s  (%d)", at.fieldSync ? "locked" : "searching", at.fieldParity);
    ImGui::Text("CFO %+.0f Hz  SRO %+.1f ppm", at.cfoHz, at.sroPpm);
    ImGui::Text("SNR %.1f dB  data %.1f dB", at.snrDb, at.dataSnrDb);
    ImGui::Text("fields %llu", (unsigned long long)at.fields);
    ImGui::Text("RS ok %llu fixed %llu", (unsigned long long)at.rsClean, (unsigned long long)at.rsCorrected);
    ImGui::Text("RS failed %llu (%.1f%%)", (unsigned long long)at.rsFailed, at.segErrorRate * 100);
    ImGui::Text("lock losses %llu", (unsigned long long)at.lockLosses);
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::EndGroup();
}

static void constellationsTab(App& a) {
    if (a.dabMode) { dabPanels(a); return; }
    if (a.rx.standard == 2) { atscPanels(a); return; }
    const RxTelemetry& rx = a.rx;
    const float availW = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
    const float side = std::max(90.f, std::min(availH - 26.f - ImGui::GetFrameHeight(), availW / 4.f - 16.f));
    const float gap = std::max(6.f, (availW - 4 * side) / 5.f);
    const ImVec2 sz(side, side);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap);
    char cap[96];
    auto caption = [&](const char* fmt, auto... args) { snprintf(cap, sizeof cap, fmt, args...); ImGui::TextDisabled("%s", cap); };
    ImGui::BeginGroup();
    if (rx.standard == 1) caption("TPS carriers, DBPSK (%zu cells)", rx.p1Const.size()); else caption("P1 carriers (%zu cells)", rx.p1Const.size());
    scatter("##c1", rx.p1Const, sz, 2.5, pal::accent(0.9f));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    if (rx.standard == 1) caption("Pilots, equalised (%zu cells)", rx.eqCells.size()); else caption("P2 cells, equalised (%zu cells)", rx.eqCells.size());
    scatter("##ceq", rx.eqCells, sz, 2.0, pal::accent(0.8f));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    if (!rx.plpConst.empty()) caption("PLP %d, decoded cells  MER %.1f dB", rx.plpId, rx.plpMerDb);
    else if (rx.standard == 1) caption("Data cells, equalised (%zu cells)", rx.eqData.size());
    else caption("Data cells, equalised (frame %llu)", (unsigned long long)rx.dataFrames);
    if (!rx.plpConst.empty()) {
        // cells of correctly decoded FEC blocks; points beyond the decision distance from the transmitted point are red
        std::vector<cf32> good, bad;
        for (size_t i = 0; i < rx.plpConst.size(); i++) (rx.plpConstErr.size() > i && rx.plpConstErr[i] ? bad : good).push_back(rx.plpConst[i]);
        const bool haveStats = a.cst.mod == rx.plpFec.mod && !a.cst.pts.empty();
        if (a.constView == 1 && haveStats) constDensityPlot(a, sz);
        else if (a.constView == 2 && haveStats) constClusterPlot(a, sz);
        else if (ImPlot::BeginPlot("##c2", sz, ImPlotFlags_NoLegend | ImPlotFlags_NoTitle | ImPlotFlags_Equal)) {
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_NoTickLabels);
            ImPlot::SetupAxisLimits(ImAxis_X1, -1.4, 1.4, ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -1.4, 1.4, ImPlotCond_Always);
            auto draw = [&](const char* id, const std::vector<cf32>& v, ImVec4 col) {
                if (v.empty()) return;
                ImPlotSpec sp; sp.Marker = ImPlotMarker_Circle; sp.MarkerSize = 1.3f; sp.Stride = sizeof(cf32);
                sp.MarkerFillColor = col; sp.MarkerLineColor = col; sp.LineColor = col;
                const float* d = reinterpret_cast<const float*>(v.data());
                ImPlot::PlotScatter(id, d, d + 1, (int)v.size(), sp);
            };
            draw("ok", good, pal::accent(0.60f));
            draw("err", bad, ImVec4(0.88f, 0.52f, 0.40f, 0.75f));
            {   // the transmitted (unrotated) constellation points, drawn on top like the reference decoders do
                const int M = 2 * (rx.plpFec.mod + 1);
                std::vector<cf32> grid;
                for (unsigned l = 0; l < (1u << M); l++) grid.push_back(qamPoint(rx.plpFec.mod, false, l));
                ImPlotSpec gs; gs.Marker = ImPlotMarker_Cross; gs.MarkerSize = 4.f; gs.Stride = sizeof(cf32);
                gs.MarkerFillColor = gs.MarkerLineColor = gs.LineColor = ImVec4(1, 1, 1, 0.85f);
                const float* g = reinterpret_cast<const float*>(grid.data());
                ImPlot::PlotScatter("ideal", g, g + 1, (int)grid.size(), gs);
            }
            ImPlot::EndPlot();
        }
        ImGui::PushID("cview");
        if (pillButton("cells", a.constView == 0, 8)) a.constView = 0;
        ImGui::SameLine(0, 4 * gUi);
        if (pillButton("density", a.constView == 1, 8)) a.constView = 1;
        ImGui::SameLine(0, 4 * gUi);
        if (pillButton("clusters", a.constView == 2, 8)) a.constView = 2;
        ImGui::PopID();
        if (a.constView == 2 && ImGui::IsItemHovered()) ImGui::SetTooltip("One ring per transmitted point: centre = average received position, radius = 1 sigma of the error.\nColour is relative to the average ring: green = tighter, red = looser.\nAt this MER the rings overlap their neighbours; the LDPC code corrects the resulting errors.");
    } else if (rx.standard == 1 && rx.dvbt.tpsOk) {
        if (ImPlot::BeginPlot("##c2", sz, ImPlotFlags_NoLegend | ImPlotFlags_NoTitle | ImPlotFlags_Equal)) {
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_NoTickLabels);
            ImPlot::SetupAxisLimits(ImAxis_X1, -1.5, 1.5, ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -1.5, 1.5, ImPlotCond_Always);
            if (!rx.eqData.empty()) {
                ImPlotSpec sp; sp.Marker = ImPlotMarker_Circle; sp.MarkerSize = 1.3f; sp.Stride = sizeof(cf32);
                const ImVec4 col(0.35f, 0.62f, 1.0f, 0.55f);
                sp.MarkerFillColor = col; sp.MarkerLineColor = col; sp.LineColor = col;
                const float* d = reinterpret_cast<const float*>(rx.eqData.data());
                ImPlot::PlotScatter("data", d, d + 1, (int)rx.eqData.size(), sp);
            }
            std::vector<dvbt::cf32> pts;
            dvbt::constellation(rx.dvbt.mod, 0, pts);
            ImPlotSpec gs; gs.Marker = ImPlotMarker_Cross; gs.MarkerSize = 4.f; gs.Stride = sizeof(dvbt::cf32);
            gs.MarkerFillColor = gs.MarkerLineColor = gs.LineColor = ImVec4(1, 1, 1, 0.85f);
            const float* g = reinterpret_cast<const float*>(pts.data());
            ImPlot::PlotScatter("ideal", g, g + 1, (int)pts.size(), gs);
            ImPlot::EndPlot();
        }
    } else
        scatter("##c2", rx.eqData, sz, 1.8, pal::accent(0.6f));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    caption("OFDM cells, raw (%zu cells)", rx.rawCells.size());
    scatter("##c3", rx.rawCells, sz, 3.0, pal::accent(0.7f));
    ImGui::EndGroup();
}

static void channelTab(App& a) {
    const RxTelemetry& rx = a.rx;
    if (!rx.chValid) { ImGui::TextDisabled("no channel estimate yet (needs a decoded P1 and a P2 symbol)"); return; }
    {
        const MultipathReport& mr = a.mpd.report();
        ImGui::TextColored(mr.level == MultipathLevel::None ? ImVec4(0.4f, 0.85f, 0.5f, 1) : mr.level == MultipathLevel::Mild ? ImVec4(0.95f, 0.8f, 0.3f, 1) : ImVec4(0.95f, 0.5f, 0.25f, 1),
                           "Multipath analysis: %s", mr.headline.c_str());
        for (auto& r : mr.reasons) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("- %s", r.c_str()); ImGui::PopTextWrapPos(); }
        ImGui::Separator();
    }
    const float h = ImGui::GetContentRegionAvail().y - 56;   // room for the two captions
    double fnMhz = rx.nativeRate / 1e6, binMhz = fnMhz / rx.fftN;
    std::vector<float> xs(rx.chMagDb.size());
    for (size_t i = 0; i < xs.size(); i++) xs[i] = (float)(a.freqMhz + ((double)(i * rx.chDecim) - (rx.chCarriers - 1) / 2.0) * binMhz);
    ImGui::TextDisabled("|H(f)| from P2 pilots, dB (%d carriers%s)", rx.chCarriers, rx.extCarriers ? ", extended" : "");
    if (ImPlot::BeginPlot("##chm", ImVec2(-1, std::max(60.f, h * 0.5f)), ImPlotFlags_NoLegend | ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes("frequency (MHz)", "dB", 0, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisFormat(ImAxis_X1, "%.2f");
        ImPlot::SetupAxisLimits(ImAxis_X1, xs.front(), xs.back(), ImPlotCond_Always);
        ImPlotSpec sp; sp.LineColor = pal::accent();
        ImPlot::PlotLine("mag", xs.data(), rx.chMagDb.data(), (int)xs.size(), sp);
        ImPlot::EndPlot();
    }
    ImGui::TextDisabled("phase of H(f), rad (slope = timing, curvature = echoes)");
    if (ImPlot::BeginPlot("##chp", ImVec2(-1, -1), ImPlotFlags_NoLegend | ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes("frequency (MHz)", "rad");
        ImPlot::SetupAxisFormat(ImAxis_X1, "%.2f");
        ImPlot::SetupAxisLimits(ImAxis_X1, xs.front(), xs.back(), ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -3.3, 3.3, ImPlotCond_Always);
        ImPlotSpec sp; sp.LineColor = ImVec4(0.95f, 0.7f, 0.2f, 1); sp.Marker = ImPlotMarker_Circle; sp.MarkerSize = 1.2f;
        sp.LineWeight = 0.0f;
        ImPlot::PlotScatter("ph", xs.data(), rx.chPhase.data(), (int)xs.size(), sp);
        ImPlot::EndPlot();
    }
}

static void impulseTab(App& a) {
    const RxTelemetry& rx = a.rx;
    if (!rx.chValid || rx.irDb.empty()) { ImGui::TextDisabled("no channel estimate yet"); return; }
    std::vector<float> xs(rx.irDb.size());
    double usPerSample = 1e6 / rx.nativeRate;
    for (size_t i = 0; i < xs.size(); i++) xs[i] = (float)((rx.irTauMin + (double)i) * usPerSample);
    ImGui::TextDisabled("power-delay profile (dB rel. strongest path). Shaded: guard interval (%.1f us)", rx.guard * usPerSample);
    if (ImPlot::BeginPlot("##ir", ImVec2(-1, -1), ImPlotFlags_NoLegend | ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes("delay (us)", "dB");
        ImPlot::SetupAxisLimits(ImAxis_Y1, -80, 5, ImPlotCond_Once);
        ImPlot::SetupAxisLimits(ImAxis_X1, xs.front(), xs.back(), ImPlotCond_Once);
        double gx[2] = {0, rx.guard * usPerSample}, gy[2] = {5, 5};
        ImPlotSpec gs; gs.FillColor = ImVec4(0.15f, 0.55f, 0.20f, 0.18f); gs.LineColor = ImVec4(0, 0, 0, 0);
        ImPlot::PlotShaded("gi", gx, gy, 2, -80.0, gs);
        ImPlotSpec sp; sp.LineColor = ImVec4(0.45f, 0.75f, 1, 1);
        ImPlot::PlotLine("pdp", xs.data(), rx.irDb.data(), (int)xs.size(), sp);
        ImPlot::EndPlot();
    }
}

static void snrTab(App& a) {
    const RxTelemetry& rx = a.rx;
    if (rx.snrDb.empty()) { ImGui::TextDisabled("per-carrier SNR appears once a full frame has been received"); if (rx.chValid) ImGui::Text("SNR estimate: %.1f dB", rx.p2SnrDb); return; }
    double binMhz = rx.nativeRate / 1e6 / rx.fftN;
    std::vector<float> xs(rx.snrDb.size());
    for (size_t i = 0; i < xs.size(); i++) xs[i] = (float)(a.freqMhz + ((double)(i * rx.snrStep) - (rx.chCarriers - 1) / 2.0) * binMhz);
    ImGui::Text("pilot-derived SNR across the channel (scattered pilots, smoothed over %d points); mean %.1f dB", 21, rx.p2SnrDb);
    if (ImPlot::BeginPlot("##snrc", ImVec2(-1, -1), ImPlotFlags_NoLegend | ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes("frequency (MHz)", "SNR (dB)", 0, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisFormat(ImAxis_X1, "%.2f");
        ImPlot::SetupAxisLimits(ImAxis_X1, xs.front(), xs.back(), ImPlotCond_Always);
        ImPlotSpec sp; sp.LineColor = ImVec4(0.35f, 0.85f, 0.45f, 1);
        ImPlot::PlotLine("snr", xs.data(), rx.snrDb.data(), (int)xs.size(), sp);
        ImPlot::EndPlot();
    }
}

static void fecTab(App& a) {
    if (a.rx.standard == 1) {
        const RxTelemetry& rx = a.rx;
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "DVB-T channel decoder: de-interleaving, Viterbi (inner code), outer de-interleaver, Reed-Solomon (204,188)");
        if (!rx.dvbt.tpsOk) { ImGui::TextDisabled("waiting for TPS"); return; }
        const double tot = (double)(rx.dvbt.rsClean + rx.dvbt.rsCorrected + rx.dvbt.rsFailed);
        if (ImGui::BeginTable("fect", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 230);
            auto row = [&](const char* k, const char* fmt, auto... args) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k); ImGui::TableNextColumn(); char b[160]; snprintf(b, sizeof b, fmt, args...); ImGui::TextUnformatted(b); };
            row("Transport stream sync", "%s", rx.dvbt.fecSync ? "locked" : "searching (puncturing phase, byte phase)");
            row("Puncturing phase", "%d", rx.dvbt.punctPhase);
            row("Viterbi path-metric margin", "%.2f   (1.0 = noiseless)", rx.dvbt.viterbiMargin);
            row("Reed-Solomon blocks", "%llu", (unsigned long long)rx.dvbt.packets);
            row("   decoded without errors", "%llu  (%.2f%%)", (unsigned long long)rx.dvbt.rsClean, tot ? 100.0 * rx.dvbt.rsClean / tot : 0.0);
            row("   corrected", "%llu  (%.2f%%)", (unsigned long long)rx.dvbt.rsCorrected, tot ? 100.0 * rx.dvbt.rsCorrected / tot : 0.0);
            row("   uncorrectable", "%llu  (%.2f%%)", (unsigned long long)rx.dvbt.rsFailed, tot ? 100.0 * rx.dvbt.rsFailed / tot : 0.0);
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Uncorrectable packets are passed on with the transport_error_indicator set, so the player skips them.");
        return;
    }
    const RxTelemetry& rx = a.rx;
    ImGui::PushFont(a.mono, 0);
    auto row = [](const char* k, const char* fmt, ...) {
        char b[200];
        va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
        ImGui::TextDisabled("%-26s", k); ImGui::SameLine(); ImGui::TextUnformatted(b);
    };
    if (!rx.plpValid) {
        ImGui::TextDisabled("waiting for L1-post (PLP configuration)...");
        if (rx.plpSkipped) ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.2f, 1), "%d frame(s) skipped: PLP uses inter-frame time interleaving or lies outside the received cells", rx.plpSkipped);
        ImGui::PopFont();
        return;
    }
    static const char* modN[] = {"QPSK", "16-QAM", "64-QAM", "256-QAM"};
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "PLP %d", rx.plpId);
    row("FEC frame / code rate", "%s, %s", rx.plpFec.shortFrame ? "short (16200)" : "normal (64800)", rateName(rx.plpFec.rate));
    row("constellation", "%s%s", modN[rx.plpFec.mod & 3], rx.plpFec.rotation ? ", rotated + cyclic Q delay" : "");
    row("FEC blocks per frame", "%d", rx.plpBlocks);
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "decoder (all frames since start)");
    double tot = (double)(rx.blocksOk + rx.blocksBad);
    row("frames decoded", "%llu  (dropped, decoder busy: %llu)", (unsigned long long)rx.plpFrames, (unsigned long long)rx.plpFramesDropped);
    row("FEC blocks OK / failed", "%llu / %llu  (%.2f%% good)", (unsigned long long)rx.blocksOk, (unsigned long long)rx.blocksBad, tot ? 100.0 * rx.blocksOk / tot : 0.0);
    row("BB headers with valid CRC", "%llu", (unsigned long long)rx.headersOk);
    row("BCH bit errors corrected", "%llu", (unsigned long long)rx.plpBchCorrected);
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "last frame");
    row("MER", "%.1f dB", rx.plpMerDb);
    row("BER before LDPC", "%.2e", rx.plpPreBer);
    row("LDPC iterations (average)", "%.1f", rx.plpIters);
    row("decode time", "%.0f ms for %.0f ms of signal (%s)", rx.plpDecodeMs, rx.frameMs, rx.plpOnGpu ? "GPU" : "CPU");
    if (rx.plpHeaderUpl) row("BB header", "UPL %d bits, DFL %d bits, SYNCD %d", rx.plpHeaderUpl, rx.plpHeaderDfl, rx.plpHeaderSyncd);
    ImGui::PopFont();
}

static const char* fmtKbps(char* b, size_t n, double k) { if (k >= 1000) snprintf(b, n, "%.2f Mbit/s", k / 1000); else snprintf(b, n, "%.0f kbit/s", k); return b; }

static void tsTab(App& a) {
    const TsSnapshot& ts = a.ts;
    ImGui::PushFont(a.mono, 0);
    char b[64], b2[64];
    ImGui::Text("network \"%s\"  onid 0x%04X  tsid 0x%04X   %s", ts.networkName.c_str(), ts.onid, ts.tsid, ts.utc.c_str());
    ImGui::Text("multiplex %s (null packets %s)   packets %llu   continuity errors %llu   TEI %llu",
                fmtKbps(b, sizeof b, ts.muxKbps), fmtKbps(b2, sizeof b2, ts.nullKbps), (unsigned long long)ts.totalPackets, (unsigned long long)ts.ccErrors, (unsigned long long)ts.teiPackets);
    ImGui::Text("BB frames %llu (lost %llu)  mode %s  ISSYI %d NPD %d  resyncs %llu  CRC-8 errors %llu", (unsigned long long)a.bb.frames, (unsigned long long)a.bb.framesLost,
                a.bb.hem ? "high-efficiency" : "normal", a.bb.issyi, a.bb.npd, (unsigned long long)a.bb.resyncs, (unsigned long long)a.bb.crcErrors);
    ImGui::PopFont();
    ImGui::Spacing();
    if (ImGui::BeginTable("pids", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp, ImVec2(0, -1))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("content");
        ImGui::TableSetupColumn("packets", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("rate", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("CC err", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("scr", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableHeadersRow();
        for (auto& p : ts.pids) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("0x%04X", p.pid);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.label.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)p.packets);
            ImGui::TableNextColumn(); { char r[32]; ImGui::TextUnformatted(fmtKbps(r, sizeof r, p.kbps)); }
            ImGui::TableNextColumn(); if (p.ccErrors) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.3f, 1), "%llu", (unsigned long long)p.ccErrors); else ImGui::TextDisabled("0");
            ImGui::TableNextColumn(); if (p.scrambled) ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.2f, 1), "yes"); else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
}

static void applyOutputs(App& a) {
    a.out.host = a.udpHost;
    a.out.path = a.filePath;
    a.out.serviceId = a.selService;
    a.engine.setOutputs(a.out);
    savePrefs(a);
}


static void scanTab(App& a) {
    if (a.dabMode) { dabScanTab(a); return; }
    ScanProgress pr = a.scanner.progress();
    auto res = a.scanner.results();
    int hw = -1;
    for (int i = 0; i < (int)a.devices.size(); i++) if (a.devices[i].isRadio()) hw = i;
    if (hw < 0) { ImGui::TextDisabled("Scanning needs a radio (HackRF, Airspy, SDRplay, ...)."); return; }
    ImGui::BeginDisabled(pr.running);
    const char* presetsDvb[] = {"UHF 474-858 MHz (8 MHz)", "VHF III 174-230 MHz (7 MHz)", "Custom"};
    const char* presetsAtsc[] = {"US UHF ch 14-36 (470-608 MHz)", "US VHF high ch 7-13 (174-216 MHz)", "Custom"};
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::Combo("##range", &a.scanPreset, a.atscMode ? presetsAtsc : presetsDvb, 3)) {
        if (a.atscMode) {
            if (a.scanPreset == 0) { a.scanCfg.startMHz = 473; a.scanCfg.stopMHz = 605; a.scanCfg.stepMHz = 6; a.scanCfg.bwMhz = 6; }
            if (a.scanPreset == 1) { a.scanCfg.startMHz = 177; a.scanCfg.stopMHz = 213; a.scanCfg.stepMHz = 6; a.scanCfg.bwMhz = 6; }
        } else {
            if (a.scanPreset == 0) { a.scanCfg.startMHz = 474; a.scanCfg.stopMHz = 858; a.scanCfg.stepMHz = 8; a.scanCfg.bwMhz = 8; }
            if (a.scanPreset == 1) { a.scanCfg.startMHz = 177.5; a.scanCfg.stopMHz = 226.5; a.scanCfg.stepMHz = 7; a.scanCfg.bwMhz = 7; }
        }
    }
    ImGui::SameLine(); ImGui::TextUnformatted("from"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(80 * gUi); ImGui::InputDouble("##from", &a.scanCfg.startMHz, 0, 0, "%.1f");
    ImGui::SameLine(); ImGui::TextUnformatted("to"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(80 * gUi); ImGui::InputDouble("##to", &a.scanCfg.stopMHz, 0, 0, "%.1f");
    ImGui::SameLine(); ImGui::TextUnformatted("step"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(60 * gUi); ImGui::InputDouble("##step", &a.scanCfg.stepMHz, 0, 0, "%.1f");
    ImGui::SameLine(); ImGui::TextUnformatted("bw"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(60 * gUi);
    ImGui::BeginDisabled(a.scanCfg.autoBandwidth);
    ImGui::InputDouble("##bw", &a.scanCfg.bwMhz, 0, 0, "%.0f");
    ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::Checkbox("detect bandwidth", &a.scanCfg.autoBandwidth);
    ImGui::Checkbox("read service names (slower, ~9 s per mux)", &a.scanCfg.identifyServices);
    ImGui::EndDisabled();
    if (a.scanCfg.stepMHz < 1) a.scanCfg.stepMHz = 1;
    if (!pr.running) {
        if (ImGui::Button("  Start scan  ")) {
            a.scanWasRunning = a.engine.running();
            if (a.scanWasRunning) a.engine.stop();
            a.scanCfg.tune = a.tune;
            a.scanCfg.atsc = a.atscMode;
            std::string err;
            if (!a.scanner.start(a.devices[hw], a.scanCfg, err)) a.engine.log("scan: " + err);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s  (uses the gains from the toolbar; stops the receiver while scanning)", pr.phase.c_str());
    } else {
        if (ImGui::Button("  Stop scan  ")) a.scanner.stop();
        ImGui::SameLine();
        ImGui::ProgressBar(pr.total ? (float)pr.index / pr.total : 0, ImVec2(260 * gUi, 0));
        ImGui::SameLine();
        ImGui::Text("%.1f MHz - %s", pr.currentMHz, pr.phase.c_str());
    }
    ImGui::Separator();
    if (ImGui::BeginTable("scan", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("level", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("result", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("mode", ImGuiTableColumnFlags_WidthFixed, 240);
        ImGui::TableSetupColumn("SNR", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("PLP", ImGuiTableColumnFlags_WidthFixed, 180);
        ImGui::TableSetupColumn("services");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < res.size(); i++) {
            auto& r = res[i];
            if (!r.occupied && !r.t2) continue; // hide empty channels
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char lbl[32]; snprintf(lbl, sizeof lbl, "%.1f##%zu", r.freqMHz, i);
            if (ImGui::Selectable(lbl, false, ImGuiSelectableFlags_SpanAllColumns) && !pr.running) {
                a.freqMhz = r.freqMHz; a.bwIdx = r.bwMhz >= 7.5 ? 0 : 1;
                for (int k = 0; k < (int)(sizeof(kBw) / sizeof(kBw[0])); k++) if (kBw[k].mhz == r.bwMhz) a.bwIdx = k;
                a.tune.centerHz = a.freqMhz * 1e6; applyBandwidth(a);
                a.engine.setComputeMode(a.computeMode); a.engine.setStandard(engineStd(a));
                a.engine.start(a.devices[hw], a.tune, a.file);
                a.devIdx = hw; a.smooth.clear(); a.peak.clear(); a.lastSeq = 0; savePrefs(a);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to tune the receiver to this channel");
            ImGui::TableNextColumn(); ImGui::Text("%.1f dBFS", r.levelDbfs);
            ImGui::TableNextColumn();
            if (r.t2) ImGui::TextColored(ImVec4(0.4f, 1, 0.5f, 1), "%s", r.standard.c_str()); else ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "other");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.mode.c_str());
            ImGui::TableNextColumn(); if (r.t2) ImGui::Text("%.1f dB", r.snrDb);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.plpInfo.c_str());
            ImGui::TableNextColumn();
            std::string sv;
            for (auto& x : r.services) { if (!sv.empty()) sv += ",  "; sv += x; }
            if (sv.empty() && !r.t2) sv = r.note;
            if (!r.unsupported.empty()) sv = (sv.empty() ? "" : sv + "  ") + "! " + r.unsupported[0];
            ImGui::TextWrapped("%s", sv.c_str());
        }
        ImGui::EndTable();
    }
}

static void outputsTab(App& a) {
    bool ch = false;
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "What to send");
    {
        std::string cur = "Whole multiplex";
        for (auto& sv : a.ts.services) if (sv.id == a.selService) cur = sv.name.empty() ? "service " + std::to_string(sv.id) : sv.name;
        ImGui::SetNextItemWidth(320 * gUi);
        if (ImGui::BeginCombo("service", cur.c_str())) {
            if (ImGui::Selectable("Whole multiplex", a.selService < 0)) { a.selService = -1; ch = true; }
            for (auto& sv : a.ts.services) {
                std::string n = (sv.name.empty() ? "service " + std::to_string(sv.id) : sv.name) + "  [" + sv.typeName() + "]";
                if (ImGui::Selectable(n.c_str(), sv.id == a.selService)) { a.selService = sv.id; ch = true; }
            }
            ImGui::EndCombo();
        }
    }
    if (ImGui::Checkbox("remove null packets", &a.out.dropNull)) ch = true;
    ImGui::SameLine(); ImGui::TextDisabled("(a single service is always rewritten with its own PAT)");
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "File (.ts)");
    ImGui::SetNextItemWidth(520 * gUi);
    ImGui::InputText("##fp", a.filePath, sizeof a.filePath);
    ImGui::SameLine();
    if (ImGui::Button("Choose...")) { auto p = saveFileDialog("recording.ts"); if (!p.empty()) snprintf(a.filePath, sizeof a.filePath, "%s", p.c_str()); }
    ImGui::SameLine();
    if (ImGui::Checkbox("record", &a.out.file)) ch = true;
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "UDP");
    ImGui::SetNextItemWidth(200 * gUi);
    if (ImGui::InputText("address", a.udpHost, sizeof a.udpHost)) {}
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    ImGui::InputInt("port", &a.out.port, 0, 0);
    ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi);
    ImGui::InputInt("TTL", &a.out.ttl, 0, 0);
    ImGui::SameLine();
    ImGui::Checkbox("RTP", &a.out.rtp);
    ImGui::SameLine();
    if (ImGui::Checkbox("stream", &a.out.udp)) ch = true;
    ImGui::TextDisabled("Unicast or multicast (e.g. 239.1.1.1). Datagrams carry 7 packets and are paced evenly. Play with: ffplay udp://@:%d", a.out.port);
    if (ch || ImGui::IsItemDeactivatedAfterEdit()) applyOutputs(a);
    if (ImGui::Button("Apply address / port / TTL")) applyOutputs(a);
    ImGui::Spacing();
    OutputStats os = a.engine.outputStats();
    ImGui::PushFont(a.mono, 0);
    if (os.fileOpen) ImGui::Text("recording: %llu packets, %.1f MB", (unsigned long long)os.filePackets, os.fileBytes / 1e6);
    else ImGui::TextDisabled("recording: off");
    if (os.udpOpen) ImGui::Text("streaming: %llu datagrams, queue %.0f ms, dropped %llu", (unsigned long long)os.udpDatagrams, os.udpQueueMs, (unsigned long long)os.udpDropped);
    else ImGui::TextDisabled("streaming: off");
    ImGui::PopFont();
    if (!os.error.empty()) ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.3f, 1), "%s", os.error.c_str());
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "Network tuner");
    {
        const NetTunerStats ns = a.net.stats();
        bool apply = false;
        ImGui::BeginDisabled(ns.running);
        ImGui::SetNextItemWidth(90 * gUi); ImGui::InputInt("port##net", &a.netPort, 0, 0);
        ImGui::SameLine(); ImGui::Checkbox("share on the network", &a.netLan);
        ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi); ImGui::InputText("key (optional)", a.netKey, sizeof a.netKey);
        ImGui::EndDisabled();
        ImGui::SameLine();
        bool on = ns.running;
        if (ImGui::Checkbox("serve", &on)) apply = true;
        if (apply) {
            if (on) {
                NetTunerConfig nc; nc.port = std::max(1024, std::min(65535, a.netPort)); nc.localOnly = !a.netLan; nc.key = a.netKey;
                a.netOn = a.net.start(nc);
                a.engine.log(a.netOn ? "network tuner on port " + std::to_string(nc.port) : "network tuner: " + a.net.stats().error);
            } else { a.net.stop(); a.netOn = false; a.engine.log("network tuner stopped"); }
        }
        const NetTunerStats ns2 = a.net.stats();
        if (!ns2.error.empty() && !ns2.running) ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.3f, 1), "%s", ns2.error.c_str());
        if (ns2.running) {
            ImGui::PushFont(a.mono, 0);
            const std::string k = a.netKey[0] ? std::string("?key=") + a.netKey : "";
            for (const auto& ad : a.net.addresses()) ImGui::Text("http://%s:%d/lineup.m3u%s", ad.c_str(), ns2.port, k.c_str());
            ImGui::Text("viewers: %d   sent: %.1f MB", ns2.clients, ns2.bytesSent / 1e6);
            ImGui::PopFont();
        }
        ImGui::TextDisabled("Open the playlist in VLC or any IPTV app; /guide.xml has the programme guide. Plex and Jellyfin: add an HDHomeRun at that address.");
        ImGui::TextDisabled("Without a key, anyone on your network can watch while \"share on the network\" is on.");
    }
}

static std::string channelLabel(const SavedChannel& c) {
    char b[200];
    snprintf(b, sizeof b, "%.3f MHz  %s%s", c.freqMhz, c.name.empty() ? "DVB-T2 mux" : c.name.c_str(), c.nServices > 1 ? (" +" + std::to_string(c.nServices - 1)).c_str() : "");
    return b;
}

// Merge DVB-T2 muxes found by the scanner into the remembered channel list.
static void harvestScan(App& a) {
    ScanProgress pr = a.scanner.progress();
    const double now = glfwGetTime();
    const bool due = pr.running ? now - a.scanHarvestT > 0.5 : a.scanWas;
    if (!due) return;
    a.scanWas = pr.running;
    a.scanHarvestT = now;
    bool changed = false;
    for (auto& r : a.scanner.results()) {
        if (!r.t2) continue;
        SavedChannel sc;
        sc.freqMhz = r.freqMHz; sc.bwMhz = r.bwMhz; sc.mode = r.mode; sc.snrDb = r.snrDb; sc.nServices = (int)r.services.size();
        if (!r.services.empty()) { sc.name = r.services[0]; size_t br = sc.name.rfind(" ["); if (br != std::string::npos) sc.name.resize(br); }
        if (sc.name.empty()) sc.name = r.networkName;
        bool found = false;
        for (auto& c : a.channels)
            if (std::fabs(c.freqMhz - sc.freqMhz) < 0.01) {
                if (c.favourite) { sc.name = c.name; sc.favourite = true; }
                if (c.name != sc.name || c.nServices != sc.nServices || c.mode != sc.mode) { c = sc; changed = true; }
                found = true; break;
            }
        if (!found) { a.channels.push_back(sc); changed = true; }
    }
    if (changed) {
        std::sort(a.channels.begin(), a.channels.end(), [](const SavedChannel& x, const SavedChannel& y) { return x.freqMhz < y.freqMhz; });
        savePrefs(a);
    }
}

// Select a remembered channel: stop what is playing and tune the HackRF to it.
static void tuneToChannel(App& a, const SavedChannel& c) {
    int hw = -1;
    for (int i = 0; i < (int)a.devices.size(); i++) if (a.devices[i].isRadio()) hw = i;
    if (hw < 0) { a.engine.log("channel selector needs a radio"); return; }
    if (a.scanner.progress().running) a.scanner.stop();
    const double oldBw = a.tune.bandwidthMhz;
    a.freqMhz = c.freqMhz;
    for (int k = 0; k < (int)(sizeof(kBw) / sizeof(kBw[0])); k++) if (kBw[k].mhz == c.bwMhz) a.bwIdx = k;
    a.tune.centerHz = a.freqMhz * 1e6;
    applyBandwidth(a);
    a.engine.player().select(-1);
    a.selService = -1;
    if (a.engine.running() && a.devIdx == hw && oldBw == a.tune.bandwidthMhz) {
        a.engine.log("channel: " + channelLabel(c));
        a.engine.retuneReset(a.tune);
    } else {
        if (a.engine.running()) a.engine.stop();
        a.devIdx = hw;
        a.engine.setComputeMode(a.computeMode); a.engine.setStandard(engineStd(a));
        a.engine.start(a.devices[hw], a.tune, a.file);
    }
    a.smooth.clear(); a.peak.clear(); a.lastSeq = 0;
    a.mpd.reset(); a.quality.reset();
    savePrefs(a);
}

static void qualityBar(App& a, float width) {
    const QualityReport& q = a.quality.report();
    const bool run = a.engine.running();
    const float pct = run && q.valid ? (float)q.percent : 0.f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), IM_COL32(120, 28, 28, 255), 3.f); // red = nothing yet
    const float t = pct / 100.f;
    const ImVec4 lo(0.95f, 0.55f, 0.15f, 1), hi(0.25f, 0.85f, 0.35f, 1);
    const ImVec4 col(lo.x + (hi.x - lo.x) * t, lo.y + (hi.y - lo.y) * t, lo.z + (hi.z - lo.z) * t, 1);
    if (pct > 0) dl->AddRectFilled(p, ImVec2(p.x + width * t, p.y + h), ImGui::ColorConvertFloat4ToU32(col), 3.f);
    dl->AddRect(p, ImVec2(p.x + width, p.y + h), IM_COL32(70, 76, 84, 255), 3.f);
    char txt[96];
    if (!run || !q.valid) snprintf(txt, sizeof txt, "signal quality: %s", run ? "no lock" : "-");
    else snprintf(txt, sizeof txt, "signal quality %.0f%%  %s", q.percent, q.label.c_str());
    ImVec2 ts = ImGui::CalcTextSize(txt);
    dl->AddText(ImVec2(p.x + (width - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), IM_COL32(235, 238, 242, 255), txt);
    ImGui::Dummy(ImVec2(width, h));
    if (ImGui::IsItemHovered() && run && q.valid) {
        ImGui::BeginTooltip();
        ImGui::Text("data SNR %.1f dB, needed for this modulation/code rate about %.1f dB", q.snrDb, q.requiredDb);
        ImGui::Text("margin %+.1f dB, FEC blocks decoded %.1f%% (last frames)", q.marginDb, q.fecOk * 100);
        ImGui::TextDisabled("0 dB margin is the edge of reception (25%%); +6 dB or more is comfortable.");
        ImGui::EndTooltip();
    }
}

static void playerTab(App& a) {
    Player& pl = a.engine.player();
    PlayerStats ps = pl.stats();
    {   // channel selector (muxes found by the scanner) and the signal-quality bar
        const SavedChannel* cur = nullptr;
        for (auto& c : a.channels) if (std::fabs(c.freqMhz - a.freqMhz) < 0.01) cur = &c;
        char fb[48]; snprintf(fb, sizeof fb, "%.3f MHz", a.freqMhz);
        const std::string curLab = cur ? channelLabel(*cur) : std::string(fb);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("channel");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(360 * gUi);
        if (ImGui::BeginCombo("##chan", ((cur && cur->favourite) ? "* " + curLab : curLab).c_str())) {
            if (a.channels.empty()) ImGui::TextDisabled("no channels yet - run a scan (Scan tab)");
            for (size_t i = 0; i < a.channels.size(); i++) {
                const SavedChannel& c = a.channels[i];
                std::string l = (c.favourite ? "* " : "") + channelLabel(c);
                char extra[64]; snprintf(extra, sizeof extra, "   SNR %.0f dB", c.snrDb);
                if (ImGui::Selectable((l + extra).c_str(), cur == &c)) tuneToChannel(a, c);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.mode.c_str());
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("+##fav")) { ImGui::OpenPopup("savefav"); if (!a.favName[0] && cur) snprintf(a.favName, sizeof a.favName, "%s", cur->name.c_str()); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save the current frequency as a favourite channel");
        if (ImGui::BeginPopup("savefav")) {
            ImGui::TextDisabled("name for %.3f MHz", a.freqMhz);
            ImGui::SetNextItemWidth(220 * gUi);
            ImGui::InputText("##favn", a.favName, sizeof a.favName);
            if (ImGui::Button("Save")) {
                SavedChannel sc; sc.freqMhz = a.freqMhz; sc.bwMhz = kBw[a.bwIdx].mhz; sc.name = a.favName; sc.favourite = true;
                if (cur) { sc.mode = cur->mode; sc.snrDb = cur->snrDb; sc.nServices = cur->nServices; }
                bool f = false;
                for (auto& c : a.channels) if (std::fabs(c.freqMhz - sc.freqMhz) < 0.01) { c = sc; f = true; }
                if (!f) a.channels.push_back(sc);
                std::sort(a.channels.begin(), a.channels.end(), [](const SavedChannel& x, const SavedChannel& y) { return x.freqMhz < y.freqMhz; });
                a.favName[0] = 0; savePrefs(a); ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (cur) {
            ImGui::SameLine();
            if (ImGui::SmallButton("x##del")) { const double f = cur->freqMhz; a.channels.erase(std::remove_if(a.channels.begin(), a.channels.end(), [&](const SavedChannel& c) { return std::fabs(c.freqMhz - f) < 0.01; }), a.channels.end()); savePrefs(a); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget this channel");
        }
        ImGui::SameLine(0, 18 * gUi);
        qualityBar(a, std::max(200.f, ImGui::GetContentRegionAvail().x - 8));
    }
    {   // PLP selector for multi-PLP multiplexes, and a notice for anything the receiver cannot decode
        const RxTelemetry& rx = a.rx;
        if (rx.plpList.size() > 1) {
            static const char* mods[] = {"QPSK", "16-QAM", "64-QAM", "256-QAM"};
            static const char* rates[] = {"1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "1/3", "2/5"};
            auto lab = [&](const RxTelemetry::PlpInfo& p) {
                char b[120];
                snprintf(b, sizeof b, "PLP %d  type %d  %s %s%s%s", p.id, p.type, p.mod >= 0 && p.mod < 4 ? mods[p.mod] : "?", p.cod >= 0 && p.cod < 8 ? rates[p.cod] : "?", p.type == 0 ? "  (common)" : "", p.supported ? "" : "  (not supported)");
                return std::string(b);
            };
            std::string cur = a.plpSel < 0 ? "automatic" : "PLP " + std::to_string(a.plpSel);
            if (a.plpSel < 0 && rx.plpSelectedId >= 0) cur += " (PLP " + std::to_string(rx.plpSelectedId) + ")";
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("PLP");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(260 * gUi);
            if (ImGui::BeginCombo("##plp", cur.c_str())) {
                if (ImGui::Selectable("automatic (first data PLP)", a.plpSel < 0)) { a.plpSel = -1; a.engine.selectPlp(-1); }
                for (auto& p : rx.plpList) if (ImGui::Selectable(lab(p).c_str(), a.plpSel == p.id)) { a.plpSel = p.id; a.engine.selectPlp(p.id); }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("This multiplex carries several physical layer pipes. The receiver decodes one at a time.");
        }
        for (auto& u : rx.unsupported) { ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.2f, 1), "! %s", u.c_str()); }
    }
    const bool haveNN = pl.selected() >= 0;
    float h = ImGui::GetContentRegionAvail().y - 44 - (haveNN ? 46.f : 0.f);
    ImGui::BeginChild("vbox", ImVec2(-1, h), ImGuiChildFlags_Borders);
    if (pl.selected() < 0) ImGui::TextDisabled("Pick a service in the list on the right to start playback.");
    else if (!a.video.has()) ImGui::TextDisabled("%s", ps.hasAudio && !ps.hasVideo ? "audio only" : ps.status.c_str());
    else a.video.draw(ImGui::GetContentRegionAvail());
    ImGui::EndChild();
    if (ImGui::Button(pl.selected() >= 0 ? "Stop" : "Play")) { if (pl.selected() >= 0) pl.select(-1); else if (a.playReq < 0 && !a.ts.services.empty()) { int sid = a.ts.services[0].id; pl.select(sid); pl.setVolume(a.volume); } }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::SliderFloat("volume", &a.volume, 0, 1, "%.2f")) pl.setVolume(a.volume);
    ImGui::SameLine();
    if (ImGui::Checkbox("mute", &a.muted)) pl.setMuted(a.muted);
    ImGui::SameLine();
    ImGui::Checkbox("pop out", &a.popOut);
    ImGui::SameLine();
    if (ImGui::Button("Fullscreen")) { a.videoOnly = true; if (!glfwGetWindowMonitor(gWindow)) toggleFullscreen(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Video fills the screen (press F or Esc to leave)");
    ImGui::SameLine();
    ImGui::Checkbox("deinterlace", &a.video.deint);
    ImGui::SameLine();
    auto tracks = pl.audioTracks();
    if (!tracks.empty()) {
        static int cur = 0;
        cur = std::min(cur, (int)tracks.size() - 1);
        std::string lab = tracks[cur].codec + " " + tracks[cur].lang;
        ImGui::SetNextItemWidth(170 * gUi);
        if (ImGui::BeginCombo("audio", lab.c_str())) {
            for (int i = 0; i < (int)tracks.size(); i++) {
                std::string l = tracks[i].codec + " " + tracks[i].lang + " (" + std::to_string(tracks[i].channels) + " ch)";
                if (ImGui::Selectable(l.c_str(), i == cur)) { cur = i; pl.setAudioTrack(i); }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }
    if (pl.subtitlesAvailable()) { if (ImGui::Checkbox("subtitles", &a.subsOn)) pl.setSubtitles(a.subsOn); }
    ImGui::NewLine();
    if (pl.selected() >= 0) {   // what is on now, and what comes next
        const EpgEvent* nx = nullptr;
        const EpgEvent* cur = epgCurrent(a, pl.selected(), &nx);
        if (cur) {
            const int64_t now = utcNowOf(a);
            const float f = std::min(1.f, std::max(0.f, (float)(now - cur->start) / (float)cur->duration));
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.4f, 1), "NOW");
            ImGui::SameLine();
            ImGui::Text("%s - %s  %s", fmtLocal(cur->start, "%H:%M").c_str(), fmtLocal(cur->end(), "%H:%M").c_str(), cur->title.c_str());
            ImGui::SameLine();
            char left[32]; snprintf(left, sizeof left, "%d min left", (int)std::max<int64_t>(0, (cur->end() - now + 30) / 60));
            ImGui::ProgressBar(f, ImVec2(160 * gUi, 12 * gUi), left);
            if (nx) { ImGui::TextDisabled("NEXT"); ImGui::SameLine(); ImGui::Text("%s  %s", fmtLocal(nx->start, "%H:%M").c_str(), nx->title.c_str()); }
        } else if (nx) {
            ImGui::TextDisabled("NEXT"); ImGui::SameLine(); ImGui::Text("%s  %s", fmtLocal(nx->start, "%H:%M").c_str(), nx->title.c_str());
        } else ImGui::TextDisabled("no programme information is broadcast for this service");
    }
}

static void rightPanel(App& a) {
    const bool run = a.engine.running();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // ---- clock: broadcast time (TDT) when the stream has one, otherwise this computer's clock
    {
        const int64_t now = utcNowOf(a);
        time_t t = (time_t)now; struct tm m; dect2::gmTime(t, &m);
        char hm[16], ss[8], date[40];
        strftime(hm, sizeof hm, "%H:%M", &m); strftime(ss, sizeof ss, ":%S", &m); strftime(date, sizeof date, "%a %d %b %Y", &m);
        ImGui::PushFont(a.ui, 40.f);
        ImGui::TextUnformatted(hm);
        ImGui::PopFont();
        ImGui::SameLine(0, 4 * gUi);
        ImGui::BeginGroup();
        ImGui::TextDisabled("UTC");
        ImGui::TextDisabled("%s", ss);
        ImGui::EndGroup();
        ImGui::TextDisabled("%s", date);
        ImGui::SameLine(0, 10 * gUi);
        ImGui::TextDisabled("local %s   (%s)", fmtLocal(now, "%H:%M").c_str(), a.ts.utcNow ? "broadcast time" : "computer clock");
    }
    ImGui::Spacing();
    if (a.dabMode) { dabStations(a); return; }
    sectionHeader(Ic::Tv, "Services");
    // ---- service cards
    const float cardH = 68;
    ImGui::BeginChild("svcs", ImVec2(0, std::max(140.f * gUi, ImGui::GetContentRegionAvail().y * 0.40f)));
    dl = ImGui::GetWindowDrawList(); // draw into the child so the cards are clipped and scroll with it
    if (a.ts.services.empty()) ImGui::TextDisabled(run ? "waiting for PAT / SDT..." : "start the receiver to see services");
    for (auto& sv : a.ts.services) {
        std::string vc, ac;
        double kb = 0;
        bool hasVideo = false;
        for (auto& st : sv.streams) { kb += st.kbps; if (st.kind == "video") { hasVideo = true; if (vc.empty()) vc = st.codec; } if (st.kind == "audio" && ac.empty()) ac = st.codec; }
        const bool sel = sv.id == a.engine.player().selected();
        ImGui::PushID(sv.id);
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##card", ImVec2(w, cardH));
        const bool hov = ImGui::IsItemHovered();
        if (clicked) { if (sel) a.engine.player().select(-1); else { a.engine.player().select(sv.id); a.engine.player().setVolume(a.volume); } }
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + cardH - 4), sel ? IM_COL32(30, 36, 40, 255) : hov ? IM_COL32(26, 27, 29, 255) : IM_COL32(16, 17, 19, 255), 4.f);
        dl->AddRect(p, ImVec2(p.x + w, p.y + cardH - 4), sel ? IM_COL32(115, 184, 209, 255) : IM_COL32(48, 50, 52, 255), 4.f, 0, sel ? 1.6f : 1.f);
        // name and bit rate
        const std::string name = sv.name.empty() ? "service " + std::to_string(sv.id) : sv.name;
        const bool isRadio = sv.type == 0x02 || sv.type == 0x0A || (!hasVideo && !ac.empty());
        icons::draw(isRadio ? Ic::Radio : Ic::Tv, ImVec2(p.x + 20, p.y + 15), 17.f, sel ? IM_COL32(120, 200, 255, 255) : IM_COL32(120, 136, 156, 255), dl);
        dl->AddText(a.ui, 16.f, ImVec2(p.x + 34, p.y + 6), IM_COL32(245, 247, 250, 255), name.c_str());
        char kbs[32]; fmtKbps(kbs, sizeof kbs, kb);
        const ImVec2 ks = ImGui::CalcTextSize(kbs);
        dl->AddText(ImVec2(p.x + w - ks.x - 10, p.y + 8), IM_COL32(150, 158, 168, 255), kbs);
        // tag pills
        float x = p.x + 10;
        const float ty = p.y + 27;
        const bool radio = sv.type == 0x02 || sv.type == 0x0A || (!hasVideo && !ac.empty());
        x += tagAt(dl, ImVec2(x, ty), radio ? "radio" : hasVideo ? "TV" : sv.typeName(), IM_COL32(34, 74, 108, 255)) + 4;
        if (!vc.empty()) x += tagAt(dl, ImVec2(x, ty), vc.c_str(), IM_COL32(38, 46, 58, 255), IM_COL32(190, 200, 214, 255)) + 4;
        if (!ac.empty()) x += tagAt(dl, ImVec2(x, ty), ac.c_str(), IM_COL32(38, 46, 58, 255), IM_COL32(190, 200, 214, 255)) + 4;
        if (sv.caFlag) x += tagAt(dl, ImVec2(x, ty), "scrambled", IM_COL32(112, 52, 50, 255)) + 4;
        // what is on
        const EpgEvent* cur = epgCurrent(a, sv.id);
        std::string line;
        if (cur) line = fmtLocal(cur->start, "%H:%M") + "-" + fmtLocal(cur->end(), "%H:%M") + "  " + cur->title;
        else if (!sv.now.empty()) line = sv.now;
        else if (!sv.provider.empty()) line = sv.provider;
        dl->PushClipRect(ImVec2(p.x + 8, p.y), ImVec2(p.x + w - 8, p.y + cardH), true);
        dl->AddText(ImVec2(p.x + 10, p.y + 47), IM_COL32(128, 136, 146, 255), line.c_str());
        dl->PopClipRect();
        ImGui::PopID();
    }
    ImGui::EndChild();

    // ---- what the player is doing, as label / value rows
    {
        const Player& pl = a.engine.player();
        const PlayerStats ps = pl.stats();
        sectionHeader(Ic::Play, "Player");
        if (pl.selected() < 0) ImGui::TextDisabled("click a service to play it");
        else if (ImGui::BeginTable("pkv", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 88);
            auto row = [&](Ic ic, const char* k, const char* fmt, auto... args) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(ic, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("%s", k); ImGui::TableNextColumn();
                char b[120]; snprintf(b, sizeof b, fmt, args...); ImGui::TextUnformatted(b);
            };
            if (ps.hasVideo) row(Ic::Tv, "Video", "%s  %dx%d  %.1f fps  %s", ps.videoCodec.c_str(), ps.width, ps.height, ps.fps, ps.hardware ? "(hardware)" : "(software)");
            if (ps.hasAudio) row(Ic::Speaker, "Audio", "%s  %d ch", ps.audioCodec.c_str(), ps.audioChannels);
            row(Ic::Clock, "Buffer", "%.0f ms     A/V %+.0f ms", ps.audioBufferMs, ps.avOffsetMs);
            row(Ic::Camera, "Pictures", "%llu shown / %llu late", (unsigned long long)ps.shown, (unsigned long long)ps.late);
            row(Ic::Warning, "Errors", "%llu     underruns %d", (unsigned long long)ps.errors, ps.underruns);
            if (ps.repairEvents) row(Ic::Camera, "Repaired", "%llu damaged spans, %llu pictures replaced", (unsigned long long)ps.repairEvents, (unsigned long long)ps.repairedFrames);
            if (ps.concealEvents) row(Ic::Pulse, "Smoothed", "%llu gaps, %llu pictures generated (%s)", (unsigned long long)ps.concealEvents, (unsigned long long)ps.concealedFrames, ps.concealBackend.c_str());
            ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(Ic::Pulse, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("Gaps"); ImGui::TableNextColumn();
            { bool on = a.engine.player().conceal(); if (ImGui::Checkbox("smooth picture gaps", &on)) a.engine.player().setConceal(on);
              if (ImGui::IsItemHovered()) ImGui::SetTooltip("When a fade swallows part of the stream or damages the pictures after it, generate the missing pictures\n(Apple ML interpolation) instead of freezing or showing smears. It hides the loss; it cannot bring the data back.\nThe sound is not changed: a dropout stays a clean silence."); }
#if defined(__APPLE__) || defined(_WIN32)
            ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(Ic::Chip, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("Decode"); ImGui::TableNextColumn();
            { bool on = a.engine.player().hardwareDecode(); if (ImGui::Checkbox("hardware video decoding", &on)) { a.engine.player().setHardwareDecode(on); plat::prefs().setB("hwVideo", on); plat::prefs().flush(); }
              if (ImGui::IsItemHovered()) ImGui::SetTooltip("Use the graphics chip to decode the video. It saves processor time for the receiver.\nSwitch it off if the picture shows green or garbled patches (a graphics driver problem)."); }
#endif
            ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(a.muted ? Ic::Mute : Ic::Speaker, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("Volume"); ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            float vol = a.volume * 100.f;
            if (ImGui::SliderFloat("##vol", &vol, 0, 100, "%.0f %%")) { a.volume = vol / 100.f; a.engine.player().setVolume(a.volume); }
            ImGui::EndTable();
        }
    }
    ImGui::Spacing();
    // ---- video preview and outputs
    {
        const float vw = ImGui::GetContentRegionAvail().x;
        const float vh = std::min(vw * 9.f / 16.f, std::max(60.f, ImGui::GetContentRegionAvail().y - 52.f));
        ImGui::BeginChild("video", ImVec2(0, vh), ImGuiChildFlags_Borders);
        const PlayerStats ps = a.engine.player().stats();
        if (a.engine.player().selected() < 0) ImGui::TextDisabled("no picture");
        else if (!ps.hasVideo && ps.hasAudio) ImGui::TextDisabled("audio only");
        else if (!a.video.has()) ImGui::TextDisabled("%s", ps.status.c_str());
        else a.video.draw(ImGui::GetContentRegionAvail());
        ImGui::EndChild();
    }
    {
        OutputStats os = a.engine.outputStats();
        iconLabel(Ic::Link, "Outputs", iconAccent());
        ImGui::SameLine(0, 10 * gUi);
        iconInline(Ic::File, os.fileOpen ? IM_COL32(120, 190, 235, 255) : IM_COL32(86, 94, 104, 255), 0.9f); ImGui::SameLine(0, 3 * gUi);
        ImGui::TextColored(os.fileOpen ? ImVec4(0.55f, 0.76f, 0.92f, 1) : ImVec4(0.45f, 0.48f, 0.52f, 1), "file");
        ImGui::SameLine(0, 10 * gUi);
        iconInline(Ic::Globe, os.udpOpen ? IM_COL32(120, 190, 235, 255) : IM_COL32(86, 94, 104, 255), 0.9f); ImGui::SameLine(0, 3 * gUi);
        ImGui::TextColored(os.udpOpen ? ImVec4(0.55f, 0.76f, 0.92f, 1) : ImVec4(0.45f, 0.48f, 0.52f, 1), "udp");
        if (os.fileOpen) { ImGui::SameLine(); ImGui::TextDisabled("%.1f MB", os.fileBytes / 1e6); }
        if (os.udpOpen) { ImGui::SameLine(); ImGui::TextDisabled("%s:%d%s", a.udpHost, a.out.port, a.out.rtp ? " RTP" : ""); }
    }
}

static void logPanel(App& a) {
    ImGui::TextDisabled("Log");
    ImGui::SameLine();
    size_t total = 0;
    auto lines = a.engine.logSnapshot(total);
    ImGui::TextDisabled("%zu lines", total);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) {
        std::string all;
        for (auto& l : lines) all += l + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) a.engine.clearLog();
    ImGui::BeginChild("logtext", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(a.mono, 0);
    for (auto& l : lines) ImGui::TextUnformatted(l.c_str());
    ImGui::PopFont();
    if (a.autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

// ------------------------------------------------------------------ the six top-level views

static void overviewTab(App& a) {
    ImGui::Checkbox("peak hold", &a.peakHold);
    ImGui::SameLine(0, 16 * gUi); ImGui::SetNextItemWidth(170 * gUi);
    ImGui::DragFloatRange2("spectrum dB", &a.yMin, &a.yMax, 1, -160, 20, "%.0f", "%.0f");
    ImGui::SameLine(0, 16 * gUi); ImGui::SetNextItemWidth(170 * gUi);
    ImGui::DragFloatRange2("waterfall dB", &a.wf.minDb, &a.wf.maxDb, 1, -160, 20, "%.0f", "%.0f");
    const float h = ImGui::GetContentRegionAvail().y;
    spectrumPlot(a, ImVec2(-1, h * 0.5f - 2));
    waterfallPlot(a, ImVec2(-1, -1));
}

static void tvTab(App& a) {
    subNav("tv", a.subTv, {"Player", "Guide", "Teletext"});
    if (a.subTv == 0) playerTab(a);
    else if (a.subTv == 1) guideTab(a);
    else teletextTab(a);
}

// channel estimate, impulse response and per-carrier SNR on one screen
static void channelDashboard(App& a) {
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("chA", ImVec2(w * 0.5f - 4, h));
    channelTab(a);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("chB", ImVec2(0, h));
    ImGui::BeginChild("chB1", ImVec2(0, h * 0.5f - 2));
    impulseTab(a);
    ImGui::EndChild();
    ImGui::BeginChild("chB2", ImVec2(0, 0));
    snrTab(a);
    ImGui::EndChild();
    ImGui::EndChild();
}

static void receiverTab(App& a) {
    if (a.dabMode) { dabEnsembleTab(a); return; }
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

static void streamTab(App& a) {
    subNav("st", a.subStream, {"Outputs", "Transport stream"});
    if (a.subStream == 0) outputsTab(a); else tsTab(a);
}

static void historyLogTab(App& a) {
    const float h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("hplots", ImVec2(0, h * 0.70f));
    historyTab(a);
    ImGui::EndChild();
    ImGui::BeginChild("hlog", ImVec2(0, 0), ImGuiChildFlags_Borders);
    logPanel(a);
    ImGui::EndChild();
}

// the old per-view names (for --tab in screenshots) map onto the new tabs and sub-views
static void routeTab(App& a, const std::string& name) {
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

static void gainControl(App& a) {
    if (!a.engine.running() || a.devices[a.devIdx].kind == DeviceInfo::File) { a.sweep = GainSweep(); return; }
    const DeviceInfo& cd = a.devices[a.devIdx];
    const bool generic = cd.kind == DeviceInfo::Soapy;
    const int gmax = generic ? std::max(1, (int)cd.gainMaxDb) : 0;
    a.agc.setGenericMax(gmax);
    GainSetting g = generic ? genericGain((int)std::lround(a.tune.gainDb), gmax) : GainSetting{a.tune.lnaDb, a.tune.vgaDb, a.tune.ampOn};
    bool changed = false;
    const double now = ImGui::GetTime();
    if (a.sweep.active()) {
        GainSweep::Sample sm;
        sm.locked = a.rx.dataValid; sm.snrDb = a.rx.dataSnrDb;
        sm.clip = a.spec.stats.clipFraction; sm.rms = a.spec.stats.rmsDbfs; sm.peak = a.spec.stats.peak;
        changed = a.sweep.update(now, sm, g);
        if (!a.sweep.active()) a.engine.log("gain helper: " + a.sweep.summary());
    } else if (a.agcOn && a.dir.state() != DirectionFinder::State::Measuring) {   // the gain must stay put while a direction is measured
        changed = a.agc.update(now, a.spec.stats, g);
    }
    if (changed) {
        if (generic) a.tune.gainDb = g.vga; else { a.tune.lnaDb = g.lna; a.tune.vgaDb = g.vga; a.tune.ampOn = g.amp; }
        a.tune.centerHz = a.freqMhz * 1e6;
        a.engine.retune(a.tune);
        if (!a.sweep.active() || true) savePrefs(a);
        if (a.agcOn && !a.sweep.active() && generic) a.engine.log("AGC: gain " + std::to_string(g.vga) + " dB");
        else if (a.agcOn && !a.sweep.active()) a.engine.log("AGC: LNA " + std::to_string(g.lna) + " dB, VGA " + std::to_string(g.vga) + " dB, amp " + (g.amp ? "on" : "off"));
    }
}


// ------------------------------------------------------------------ antenna direction finder

static ImU32 scoreColour(double sc) {
    const float t = (float)std::min(1.0, std::max(0.0, sc / 100.0));
    return IM_COL32((int)(230 * (1 - t) + 40 * t), (int)(70 + 150 * t), (int)(60 + 30 * t), 255);
}

static void feedDirection(App& a) {
    DirectionFinder& d = a.dir;
    if (d.state() != DirectionFinder::State::Measuring) return;
    if (!a.engine.running()) return;
    DirSample s;
    const QualityReport& q = a.quality.report();
    s.locked = a.rx.dataValid && q.valid && q.percent > 0;
    s.qualityPct = s.locked ? q.percent : 0;
    s.snrDb = a.rx.dataValid ? a.rx.dataSnrDb : 0;
    s.lossPct = q.valid ? 100.0 * (1.0 - q.fecOk) : 100.0;
    s.multipath = a.mpd.report().level;
    s.clipFraction = a.spec.stats.clipFraction;
    s.occupancyDb = occupancyDb(a.spec.dbfs, a.tune.sampleRate / 1e6, a.tune.bandwidthMhz);
    d.addSample(ImGui::GetTime(), s);
}

static void compassRose(App& a, ImVec2 size) {
    const DirectionFinder& d = a.dir;
    ImGui::InvisibleButton("##rose", size);
    const ImVec2 p0 = ImGui::GetItemRectMin();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float R = std::min(size.x, size.y) * 0.5f - 22;
    const ImVec2 c(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);
    auto pt = [&](double hdg, float r) { const double th = hdg * M_PI / 180.0; return ImVec2(c.x + (float)std::sin(th) * r, c.y - (float)std::cos(th) * r); };
    dl->AddCircleFilled(c, R, IM_COL32(14, 17, 22, 255), 64);
    for (int k = 1; k <= 4; k++) dl->AddCircle(c, R * k / 4, IM_COL32(48, 56, 66, 255), 64, k == 4 ? 1.5f : 1.f);
    for (int k = 0; k < 16; k++) dl->AddLine(pt(k * 22.5, k % 2 ? R * 0.9f : R * 0.82f), pt(k * 22.5, R), IM_COL32(70, 80, 92, 255));
    for (int k = 0; k < 8; k++) dl->AddLine(c, pt(k * 45.0, R), IM_COL32(34, 40, 48, 255));
    const char* nm[4] = {"N", "E", "S", "W"};
    for (int k = 0; k < 4; k++) {
        const ImVec2 q = pt(k * 90.0, R + 12), ts = ImGui::CalcTextSize(nm[k]);
        dl->AddText(ImVec2(q.x - ts.x * 0.5f, q.y - ts.y * 0.5f), k == 0 ? IM_COL32(240, 110, 90, 255) : IM_COL32(190, 198, 208, 255), nm[k]);
    }
    if (d.state() == DirectionFinder::State::Idle) return;
    if (d.kind() != AntennaKind::Omni) {
        // one spoke per measured heading, length = score
        for (const DirResult& r : d.results()) {
            const float L = R * (0.06f + 0.94f * (float)r.score / 100.f);
            dl->AddLine(c, pt(r.heading, L), scoreColour(r.score), 9.f);
            dl->AddCircleFilled(pt(r.heading, L), 6.f, scoreColour(r.score));
        }
        if (d.kind() == AntennaKind::Dipole)
            for (const DirResult& r : d.results()) dl->AddLine(c, pt(r.heading + 180, R * 0.06f + R * 0.94f * (float)r.score / 100.f), IM_COL32(100, 110, 122, 120), 2.f);
        if (d.state() != DirectionFinder::State::Done) {
            const float pulse = 0.55f + 0.45f * (float)std::sin(ImGui::GetTime() * 5);
            const ImU32 col = IM_COL32(255, 205, 60, (int)(255 * pulse));
            dl->AddLine(pt(d.targetHeading(), R * 0.15f), pt(d.targetHeading(), R), col, 3.f);
            const ImVec2 tip = pt(d.targetHeading(), R + 2), l = pt(d.targetHeading() + 5, R - 16), r2 = pt(d.targetHeading() - 5, R - 16);
            dl->AddTriangleFilled(tip, l, r2, col);
        } else if (d.recommendation().valid && !d.recommendation().flat) {
            const double h = d.recommendation().heading;
            const ImU32 col = IM_COL32(70, 230, 120, 255);
            dl->AddLine(c, pt(h, R - 14), col, 5.f);
            dl->AddTriangleFilled(pt(h, R + 2), pt(h + 7, R - 22), pt(h - 7, R - 22), col);
            if (d.recommendation().symmetric) dl->AddLine(c, pt(h + 180, R - 14), IM_COL32(70, 230, 120, 120), 3.f);
        }
    }
    dl->AddCircleFilled(c, 5.f, IM_COL32(220, 226, 234, 255));
}

static void antennaTab(App& a) {
    DirectionFinder& d = a.dir;
    const DirectionFinder::State st = d.state();
    const bool running = a.engine.running();
    const bool inDevice = running && a.devices[a.devIdx].isRadio();
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetContentRegionAvail().y;
    const float leftW = std::max(300.f, w * 0.52f);

    ImGui::BeginChild("antL", ImVec2(leftW, h));
    if (st == DirectionFinder::State::Idle || st == DirectionFinder::State::Done) {
        ImGui::TextColored(ImVec4(0.7f, 0.76f, 0.84f, 1), "Which antenna do you use?");
        ImGui::Spacing();
        const char* names[3] = {"Directional (Yagi, log-periodic, panel)", "Dipole / rabbit ears / loop (picks up two opposite directions)", "Omnidirectional (same in all directions)"};
        for (int i = 0; i < 3; i++) if (ImGui::RadioButton(names[i], a.antKind == i)) a.antKind = i;
        ImGui::Spacing();
        ImGui::TextWrapped("%s", a.antKind == 0 ? "You will be asked to point the antenna north, east, south and west first, then the directions in between, then in smaller steps around the best one. Use your own north: a compass or phone app helps, but any fixed reference works."
                                  : a.antKind == 1 ? "The antenna hears two opposite ends equally, so the search finds the line to aim along. You will be asked to turn it to several headings."
                                  : "An omnidirectional antenna has no direction to find. Instead you will be asked to try different places (windowsill, higher up, another room), and the best one is picked.");
        ImGui::Spacing();
        if (!running) ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1), "Start the receiver on the channel you want to improve first.");
        else if (!inDevice) ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1), "This is a recording or the synthetic signal: turning an antenna will not change anything. Try it with a HackRF.");
        ImGui::BeginDisabled(!running);
        if (ImGui::Button("  Start  ", ImVec2(130 * gUi, 0))) { d.start((AntennaKind)a.antKind); a.dirAgcWas = false; }
        ImGui::EndDisabled();
        if (st == DirectionFinder::State::Done) {
            const DirRecommendation& r = d.recommendation();
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            if (r.valid && !r.flat && a.antKind != 2) {
                ImGui::PushFont(a.ui, 26);
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1), "Point at %s  (%.0f\xC2\xB0)", r.label.c_str(), r.heading);
                ImGui::PopFont();
                char cb[32]; snprintf(cb, sizeof cb, "confidence %.0f%%", r.confidence * 100);
                gaugePill(220, (float)r.confidence, r.confidence > 0.66 ? IM_COL32(40, 160, 90, 255) : r.confidence > 0.33 ? IM_COL32(200, 160, 40, 255) : IM_COL32(190, 70, 50, 255), cb);
            }
            ImGui::TextWrapped("%s", r.text.c_str());
        }
        if (!d.results().empty() && st == DirectionFinder::State::Done && ImGui::Button("Clear")) d.stop();
    } else {
        ImGui::PushFont(a.ui, 24);
        const std::string ins = d.instruction();
        ImGui::TextColored(st == DirectionFinder::State::WaitConfirm ? ImVec4(1, 0.82f, 0.25f, 1) : ImVec4(0.6f, 0.85f, 1, 1), "%s", ins.c_str());
        ImGui::PopFont();
        ImGui::Spacing();
        if (st == DirectionFinder::State::WaitConfirm) {
            ImGui::TextWrapped("Turn the antenna to this heading, hold it still, then press Measure. The receiver then listens for about %.0f seconds.", 14.0);
            ImGui::Spacing();
            if (ImGui::Button("  Measure  ", ImVec2(140 * gUi, 0))) { d.confirm(ImGui::GetTime()); if (a.agcOn) a.dirAgcWas = true; }
            ImGui::SameLine();
            if (ImGui::Button("Skip")) d.skip();
            if (d.kind() == AntennaKind::Omni && d.results().size() >= 2) { ImGui::SameLine(); if (ImGui::Button("That is enough: pick the best")) d.finishNow(); }
        } else {
            gaugePill(ImGui::GetContentRegionAvail().x - 8, (float)d.progress(ImGui::GetTime()), IM_COL32(52, 92, 108, 255), "measuring - do not touch the antenna");
            const QualityReport& q = a.quality.report();
            ImGui::Spacing();
            ImGui::Text("now: quality %.0f%%   SNR %.1f dB   %s", q.valid ? q.percent : 0.0, a.rx.dataValid ? a.rx.dataSnrDb : 0.f, a.rx.dataValid ? "locked" : "no lock");
            if (a.spec.stats.clipFraction > 0.005) ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.3f, 1), "ADC overload: lower the gain (AGC is paused during a measurement)");
        }
        ImGui::Spacing();
        if (ImGui::SmallButton("Stop and use what we have")) d.finishNow();
        ImGui::SameLine(); if (ImGui::SmallButton("Cancel")) d.stop();
        if (!running) { d.stop(); }
    }
    // results table
    ImGui::Spacing();
    if (!d.results().empty() && ImGui::BeginTable("dirres", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Heading"); ImGui::TableSetupColumn("Score"); ImGui::TableSetupColumn("SNR"); ImGui::TableSetupColumn("Lost"); ImGui::TableSetupColumn("Lock"); ImGui::TableSetupColumn("Multipath");
        ImGui::TableHeadersRow();
        const int best = d.recommendation().bestIndex;
        for (size_t i = 0; i < d.results().size(); i++) {
            const DirResult& r = d.results()[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (d.kind() == AntennaKind::Omni) ImGui::Text("%s", r.label.c_str()); else ImGui::Text("%s  %.0f\xC2\xB0", r.label.c_str(), r.heading);
            if ((int)i == best) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1), "best"); }
            ImGui::TableNextColumn(); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(scoreColour(r.score)), "%.0f", r.score);
            ImGui::TableNextColumn(); if (r.lockFraction > 0.3) ImGui::Text("%.1f dB", r.snrDb); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%.1f%%", r.lossPct);
            ImGui::TableNextColumn(); ImGui::Text("%.0f%%", r.lockFraction * 100);
            ImGui::TableNextColumn();
            const char* ml[] = {"-", "none", "mild", "likely", "severe"};
            const int mi = (int)r.multipath;
            ImGui::TextUnformatted(mi >= 0 && mi < 5 ? ml[mi] : "-");
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("antR", ImVec2(0, h));
    if (a.antKind == 2 || d.kind() == AntennaKind::Omni) {
        if (d.kind() == AntennaKind::Omni && !d.results().empty()) {
            for (const DirResult& r : d.results()) {
                char t[48]; snprintf(t, sizeof t, "%s  score %.0f", r.label.c_str(), r.score);
                gaugePill(ImGui::GetContentRegionAvail().x - 10, (float)r.score / 100.f, scoreColour(r.score), t);
            }
        } else ImGui::TextDisabled("Places you try will be compared here.");
    } else {
        compassRose(a, ImGui::GetContentRegionAvail());
    }
    ImGui::EndChild();
}


// Automatic bandwidth: follow what the engine detected; a change between 8 and 10 Msps (7/8 MHz vs narrower) needs a restart
static void followBandwidth(App& a) {
    a.engine.setBandwidthAuto(a.bwAuto && !a.atscMode);
    if (!a.bwAuto || a.atscMode || !a.engine.running()) return;
    const bool hw = a.devices[a.devIdx].isRadio();
    const double want = a.engine.activeBandwidth();
    if (want == a.tune.bandwidthMhz) return;
    int idx = -1;
    for (int k = 0; k < (int)(sizeof kBw / sizeof *kBw); k++) if (kBw[k].mhz == want) idx = k;
    if (idx < 0) return;
    const double oldRate = a.tune.sampleRate;
    a.bwIdx = idx;
    applyBandwidth(a);
    savePrefs(a);
    if (hw && a.tune.sampleRate != oldRate) {
        a.engine.log(a.tune.sampleRate > oldRate ? "restarting at 10 Msps" : "restarting at 8 Msps");
        a.engine.stop();
        a.engine.setComputeMode(a.computeMode); a.engine.setStandard(engineStd(a));
        a.engine.start(a.devices[a.devIdx], a.tune, a.file);
        a.smooth.clear(); a.peak.clear(); a.lastSeq = 0;
        a.mpd.reset(); a.quality.reset();
    }
}


// DVB <-> ATSC switch under the tuner settings
static void standardSwitch(App& a) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* names[3] = {"DVB", "ATSC", "DAB / DAB+"};
    const ImU32 cols[3] = {IM_COL32(52, 92, 108, 255), IM_COL32(150, 100, 30, 255), IM_COL32(40, 130, 96, 255)};
    const float h = ImGui::GetFrameHeight() - 2;
    const float segW[3] = {ImGui::CalcTextSize(names[0]).x + 22, ImGui::CalcTextSize(names[1]).x + 22, ImGui::CalcTextSize(names[2]).x + 22};
    const float total = segW[0] + segW[1] + segW[2];
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::AlignTextToFramePadding();
    // track, sliding knob under the selected segment, three labels
    dl->AddRectFilled(p, ImVec2(p.x + total, p.y + h), IM_COL32(18, 22, 28, 255), 3.f);
    dl->AddRect(p, ImVec2(p.x + total, p.y + h), IM_COL32(52, 60, 72, 255), 3.f);
    float x = p.x;
    float selX = p.x, selW = segW[0];
    for (int i = 0; i < 3; i++) { if (i == a.family) { selX = x; selW = segW[i]; } x += segW[i]; }
    // animate the knob
    static float knobX = -1, knobW = 0;
    if (knobX < 0) { knobX = selX - p.x; knobW = selW; }
    knobX += (selX - p.x - knobX) * 0.35f; knobW += (selW - knobW) * 0.35f;
    dl->AddRectFilled(ImVec2(p.x + knobX + 2, p.y + 2), ImVec2(p.x + knobX + knobW - 2, p.y + h - 2), cols[a.family], 3.f);
    x = p.x;
    for (int i = 0; i < 3; i++) {
        ImGui::SetCursorScreenPos(ImVec2(x, p.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(segW[i], h))) {
            if (a.engine.running()) a.engine.log("stop the receiver before switching between DVB, ATSC and DAB");
            else if (i != a.family) { setFamily(a, i); savePrefs(a); }
        }
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImVec2 ts = ImGui::CalcTextSize(names[i]);
        dl->AddText(ImVec2(x + (segW[i] - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), i == a.family ? IM_COL32(255, 255, 255, 255) : hov ? IM_COL32(220, 228, 236, 255) : IM_COL32(140, 152, 166, 255), names[i]);
        x += segW[i];
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + total + 14, p.y));
    ImGui::AlignTextToFramePadding();
    if (a.family == 1) ImGui::TextDisabled("ATSC 8-VSB, 6 MHz channel (the DVB-only settings are off)");
    else if (a.family == 2) ImGui::TextDisabled("DAB / DAB+ digital radio, Band III channels 5A to 13F");
    else ImGui::TextDisabled("DVB-T2 / DVB-T, detected automatically");
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + ImGui::GetStyle().ItemSpacing.y));
    if (ImGui::IsMouseHoveringRect(p, ImVec2(p.x + total, p.y + h))) ImGui::SetTooltip("Left: DVB (T2 and T, automatic).\nMiddle: ATSC (US, Canada, Mexico, South Korea).\nRight: DAB / DAB+ digital radio.");
}

#include "dab_ui.inc"

#include "wizard.inc"

static void drawUI(App& a, ImVec2 disp) {
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
    ImGui::SameLine(disp.x - 64);
    if (ImGui::SmallButton("Tour")) { a.wizOpen = true; a.wizX = -1; a.wizStep = 0; a.wizStepT = ImGui::GetTime(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Take the guided tour with Onny");
    a.tgMin[TgSwitch] = ImVec2(sw0.x - 2, sw0.y - 2); a.tgMax[TgSwitch] = ImVec2(sw0.x + 330, ImGui::GetCursorScreenPos().y);
    ImGui::Separator();
    statusBar(a);
    ImGui::Separator();

    float logH = 250 * gUi, rightW = 350 * gUi;
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

    ImGui::Separator();
    ImGui::BeginChild("constellations", ImVec2(0, 0));
    a.tgMin[TgConst] = ImGui::GetWindowPos(); a.tgMax[TgConst] = ImVec2(a.tgMin[TgConst].x + ImGui::GetWindowSize().x, a.tgMin[TgConst].y + ImGui::GetWindowSize().y);
    constellationsTab(a);
    ImGui::EndChild();
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

// ------------------------------------------------------------------ main

int main(int argc, char** argv) {
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
    ImPlot::CreateContext();
    gfx::Backend* gfxBackend = gfx::create(window);
    if (!gfxBackend) { fprintf(stderr, "could not start the graphics back end\n"); return 1; }
    gGfx = gfxBackend;

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    applyTheme();
    static ImGuiStyle baseStyle = ImGui::GetStyle();   // the style at scale 1
    static ImPlotStyle basePlotStyle = ImPlot::GetStyle();

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
                    ImPlotStyle& ps = ImPlot::GetStyle();
                    ps = basePlotStyle;
                    ps.PlotPadding = ImVec2(basePlotStyle.PlotPadding.x * s, basePlotStyle.PlotPadding.y * s);
                    ps.LabelPadding = ImVec2(basePlotStyle.LabelPadding.x * s, basePlotStyle.LabelPadding.y * s);
                    ps.LegendPadding = ImVec2(basePlotStyle.LegendPadding.x * s, basePlotStyle.LegendPadding.y * s);
                    ps.MinorTickLen = ImVec2(basePlotStyle.MinorTickLen.x * s, basePlotStyle.MinorTickLen.y * s);
                    ps.MajorTickLen = ImVec2(basePlotStyle.MajorTickLen.x * s, basePlotStyle.MajorTickLen.y * s);
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
                if (app.devices[app.devIdx].kind == DeviceInfo::Soapy) app.tune.gainDb = 20 + 5 * (stressStep % 5);
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
            gfxBackend->endFrame(ImGui::GetDrawData(), shotNow ? shotPath : nullptr);
            if (shotNow) {
                { PlayerStats ps = app.engine.player().stats(); { BbStats bb = app.engine.bbStats(); fprintf(stderr, "stream: BB frames %llu lost %llu, PLP frames dropped by busy decoder %llu, FEC blocks ok %llu bad %llu, samples dropped %llu, CPU decode %.0f ms\n", (unsigned long long)bb.frames, (unsigned long long)bb.framesLost, (unsigned long long)app.rx.plpFramesDropped, (unsigned long long)app.rx.blocksOk, (unsigned long long)app.rx.blocksBad, (unsigned long long)app.engine.droppedSamples(), app.rx.plpDecodeMs); }
                fprintf(stderr, "player: %s hw=%d %s %dx%d decoded %llu shown %llu late %llu errors %llu audio %s ch %d buf %.0f ms underruns %d A/V %+.0f ms\n", ps.status.c_str(), ps.hardware, ps.videoCodec.c_str(), ps.width, ps.height, (unsigned long long)ps.decoded, (unsigned long long)ps.shown, (unsigned long long)ps.late, (unsigned long long)ps.errors, ps.audioCodec.c_str(), ps.audioChannels, ps.audioBufferMs, ps.underruns, ps.avOffsetMs); }
                glfwSetWindowShouldClose(window, 1);
            }
        }
    }
    app.engine.stop();
    gfxBackend->shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
