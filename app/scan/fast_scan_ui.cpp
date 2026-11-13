// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The "Fast scan" scope of the merged Scanner view. See fast_scan_ui.h and docs/fast-scan.md.
#include "dect2/common/thread_name.h"
#include "dect2/fastscan/fwsweep.h"
#include "dect2/fastscan/profile.h"
#include "dect2/fastscan/scanner.h"
#include "dect2/sweep/sweep.h"
#include "../app.h"
#include "../gfx/gfx.h"
#include "../memory/memory_ui.h"
#include "../ui/ui_common.h"
#include "fast_scan_ui.h"
#include "scan_plan.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {
using namespace dect2;
using namespace dect2::fastscan;

struct HitRow { double hz; std::string mode; };

struct St {
    ScanProfile prof;
    std::vector<ScanProfile> presets;      // the user's
    char bands[256] = "", excl[256] = "", prio[128] = "", name[48] = "";
    bool loaded = false, autoSettle = true, persistFloor = true, autoParams = true, reliableOnly = true, fwSweep = true, usedFw = false;
    float gainDb = 30;
    int presetSel = -1;
    // traces
    bool showCur = true, showAvg = true, showMax = true, showFloor = true;
    // run
    std::thread th;
    std::atomic<bool> cancel{false}, running{false}, ready{false};
    std::unique_ptr<FastScanner> sc;
    RecorderBank rec;
    RadioProfile radio;
    std::mutex mu;
    std::string msg;
    double settleMeasured = 0;
    std::vector<HitRow> modes;              // memory rows: frequency -> mode, copied at start
    // view
    SpectrumView view;
    std::vector<Hit> hits;
    std::vector<HitLabel> labels;
    bool identifyNew = false;
    std::vector<ChannelInfo> chans;         // the same hits with confirmation, class and spur / image flags
    std::unordered_map<uint64_t, size_t> chanOf;
    AutoScan applied;                       // step / dwell / threshold the automatic mode chose for this scan
    double lastPull = 0;
    std::deque<std::vector<float>> hist;
    uint64_t histSeq = 0;
    uint64_t sel = 0;
    double selA = 0, selB = 0;              // MHz, Ctrl+drag
    bool selActive = false;
    ImVec2 clickPos{0, 0};
    std::string lockedText, prioText;       // MHz lists, comma separated
    char lockBuf[512] = "";
    bool fitOnce = true;
} S;

constexpr int kHistCols = 600, kHistRows = 120;

std::vector<double> centers(const std::string& text) {
    std::vector<double> v;
    for (const FreqRange& r : parseRanges(text)) v.push_back((r.loHz + r.hiHz) / 2);
    return v;
}
std::string pointText(const std::string& text, double mhz) {
    char b[48]; snprintf(b, sizeof b, "%.6f", mhz);
    return text.empty() ? std::string(b) : text + ", " + b;
}

void toBuffers() {
    snprintf(S.bands, sizeof S.bands, "%s", S.prof.bands.c_str());
    snprintf(S.excl, sizeof S.excl, "%s", S.prof.exclusions.c_str());
    snprintf(S.prio, sizeof S.prio, "%s", S.prof.priorityBands.c_str());
    snprintf(S.name, sizeof S.name, "%s", S.prof.name.c_str());
}
void fromBuffers() { S.prof.bands = S.bands; S.prof.exclusions = S.excl; S.prof.priorityBands = S.prio; S.prof.name = S.name; }

void saveState() {
    fromBuffers();
    plat::Prefs& p = plat::prefs();
    p.setS("fsProfile", serializeProfile(S.prof));
    std::string all;
    for (const ScanProfile& q : S.presets) all += (all.empty() ? "" : "|") + serializeProfile(q);
    p.setS("fsPresets", all);
    p.setS("fsLocked", S.lockedText); p.setS("fsPrio", S.prioText);
    p.setB("fsFw", S.fwSweep); p.setB("fsAutoParams", S.autoParams); p.setB("fsReliable", S.reliableOnly);
    p.setB("fsAutoSettle", S.autoSettle); p.setB("fsPersist", S.persistFloor); p.setD("fsGain", S.gainDb);
}
void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& p = plat::prefs();
    parseProfile(p.getS("fsProfile", ""), S.prof);
    const std::string all = p.getS("fsPresets", "");
    size_t pos = 0;
    while (pos < all.size()) {
        size_t e = all.find('|', pos);
        if (e == std::string::npos) e = all.size();
        ScanProfile q;
        if (parseProfile(all.substr(pos, e - pos), q)) S.presets.push_back(q);
        pos = e + 1;
    }
    S.lockedText = p.getS("fsLocked", ""); S.prioText = p.getS("fsPrio", "");
    S.fwSweep = p.getB("fsFw", true); S.autoParams = p.getB("fsAutoParams", true); S.reliableOnly = p.getB("fsReliable", true);
    S.autoSettle = p.getB("fsAutoSettle", true); S.persistFloor = p.getB("fsPersist", true); S.gainDb = (float)p.getD("fsGain", 30);
    toBuffers();
}

