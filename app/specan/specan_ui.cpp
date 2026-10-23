// Spectrum analyser screens: the sweep controls (range and presets, resolution, gains, averaging), one wide chart with live, peak-hold
// and averaged traces or a persistence display, the band allocations in a strip above the chart (height from the font), a hover read-out, a marker and a delta marker,
// the waterfall of the sweeps below, and CSV export of the current sweep.
#include "dect2/common/thread_name.h"
#include "app.h"
#include "dect2/specan/burst_monitor.h"
#include "dect2/specan/leo_detect.h"
#include "dect2/specan/geo_look.h"
#include "dect2/specan/specan_dsp.h"
#include "dect2/specan/specan_rx.h"
#include "dect2/specan/specan_tel.h"
#include "dect2/specan/specan_res.h"
#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    float loMhz = 1, hiMhz = 6000;
    int preset = 0;
    double rbwHz = 100e3;         // the resolution asked for: one of the bin widths the rate allows (dect2::saBinChoices)
    int avgN = 8;
    bool showLive = true, showPeak = true, showAvg = false, persist = false, bands = true, spurs = true;
    float refDb = 0, rangeDb = 100;
    double m1 = 0, m2 = 0;        // markers, Hz (0 none)
    uint64_t seenSeq = 0;
    // waterfall and persistence (display columns)
    static constexpr int kCols = 1024, kRows = 160, kLevels = 100;
    std::vector<uint32_t> wf, ps;
    std::vector<float> hist;      // kCols x kLevels hit counts (decaying)
    gfx::Image* wfImg = nullptr;
    gfx::Image* psImg = nullptr;
    bool wfDirty = false, psDirty = false;
    std::string note;
    bool applied = false;
    // automatic CSV saving
    bool autoOn = false;
    int autoSecs = 60, autoKeep = 0;          // keep 0 = all
    char autoDir[512] = "";
    double autoNext = 0;                      // steady time of the next save
    int autoCount = 0;
    std::string autoLast;                     // text: when the last file was saved
    std::vector<std::string> autoFiles;
    std::atomic<bool> autoBusy{false};
    std::thread autoTh;
    bool prefsLoaded = false;
    // burst monitor: one level per sweep at the watched frequency
    bool monOn = false, monOpen = false;
    double monMhz = 20.1;
    float monRiseDb = 6, monMinMs = 200;
    dect2::BurstDetector mon;
    float monLast = -200, monBase = -200;
    struct MonEvent { std::time_t wall; double mhz; dect2::BurstEvent ev; };
    std::vector<MonEvent> monLog;
    // LEO monitor (detect only): the watched slice of each sweep, and the carriers found in it
    bool leoOn = false;
    double leoMhz = 137.5;        // centre of the watched slice
    float leoSpanKhz = 200;       // width of the watched slice
    std::vector<dect2::LeoFrame> leoFrames;
    double leoStartHz = 0, leoBinHz = 0;   // grid the frames were taken on
    size_t leoBins = 0;
    double leoLastRun = 0;        // ms, last detector run
    std::vector<dect2::LeoCarrier> leoFound;
    // dish alignment (WIP): beacon level at a watched IF, peak hold, 60 s history; look-angle helper
    double dishMhz = 1000;
    float dishLevel = -200, dishPeak = -200;
    std::vector<std::pair<double, float>> dishHist;   // (steady seconds, dB)
    double latMine = 0, lonMine = 0, lonSat = 19.2;
};
State S;
std::mutex& autoMu() { static std::mutex m; return m; }
// a bin width as text: the real value, since the FFT size is a power of two and only some widths exist
std::string rbwText(double hz) {
    char t[32];
    if (hz >= 1e6) snprintf(t, sizeof t, "%.2f MHz", hz / 1e6);
    else if (hz >= 100e3) snprintf(t, sizeof t, "%.0f kHz", hz / 1e3);
    else if (hz >= 1e3) snprintf(t, sizeof t, "%.2f kHz", hz / 1e3);
    else snprintf(t, sizeof t, "%.0f Hz", hz);
    return t;
}

std::string sizeText(size_t b) {
    char t[32];
    snprintf(t, sizeof t, b >= (1u << 20) ? "%.1f MB" : "%.0f kB", b >= (1u << 20) ? b / 1048576.0 : b / 1024.0);
    return t;
}

double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::string defaultDir() {
    const char* h = getenv("HOME");
    if (!h) h = getenv("USERPROFILE");
    return std::string(h ? h : ".") + "/SpectrumSweeps";
}

void loadAutoPrefs() {
    if (S.prefsLoaded) return;
    S.prefsLoaded = true;
    auto& p = plat::prefs();
    S.autoSecs = (int)p.getI("specanAutoSecs", 60);
    S.autoKeep = (int)p.getI("specanAutoKeep", 0);
    snprintf(S.autoDir, sizeof S.autoDir, "%s", p.getS("specanAutoDir", defaultDir()).c_str());
}

void saveAutoPrefs() {
    auto& p = plat::prefs();
    p.setI("specanAutoSecs", S.autoSecs); p.setI("specanAutoKeep", S.autoKeep); p.setS("specanAutoDir", S.autoDir);
    p.flush();
}

