// The sweep scanner (View > Sweep scanner): a fast wideband sweep of a frequency range with a noise-floor measurement, a trace with the floor
// and the detection threshold, a waterfall of the sweeps, and a log of the signals found. Click a signal to tune it. The sweep itself is in
// core/src/sweep (docs/scanner.md); this file is the window and the worker thread.
#include "../d40/d40_ui.h"
#include "dect2/common/thread_name.h"
#include "../app.h"
#include "dect2/sweep/sweep.h"
#include "dect2/sweep/survey.h"
#include "../gfx/gfx.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <thread>

namespace {
using namespace dect2::sweep;

struct State {
    // settings
    float fromMHz = 87.5f, toMHz = 108.f, binKHz = 25.f, dwellMs = 10.f, settleMs = 3.f, floorPct = 50.f, thrDb = 8.f, everySec = 0.f, gainDb = 30.f;
    bool autoSettle = true;
    int rateIdx = 1;    // 0 radio's best, 1 10 Msps, 2 5 Msps, 3 2.4 Msps (limited radios)
    int modeIdx = -1;
    // run state
    std::thread th;
    std::atomic<bool> cancel{false}, running{false};
    std::mutex mu;
    Sweep last, base;
    bool haveBase = false;
    SignalLog log;
    std::deque<std::vector<float>> history;   // resampled sweeps, newest first
    int histCols = 0;
    std::string msg, info;
    double settleMeasured = 0;
    float compareChange = 0; bool haveCompare = false; SweepDiff diff;
    int sel = -1;
    uint64_t seq = 0;
    // band survey
    Survey survey, prevSurvey;
    bool recordSurvey = true, havePrev = false;
    float minOccPct = 50.f, slotKHz = 200.f;
    std::vector<PersistentSignal> persist;
    std::vector<QuietSlot> quiet;
    SurveyDiff sdiff;
    uint64_t cacheSeq = ~0ull; float cacheThr = -1, cacheOcc = -1, cacheSlot = -1; bool cachePrev = false;
};
State S;

constexpr int kHistRows = 120, kHistCols = 600;

void worker(DeviceInfo dev, TuneSettings t, SweepConfig c, bool autoSettle) {
    std::string err;
    IqSweepSource src(dev, t);
    if (!src.open(err)) { std::lock_guard<std::mutex> l(S.mu); S.msg = err; S.running = false; return; }
    if (autoSettle) {
        const double st = calibrateSettle(src, c.startHz, c.stopHz, err);
        if (st > 0) { c.settleSec = st; std::lock_guard<std::mutex> l(S.mu); S.settleMeasured = st; }
    }
    runSchedule(src, c, [&](const Sweep& s) {
        std::lock_guard<std::mutex> l(S.mu);
        S.last = s; S.log.update(s); S.seq++;
        d40ScanFeed(s);   // 4.0: activity map and new-signal alert (app/d40)
        if (S.recordSurvey) S.survey.add(s);
        std::vector<float> row(kHistCols, -150.f);
        for (size_t i = 0; i < s.bins(); i++) { float& r = row[std::min<size_t>(kHistCols - 1, i * kHistCols / s.bins())]; r = std::max(r, s.db[i]); }
        S.history.push_front(std::move(row));
        if ((int)S.history.size() > kHistRows) S.history.pop_back();
        char b[200];
        snprintf(b, sizeof b, tr("%.0f MHz/s, %d hops, %d averages, %.0f ms per sweep"), (c.stopHz - c.startHz) / 1e6 / std::max(1e-6, s.elapsedSec), s.hops, s.averages, s.elapsedSec * 1e3);
        S.info = b;
        return !S.cancel.load();
    }, err, &S.cancel);
    src.close();
    std::lock_guard<std::mutex> l(S.mu);
    if (!err.empty() && err != "cancelled") S.msg = err;
    S.running = false;
}

void stopSweep() {
    S.cancel = true;
    if (S.th.joinable()) S.th.join();
    S.running = false;
}

void startSweep(App& a) {
    stopSweep();
    if (a.engine.running()) a.engine.stop();
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size() || !a.devices[a.devIdx].isRadio()) { S.msg = tr("pick a radio first (the test signal and files cannot sweep)"); return; }
    DeviceInfo dev = a.devices[a.devIdx];
    TuneSettings t = a.tune;
    static const double kRates[4] = {20e6, 10e6, 5e6, 2.4e6};
    t.sampleRate = kRates[std::min(3, std::max(0, S.rateIdx))];
    if (dev.maxRateHz > 0) t.sampleRate = std::min(t.sampleRate, dev.maxRateHz);
    if (dev.isGeneric()) t.gainDb = S.gainDb;
    t.centerHz = S.fromMHz * 1e6;
    t.bandwidthMhz = t.sampleRate / 1e6;
    SweepConfig c;
    c.startHz = S.fromMHz * 1e6; c.stopHz = std::max(S.toMHz, S.fromMHz + 0.01f) * 1e6; c.binHz = S.binKHz * 1e3; c.dwellSec = S.dwellMs / 1e3;
    c.settleSec = S.settleMs / 1e3; c.floorPercentile = S.floorPct; c.thresholdDb = S.thrDb; c.minIntervalSec = S.everySec;
    S.cancel = false; S.running = true; S.msg.clear(); S.history.clear(); S.log.clear();
    if (!S.survey.rows() || std::fabs(S.survey.startHz() - c.startHz) > 1e3) S.survey.clear();   // a new range starts a new survey; the same range goes on
    S.th = dect2::namedThread("onair-sweep", worker, dev, t, c, S.autoSettle);
}