// the mode a hit is listened to / recorded in: the memory row on that frequency, else a guess from the frequency
std::string modeFor(const std::vector<HitRow>& rows, double hz) {
    for (const HitRow& r : rows) if (std::fabs(r.hz - hz) <= 6500) return r.mode;
    return guessMode(hz, 0);
}

// lockout and priority: the lists typed here plus the memory rows marked locked out / priority
void filters(std::vector<double>& locked, std::vector<double>& prio) {
    locked = centers(S.lockedText);
    prio = centers(S.prioText);
    for (const dect2::Memory& m : memoryBank().all()) {
        scanplan::Row r;
        scanplan::readRow(m.options, r);
        if (r.lockout) locked.push_back((double)m.freqHz);
        if (r.priority) prio.push_back((double)m.freqHz);
    }
}
void pushFilters() {
    if (!S.sc || !S.ready) return;
    std::vector<double> lk, pr;
    filters(lk, pr);
    S.sc->setFilters(parseRanges(S.prof.exclusions), lk, pr);
}

std::string floorPath(const FastScanConfig& c) {
    double lo = 1e18, hi = 0;
    for (const FreqRange& b : c.plan.bands) { lo = std::min(lo, b.loHz); hi = std::max(hi, b.hiHz); }
    char b[96]; snprintf(b, sizeof b, "/scan-floor-%.0f-%.0f-%.0f.bin", lo / 1e3, hi / 1e3, c.plan.binHz);
    return plat::dataDir() + b;
}

void worker(DeviceInfo dev, TuneSettings t, FastScanConfig cfg, ScanProfile prof, bool useFw) {
    std::string err;
    if (useFw) {
        // HackRF: the firmware sweeps by itself (hackrf_init_sweep), 20 Msps hops of 15 MHz; when it cannot start the retune loop below takes over
        const FwSweepPlan fp = planFirmwareSweep(cfg.plan, S.radio);
        FastScanConfig fc = cfg;
        fc.plan = fp.plan;
        fc.plan.binHz = std::max(fc.plan.binHz, 30000.0);
        RadioProfile r20 = S.radio; r20.rateHz = 20e6;
        HackrfFwSweep fw;
        std::string ferr;
        if (S.sc->configure(fc, r20, ferr) && fw.open(fp, t.lnaDb, t.vgaDb, t.ampOn, dev.serial, ferr)) {
            pushFilters();
            RunHooks hk;
            { std::lock_guard<std::mutex> l(S.mu); S.usedFw = true; S.msg = tr("HackRF firmware sweep (receive only)."); }
            S.ready = true;
            S.sc->runBlocks(fw, &S.cancel, hk, err);
            fw.close();
            if (S.persistFloor && !fc.floorFile.empty()) S.sc->saveFloor();
            std::lock_guard<std::mutex> l(S.mu);
            if (!err.empty()) S.msg = err;
            S.running = false;
            return;
        }
        std::lock_guard<std::mutex> l(S.mu);
        S.usedFw = false;
        S.msg = std::string(tr("The firmware sweep is not available, using the retune loop: ")) + ferr;
    }
    sweep::IqSweepSource src(dev, t);
    if (!src.open(err)) { std::lock_guard<std::mutex> l(S.mu); S.msg = err; S.running = false; return; }
    if (S.autoSettle && !cfg.plan.bands.empty()) {
        const double st = sweep::calibrateSettle(src, cfg.plan.bands[0].loHz, cfg.plan.bands[0].hiHz, err);
        if (st > 0) { cfg.plan.settleSec = st; std::lock_guard<std::mutex> l(S.mu); S.settleMeasured = st; }
    }
    if (!S.sc->configure(cfg, S.radio, err)) { src.close(); std::lock_guard<std::mutex> l(S.mu); S.msg = err; S.running = false; return; }
    pushFilters();
    RunHooks hk;
    if (prof.record) {
        RecSettings rs;
        rs.dir = recordingsSub("scan-recordings");
        rs.budgetBytes = (uint64_t)(prof.budgetMb * 1048576.0);
        rs.chanBwHz = prof.recordKhz * 1e3; rs.iqSlice = prof.recordIq; rs.maxChannels = prof.maxRecordings;
        if (S.rec.configure(S.sc->plan().rateHz, S.sc->plan().hops[0].centerHz, rs)) hk.rec = &S.rec;
        else { std::lock_guard<std::mutex> l(S.mu); S.msg = tr("Recording is not possible at this sample rate and channel width."); }
    }
    const std::vector<HitRow> modes = S.modes;
    hk.modeOf = [modes](double hz) { return modeFor(modes, hz); };
    S.ready = true;
    S.sc->run(src, &S.cancel, hk, err);
    S.rec.closeAll();
    if (S.persistFloor && !cfg.floorFile.empty()) S.sc->saveFloor();
    src.close();
    std::lock_guard<std::mutex> l(S.mu);
    if (!err.empty()) S.msg = err;
    S.running = false;
}