// One automatic save: the sweep is copied here, the file is written on a worker thread (the UI keeps running)
void autoSave(const App& a) {
    if (S.autoBusy.load()) return;
    if (S.autoTh.joinable()) S.autoTh.join();
    const auto& t = a.rx.specan;
    const dect2::SaGrid g = dect2::saMakeGrid(t.loHz, t.loHz + t.binHz * t.live.size(), t.binHz);
    char nm[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(nm, sizeof nm, "specan_%Y%m%d_%H%M%S.csv", std::localtime(&now));
    std::error_code ec;
    std::filesystem::create_directories(S.autoDir, ec);
    const std::string path = std::string(S.autoDir) + "/" + nm;
    char when[32];
    snprintf(when, sizeof when, "%s", dect2::timefmt::clock((int64_t)now).c_str());
    S.autoBusy = true;
    S.autoTh = dect2::namedThread("onair-specan", [g, live = t.live, peak = t.peak, avg = t.avg, path, w = std::string(when)] {
        const bool ok = dect2::saCsvWriteFile(path, g, live, peak, avg);
        std::lock_guard<std::mutex> lk(autoMu());
        if (ok) { S.autoCount++; S.autoLast = w; S.autoFiles.push_back(path); S.note = "auto-saved " + path; }
        else S.note = "auto-save failed: " + path;
        S.autoBusy = false;
    });
}

bool live(const App& a) { return a.rx.standard == 43 && !a.rx.specan.live.empty(); }

void pushSettings(App& a, bool rangeChanged) {
    auto& r = a.engine.specan();
    r.setRange(S.loMhz * 1e6, S.hiMhz * 1e6);
    r.setBinHz(S.rbwHz);
    r.setAveraging(S.avgN);
    r.setRemoveSpurs(S.spurs);
    a.tune.synth.modeVal[0] = S.loMhz;
    a.tune.synth.modeVal[1] = S.hiMhz;
    // the HackRF sweeps the range it was started with: start it again over the new one
    if (rangeChanged && a.engine.running() && a.devices[a.devIdx].kind == DeviceInfo::HackRF) startReceiver(a);
}

// trace value of display column c (max over the bins it covers)
float colMax(const std::vector<float>& v, const dect2::SpecanTelemetry& t, double lo, double hi, int c, int cols) {
    if (v.empty() || t.binHz <= 0) return -200;
    const double f0 = lo + (hi - lo) * c / cols, f1 = lo + (hi - lo) * (c + 1) / cols;
    long k0 = (long)((f0 - t.loHz) / t.binHz), k1 = (long)((f1 - t.loHz) / t.binHz);
    k0 = std::max(0L, k0); k1 = std::min((long)v.size() - 1, std::max(k0, k1));
    float m = -200;
    for (long k = k0; k <= k1; k++) m = std::max(m, v[(size_t)k]);
    return m;
}

void applyMonitor() {
    dect2::BurstConfig c;
    c.riseDb = S.monRiseDb;
    c.minMs = S.monMinMs;
    S.mon = dect2::BurstDetector(c);
    S.monLast = S.monBase = -200;
}

// The level in the bin nearest the watched frequency, from this sweep, goes to the burst detector.
void feedMonitor(const dect2::SpecanTelemetry& t) {
    if (!S.monOn || t.binHz <= 0 || t.live.empty()) return;
    const double k = (S.monMhz * 1e6 - t.loHz) / t.binHz;
    if (k < 0 || k >= (double)t.live.size()) { S.monLast = -200; return; }
    const float v = t.live[(size_t)k];
    S.monLast = v;
    dect2::BurstEvent ev;
    if (S.mon.push(v, nowS() * 1000.0, ev)) {
        S.monLog.push_back({std::time(nullptr), S.monMhz, ev});
        if (S.monLog.size() > 200) S.monLog.erase(S.monLog.begin());
    }
    S.monBase = S.mon.lastBaseDb();
}

// LEO monitor: keep the bins of the watched slice from each sweep (last two minutes), and run the detector about once a second.
// A change of grid (centre, span, resolution or sweep range) starts the frames again, because the detector needs one grid.
void feedLeo(const dect2::SpecanTelemetry& t) {
    if (!S.leoOn || t.binHz <= 0 || t.live.empty()) return;
    const double c0 = S.leoMhz * 1e6 - S.leoSpanKhz * 500.0, c1 = c0 + S.leoSpanKhz * 1e3;
    long k0 = (long)std::ceil((c0 - t.loHz) / t.binHz), k1 = (long)std::floor((c1 - t.loHz) / t.binHz);
    k0 = std::max(0L, k0);
    k1 = std::min((long)t.live.size() - 1, k1);
    if (k1 < k0) return;   // the watched slice is outside the sweep range
    const double start = t.loHz + (double)k0 * t.binHz;
    const size_t bins = (size_t)(k1 - k0 + 1);
    if (start != S.leoStartHz || t.binHz != S.leoBinHz || bins != S.leoBins) {
        S.leoFrames.clear();
        S.leoFound.clear();
        S.leoStartHz = start;
        S.leoBinHz = t.binHz;
        S.leoBins = bins;
    }
    dect2::LeoFrame f;
    f.tMs = nowS() * 1000.0;
    f.db.assign(t.live.begin() + k0, t.live.begin() + k1 + 1);
    S.leoFrames.push_back(std::move(f));
    const double cut = S.leoFrames.back().tMs - 120000.0;
    while (!S.leoFrames.empty() && (S.leoFrames.front().tMs < cut || S.leoFrames.size() > 600)) S.leoFrames.erase(S.leoFrames.begin());
    if (S.leoFrames.back().tMs - S.leoLastRun >= 1000.0) {
        S.leoLastRun = S.leoFrames.back().tMs;
        S.leoFound = dect2::detectDopplerCarriers(S.leoFrames, dect2::LeoBand{S.leoStartHz, S.leoBinHz});
    }
}

// Dish alignment: the largest bin within +-5 kHz of the beacon, from this sweep, goes to the peak hold and the 60 s history.
void feedDish(const dect2::SpecanTelemetry& t) {
    if (t.binHz <= 0 || t.live.empty()) return;
    const double c = (S.dishMhz * 1e6 - t.loHz) / t.binHz, w = 5e3 / t.binHz;
    const long last = (long)t.live.size() - 1;
    if (c + w < 0 || c - w > (double)last) { S.dishLevel = -200; return; }
    const long k0 = std::max(0L, (long)std::floor(c - w)), k1 = std::min(last, (long)std::ceil(c + w));
    float v = -200;
    for (long k = k0; k <= k1; k++) v = std::max(v, t.live[(size_t)k]);
    S.dishLevel = v;
    if (v <= -199) return;
    const double now = nowS();
    S.dishHist.push_back({now, v});
    S.dishPeak = std::max(S.dishPeak, v);
    size_t drop = 0;
    while (drop < S.dishHist.size() && now - S.dishHist[drop].first > 60.0) drop++;
    if (drop) S.dishHist.erase(S.dishHist.begin(), S.dishHist.begin() + (long)drop);
}

void ingest(App& a) {
    const auto& t = a.rx.specan;
    if (!live(a) || t.seq == S.seenSeq) return;
    S.seenSeq = t.seq;
    const double lo = t.loHz, hi = t.loHz + t.binHz * t.live.size();
    const int W = State::kCols;
    if ((int)S.wf.size() != W * State::kRows) { S.wf.assign((size_t)W * State::kRows, 0xFF000000u); S.ps.assign((size_t)W * State::kLevels, 0xFF000000u); S.hist.assign((size_t)W * State::kLevels, 0.f); }
    std::copy_backward(S.wf.begin(), S.wf.end() - W, S.wf.end());
    const float bot = S.refDb - S.rangeDb;
    for (auto& h : S.hist) h *= 0.92f;
    for (int c = 0; c < W; c++) {
        const float v = colMax(t.live, t, lo, hi, c, W);
        S.wf[(size_t)c] = Waterfall::jet(std::clamp((v - bot) / S.rangeDb, 0.f, 1.f));
        const int lv = (int)std::clamp((S.refDb - v) / S.rangeDb * State::kLevels, 0.f, State::kLevels - 1.f);
        S.hist[(size_t)lv * W + c] += 1.f;
    }
    for (size_t i = 0; i < S.hist.size(); i++) {
        const float h = std::min(1.f, S.hist[i] / 6.f);
        S.ps[i] = h <= 0.01f ? 0xFF000000u : Waterfall::jet(h);
    }
    S.wfDirty = S.psDirty = true;
    feedMonitor(t);
    feedLeo(t);
    feedDish(t);
}

std::string mhzText(double hz) {
    char b[32];
    snprintf(b, sizeof b, std::fabs(hz) >= 1e9 ? "%.4f MHz" : "%.3f MHz", hz / 1e6);
    return b;
}

void controls(App& a) {
    int np = 0;
    const dect2::SaPreset* pr = dect2::saPresets(np);
    bool range = false, other = false;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Range");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150 * gUi);
    if (ImGui::BeginCombo("##sapre", S.preset >= 0 ? pr[S.preset].name : "custom")) {
        for (int i = 0; i < np; i++) if (ImGui::Selectable(pr[i].name, S.preset == i)) { S.preset = i; S.loMhz = (float)pr[i].loMhz; S.hiMhz = (float)pr[i].hiMhz; range = true; }
        ImGui::EndCombo();
    }
    flowNext(6 * gUi);
    ImGui::SetNextItemWidth(80 * gUi);
    ImGui::InputFloat("##salo", &S.loMhz, 0, 0, "%.1f");
    if (ImGui::IsItemDeactivatedAfterEdit()) { S.preset = -1; range = true; }
    ImGui::SameLine(); ImGui::TextDisabled("to"); ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * gUi);
    ImGui::InputFloat("MHz##sahi", &S.hiMhz, 0, 0, "%.1f");
    if (ImGui::IsItemDeactivatedAfterEdit()) { S.preset = -1; range = true; }
    flowNext(10 * gUi);
    ImGui::TextDisabled("RBW"); ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    {
        const auto& tt = a.rx.specan;
        const bool nat = tt.inputRate > 0 ? tt.native : true;   // before the first sweep: the HackRF's own
        // #85: round widths. The FFT bins are a power-of-two fraction of the rate; the output grid takes any width at or above the finest
        // bin (specan_rx: saMakeGrid at max(asked, bin)), so 3, 5, 10, 25 kHz ... are exact. Below the finest bin, the finest bin is offered.
        const std::vector<double> fine = dect2::saBinChoices(tt.inputRate, nat);
        const double finest = fine.empty() ? 0 : fine.back();
        std::vector<double> ch;
        if (finest > 0) ch.push_back(finest);
        for (double h : {1e3, 2e3, 3e3, 5e3, 10e3, 25e3, 50e3, 100e3, 250e3, 500e3, 1e6}) if (h > finest * 1.01) ch.push_back(h);
        const double applied = std::max(S.rbwHz, dect2::saPickRes(S.rbwHz, tt.inputRate, nat).binHz);
        if (ImGui::BeginCombo("##sarbw", rbwText(applied).c_str())) {
            for (double h : ch) if (ImGui::Selectable(rbwText(h).c_str(), std::fabs(h - applied) < 1e-6 * h)) { S.rbwHz = h; other = true; }
            ImGui::Separator();
            static float customKhz = 0;
            if (ImGui::IsWindowAppearing()) customKhz = (float)(applied / 1e3);
            ImGui::SetNextItemWidth(80 * gUi);
            ImGui::InputFloat("kHz##sarbwc", &customKhz, 0, 0, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit() && customKhz > 0) { S.rbwHz = std::max((double)customKhz * 1e3, finest); other = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Any width at or above the finest bin (%s here); press Enter", rbwText(finest).c_str());
            ImGui::EndCombo();
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Resolution: the width of each point of the trace and of the CSV export (exact; any width from the finest FFT bin up). The HackRF sweep: 20 MHz / 4096 = 4.88 kHz is the finest; the finest bins are slow over a wide range");
    flowNext(10 * gUi);
    ImGui::TextDisabled("Avg"); ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * gUi);
    if (ImGui::SliderInt("##saavg", &S.avgN, 1, 64)) other = true;
    flowNext(10 * gUi);
    if (ImGui::Checkbox("Remove spurs", &S.spurs)) other = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Take out the radio's DC spike / LO leak at the centre of every tuning step");
    flowEnd();
    // gains: the HackRF's three stages (other radios: the overall gain of the top bar)
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("LNA"); ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    bool g = ImGui::SliderInt("##salna", &a.tune.lnaDb, 0, 40, "%d dB");
    if (g) a.tune.lnaDb = a.tune.lnaDb / 8 * 8;
    flowNext(6 * gUi);
    ImGui::TextDisabled("VGA"); ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::SliderInt("##savga", &a.tune.vgaDb, 0, 62, "%d dB")) { a.tune.vgaDb = a.tune.vgaDb / 2 * 2; g = true; }
    flowNext(6 * gUi);
    g |= ImGui::Checkbox("Amp", &a.tune.ampOn);
    if (g && a.engine.running()) a.engine.retune(a.tune);
    flowNext(12 * gUi);
    ImGui::Checkbox("Live", &S.showLive); ImGui::SameLine();
    ImGui::Checkbox("Peak", &S.showPeak); ImGui::SameLine();
    ImGui::Checkbox("Average", &S.showAvg); ImGui::SameLine();
    ImGui::Checkbox("Persistence", &S.persist); ImGui::SameLine();
    ImGui::Checkbox("Bands", &S.bands);
    flowNext(10 * gUi);
    if (ImGui::SmallButton("Reset peak")) a.engine.specan().resetPeak();
    ImGui::SameLine();
    {
        const auto& t = a.rx.specan;
        const size_t bins = t.live.size();
        if (ImGui::SmallButton("Export CSV") && live(a)) {
            char nm[64];
            const std::time_t now = std::time(nullptr);
            std::strftime(nm, sizeof nm, "sweep_%Y%m%d_%H%M%S.csv", std::localtime(&now));
            const std::string path = saveFileDialog(nm);
            if (!path.empty())
                S.note = dect2::saCsvWriteFile(path, dect2::saMakeGrid(t.loHz, t.loHz + t.binHz * bins, t.binHz), t.live, t.peak, t.avg) ? "saved " + path : "could not write " + path;
        }
        if (live(a) && bins) {
            const size_t est = dect2::saCsvEstimateBytes(bins);
            ImGui::SameLine();
            ImGui::TextDisabled("%zu bins of %.1f kHz, about %s%s", bins, t.binHz / 1e3, sizeText(est).c_str(), est > (6u << 20) ? " (large)" : "");
        }
    }
    if (!S.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.note.c_str()); }
    flowEnd();
    loadAutoPrefs();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Auto-save CSV every");
    ImGui::SameLine();
    bool pc = false;
    ImGui::SetNextItemWidth(70 * gUi);
    if (ImGui::InputInt("s##saasec", &S.autoSecs, 0, 0)) { S.autoSecs = std::max(1, S.autoSecs); pc = true; }
    flowNext(6 * gUi);
    ImGui::TextDisabled("keep"); ImGui::SameLine();
    ImGui::SetNextItemWidth(60 * gUi);
    if (ImGui::InputInt("files (0 = all)##saakeep", &S.autoKeep, 0, 0)) { S.autoKeep = std::max(0, S.autoKeep); pc = true; }
    flowNext(6 * gUi);
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::InputText("##saadir", S.autoDir, sizeof S.autoDir)) pc = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Folder...")) {   // pick any file name inside the wanted folder
        const std::string p = saveFileDialog("specan_folder.csv");
        if (!p.empty()) { const auto d = std::filesystem::path(p).parent_path().string(); snprintf(S.autoDir, sizeof S.autoDir, "%s", d.c_str()); pc = true; }
    }
    if (pc) saveAutoPrefs();
    flowNext(6 * gUi);
    if (ImGui::Checkbox("Auto-save on", &S.autoOn)) { S.autoNext = nowS(); if (S.autoOn) S.autoCount = 0, S.autoFiles.clear(); }
    {
        std::lock_guard<std::mutex> lk(autoMu());
        if (S.autoCount || S.autoOn) { ImGui::SameLine(); ImGui::TextDisabled("%d saved, last %s", S.autoCount, S.autoLast.empty() ? "-" : S.autoLast.c_str()); }
    }
    flowEnd();
    if (S.autoOn && live(a) && !S.autoBusy.load() && nowS() >= S.autoNext) {
        S.autoNext = nowS() + S.autoSecs;
        autoSave(a);
    }
    if (S.autoKeep > 0 && !S.autoBusy.load()) {   // trim the oldest of the files this run wrote
        std::lock_guard<std::mutex> lk(autoMu());
        while ((int)S.autoFiles.size() > S.autoKeep) { std::error_code ec; std::filesystem::remove(S.autoFiles.front(), ec); S.autoFiles.erase(S.autoFiles.begin()); }
    }
    if (range || other || !S.applied) { if (S.hiMhz < S.loMhz + 0.1f) S.hiMhz = S.loMhz + 0.1f; pushSettings(a, range); S.applied = true; }
}

