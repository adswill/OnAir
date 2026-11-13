// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The interference hunter (View > Interference hunter): watches a band for new interference and finds where it comes from.
// Monitor: repeated sweeps against a learned baseline, alerts for new spurs and a rising noise floor, with a guess of the kind of source.
// Walk: a loud / quiet meter while you carry the radio around, with a log of readings by time and position (docs/rfi-hunter.md).
// The sweeps come from the radio through the same sweep engine as the Sweep scanner, or from a built-in simulator (demo). The analysis is in core/src/rfi.
#include "dect2/common/thread_name.h"
#include "../app.h"
#include "../ui/scale.h"
#include "dect2/rfi/rfi.h"
#include "dect2/sweep/sweep.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#endif

namespace {
using namespace dect2::rfi;

enum GpsMode { GpsNone, GpsReceiver, GpsManual, GpsNmea, NumGps };

struct State {
    bool loaded = false;
    float fromMHz = 87.5f, toMHz = 108.f, binKHz = 25.f, dwellMs = 10.f, everySec = 5.f, gainDb = 30.f, thrDb = 8.f, floorRiseDb = 3.f;
    int rateIdx = 1, confirm = 2;
    bool demo = false;
    // sweeps
    std::thread th;
    std::atomic<bool> cancel{false}, running{false};
    std::mutex mu;
    std::deque<dect2::sweep::Sweep> pending;
    std::string msg, info;
    // the analysis (UI thread only)
    Monitor mon;
    Walk walk;
    dect2::sweep::Sweep last;
    size_t shownLog = 0;
    std::string banner;
    double demoT = 0, demoNextAt = 0;
    // position
    int gpsMode = GpsReceiver;
    double manLat = 0, manLon = 0;
    char nmeaPath[400] = "";
    std::thread gth;
    std::atomic<bool> gcancel{false}, grun{false};
    std::mutex gmu;
    Fix fix;
    double fixAt = 0;
    std::string gpsMsg;
};
State S;

const char* const kKindNames[] = {N_("Unknown"), N_("Switching power supply"), N_("LED lamp or driver"), N_("Powerline (PLC)"), N_("Local oscillator or clock leak")};
const char* kindText(Kind k) { return tr(kKindNames[(int)k]); }

void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& p = plat::prefs();
    S.fromMHz = (float)p.getD("rfiFrom", 87.5); S.toMHz = (float)p.getD("rfiTo", 108.0); S.binKHz = (float)p.getD("rfiBin", 25.0);
    S.everySec = (float)p.getD("rfiEvery", 5.0); S.gpsMode = std::clamp((int)p.getI("rfiGps", GpsReceiver), 0, NumGps - 1);
    S.manLat = p.getD("rfiLat", 0); S.manLon = p.getD("rfiLon", 0);
    snprintf(S.nmeaPath, sizeof S.nmeaPath, "%s", p.getS("rfiNmea", "").c_str());
}
void save() {
    plat::Prefs& p = plat::prefs();
    p.setD("rfiFrom", S.fromMHz); p.setD("rfiTo", S.toMHz); p.setD("rfiBin", S.binKHz); p.setD("rfiEvery", S.everySec);
    p.setI("rfiGps", S.gpsMode); p.setD("rfiLat", S.manLat); p.setD("rfiLon", S.manLon); p.setS("rfiNmea", S.nmeaPath);
}

// ---- GPS from an NMEA stream (a serial GPS puck or a log file); lines are read until the window is closed ----
void gpsWorker(std::string path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) { std::lock_guard<std::mutex> l(S.gmu); S.gpsMsg = tr("Cannot open the GPS device or file."); S.grun = false; return; }
#ifndef _WIN32
    if (isatty(fileno(f))) {   // a serial GPS: 9600 baud 8N1 raw, a read gives up after 0.2 s so the thread can be stopped
        termios t;
        if (tcgetattr(fileno(f), &t) == 0) {
            cfmakeraw(&t); cfsetspeed(&t, B9600); t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 2;
            tcsetattr(fileno(f), TCSANOW, &t);
        }
    }