RadioProfile radioOf(const App& a) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return radioProfile("rtlsdr");
    const DeviceInfo& d = a.devices[a.devIdx];
    return profileForBoard(d.board + " " + d.name, d.maxRateHz);
}

void startScan(App& a) {
    fastscanui::stop();
    fromBuffers();
    scannerControl(a, 0);
    if (a.engine.running()) a.engine.stop();
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size() || !a.devices[a.devIdx].isRadio()) { S.msg = tr("Pick a radio first (the test signal and files cannot scan)."); return; }
    const DeviceInfo dev = a.devices[a.devIdx];
    S.radio = profileForBoard(dev.board + " " + dev.name, dev.maxRateHz);
    if (S.autoParams && !parseRanges(S.prof.bands).empty()) {
        // one-button mode: the step, dwell and threshold follow from the radio and the width of the span
        double lo = 1e18, hi = 0;
        for (const FreqRange& r : parseRanges(S.prof.bands)) { lo = std::min(lo, r.loHz); hi = std::max(hi, r.hiHz); }
        S.applied = autoScan(S.radio, hi - lo);
        if (hi - lo > 150e6) { S.prof.stepKhz = S.applied.stepKhz; S.prof.dwellMs = S.applied.dwellMs; S.prof.thresholdDb = std::max(S.prof.thresholdDb, S.applied.thresholdDb); }
    }
    FastScanConfig cfg = toConfig(S.prof);
    cfg.identifyNew = S.identifyNew;
    if (cfg.plan.bands.empty()) { S.msg = tr("No band: type ranges in MHz, for example 118-137."); return; }
    for (const FreqRange& r : parseRanges(S.prioText)) cfg.plan.priority.push_back(r);
    if (S.persistFloor) cfg.floorFile = floorPath(cfg);
    TuneSettings t = a.tune;
    t.sampleRate = S.radio.rateHz;
    if (dev.isGeneric()) t.gainDb = S.gainDb;
    t.centerHz = cfg.plan.bands[0].loHz;
    t.bandwidthMhz = t.sampleRate / 1e6;
    S.modes.clear();
    for (const dect2::Memory& m : memoryBank().all()) S.modes.push_back({(double)m.freqHz, m.mode});
    S.sc = std::make_unique<FastScanner>();
    S.cancel = false; S.running = true; S.ready = false; S.msg.clear(); S.hist.clear(); S.hits.clear(); S.settleMeasured = 0; S.fitOnce = true;
    saveState();
    bool useFw = false;
    if (S.fwSweep && S.radio.id == "hackrf" && !S.prof.record) {
        double lo = 1e18, hi = 0;
        for (const FreqRange& r : cfg.plan.bands) { lo = std::min(lo, r.loHz); hi = std::max(hi, r.hiHz); }
        useFw = hi - lo >= 30e6;
    }
    S.usedFw = false;
    S.th = dect2::namedThread("onair-fastscan", worker, dev, t, cfg, S.prof, useFw);
}

void tuneTo(App& a, double hz) {
    const std::string mode = modeFor(S.modes, hz);
    fastscanui::stop();
    const int mi = scanModeIndex(mode);
    if (mi >= 0 && !modeSelected(a, kModes[mi])) { if (a.engine.running()) a.engine.stop(); selectMode(a, kModes[mi].family, kModes[mi].subId); }
    a.freqMhz = hz / 1e6;
    if (a.engine.running()) { a.tune.centerHz = hz; a.engine.retune(a.tune); } else startReceiver(a);
}

void saveToMemory(const Hit& h) {
    HitMemory hm = hitToMemory(h, modeFor(S.modes, h.freqHz));
    dect2::Memory m;
    m.freqHz = (uint64_t)std::llround(hm.mhz * 1e6);
    m.mode = hm.mode; m.name = hm.name; m.group = hm.group;
    m.bandwidthHz = h.bwHz;
    const dect2::MemoryAddResult r = memoryBank().add(m);
    if (r.what == dect2::MemoryAdd::Added) { memoryChanged(); S.msg = tr("Saved to the memory table (group Fast scan)."); }
    else S.msg = tr(r.what == dect2::MemoryAdd::Duplicate ? "This frequency and mode is already in the table." : "The table is full.");
}

bool shown(uint64_t id) {
    if (!S.reliableOnly) return true;
    auto it = S.chanOf.find(id);
    return it == S.chanOf.end() || S.chans[it->second].reliable();
}