void tuneTo(App& a, double hz) {
    stopSweep();
    const int cur = S.modeIdx;
    if (cur >= 0 && cur < kNumModes) {
        const ModeDef& md = kModes[cur];
        if (!modeSelected(a, md)) { if (a.engine.running()) a.engine.stop(); setMode(a, md.family, subIndexOf(md.subId)); }
    }
    a.freqMhz = hz / 1e6;
    if (a.engine.running()) { a.tune.centerHz = hz; a.engine.retune(a.tune); } else startReceiver(a);
}

void csvButtons(App& a) {
    (void)a;
    std::string err;
    if (ImGui::SmallButton(tr("Save scan..."))) {
        const std::string p = saveFileDialog("sweep.csv");
        if (!p.empty()) { std::lock_guard<std::mutex> l(S.mu); std::vector<Sweep> v{S.last}; if (!writeCsv(p, v, err)) S.msg = err; }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("rtl_power compatible CSV: opens in Heatmap.py, rtl_power_fftw tools and spreadsheets"));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Load as baseline..."))) {
        const std::string p = openFileDialog();
        std::vector<Sweep> v;
        if (!p.empty()) {
            std::lock_guard<std::mutex> l(S.mu);
            if (readCsv(p, v, err) && !v.empty()) { S.base = v.back(); S.haveBase = true; } else S.msg = err;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Keep this scan as baseline"))) { std::lock_guard<std::mutex> l(S.mu); S.base = S.last; S.haveBase = S.last.bins() > 0; }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Save signal log..."))) {
        const std::string p = saveFileDialog("signals.csv");
        if (!p.empty()) { std::lock_guard<std::mutex> l(S.mu); if (!writeSignalsCsv(p, S.log.records(), err)) S.msg = err; }
    }
}