#endif
    char line[512];
    while (!S.gcancel.load()) {
        if (!fgets(line, sizeof line, f)) { clearerr(f); std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
        Fix fx;
        if (parseNmea(line, fx) && fx.valid) {
            std::lock_guard<std::mutex> l(S.gmu);
            S.fix = fx; S.fixAt = ImGui::GetTime(); S.gpsMsg.clear();
        }
    }
    fclose(f);
    S.grun = false;
}
void stopGps() { S.gcancel = true; if (S.gth.joinable()) S.gth.join(); S.grun = false; }
void startGps() {
    stopGps();
    if (S.gpsMode != GpsNmea || !S.nmeaPath[0]) return;
    S.gcancel = false; S.grun = true; S.gpsMsg = tr("Waiting for a position fix.");
    S.gth = dect2::namedThread("onair-rfigps", gpsWorker, std::string(S.nmeaPath));
}
bool position(double& lat, double& lon) {
    if (S.gpsMode == GpsReceiver) {
        if (!receiverLocationSet()) return false;
        lat = plat::prefs().getD("adsbLat", 0); lon = plat::prefs().getD("adsbLon", 0); return true;
    }
    if (S.gpsMode == GpsManual) { lat = S.manLat; lon = S.manLon; return S.manLat != 0 || S.manLon != 0; }
    if (S.gpsMode == GpsNmea) {
        std::lock_guard<std::mutex> l(S.gmu);
        if (!S.fix.valid || ImGui::GetTime() - S.fixAt > 10) return false;
        lat = S.fix.lat; lon = S.fix.lon; return true;
    }
    return false;
}

// ---- sweeps ----
void worker(DeviceInfo dev, TuneSettings t, dect2::sweep::SweepConfig c) {
    using namespace dect2::sweep;
    std::string err;
    IqSweepSource src(dev, t);
    if (!src.open(err)) { std::lock_guard<std::mutex> l(S.mu); S.msg = err; S.running = false; return; }
    const double st = calibrateSettle(src, c.startHz, c.stopHz, err);
    if (st > 0) c.settleSec = st;
    runSchedule(src, c, [&](const Sweep& s) {
        std::lock_guard<std::mutex> l(S.mu);
        S.pending.push_back(s);
        while (S.pending.size() > 200) S.pending.pop_front();
        return !S.cancel.load();
    }, err, &S.cancel);
    src.close();
    std::lock_guard<std::mutex> l(S.mu);
    if (!err.empty() && err != "cancelled") S.msg = err;
    S.running = false;
}
void stopSweep() { S.cancel = true; if (S.th.joinable()) S.th.join(); S.running = false; }

void resetAnalysis() {
    dect2::rfi::MonitorConfig mc; mc.confirmSweeps = S.confirm; mc.floorRiseDb = S.floorRiseDb; mc.spur.thresholdDb = S.thrDb;
    S.mon = Monitor(mc);
    WalkConfig wc; wc.spur.thresholdDb = S.thrDb;
    S.walk = Walk(wc);
    S.last = dect2::sweep::Sweep();
    S.shownLog = 0; S.banner.clear();
    std::lock_guard<std::mutex> l(S.mu);
    S.pending.clear();
}

void startSweep(App& a) {
    stopSweep();
    resetAnalysis();
    S.msg.clear();
    if (S.demo) { S.running = true; S.demoT = 0; S.demoNextAt = 0; return; }
    if (a.engine.running()) a.engine.stop();
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size() || !a.devices[a.devIdx].isRadio()) { S.msg = tr("pick a radio first (the test signal and files cannot sweep)"); return; }
    DeviceInfo dev = a.devices[a.devIdx];
    TuneSettings t = a.tune;
    static const double kRates[4] = {20e6, 10e6, 5e6, 2.4e6};
    t.sampleRate = kRates[std::clamp(S.rateIdx, 0, 3)];
    if (dev.maxRateHz > 0) t.sampleRate = std::min(t.sampleRate, dev.maxRateHz);
    if (dev.isGeneric()) t.gainDb = S.gainDb;
    t.centerHz = S.fromMHz * 1e6;
    t.bandwidthMhz = t.sampleRate / 1e6;
    dect2::sweep::SweepConfig c;
    c.startHz = S.fromMHz * 1e6; c.stopHz = std::max(S.toMHz, S.fromMHz + 0.01f) * 1e6; c.binHz = S.binKHz * 1e3; c.dwellSec = S.dwellMs / 1e3;
    c.thresholdDb = S.thrDb; c.minIntervalSec = S.everySec;
    S.cancel = false; S.running = true;
    S.th = dect2::namedThread("onair-rfi", worker, dev, t, c);
}