void pull() {
    const double now = ImGui::GetTime();
    if (!S.sc || !S.ready || now - S.lastPull < 0.066) return;
    S.lastPull = now;
    S.view = S.sc->snapshot();
    S.hits = S.sc->hitsSnapshot();
    S.chans = S.sc->channels(false);
    S.labels = S.sc->labels();
    S.chanOf.clear();
    for (size_t i = 0; i < S.chans.size(); i++) S.chanOf[S.chans[i].hit.id] = i;
    if (S.view.seq != S.histSeq && S.view.bins() > 0) {
        S.histSeq = S.view.seq;
        std::vector<float> row(kHistCols, -150.f);
        const size_t n = S.view.bins();
        for (size_t i = 0; i < n; i++) { float& r = row[std::min<size_t>(kHistCols - 1, i * kHistCols / n)]; r = std::max(r, S.view.cur[i]); }
        S.hist.push_front(std::move(row));
        if ((int)S.hist.size() > kHistRows) S.hist.pop_back();
    }
}

void presetBar() {
    ImGui::SetNextItemWidth(260 * gUi);
    const char* cur = S.presetSel >= 0 ? (S.presetSel < (int)S.presets.size() ? S.presets[S.presetSel].name.c_str() : "") : tr("Profile preset...");
    if (ImGui::BeginCombo("##fspreset", cur)) {
        const std::vector<ScanProfile> bi = builtinProfiles();
        for (const ScanProfile& p : bi) if (ImGui::Selectable(p.name.c_str())) { S.prof = p; toBuffers(); S.presetSel = -1; saveState(); }
        if (!S.presets.empty()) ImGui::Separator();
        for (int i = 0; i < (int)S.presets.size(); i++) if (ImGui::Selectable((S.presets[i].name + "##u" + std::to_string(i)).c_str(), i == S.presetSel)) { S.prof = S.presets[i]; toBuffers(); S.presetSel = i; saveState(); }
        ImGui::EndCombo();
    }
    ImGui::SameLine(); ImGui::SetNextItemWidth(150 * gUi); ImGui::InputText("##fsname", S.name, sizeof S.name);
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Save preset"))) {
        fromBuffers();
        bool found = false;
        for (ScanProfile& p : S.presets) if (p.name == S.prof.name) { p = S.prof; found = true; }
        if (!found) S.presets.push_back(S.prof);
        saveState();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(S.presetSel < 0 || S.presetSel >= (int)S.presets.size());
    if (ImGui::SmallButton(tr("Delete preset"))) { S.presets.erase(S.presets.begin() + S.presetSel); S.presetSel = -1; saveState(); }
    ImGui::EndDisabled();
}

}  // namespace