void chart(App& a, float h) {
    const auto& t = a.rx.specan;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float axisW = 44 * gUi, lh = ImGui::GetTextLineHeight();
    const float bandH = S.bands ? bandPlanStripHeight() : 0;
    const float l = p0.x + axisW, r = p0.x + w - 4 * gUi, top = p0.y + bandH + lh / 2 + 2;
    const float bot = p0.y + h - lh - 6 * gUi;
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(10, 11, 13, 255));
    const double lo = on ? t.loHz : S.loMhz * 1e6, hi = on ? t.loHz + t.binHz * t.live.size() : S.hiMhz * 1e6;
    auto X = [&](double hz) { return (float)(l + (hz - lo) / (hi - lo) * (r - l)); };
    auto Y = [&](float db) { return top + (S.refDb - db) / S.rangeDb * (bot - top); };
    const ImU32 grid = IM_COL32(40, 44, 50, 255), txt = IM_COL32(130, 136, 144, 255);
    char b[64];
    const float dStep = (bot - top) * 10 / S.rangeDb > lh * 1.3f ? 10.f : (bot - top) * 20 / S.rangeDb > lh * 1.3f ? 20.f : 50.f;
    for (float d = S.refDb; d >= S.refDb - S.rangeDb; d -= dStep) {
        dl->AddLine(ImVec2(l, Y(d)), ImVec2(r, Y(d)), grid);
        snprintf(b, sizeof b, "%.0f", d);
        dl->AddText(ImVec2(p0.x + 4, Y(d) - lh / 2), txt, b);
    }
    const double stepMhz = dect2::saTickStep(lo / 1e6, hi / 1e6, std::max(2, (int)((r - l) / (80 * gUi))));
    for (double f = std::ceil(lo / 1e6 / stepMhz) * stepMhz; f <= hi / 1e6 + 1e-9; f += stepMhz) {
        const float x = X(f * 1e6);
        dl->AddLine(ImVec2(x, top), ImVec2(x, bot), grid);
        snprintf(b, sizeof b, stepMhz < 0.1 ? "%.3f" : stepMhz < 1 ? "%.1f" : "%.0f", f);
        const float tw = ImGui::CalcTextSize(b).x;
        dl->AddText(ImVec2(std::clamp(x - tw / 2, p0.x, p0.x + w - tw - 2), bot + 3 * gUi), txt, b);
    }
    if (S.bands) drawBandPlanStrip(dl, l, r, p0.y, bandH, lo / 1e6, hi / 1e6);
    if (!on) {
        dl->AddText(ImVec2(l + 10, top + 10), txt, a.engine.running() ? "sweeping ..." : "start the receiver to sweep");
    } else {
        const int cols = std::max(2, (int)(r - l));
        if (S.persist && S.psImg) dl->AddImage(S.psImg->texture(), ImVec2(l, top), ImVec2(r, bot));
        auto trace = [&](const std::vector<float>& v, ImU32 c) {
            std::vector<ImVec2> pts((size_t)cols);
            for (int i = 0; i < cols; i++) pts[(size_t)i] = ImVec2(l + i, std::clamp(Y(colMax(v, t, lo, hi, i, cols)), top, bot));
            dl->AddPolyline(pts.data(), cols, c, 0, 1.2f * gUi);
        };
        if (S.showAvg) trace(t.avg, IM_COL32(120, 230, 120, 255));
        if (S.showPeak) trace(t.peak, IM_COL32(255, 170, 60, 200));
        if (S.showLive && !S.persist) trace(t.live, IM_COL32(110, 190, 255, 255));
        auto val = [&](double hz) { const long k = (long)((hz - t.loHz) / t.binHz); return k >= 0 && k < (long)t.live.size() ? t.live[(size_t)k] : -200.f; };
        auto marker = [&](double hz, ImU32 c, const char* nm) {
            if (hz <= 0 || hz < lo || hz > hi) return;
            const float x = X(hz), y = std::clamp(Y(val(hz)), top, bot);
            dl->AddLine(ImVec2(x, top), ImVec2(x, bot), c);
            dl->AddTriangleFilled(ImVec2(x - 5, y - 9), ImVec2(x + 5, y - 9), ImVec2(x, y), c);
            dl->AddText(ImVec2(x + 4, top + 2), c, nm);
        };
        marker(S.m1, IM_COL32(255, 240, 100, 255), "M1");
        marker(S.m2, IM_COL32(255, 120, 200, 255), "M2");
        float yy = top + 2;
        if (S.m1 > 0) { snprintf(b, sizeof b, "M1 %s  %.1f dBFS", mhzText(S.m1).c_str(), val(S.m1)); dl->AddText(ImVec2(r - 260 * gUi, yy), IM_COL32(255, 240, 100, 255), b); yy += lh; }
        if (S.m1 > 0 && S.m2 > 0) {
            snprintf(b, sizeof b, "M2-M1 %+.3f MHz  %+.1f dB", (S.m2 - S.m1) / 1e6, val(S.m2) - val(S.m1));
            dl->AddText(ImVec2(r - 260 * gUi, yy), IM_COL32(255, 120, 200, 255), b);
        }
    }
    ImGui::InvisibleButton("##sachart", ImVec2(w, h));
    if (on && ImGui::IsItemHovered()) {
        const float mx = ImGui::GetIO().MousePos.x;
        const double hz = lo + (mx - l) / std::max(1.f, r - l) * (hi - lo);
        if (mx >= l && mx <= r && ImGui::GetIO().MousePos.y >= top) {
            const long k = (long)((hz - t.loHz) / t.binHz);
            const float v = k >= 0 && k < (long)t.live.size() ? t.live[(size_t)k] : -200;
            const float pk = k >= 0 && k < (long)t.peak.size() ? t.peak[(size_t)k] : -200;
            const dect2::SaBand* bd = dect2::saAllocationAt(hz / 1e6);
            ImGui::SetTooltip("%s\n%.1f dBFS (peak %.1f)%s%s%s\nclick: marker 1, shift-click: marker 2, right-click: clear", mhzText(hz).c_str(), v, pk,
                              bd ? "\n" : "", bd ? bd->name : "", bd ? (std::string(" - ") + bd->use).c_str() : "");
            if (ImGui::IsMouseClicked(0)) { if (ImGui::GetIO().KeyShift) S.m2 = hz; else S.m1 = hz; }
            if (ImGui::IsMouseClicked(1)) S.m1 = S.m2 = 0;
        }
        if (ImGui::GetIO().MouseWheel != 0) S.refDb = std::clamp(S.refDb + ImGui::GetIO().MouseWheel * 5, -100.f, 30.f);
    }
}