// the demo: three broadcast carriers, then a switching supply, an LED lamp and an oscillator leak come and go (simulated time runs 10 s per sweep)
SimScene demoScene() {
    SimScene sc;
    sc.startHz = S.fromMHz * 1e6; sc.stopHz = std::max(S.toMHz, S.fromMHz + 0.5f) * 1e6; sc.binHz = S.binKHz * 1e3; sc.seed = 9;
    auto add = [&](SimSource::Type ty, double f, float lvl, double on = 0, double off = 1e18, double sp = 0, int teeth = 0, double w = 0) {
        SimSource s; s.type = ty; s.freqHz = f; s.levelDb = lvl; s.onSec = on; s.offSec = off; s.spacingHz = sp; s.teeth = teeth; s.widthHz = w; sc.sources.push_back(s);
    };
    const double a = sc.startHz, span = sc.stopHz - sc.startHz;
    add(SimSource::Carrier, a + 0.15 * span, 45); add(SimSource::Carrier, a + 0.5 * span, 38); add(SimSource::Carrier, a + 0.8 * span, 42);
    add(SimSource::Comb, a + 0.25 * span, 24, 150, 450, std::max(5.0 * sc.binHz, 0.01 * span), 9);
    add(SimSource::Hump, a + 0.55 * span, 10, 600, 1000, 0, 0, 0.2 * span);
    add(SimSource::Comb, a + 0.55 * span + 2.5 * sc.binHz, 20, 600, 1000, 6 * sc.binHz, 8);
    add(SimSource::Lo, a + 0.68 * span, 30, 1100, 1e18);
    return sc;
}

void pump() {
    std::deque<dect2::sweep::Sweep> got;
    if (S.demo && S.running.load() && ImGui::GetTime() >= S.demoNextAt) {
        S.demoNextAt = ImGui::GetTime() + 0.4;
        static SimScene sc; if (S.demoT == 0) sc = demoScene();
        dect2::sweep::Sweep s = simulateSweep(sc, S.demoT);
        s.timeSec = 1.7e9 + S.demoT;   // a clock-like time for the logs
        got.push_back(s);
        S.demoT += 10;
        if (S.demoT > 1500) S.demoT = 0, S.mon.relearn();
    }
    { std::lock_guard<std::mutex> l(S.mu); got.insert(got.end(), S.pending.begin(), S.pending.end()); S.pending.clear(); }
    for (const auto& s : got) {
        S.last = s;
        double la = 0, lo = 0;
        const bool pos = position(la, lo);
        S.walk.add(s, pos ? &la : nullptr, pos ? &lo : nullptr);
        S.mon.add(s);
    }
    const auto& lg = S.mon.log();
    for (; S.shownLog < lg.size(); S.shownLog++) {
        const Alert& a = lg[S.shownLog];
        char b[256];
        if (a.type == Alert::Cleared) snprintf(b, sizeof b, "%s", tr("Interference gone"));
        else if (a.type == Alert::FloorRise) snprintf(b, sizeof b, tr("The noise floor rose by %.1f dB"), a.levelDb);
        else if (a.type == Alert::NewHump) snprintf(b, sizeof b, tr("New broadband noise %.2f - %.2f MHz, %.0f dB: %s"), a.freqHz / 1e6, (a.freqHz + a.bwHz) / 1e6, a.levelDb, kindText(a.kind));
        else snprintf(b, sizeof b, tr("New signal at %.4f MHz, %.0f dB above the baseline: %s"), a.freqHz / 1e6, a.levelDb, kindText(a.kind));
        S.banner = b;
    }
}

std::string hhmmss(double t) { return dect2::timefmt::clock((int64_t)t); }