namespace fastscanui {

bool running() { return S.running.load(); }

void stop() {
    S.cancel = true;
    if (S.th.joinable()) S.th.join();
    S.running = false;
}

void controls(App& a) {
    load();
    const bool run = S.running.load();
    if (S.th.joinable() && !run) S.th.join();
    if (ImGui::Button(tr(run ? "Stop" : "Start fast scan"))) { if (run) stop(); else startScan(a); }
    ImGui::SameLine();
    ImGui::BeginDisabled(run);
    if (ImGui::Button(tr("Whole span"))) {
        // the radio's whole tuning range with the step and dwell that suit it; the record settings of the current profile stay
        ScanProfile w = wholeSpanProfile(radioOf(a));
        w.record = S.prof.record; w.recordIq = S.prof.recordIq; w.maxRecordings = S.prof.maxRecordings; w.budgetMb = S.prof.budgetMb; w.recordKhz = S.prof.recordKhz;
        S.prof = w; S.presetSel = -1; toBuffers(); saveState(); startScan(a);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Sweeps everything this radio can tune (RTL-SDR 25-1700 MHz, HackRF 1-6000 MHz ...) with an automatic step and dwell. Confirmed channels appear in the list below."));
    ImGui::SameLine();
    if (ImGui::SmallButton("?##fshelp")) ImGui::OpenPopup("fshelp");
    if (ImGui::BeginPopup("fshelp")) {
        ImGui::PushTextWrapPos(460 * gUi);
        ImGui::TextUnformatted(tr("Fast scan shows a pseudo-spectrum of a very wide span by hopping the radio quickly, and finds every signal in each window at once."));
        ImGui::Spacing();
        ImGui::BulletText("%s", tr("Whole span: one button sweeps everything this radio can tune, with an automatic step and dwell."));
        ImGui::BulletText("%s", tr("A channel is listed once it was seen in two sweeps. Images of strong signals, steady spurs and the radio's DC spike are hidden (untick \"confirmed channels only\" to see them flagged)."));
        ImGui::BulletText("%s", tr("Click a channel to tune and listen; Save puts it in the memory table, Lock ignores it from now on."));
        ImGui::BulletText("%s", tr("On a HackRF the firmware does the hopping (hackrf_sweep style), which is several times faster than retuning from the computer. Receive only."));
        ImGui::BulletText("%s", tr("Everything else (step, dwell, threshold, recording) is under Advanced."));
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    presetBar();
    bool ch = false;
    ImGui::BeginDisabled(run);
    ImGui::SetNextItemWidth(260 * gUi); ch |= ImGui::InputText(tr("bands MHz"), S.bands, sizeof S.bands);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Ranges in MHz, comma separated: 118-137, 156-162.025. A lone number is one channel."));
    ImGui::SameLine(); ImGui::SetNextItemWidth(200 * gUi); ch |= ImGui::InputText(tr("exclude MHz"), S.excl, sizeof S.excl);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Known interferers and FM broadcast: hops wholly inside are skipped, bins inside never report."));
    ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi); ch |= ImGui::InputText(tr("priority MHz"), S.prio, sizeof S.prio);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Ranges visited more often (one hop in four)."));
    ImGui::EndDisabled();
    ch |= ImGui::Checkbox(tr("automatic step and dwell"), &S.autoParams);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Wide spans (over 150 MHz) get wider bins and a short dwell that suit the radio; narrow bands keep the values under Advanced."));
    ImGui::SameLine(); ch |= ImGui::Checkbox(tr("confirmed channels only"), &S.reliableOnly);
    ImGui::SameLine(); ch |= ImGui::Checkbox(tr("name new signals"), &S.identifyNew);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Stay on a new signal for about half a second and say what it probably is. The scan is slower while it does."));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Hides signals seen in one sweep only, steady spurs and images of strong signals. Untick to see every raw hit."));
    const bool adv = ImGui::TreeNode(trw("Advanced (step, dwell, threshold, floor, recording)"));
    if (adv) {
    ImGui::BeginDisabled(run);
    ch |= ImGui::Checkbox(tr("HackRF firmware sweep"), &S.fwSweep);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("On a HackRF the firmware retunes by itself for spans over 30 MHz (much faster, receive only, no hit recording). Falls back to the normal retune loop when the radio cannot do it."));
    ImGui::SetNextItemWidth(80 * gUi); ch |= ImGui::InputDouble("step kHz", &S.prof.stepKhz, 0, 0, "%.2f");
    ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ch |= ImGui::InputDouble("dwell ms", &S.prof.dwellMs, 0, 0, "%.1f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Capture per hop after the radio has settled: longer averages more (weak signals show) but scans slower."));
    ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi);
    { float f = (float)S.prof.channelKhz; if (ImGui::InputFloat("channel kHz", &f, 0, 0, "%.2f")) { S.prof.channelKhz = f; ch = true; } }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("0: hits are free frequencies. A channel width snaps hits to that raster (25, 12.5, 8.33 ...)."));
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(130 * gUi); { float f = (float)S.prof.thresholdDb; if (ImGui::SliderFloat("##fsthr", &f, 3, 30, tr("floor +%.0f dB"))) { S.prof.thresholdDb = f; ch = true; } }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A hit starts this far above the dynamic noise floor of its own channel."));
    ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ch |= ImGui::InputDouble("min s", &S.prof.minDurSec, 0, 0, "%.2f");
    ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ch |= ImGui::InputDouble("hang s", &S.prof.hangSec, 0, 0, "%.1f");
    ImGui::BeginDisabled(run);
    ImGui::SameLine(); ch |= ImGui::Checkbox(tr("measure retune"), &S.autoSettle);
    ImGui::SameLine(); ch |= ImGui::Checkbox(tr("keep floor"), &S.persistFloor);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Saves the noise floor of the band when the scan stops and starts from it next time."));
    ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi); ch |= ImGui::SliderFloat("##fsgain", &S.gainDb, 0, 60, tr("gain %.0f dB"));
    ch |= ImGui::Checkbox(tr("record hits"), &S.prof.record);
    ImGui::SameLine(); kit::recordingsButton("fs", "scan-recordings");
    ImGui::EndDisabled();
    if (S.prof.record) {
        ImGui::SameLine(); ImGui::BeginDisabled(run); ch |= ImGui::Checkbox(tr("IQ instead of audio"), &S.prof.recordIq);
        ImGui::SameLine(); ImGui::SetNextItemWidth(60 * gUi); ch |= ImGui::InputInt("channels", &S.prof.maxRecordings, 0, 0);
        S.prof.maxRecordings = std::max(1, std::min(100, S.prof.maxRecordings));
        ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ch |= ImGui::InputDouble("MB budget", &S.prof.budgetMb, 0, 0, "%.0f");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Recordings go to scan-recordings in the data folder; the oldest are deleted to stay inside this size."));
        if (run) {
            const RecStats rs = S.rec.stats();
            ImGui::SameLine();
            ImGui::TextColored(rs.active ? pal::warnAmber() : pal::grey(), tr("recording %d channels (cap %d, CPU %.0f%%)%s"), rs.active, rs.cap, rs.load * 100.0, rs.throttled ? tr(", CPU guard on") : "");
        }
    }
    ImGui::TreePop();
    }
    if (ch) { fromBuffers(); S.prof.stepKhz = std::max(0.5, S.prof.stepKhz); S.prof.dwellMs = std::max(0.5, S.prof.dwellMs); saveState(); if (run) pushFilters(); }
    if (!S.msg.empty()) ImGui::TextColored(pal::warnAmber(), "%s", S.msg.c_str());
    if (run && S.ready && S.sc) {
        const ScanStats st = S.sc->stats();
        ImGui::TextDisabled(tr("%s: %.0f MHz/s measured (retune-limited ceiling %.0f), %d hops/pass, pass %.2f s, settle %.1f ms%s, %d active hits%s"),
                            S.radio.name.c_str(), st.mhzPerSec, ceilingMhzPerSec(S.radio, S.prof.dwellMs / 1e3), (int)S.sc->plan().hops.size(), st.passSec,
                            S.sc->plan().settleSec * 1e3, S.settleMeasured > 0 ? tr(" (measured)") : "", st.activeHits, st.parked ? tr(", parked on a hop to record") : "");
        int ok = 0;
        for (const ChannelInfo& c : S.chans) ok += c.reliable();
        ImGui::TextDisabled(tr("%d confirmed channels of %zu raw hits; step %.1f kHz, dwell %.1f ms%s"), ok, S.chans.size(), S.prof.stepKhz, S.prof.dwellMs, S.autoParams ? tr(" (automatic)") : "");
    }
}