// LEO monitor (WIP, detect only): carriers in the watched slice whose frequency drifts like a satellite downlink.
void leoMonitor(App& a) {
    if (!ImGui::CollapsingHeader("LEO monitor (WIP, detect only)")) return;
    ImGui::TextWrapped("Detect only: lists carriers that drift like a satellite downlink. Nothing is decoded.");
    bool changed = ImGui::Checkbox("Watch##leo", &S.leoOn);
    ImGui::SameLine();
    if (ImGui::SmallButton("137 MHz")) { S.leoMhz = 137.5; changed = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("1.5 GHz")) { S.leoMhz = 1500.0; changed = true; }
    ImGui::SameLine();
    changed |= ImGui::InputDouble("MHz##leo", &S.leoMhz, 0.01, 1.0, "%.3f");
    ImGui::SameLine();
    changed |= ImGui::InputFloat("Span (kHz)##leo", &S.leoSpanKhz, 10.f, 100.f, "%.0f");
    if (changed) { S.leoFrames.clear(); S.leoFound.clear(); S.leoLastRun = 0; }
    if (!live(a)) ImGui::TextDisabled("Needs a running sweep that covers the watched slice.");
    else if (S.leoFrames.empty()) ImGui::TextDisabled(S.leoOn ? "Waiting for sweeps over the watched slice." : "Tick Watch to collect sweeps.");
    else ImGui::Text("%zu sweeps over %.0f s, %zu carriers", S.leoFrames.size(), (S.leoFrames.back().tMs - S.leoFrames.front().tMs) / 1000.0, S.leoFound.size());
    if (ImGui::SmallButton("Clear##leo")) { S.leoFrames.clear(); S.leoFound.clear(); S.leoLastRun = 0; }
    ImGui::SameLine();
    ImGui::TextDisabled("First seen        MHz      Drift Hz/s  Duration s");
    const std::time_t now = std::time(nullptr);
    const double nowMs = nowS() * 1000.0;
    for (const auto& c : S.leoFound) {
        const std::time_t seen = now - (std::time_t)((nowMs - c.firstSeenMs) / 1000.0);
        char when[16] = "";
        snprintf(when, sizeof when, "%s", dect2::timefmt::clock((int64_t)seen).c_str());
        ImGui::Text("%s  %10.3f  %+11.1f  %11.1f", when, c.carrierHz / 1e6, c.driftHzPerS, c.durationMs / 1000.0);
    }
}