void controls(App& a) {
    const bool run = S.running.load();
    if (S.th.joinable() && !run) S.th.join();
    ImGui::BeginDisabled(run);
    ImGui::Checkbox(tr("Demo (simulated sweeps)"), &S.demo);
    ImGui::SetNextItemWidth(90 * gUi); ImGui::InputFloat(tr("from MHz"), &S.fromMHz, 0, 0, "%.3f");
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ImGui::InputFloat(tr("to MHz"), &S.toMHz, 0, 0, "%.3f");
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); ImGui::SliderFloat("##rbin", &S.binKHz, 5, 200, tr("bin %.0f kHz"), ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A comb of spurs needs a spacing of at least 5 bins to be told from a wide signal: use finer bins to see closely spaced spurs"));
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); ImGui::SliderFloat("##rdwell", &S.dwellMs, 1, 200, tr("dwell %.0f ms"), ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); ImGui::SliderFloat("##revery", &S.everySec, 0, 600, tr(S.everySec > 0 ? "every %.0f s" : "back to back"), ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Pause between sweeps: the monitor can run for hours with long pauses; the walk wants back to back"));
    static const char* const kRates[4] = {N_("rate: radio's best"), N_("rate: 10 Msps"), N_("rate: 5 Msps"), N_("rate: 2.4 Msps")};
    ImGui::SameLine(); ImGui::SetNextItemWidth(150 * gUi);
    if (ImGui::BeginCombo("##rrate", tr(kRates[S.rateIdx]))) { for (int i = 0; i < 4; i++) if (ImGui::Selectable(tr(kRates[i]), i == S.rateIdx)) S.rateIdx = i; ImGui::EndCombo(); }
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); ImGui::SliderFloat("##rgain", &S.gainDb, 0, 60, tr("gain %.0f dB"));
    ImGui::EndDisabled();
    if (ImGui::Button(tr(run ? "Stop" : "Start"))) { if (run) { stopSweep(); save(); } else { save(); startSweep(a); } }
    ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi);
    if (ImGui::SliderFloat("##rthr", &S.thrDb, 4, 25, tr("spur threshold %.0f dB"))) { dect2::rfi::MonitorConfig mc = S.mon.config(); mc.spur.thresholdDb = S.thrDb; S.mon.setConfig(mc); WalkConfig wc = S.walk.config(); wc.spur.thresholdDb = S.thrDb; S.walk.setConfig(wc); }
    ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi);
    if (ImGui::SliderFloat("##rrise", &S.floorRiseDb, 1, 12, tr("floor alert %.0f dB"))) { dect2::rfi::MonitorConfig mc = S.mon.config(); mc.floorRiseDb = S.floorRiseDb; S.mon.setConfig(mc); }
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::SliderInt("##rconf", &S.confirm, 1, 5, tr("confirm in %d sweeps"))) { dect2::rfi::MonitorConfig mc = S.mon.config(); mc.confirmSweeps = S.confirm; S.mon.setConfig(mc); }
    {
        std::lock_guard<std::mutex> l(S.mu);
        if (!S.msg.empty()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, .7f, .3f, 1), "%s", S.msg.c_str()); }
    }
    if (!S.banner.empty()) ImGui::TextColored(ImVec4(1, .55f, .25f, 1), "%s", S.banner.c_str());
}

void tracePlot() {
    const auto& s = S.last;
    if (s.bins() < 2) return;
    const auto& base = S.mon.baseline();
    const size_t per = std::max<size_t>(1, s.bins() / 1600);
    std::vector<double> xs, ys, bs;
    for (size_t i = 0; i < s.bins(); i += per) {
        float mx = -200, bm = -200;
        for (size_t j = i; j < std::min(s.bins(), i + per); j++) { mx = std::max(mx, s.db[j]); if (base.size() == s.bins()) bm = std::max(bm, base[j]); }
        xs.push_back(s.freqOf(i) / 1e6); ys.push_back(mx); bs.push_back(bm);
    }
    if (plt::BeginPlot("##rfitrace", ImVec2(-1, 230 * gUi), plt::Flags_NoLegend)) {
        plt::SetupAxes("MHz", "dBFS");
        plt::SetupAxisLimits(plt::X1, xs.front(), xs.back(), plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, s.floorOverallDb - 12, std::max((double)s.floorOverallDb + 40.0, *std::max_element(ys.begin(), ys.end()) + 5), plt::Cond_Once);
        if (!S.mon.baseline().empty()) { plt::Spec sb; sb.LineColor = ImVec4(0.5f, 0.5f, 0.5f, 0.9f); plt::PlotLine("baseline", xs.data(), bs.data(), (int)xs.size(), sb); }
        plt::Spec sp; sp.LineColor = ImVec4(0.4f, 0.8f, 1, 1);
        plt::PlotLine("now", xs.data(), ys.data(), (int)xs.size(), sp);
        for (const Finding& f : S.mon.active())
            for (double m : f.members) {
                const double x = m / 1e6, y = s.floorOverallDb + f.levelDb;
                plt::Spec mk; mk.LineColor = ImVec4(1, 0.4f, 0.3f, 1); mk.Marker = plt::Marker_Circle; mk.MarkerSize = 4;
                plt::PlotScatter("rfi", &x, &y, 1, mk);
            }
        plt::EndPlot();
    }
}