void body(App& a) {
    load();
    pull();
    const bool have = S.view.bins() > 0;
    ImGui::Checkbox(tr("current"), &S.showCur); ImGui::SameLine();
    ImGui::Checkbox(tr("average"), &S.showAvg); ImGui::SameLine();
    ImGui::Checkbox(tr("max-hold"), &S.showMax); ImGui::SameLine();
    ImGui::Checkbox(tr("floor"), &S.showFloor); ImGui::SameLine();
    if (ImGui::SmallButton(tr("Reset traces")) && S.sc && S.ready) S.sc->resetTraces();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr("click: tune   wheel: zoom   drag: pan   Ctrl+drag: pick a range"));
    if (S.selActive) {
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Scan selection"))) {
            char b[64]; snprintf(b, sizeof b, "%.6f-%.6f", std::min(S.selA, S.selB), std::max(S.selA, S.selB));
            snprintf(S.bands, sizeof S.bands, "%s", b);
            S.selActive = false; fromBuffers(); saveState();
            startScan(a);
        }
        ImGui::SameLine(); if (ImGui::SmallButton(tr("Clear selection"))) S.selActive = false;
    }
    if (have) {
        const double x0 = S.view.freqOf(0) / 1e6, x1 = S.view.freqOf(S.view.bins() - 1) / 1e6;
        // columns for the visible range: the zoomed-in view keeps its bins
        static double vlo = 0, vhi = 0;
        if (S.fitOnce || vhi <= vlo) { vlo = x0; vhi = x1; }
        const double bMhz = S.view.binHz / 1e6;
        const long b0 = std::max(0L, (long)std::floor((vlo - x0) / bMhz)), b1 = std::min((long)S.view.bins() - 1, (long)std::ceil((vhi - x0) / bMhz));
        const long per = std::max(1L, (b1 - b0) / 1800);
        std::vector<double> xs, cur, avg, mx, fl, th;
        for (long i = b0; i <= b1; i += per) {
            float c = -200, av = -200, m = -200, f = 0;
            const long e = std::min(b1 + 1, i + per);
            for (long j = i; j < e; j++) { c = std::max(c, S.view.cur[j]); av = std::max(av, S.view.avg[j]); m = std::max(m, S.view.maxhold[j]); f += S.view.floor[j]; }
            f /= (float)(e - i);
            xs.push_back(S.view.freqOf(i) / 1e6); cur.push_back(c); avg.push_back(av); mx.push_back(m); fl.push_back(f); th.push_back(f + S.prof.thresholdDb);
        }
        double yMin = 1e9, yMax = -1e9;
        for (size_t i = 0; i < xs.size(); i++) { yMin = std::min(yMin, fl[i]); yMax = std::max(yMax, mx[i]); }
        if (plt::BeginPlot("##fspseudo", ImVec2(-1, 280 * gUi), plt::Flags_NoLegend)) {
            plt::SetupAxes("MHz", "dBFS");
            if (S.fitOnce) { plt::SetupAxisLimits(plt::X1, x0, x1, plt::Cond_Always); S.fitOnce = false; }
            plt::SetupAxisLimits(plt::Y1, yMin - 6, yMax + 8, plt::Cond_Once);
            if (xs.size() > 1) {
                if (S.showMax) { plt::Spec s; s.LineColor = ImVec4(0.95f, 0.6f, 0.25f, 0.9f); plt::PlotLine("max", xs.data(), mx.data(), (int)xs.size(), s); }
                if (S.showAvg) { plt::Spec s; s.LineColor = ImVec4(0.4f, 0.65f, 1, 1); plt::PlotLine("avg", xs.data(), avg.data(), (int)xs.size(), s); }
                if (S.showCur) { plt::Spec s; s.LineColor = ImVec4(0.85f, 0.88f, 0.9f, 0.8f); plt::PlotLine("cur", xs.data(), cur.data(), (int)xs.size(), s); }
                if (S.showFloor) {
                    plt::Spec s; s.LineColor = ImVec4(0.4f, 0.9f, 0.4f, 1); plt::PlotLine("floor", xs.data(), fl.data(), (int)xs.size(), s);
                    plt::Spec t; t.LineColor = ImVec4(1, 0.4f, 0.35f, 0.8f); plt::PlotLine("threshold", xs.data(), th.data(), (int)xs.size(), t);
                }
            }
            for (const Hit& h : S.hits) {
                const double f = h.freqHz / 1e6, y = h.peakDb;
                if (f < vlo || f > vhi) continue;
                { auto it = S.chanOf.find(h.id); if (S.reliableOnly && it != S.chanOf.end() && !S.chans[it->second].reliable()) continue; }
                plt::Spec m; m.LineColor = h.id == S.sel ? ImVec4(1, 1, 0.2f, 1) : (h.active ? ImVec4(0.3f, 1, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1));
                m.Marker = plt::Marker_Circle; m.MarkerSize = 4;
                plt::PlotScatter("hit", &f, &y, 1, m);
            }
            if (S.selActive) { plt::Spec s; s.FillColor = ImVec4(1, 1, 1, 0.12f); plt::PlotVBand("sel", std::min(S.selA, S.selB), std::max(S.selA, S.selB), s); }
            const plt::Rect lim = plt::GetPlotLimits();
            vlo = lim.X.Min; vhi = lim.X.Max;
            if (plt::IsPlotHovered()) {
                const double mhz = plt::GetPlotMousePos().x;
                const bool ctrl = ImGui::GetIO().KeyCtrl;
                if (ctrl && ImGui::IsMouseClicked(0)) { S.selA = S.selB = mhz; S.selActive = true; }
                else if (ctrl && ImGui::IsMouseDown(0) && S.selActive) S.selB = mhz;
                else if (!ctrl && ImGui::IsMouseClicked(0)) S.clickPos = ImGui::GetMousePos();
                else if (!ctrl && ImGui::IsMouseReleased(0)) {
                    const ImVec2 p = ImGui::GetMousePos();
                    if (std::fabs(p.x - S.clickPos.x) < 4 && std::fabs(p.y - S.clickPos.y) < 4) {
                        // snap to the nearest hit within a few bins, else the clicked frequency
                        double hz = mhz * 1e6, best = 10 * S.view.binHz;
                        for (const Hit& h : S.hits) if (std::fabs(h.freqHz - hz) < best && shown(h.id)) { best = std::fabs(h.freqHz - hz); hz = h.freqHz; }
                        tuneTo(a, hz);
                    }
                }
            }
            plt::EndPlot();
        }
        if (S.hist.size() > 1 && plt::BeginPlot("##fswf", ImVec2(-1, 130 * gUi), plt::Flags_NoLegend)) {
            plt::SetupAxes("MHz", "");
            plt::SetupAxisLimits(plt::X1, vlo, vhi, plt::Cond_Always);
            plt::SetupAxisLimits(plt::Y1, 0, 1, plt::Cond_Always);
            std::vector<float> flat;
            flat.reserve(S.hist.size() * kHistCols);
            for (const auto& r : S.hist) flat.insert(flat.end(), r.begin(), r.end());
            float lo = 1e9f;
            for (size_t i = 0; i < S.view.bins(); i += 7) lo = std::min(lo, S.view.floor[i]);
            plt::PushColormap(0);
            plt::PlotHeatmap("wf", flat.data(), (int)S.hist.size(), kHistCols, lo - 3, lo + 35, nullptr, plt::Point(x0, 0), plt::Point(x1, 1));
            plt::PopColormap();
            plt::EndPlot();
        }
    } else ImGui::TextDisabled("%s", tr("The pseudo-spectrum fills in as the scan goes through the bands."));
    // hits
    { size_t n = 0; for (const Hit& h : S.hits) n += shown(h.id); ImGui::Text(tr("Channels (%zu)"), n); }
    ImGui::SameLine(); ImGui::TextDisabled("%s", tr("locked: "));
    ImGui::SameLine(); ImGui::TextDisabled("%s", S.lockedText.empty() ? "-" : S.lockedText.c_str());
    if (!S.lockedText.empty()) { ImGui::SameLine(); if (ImGui::SmallButton(tr("Clear lockout list"))) { S.lockedText.clear(); saveState(); pushFilters(); } }
    uint64_t save = 0, lock = 0, prio = 0, rem = 0, tune = 0;
    if (ImGui::BeginTable("##fshits", 12, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(0, std::max(120.f * gUi, ImGui::GetContentRegionAvail().y)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("MHz"), 0, 90 * gUi); ImGui::TableSetupColumn(tr("Type"), 0, 110 * gUi); ImGui::TableSetupColumn(tr("Seen"), 0, 70 * gUi); ImGui::TableSetupColumn(tr("BW kHz"), 0, 55 * gUi); ImGui::TableSetupColumn(tr("Peak dBFS"), 0, 70 * gUi);
        ImGui::TableSetupColumn(tr("SNR dB"), 0, 50 * gUi); ImGui::TableSetupColumn(tr("Best"), 0, 45 * gUi); ImGui::TableSetupColumn(tr("First seen"), 0, 75 * gUi);
        ImGui::TableSetupColumn(tr("Last seen"), 0, 75 * gUi); ImGui::TableSetupColumn(tr("Hits"), 0, 40 * gUi); ImGui::TableSetupColumn(tr("State"), 0, 70 * gUi);
        ImGui::TableSetupColumn(tr(""), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        std::vector<const Hit*> order;
        for (const Hit& h : S.hits) if (shown(h.id)) order.push_back(&h);
        std::sort(order.begin(), order.end(), [](const Hit* x, const Hit* y) { return x->active != y->active ? x->active : x->lastSeen > y->lastSeen; });
        for (const Hit* hp : order) {
            const Hit& h = *hp;
            ImGui::PushID((int)h.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char b[32]; snprintf(b, sizeof b, "%.4f", h.freqHz / 1e6);
            if (ImGui::Selectable(b, S.sel == h.id, ImGuiSelectableFlags_SpanAllColumns)) { S.sel = h.id; tune = h.id; }   // a click tunes and listens
            const ChannelInfo* ci = nullptr;
            { auto it = S.chanOf.find(h.id); if (it != S.chanOf.end()) ci = &S.chans[it->second]; }
            ImGui::TableNextColumn();
            if (ci) {
                const char* flag = ci->spur ? tr(" (spur)") : ci->image ? tr(" (image)") : ci->dc ? tr(" (DC)") : !ci->confirmed ? tr(" (unconfirmed)") : "";
                const dect2::fastscan::HitLabel* lab = nullptr;
                for (const auto& q : S.labels) if (std::fabs(q.freqHz - h.freqHz) < std::max(5000.0, 0.5 * h.bwHz)) lab = &q;
                ImGui::Text("%s%s", lab && !lab->name.empty() ? lab->name.c_str() : ci->hint.c_str(), flag);
                if (lab && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", lab->headline.c_str());
            }
            ImGui::TableNextColumn();
            if (ci) ImGui::Text(tr("%d sweeps, %.0f%%"), ci->sweeps, ci->presence * 100.0);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", h.bwHz / 1e3);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", h.peakDb);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", h.snrDb);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", h.maxSnrDb);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(fmtLocal((int64_t)h.firstSeen, "%H:%M:%S").c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(fmtLocal((int64_t)h.lastSeen, "%H:%M:%S").c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", h.count);
            ImGui::TableNextColumn();
            const bool recd = S.rec.recording(h.id);
            if (h.active) ImGui::TextColored(recd ? pal::warnAmber() : pal::okGreen(), "%s", recd ? tr("REC") : tr("active")); else ImGui::TextDisabled("%s", tr("idle"));
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(tr("Tune"))) tune = h.id;
            ImGui::SameLine(); if (ImGui::SmallButton(tr("Save"))) save = h.id;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Add to the memory table (group Fast scan) with the guessed mode"));
            ImGui::SameLine(); if (ImGui::SmallButton(tr("Lock"))) lock = h.id;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Lockout: ignore this frequency from now on"));
            ImGui::SameLine(); if (ImGui::SmallButton(tr("Prio"))) prio = h.id;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Priority: its hop is visited more often"));
            ImGui::SameLine(); if (ImGui::SmallButton("x")) rem = h.id;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    for (const Hit& h : S.hits) {
        if (h.id == tune) tuneTo(a, h.freqHz);
        if (h.id == save) saveToMemory(h);
        if (h.id == lock) { S.lockedText = pointText(S.lockedText, h.freqHz / 1e6); saveState(); pushFilters(); if (S.sc) S.sc->removeHit(h.id); }
        if (h.id == prio) { S.prioText = pointText(S.prioText, h.freqHz / 1e6); saveState(); pushFilters(); }
        if (h.id == rem && S.sc) S.sc->removeHit(h.id);
    }
}

}  // namespace fastscanui