void waterfall(App& a, float h) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(6, 7, 8, 255));
    if (live(a) && S.wfImg) dl->AddImage(S.wfImg->texture(), ImVec2(p0.x + 44 * gUi, p0.y), ImVec2(p0.x + w - 4 * gUi, p0.y + h));
    ImGui::Dummy(ImVec2(w, h));
}

// Burst monitor: watch one frequency sweep by sweep; a rise of Rise dB above the running median for at least Minimum ms is logged.
void monitor(App& a) {
    if (!ImGui::CollapsingHeader("Burst monitor (solar and Jupiter bursts)")) return;
    bool changed = ImGui::Checkbox("Watch", &S.monOn);
    ImGui::SameLine();
    changed |= ImGui::InputDouble("MHz", &S.monMhz, 0.01, 1.0, "%.3f");
    ImGui::SameLine();
    changed |= ImGui::SliderFloat("Rise (dB)", &S.monRiseDb, 1.f, 30.f, "%.0f");
    ImGui::SameLine();
    changed |= ImGui::InputFloat("Minimum (ms)", &S.monMinMs, 50.f, 500.f, "%.0f");
    if (changed) applyMonitor();
    if (!live(a)) ImGui::TextDisabled("Needs a running sweep that covers the watched frequency.");
    else ImGui::Text("Level %.1f dB, median %.1f dB, %s", S.monLast, S.monBase, S.mon.inBurst() ? "in a burst" : "quiet");
    ImGui::TextDisabled("One level per sweep, so the time resolution is one sweep. Sweeps are read while this tab is open.");
    if (ImGui::SmallButton("Clear log")) S.monLog.clear();
    ImGui::SameLine();
    ImGui::TextDisabled("Ended        MHz       Peak dB  Median dB  Duration ms");
    for (size_t i = S.monLog.size(); i-- > 0;) {
        const auto& e = S.monLog[i];
        char when[16] = "";
        snprintf(when, sizeof when, "%s", dect2::timefmt::clock((int64_t)e.wall).c_str());
        ImGui::Text("%s  %10.3f  %8.1f  %9.1f  %11.0f", when, e.mhz, e.ev.peakDb, e.ev.baseDb, e.ev.durationMs);
    }
}