void floorPlot() {
    const auto& h = S.mon.history();
    if (h.size() < 2) return;
    std::vector<double> xs, ys;
    const double t0 = h.front().time;
    for (const FloorPoint& p : h) { xs.push_back((p.time - t0) / 60.0); ys.push_back(p.deltaDb); }
    if (plt::BeginPlot("##rfifloor", ImVec2(-1, 120 * gUi), plt::Flags_NoLegend)) {
        plt::SetupAxes(tr("minutes"), tr("floor change dB"));
        plt::SetupAxisLimits(plt::X1, 0, std::max(1.0, xs.back()), plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, -3, std::max(8.0, *std::max_element(ys.begin(), ys.end()) + 1), plt::Cond_Once);
        plt::Spec sp; sp.LineColor = ImVec4(0.4f, 0.9f, 0.4f, 1);
        plt::PlotLine("floor", xs.data(), ys.data(), (int)xs.size(), sp);
        const double lim[2] = {0, xs.back()}, thr[2] = {S.floorRiseDb, S.floorRiseDb};
        plt::Spec st; st.LineColor = ImVec4(1, 0.6f, 0.2f, 0.9f);
        plt::PlotLine("alert", lim, thr, 2, st);
        plt::EndPlot();
    }
}

void monitorTab() {
    std::string err;
    if (S.mon.learning()) ImGui::TextDisabled(tr("Learning the baseline: %d of %d sweeps. Keep the band as it normally is."), S.mon.learned(), S.mon.config().learnSweeps);
    else if (S.last.bins()) ImGui::Text(tr("Watching. Noise floor %.1f dBFS, baseline %.1f dBFS, %zu known signals."), S.last.floorOverallDb, S.mon.baselineFloorDb(), S.mon.baselineSpurs().size());
    else ImGui::TextDisabled("%s", tr("Press Start. The first sweeps become the baseline: what is normal in this band."));
    if (ImGui::SmallButton(tr("Learn the baseline again"))) { S.mon.relearn(); S.shownLog = 0; }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("What is there now is normal"))) S.mon.accept();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Adds the interference that has been reported to the baseline, so it raises no more alerts"));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Save the alerts..."))) { const std::string p = saveFileDialog("rfi-alerts.csv"); if (!p.empty() && !S.mon.exportLogCsv(p, err)) S.msg = err; }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Save the noise floor log..."))) { const std::string p = saveFileDialog("rfi-floor.csv"); if (!p.empty() && !S.mon.exportFloorCsv(p, err)) S.msg = err; }
    tracePlot();
    floorPlot();
    ImGui::Text("%s", tr("Interference now"));
    if (ImGui::BeginTable("##rfiact", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders, ImVec2(0, 100 * gUi))) {
        ImGui::TableSetupColumn(tr("Likely source")); ImGui::TableSetupColumn(tr("Confidence")); ImGui::TableSetupColumn(tr("From MHz"));
        ImGui::TableSetupColumn(tr("To MHz")); ImGui::TableSetupColumn(tr("Spacing kHz"));
        ImGui::TableHeadersRow();
        for (const Finding& f : S.mon.active()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", kindText(f.kind));
            ImGui::TableNextColumn(); ImGui::Text("%.0f%%", f.confidence * 100.0);
            ImGui::TableNextColumn(); ImGui::Text("%.4f", f.startHz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.4f", f.stopHz / 1e6);
            ImGui::TableNextColumn(); if (f.spacingHz > 0) ImGui::Text("%.1f (%d)", f.spacingHz / 1e3, f.count); else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
    ImGui::Text("%s", tr("Alerts"));
    if (ImGui::BeginTable("##rfilog", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 130 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(tr("Event")); ImGui::TableSetupColumn(tr("MHz"));
        ImGui::TableSetupColumn(tr("dB above baseline")); ImGui::TableSetupColumn(tr("Likely source"));
        ImGui::TableHeadersRow();
        const auto& lg = S.mon.log();
        for (size_t i = lg.size(); i-- > 0;) {
            const Alert& x = lg[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", hhmmss(x.time).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(x.type == Alert::NewSpur ? tr("new signal") : x.type == Alert::FloorRise ? tr("noise floor up") : x.type == Alert::NewHump ? tr("new broadband noise") : tr("gone"));
            ImGui::TableNextColumn(); if (x.freqHz > 0) ImGui::Text("%.4f", x.freqHz / 1e6); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%.1f", x.levelDb);
            ImGui::TableNextColumn(); ImGui::Text("%s", x.kind == Kind::Unknown ? "-" : kindText(x.kind));
        }
        ImGui::EndTable();
    }
}

void gpsPanel() {
    static const char* const kGps[NumGps] = {N_("no position"), N_("the receiver position (set for ADS-B)"), N_("type a position"), N_("GPS device or NMEA file")};
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::BeginCombo("##rgps", tr(kGps[S.gpsMode]))) {
        for (int i = 0; i < NumGps; i++) if (ImGui::Selectable(tr(kGps[i]), i == S.gpsMode)) { S.gpsMode = i; save(); startGps(); }
        ImGui::EndCombo();
    }
    if (S.gpsMode == GpsManual) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); ImGui::InputDouble(tr("latitude"), &S.manLat, 0, 0, "%.5f");
        ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); ImGui::InputDouble(tr("longitude"), &S.manLon, 0, 0, "%.5f");
    }
    if (S.gpsMode == GpsNmea) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(260 * gUi); ImGui::InputText("##rnmea", S.nmeaPath, sizeof S.nmeaPath);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A serial GPS (for example /dev/cu.usbmodem1101 or COM3 on a PC; 9600 baud) or a text file of NMEA sentences"));
        ImGui::SameLine();
        if (ImGui::SmallButton(tr(S.grun.load() ? "Reconnect" : "Connect"))) { save(); startGps(); }
        std::lock_guard<std::mutex> l(S.gmu);
        if (!S.gpsMsg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.gpsMsg.c_str()); }
    }
    double la, lo;
    if (position(la, lo)) ImGui::TextDisabled(tr("Readings are logged at %.5f, %.5f"), la, lo);
    else ImGui::TextDisabled("%s", tr("Readings are logged without a position."));
}