// The band survey: the sweeps of one range kept over time (core/src/sweep/survey.cpp)
void surveyPanel(App& a) {
    (void)a;
    if (!ImGui::CollapsingHeader(trw("Band survey"))) return;
    std::string err;
    std::lock_guard<std::mutex> l(S.mu);
    S.survey.setThreshold(S.thrDb);
    ImGui::Checkbox(tr("record every sweep into the survey"), &S.recordSurvey);
    ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi); ImGui::SliderFloat("##socc", &S.minOccPct, 5, 100, tr("persistent at %.0f%%"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A signal is persistent when it is above the threshold in at least this share of the sweeps"));
    ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi); ImGui::SliderFloat("##sslot", &S.slotKHz, 10, 2000, tr("quiet slot %.0f kHz"), ImGuiSliderFlags_Logarithmic);
    if (ImGui::SmallButton(tr("Clear survey"))) { S.survey.clear(); S.cacheSeq = ~0ull; }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Save survey..."))) { const std::string p = saveFileDialog("survey.oasurv"); if (!p.empty() && !S.survey.save(p, err)) S.msg = err; }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Load as previous survey..."))) {
        const std::string p = openFileDialog();
        if (!p.empty()) { if (S.prevSurvey.load(p, err)) { S.havePrev = true; S.cacheSeq = ~0ull; } else S.msg = err; }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Export occupancy CSV..."))) { const std::string p = saveFileDialog("occupancy.csv"); if (!p.empty() && !S.survey.exportOccupancyCsv(p, err)) S.msg = err; }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Export persistent signals CSV..."))) { const std::string p = saveFileDialog("persistent.csv"); if (!p.empty() && !S.survey.exportSignalsCsv(p, S.minOccPct / 100.0, err)) S.msg = err; }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Export picture..."))) {
        const std::string p = saveFileDialog("survey.png");
        if (!p.empty()) { const int w = 1600, h = (int)std::min<size_t>(1200, std::max<size_t>(100, S.survey.rows())); const auto img = S.survey.heatmap(w, h); if (!gfx::writePng(p.c_str(), img.data(), w, h)) S.msg = tr("cannot write the picture"); }
    }
    if (S.survey.rows() == 0) { ImGui::TextDisabled("%s", tr("Run a sweep: every sweep adds a row to the survey. With \"every\" set to a few minutes it can run for hours in the background.")); return; }
    if (S.cacheSeq != S.seq || S.cacheThr != S.thrDb || S.cacheOcc != S.minOccPct || S.cacheSlot != S.slotKHz || S.cachePrev != S.havePrev) {
        S.persist = S.survey.persistent(S.minOccPct / 100.0);
        S.quiet = S.survey.quiet(S.slotKHz * 1e3, 8);
        if (S.havePrev) S.sdiff = compareSurveys(S.prevSurvey, S.survey, S.minOccPct / 100.0);
        S.cacheSeq = S.seq; S.cacheThr = S.thrDb; S.cacheOcc = S.minOccPct; S.cacheSlot = S.slotKHz; S.cachePrev = S.havePrev;
    }
    ImGui::Text(tr("%zu sweeps over %.1f h"), S.survey.rows(), (S.survey.lastTime() - S.survey.firstTime()) / 3600.0);
    if (S.havePrev) {
        ImGui::TextColored(ImVec4(.6f, .9f, 1, 1), tr("since the previous survey: floor %+.1f dB, occupancy %+.1f%%, %zu new, %zu gone"), S.sdiff.floorChangeDb,
                           S.sdiff.occupancyChange * 100.0, S.sdiff.appeared.size(), S.sdiff.vanished.size());
        for (const auto& p : S.sdiff.appeared) ImGui::BulletText(tr("new %.4f MHz, %.0f%% of the time"), p.freqHz / 1e6, p.occupancy * 100.0);
        for (const auto& p : S.sdiff.vanished) ImGui::BulletText(tr("gone %.4f MHz, was %.0f%% of the time"), p.freqHz / 1e6, p.occupancy * 100.0);
    }
    // occupancy heatmap: time down, frequency across
    {
        const int cols = 600, rws = (int)std::min<size_t>(150, S.survey.rows());
        const size_t B = S.survey.bins(), R = S.survey.rows();
        std::vector<float> g((size_t)rws * cols, 0.f);
        for (int y = 0; y < rws; y++) {
            const size_t r0 = (size_t)y * R / rws, r1 = std::max(r0 + 1, (size_t)(y + 1) * R / rws);
            for (size_t r = r0; r < r1 && r < R; r++)
                for (size_t i = 0; i < B; i++) { float& c = g[(size_t)y * cols + std::min<size_t>(cols - 1, i * cols / B)]; c = std::max(c, S.survey.level(r, i)); }
        }
        if (plt::BeginPlot("##surveyhm", ImVec2(-1, 200 * gUi), plt::Flags_NoLegend)) {
            plt::SetupAxes("MHz", "");
            const double x0 = S.survey.startHz() / 1e6, x1 = S.survey.freqOf(B - 1) / 1e6;
            plt::SetupAxisLimits(plt::X1, x0, x1, plt::Cond_Always);
            plt::SetupAxisLimits(plt::Y1, 0, 1, plt::Cond_Always);
            plt::PushColormap(0);
            plt::PlotHeatmap("occ", g.data(), rws, cols, 0.f, 35.f, nullptr, plt::Point(x0, 0), plt::Point(x1, 1));
            plt::EndPlot();
        }
    }
    if (ImGui::BeginTable("##persist", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 130 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("MHz")); ImGui::TableSetupColumn(tr("Bandwidth kHz")); ImGui::TableSetupColumn(tr("Occupancy %"));
        ImGui::TableSetupColumn(tr("Mean dB above floor")); ImGui::TableSetupColumn(tr("Peak dB above floor"));
        ImGui::TableHeadersRow();
        for (const auto& p : S.persist) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.4f", p.freqHz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", p.bwHz / 1e3);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", p.occupancy * 100.0);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", p.meanDb);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", p.maxDb);
        }
        ImGui::EndTable();
    }
    {
        const auto rec = S.survey.recurring();
        if (!rec.empty()) {
            ImGui::Text("%s", tr("Signals on a schedule"));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Signals that come back at regular times during this survey, and when each is due next."));
            const double now = (double)std::time(nullptr);
            for (size_t i = 0; i < rec.size() && i < 20; i++) {
                const auto& q = rec[i];
                ImGui::Text("%.3f MHz  %s %.0f min  %s %.0f min  (%d %s)", q.freqHz / 1e6, tr("every"), q.periodSec / 60.0, tr("next in"), std::max(0.0, q.nextSec - now) / 60.0, q.starts, tr("times"));
            }
        }
    }
    d40ScanCard(S.persist);   // 4.0: activity map, signal names, new-signal alerts (app/d40)
    ImGui::Text("%s", tr("Quietest slots"));
    if (ImGui::BeginTable("##quiet", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn(tr("From MHz")); ImGui::TableSetupColumn(tr("Width kHz")); ImGui::TableSetupColumn(tr("Occupancy %")); ImGui::TableSetupColumn(tr("Peak dB above floor"));
        ImGui::TableHeadersRow();
        for (const auto& q : S.quiet) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.4f", q.startHz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", q.widthHz / 1e3);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", q.occupancy * 100.0);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", q.maxDb);
        }
        ImGui::EndTable();
    }
}