// Dish alignment (WIP): the beacon level in dB with a peak hold, a 60 s strip chart and a level bar, and a geostationary look-angle helper.
// Pointing arc and LNB calibration are not done yet.
void dish(App& a) {
    if (!ImGui::CollapsingHeader("Dish alignment (WIP): beacon level and look angles")) return;
    ImGui::TextDisabled("Work in progress: no dish-pointing arc and no LNB calibration yet.");
    ImGui::SetNextItemWidth(130 * gUi);
    if (ImGui::InputDouble("Beacon IF (MHz)", &S.dishMhz, 1.0, 10.0, "%.3f")) {
        S.dishHist.clear();
        S.dishPeak = S.dishLevel = -200;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Zero")) S.dishPeak = S.dishLevel;
    ImGui::TextDisabled("Enter the frequency the receiver sees (after the LNB). The level is the strongest bin within +-5 kHz.");
    if (!live(a)) {
        ImGui::TextDisabled("Needs a running sweep that covers the beacon frequency.");
    } else if (S.dishLevel <= -199) {
        ImGui::TextDisabled("The beacon frequency is outside the sweep.");
    } else {
        ImGui::SetWindowFontScale(2.0f);
        ImGui::Text("%.1f dB", S.dishLevel);
        ImGui::SetWindowFontScale(1.0f);
    }
    if (S.dishPeak > -199) ImGui::Text("Peak hold %.1f dB", S.dishPeak);
    else ImGui::TextDisabled("Peak hold --");

    // level bar on the chart's dB scale
    const float bot = S.refDb - S.rangeDb;
    const float frac = S.dishLevel > -199 ? std::clamp((S.dishLevel - bot) / S.rangeDb, 0.f, 1.f) : 0.f;
    const float pfrac = S.dishPeak > -199 ? std::clamp((S.dishPeak - bot) / S.rangeDb, 0.f, 1.f) : 0.f;
    const float barW = std::min(360 * gUi, ImGui::GetContentRegionAvail().x), barH = 16 * gUi;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + barW, p.y + barH), IM_COL32(40, 40, 46, 255));
    dl->AddRectFilled(p, ImVec2(p.x + barW * frac, p.y + barH), IM_COL32(90, 200, 120, 255));
    if (S.dishPeak > -199) dl->AddLine(ImVec2(p.x + barW * pfrac, p.y), ImVec2(p.x + barW * pfrac, p.y + barH), IM_COL32(255, 210, 80, 255), 2.f);
    ImGui::Dummy(ImVec2(barW, barH));
    ImGui::TextDisabled("Bar scale %.0f to %.0f dB", bot, S.refDb);

    // 60 s strip chart of the level
    const float chartW = std::min(600 * gUi, ImGui::GetContentRegionAvail().x), chartH = 110 * gUi;
    p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + chartW, p.y + chartH), IM_COL32(20, 22, 28, 255));
    dl->AddRect(p, ImVec2(p.x + chartW, p.y + chartH), IM_COL32(90, 90, 100, 255));
    const double now = nowS();
    std::vector<ImVec2> pts;
    pts.reserve(S.dishHist.size());
    for (const auto& s : S.dishHist) {
        const float x = (float)(1.0 - (now - s.first) / 60.0), y = (S.refDb - s.second) / S.rangeDb;
        pts.push_back(ImVec2(p.x + chartW * std::clamp(x, 0.f, 1.f), p.y + chartH * std::clamp(y, 0.f, 1.f)));
    }
    if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(110, 190, 255, 255), 0, 1.5f);
    ImGui::Dummy(ImVec2(chartW, chartH));
    ImGui::TextDisabled("Last 60 s, top %.0f dB, bottom %.0f dB", S.refDb, bot);

    // geostationary look angle from the user's position
    ImGui::Separator();
    ImGui::TextDisabled("Look angle to a geostationary satellite (spherical Earth, no refraction)");
    ImGui::SetNextItemWidth(100 * gUi);
    ImGui::InputDouble("Your latitude", &S.latMine, 0.1, 1.0, "%.4f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100 * gUi);
    ImGui::InputDouble("Your longitude", &S.lonMine, 0.1, 1.0, "%.4f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100 * gUi);
    ImGui::InputDouble("Satellite longitude", &S.lonSat, 0.1, 1.0, "%.2f");
    S.latMine = std::clamp(S.latMine, -90.0, 90.0);
    S.lonMine = std::clamp(S.lonMine, -180.0, 180.0);
    S.lonSat = std::clamp(S.lonSat, -180.0, 180.0);
    const dect2::GeoLookAngle la = dect2::geoLookAngle(S.latMine, S.lonMine, S.lonSat);
    ImGui::Text("Azimuth %.1f deg, elevation %.1f deg", la.azimuthDeg, la.elevationDeg);
    if (la.elevationDeg < 0) ImGui::TextDisabled("The satellite is below your horizon.");
}

void tab(App& a) {
    ingest(a);
    if (gGfx && !S.wf.empty()) {
        if (!S.wfImg) S.wfImg = gGfx->createImage(State::kCols, State::kRows, 0xFF000000u);
        if (!S.psImg) S.psImg = gGfx->createImage(State::kCols, State::kLevels, 0xFF000000u);
        if (S.wfDirty) { S.wfImg->update(0, 0, State::kCols, State::kRows, S.wf.data()); S.wfDirty = false; }
        if (S.psDirty) { S.psImg->update(0, 0, State::kCols, State::kLevels, S.ps.data()); S.psDirty = false; }
    }
    controls(a);
    monitor(a);
    leoMonitor(a);
    dish(a);
    const float avail = ImGui::GetContentRegionAvail().y;
    chart(a, std::max(120 * gUi, avail * 0.62f));
    waterfall(a, std::max(40 * gUi, ImGui::GetContentRegionAvail().y - 2));
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "sweeping" : "stopped"); return; }
    // the strongest bins of the sweep, one per allocation band
    const auto& t = a.rx.specan;
    std::vector<std::pair<float, size_t>> pk;
    for (size_t k = 1; k + 1 < t.live.size(); k++)
        if (t.live[k] > t.live[k - 1] && t.live[k] >= t.live[k + 1]) pk.push_back({t.live[k], k});
    std::sort(pk.rbegin(), pk.rend());
    int shown = 0;
    for (const auto& p : pk) {
        if (shown >= 15) break;
        const double hz = t.loHz + (p.second + 0.5) * t.binHz;
        const dect2::SaBand* bd = dect2::saAllocationAt(hz / 1e6);
        char l[96];
        snprintf(l, sizeof l, "%10.3f  %6.1f  %s##sp%d", hz / 1e6, p.first, bd ? bd->name : "", shown);
        if (ImGui::Selectable(l)) S.m1 = hz;
        shown++;
    }
}

void receiver(App& a) {
    const auto& t = a.rx.specan;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "sweeping" : "stopped"); return; }
    ImGui::Text("%s", dect2::specanSummary(t).c_str());
    ImGui::TextWrapped("%s, %d tuning steps per sweep, %zu bins of %.1f kHz", t.native ? "HackRF native sweep (20 MHz steps)" : "stepped retuning", t.steps, t.live.size(), t.binHz / 1e3);
    ImGui::TextWrapped("%llu sweeps, %llu with gaps, input %.2f Msps", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.inputRate / 1e6);
}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::specanSummary(a.rx.specan).c_str() : (a.engine.running() ? "sweeping" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Spectrum analyser";
    if (live(a)) l2 = dect2::specanSummary(a.rx.specan);
}

} // namespace

extern const ModeUi kSpecanUi;
const ModeUi kSpecanUi = {
    .sideTitle = "PEAKS",
    .tabName = "Analyser",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
};