void walkTab() {
    std::string err;
    gpsPanel();
    const float m = S.walk.meter();
    ImVec4 col = m < 0.33f ? ImVec4(0.25f, 0.75f, 0.35f, 1) : m < 0.66f ? ImVec4(0.9f, 0.75f, 0.2f, 1) : ImVec4(0.9f, 0.25f, 0.2f, 1);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    char ov[96];
    snprintf(ov, sizeof ov, tr("%+.1f dB above the quiet level"), S.walk.readings().empty() ? 0.f : S.walk.smoothedDb() - S.walk.referenceDb());
    ImGui::ProgressBar(m, ImVec2(-1, 46 * gUi), ov);
    ImGui::PopStyleColor();
    if (S.walk.readings().empty()) ImGui::TextDisabled("%s", tr("Press Start and walk. The meter is full when the interference is 25 dB above the quietest place so far."));
    else {
        const int tr_ = S.walk.trend();
        ImGui::TextColored(col, "%s", m < 0.33f ? tr("Quiet") : m < 0.66f ? tr("Some interference") : tr("Loud"));
        ImGui::SameLine();
        ImGui::TextUnformatted(tr_ > 0 ? tr("- getting louder") : tr_ < 0 ? tr("- getting quieter") : tr("- steady"));
        const Reading& r = S.walk.readings().back();
        if (r.peakDb > 0) { ImGui::SameLine(); ImGui::TextDisabled(tr("strongest spur %.4f MHz, %.0f dB"), r.peakHz / 1e6, r.peakDb); }
        if (r.kind != Kind::Unknown) { ImGui::SameLine(); ImGui::TextDisabled("%s", kindText(r.kind)); }
    }
    if (ImGui::SmallButton(tr("This is the quiet level"))) S.walk.setReferenceNow();
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Leave out the signals seen now"))) S.walk.ignoreNow();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Broadcast carriers and devices you already know are left out of the meter: do this where it is quiet, before walking"));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Clear the readings"))) S.walk.clear();
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Save as CSV..."))) { const std::string p = saveFileDialog("rfi-walk.csv"); if (!p.empty() && !S.walk.exportCsv(p, err)) S.msg = err; }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Save as GPX..."))) { const std::string p = saveFileDialog("rfi-walk.gpx"); if (!p.empty() && !S.walk.exportGpx(p, err)) S.msg = err; }
    const auto& rd = S.walk.readings();
    if (rd.size() > 1 && plt::BeginPlot("##rfiwalk", ImVec2(-1, 170 * gUi), plt::Flags_NoLegend)) {
        std::vector<double> xs, ys;
        const size_t from = rd.size() > 600 ? rd.size() - 600 : 0;
        for (size_t i = from; i < rd.size(); i++) { xs.push_back((double)i); ys.push_back(rd[i].levelDb); }
        plt::SetupAxes(tr("reading"), tr("dB above the quiet level"));
        plt::SetupAxisLimits(plt::X1, xs.front(), xs.back() + 1, plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, -3, std::max(10.0, *std::max_element(ys.begin(), ys.end()) + 3), plt::Cond_Once);
        plt::Spec sp; sp.LineColor = ImVec4(1, 0.6f, 0.3f, 1);
        plt::PlotLine("level", xs.data(), ys.data(), (int)xs.size(), sp);
        plt::EndPlot();
    }
    const int li = S.walk.loudest();
    if (li >= 0) {
        const Reading& r = rd[(size_t)li];
        if (r.hasPos) ImGui::Text(tr("Loudest reading: %+.1f dB at %.5f, %.5f (%s)"), r.levelDb, r.lat, r.lon, hhmmss(r.time).c_str());
        else ImGui::Text(tr("Loudest reading: %+.1f dB (%s), no position"), r.levelDb, hhmmss(r.time).c_str());
    }
    if (ImGui::BeginTable("##rfireads", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(tr("dB above the quiet level")); ImGui::TableSetupColumn(tr("Position"));
        ImGui::TableSetupColumn(tr("Strongest spur MHz")); ImGui::TableSetupColumn(tr("Likely source"));
        ImGui::TableHeadersRow();
        for (size_t i = rd.size(); i-- > 0 && rd.size() - i <= 200;) {
            const Reading& r = rd[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", hhmmss(r.time).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%+.1f", r.levelDb);
            ImGui::TableNextColumn(); if (r.hasPos) ImGui::Text("%.5f, %.5f", r.lat, r.lon); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (r.peakDb > 0) ImGui::Text("%.4f", r.peakHz / 1e6); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%s", r.kind == Kind::Unknown ? "-" : kindText(r.kind));
        }
        ImGui::EndTable();
    }
}

void windowPanel(App& a) {
    load();
    firstWindowSize(940 * gUi, 780 * gUi);
    if (!ImGui::Begin(trw("Interference hunter"), &a.rfiOpen)) { ImGui::End(); return; }
    pump();
    controls(a);
    if (ImGui::BeginTabBar("##rfitabs")) {
        if (ImGui::BeginTabItem(trw("Monitor"))) { monitorTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Walk"))) { walkTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
} // namespace

void rfiWindow(App& a) {
    if (a.rfiOpen) windowPanel(a);
    else {
        if (S.running.load()) stopSweep();   // closing the window gives the radio back
        if (S.grun.load()) stopGps();
    }
}