void windowPanel(App& a) {
    firstWindowSize(900 * gUi, 760 * gUi);
    if (!ImGui::Begin(trw("Sweep scanner"), &a.sweepOpen)) { ImGui::End(); return; }
    const bool run = S.running.load();
    if (S.th.joinable() && !run) S.th.join();
    ImGui::BeginDisabled(run);
    ImGui::SetNextItemWidth(90 * gUi); ImGui::InputFloat(tr("from MHz"), &S.fromMHz, 0, 0, "%.3f");
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ImGui::InputFloat(tr("to MHz"), &S.toMHz, 0, 0, "%.3f");
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); ImGui::SliderFloat("##bin", &S.binKHz, 1, 500, tr("bin %.0f kHz"), ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); ImGui::SliderFloat("##dwell", &S.dwellMs, 1, 200, tr("dwell %.0f ms"), ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Time spent on each hop: longer averages more FFTs (a steadier floor, weak signals show) but sweeps slower"));
    ImGui::SameLine(); ImGui::Checkbox(tr("measure retune time"), &S.autoSettle);
    if (!S.autoSettle) { ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi); ImGui::SliderFloat("##settle", &S.settleMs, 0, 50, tr("settle %.0f ms")); }
    static const char* const kRates[4] = {N_("rate: radio's best"), N_("rate: 10 Msps"), N_("rate: 5 Msps"), N_("rate: 2.4 Msps")};
    ImGui::SetNextItemWidth(150 * gUi);
    if (ImGui::BeginCombo("##rate", tr(kRates[S.rateIdx]))) { for (int i = 0; i < 4; i++) if (ImGui::Selectable(tr(kRates[i]), i == S.rateIdx)) S.rateIdx = i; ImGui::EndCombo(); }
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); ImGui::SliderFloat("##gain", &S.gainDb, 0, 60, tr("gain %.0f dB"));
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); ImGui::SliderFloat("##every", &S.everySec, 0, 600, tr(S.everySec > 0 ? "every %.0f s" : "back to back"), ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Pause between sweeps: a repeated sweep every so many seconds keeps the CPU and the battery calm"));
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(130 * gUi); ImGui::SliderFloat("##thr", &S.thrDb, 3, 30, tr("threshold floor+%.0f dB"));
    ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi); ImGui::SliderFloat("##pct", &S.floorPct, 5, 90, tr("floor percentile %.0f"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The noise floor is this percentile of the bin power in each hop (50 = median). Lower it in crowded bands, raise it on empty ones"));
    ImGui::SameLine();
    if (ImGui::Button(tr(run ? "Stop" : "Sweep"))) { if (run) stopSweep(); else startSweep(a); }
    {
        std::lock_guard<std::mutex> l(S.mu);
        if (!S.msg.empty()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, .7f, .3f, 1), "%s", S.msg.c_str()); }
        if (!S.info.empty()) ImGui::TextDisabled("%s", S.info.c_str());
        if (S.settleMeasured > 0) { ImGui::SameLine(); ImGui::TextDisabled(tr("retune settle %.1f ms"), S.settleMeasured * 1e3); }
    }
    csvButtons(a);
    ImGui::Separator();
    Sweep sw;
    std::vector<SignalRecord> recs;
    std::vector<float> hist;
    int rows = 0;
    {
        std::lock_guard<std::mutex> l(S.mu);
        sw = S.last; recs = S.log.records();
        rows = (int)S.history.size();
        hist.reserve((size_t)rows * kHistCols);
        for (auto& r : S.history) hist.insert(hist.end(), r.begin(), r.end());
        S.haveCompare = S.haveBase && sw.bins() > 0;
        if (S.haveCompare) S.diff = compareSweeps(S.base, sw);
    }
    if (sw.bins()) {
        const float thrLine = sw.floorOverallDb + S.thrDb;
        ImGui::Text(tr("noise floor %.1f dBFS per bin (%.0f kHz bins), threshold %.1f dBFS, %zu signals in this sweep"), sw.floorOverallDb, sw.binHz / 1e3, thrLine, sw.signals.size());
        if (S.haveCompare) { ImGui::SameLine(); ImGui::TextColored(ImVec4(.6f, .9f, 1, 1), tr("vs baseline: floor %+.1f dB, largest rise %+.1f dB at %.3f MHz"), S.diff.floorChangeDb, S.diff.maxRiseDb, S.diff.maxRiseHz / 1e6); }
        // trace (max over the bins that share a pixel column), floor and threshold
        const int W = 1600;
        std::vector<double> xs, ys, fl, th;
        const size_t per = std::max<size_t>(1, sw.bins() / W);
        for (size_t i = 0; i < sw.bins(); i += per) {
            float mx = -200; size_t e = std::min(sw.bins(), i + per);
            for (size_t j = i; j < e; j++) mx = std::max(mx, sw.db[j]);
            xs.push_back(sw.freqOf(i) / 1e6); ys.push_back(mx); fl.push_back(sw.floorDb[i]); th.push_back(sw.floorDb[i] + S.thrDb);
        }
        if (plt::BeginPlot("##sweeptrace", ImVec2(-1, 260 * gUi), plt::Flags_NoLegend)) {
            plt::SetupAxes("MHz", "dBFS");
            plt::SetupAxisLimits(plt::X1, xs.front(), xs.back(), plt::Cond_Always);
            plt::SetupAxisLimits(plt::Y1, sw.floorOverallDb - 12, std::max((double)sw.floorOverallDb + 40.0, *std::max_element(ys.begin(), ys.end()) + 5), plt::Cond_Once);
            plt::Spec sp; sp.LineColor = ImVec4(0.4f, 0.8f, 1, 1);
            plt::PlotLine("trace", xs.data(), ys.data(), (int)xs.size(), sp);
            plt::Spec sf; sf.LineColor = ImVec4(0.4f, 0.9f, 0.4f, 1);
            plt::PlotLine("floor", xs.data(), fl.data(), (int)xs.size(), sf);
            plt::Spec st; st.LineColor = ImVec4(1, 0.6f, 0.2f, 0.9f);
            plt::PlotLine("threshold", xs.data(), th.data(), (int)xs.size(), st);
            if (plt::IsPlotHovered() && ImGui::IsMouseClicked(0)) {   // click a signal: select the nearest detection
                const double mx = plt::GetPlotMousePos().x * 1e6;
                double best = 1e18;
                for (int i = 0; i < (int)recs.size(); i++) if (std::fabs(recs[i].freqHz - mx) < best) { best = std::fabs(recs[i].freqHz - mx); S.sel = i; }
                if (best > 20 * sw.binHz) S.sel = -1;
            }
            for (int i = 0; i < (int)recs.size(); i++) {
                const double f = recs[i].freqHz / 1e6, y = recs[i].peakDb;
                plt::Spec m; m.LineColor = i == S.sel ? ImVec4(1, 1, 0.2f, 1) : ImVec4(1, 0.4f, 0.4f, 1); m.Marker = plt::Marker_Circle; m.MarkerSize = 4;
                plt::PlotScatter("sig", &f, &y, 1, m);
            }
            plt::EndPlot();
        }
        if (rows > 1 && plt::BeginPlot("##sweepwf", ImVec2(-1, 150 * gUi), plt::Flags_NoLegend)) {
            plt::SetupAxes("MHz", "");
            plt::SetupAxisLimits(plt::X1, sw.startHz / 1e6, sw.freqOf(sw.bins() - 1) / 1e6, plt::Cond_Always);
            plt::SetupAxisLimits(plt::Y1, 0, 1, plt::Cond_Always);
            plt::PushColormap(0);
            plt::PlotHeatmap("wf", hist.data(), rows, kHistCols, sw.floorOverallDb - 5, std::max(sw.floorOverallDb + 35.f, sw.floorOverallDb + 20.f), nullptr,
                             plt::Point(sw.startHz / 1e6, 0), plt::Point(sw.freqOf(sw.bins() - 1) / 1e6, 1));
            plt::EndPlot();
        }
    }
    surveyPanel(a);
    // activity log
    ImGui::Text("%s", tr("Signals found"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220 * gUi);
    {
        const char* cur = S.modeIdx >= 0 ? kModes[S.modeIdx].name : tr("mode: as it is");
        if (ImGui::BeginCombo("##tmode", cur)) {
            if (ImGui::Selectable(tr("mode: as it is"), S.modeIdx < 0)) S.modeIdx = -1;
            for (int i = 0; i < kNumModes; i++) if (ImGui::Selectable(kModes[i].name, i == S.modeIdx)) S.modeIdx = i;
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(S.sel < 0 || S.sel >= (int)recs.size());
    if (ImGui::Button(tr("Tune to selected"))) tuneTo(a, recs[S.sel].freqHz);
    ImGui::SameLine();
    if (ImGui::Button(tr("Identify selected"))) { stopSweep(); identifyTuned(a, recs[S.sel].freqHz, recs[S.sel].bwHz); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Tune the radio to the selected signal and say what it is (needs the receiver in a mode whose sample rate covers it)"));
    ImGui::EndDisabled();
    if (ImGui::BeginTable("##sweepsigs", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders | ImGuiTableFlags_Sortable, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("MHz")); ImGui::TableSetupColumn(tr("Bandwidth kHz")); ImGui::TableSetupColumn(tr("Peak dBFS"));
        ImGui::TableSetupColumn(tr("SNR dB")); ImGui::TableSetupColumn(tr("Best SNR dB")); ImGui::TableSetupColumn(tr("Seen")); ImGui::TableSetupColumn(tr("Last seen"));
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)recs.size(); i++) {
            const auto& r = recs[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char b[64]; snprintf(b, sizeof b, "%.4f", r.freqHz / 1e6);
            if (ImGui::Selectable(b, S.sel == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) { S.sel = i; if (ImGui::IsMouseDoubleClicked(0)) tuneTo(a, r.freqHz); }
            ImGui::TableNextColumn(); ImGui::Text("%.1f", r.bwHz / 1e3);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", r.peakDb);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", r.snrDb);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", r.maxSnrDb);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.count);
            ImGui::TableNextColumn(); ImGui::Text("%s", dect2::timefmt::clock((int64_t)r.lastSeen).c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
} // namespace

void sweepWindow(App& a) {
    if (a.sweepOpen) windowPanel(a);
    else if (S.running.load()) stopSweep();   // closing the window gives the radio back
}
