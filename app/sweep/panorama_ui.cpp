// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The panorama strip (#658): a 5 / 10 / 20 MHz overview above the spectrum for a radio whose sample rate is smaller than that (an RTL-SDR does 2.4 Msps).
// One button at the top right of the spectrum turns it on: the receiver stops (nothing is decoded while it scans), the radio is retuned in steps of
// its own sample rate and the steps are stitched into one trace (core/src/sweep/panorama.cpp, runSweep). A click on the strip tunes the radio there,
// turns the panorama off and starts the receiver again. Off until clicked; nothing else changes.
#include "../app.h"
#include "dect2/common/thread_name.h"
#include "dect2/sweep/panorama.h"
#include "../gfx/gfx.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

namespace {
using namespace dect2::sweep;

struct Pano {
    std::thread th;
    std::atomic<bool> cancel{false}, running{false};
    std::mutex mu;
    Sweep last;
    std::string msg;
    int spanIdx = 1;   // 10 MHz
    bool on = false;
};
Pano P;

constexpr int kCols = 700;

void worker(DeviceInfo dev, TuneSettings t, SweepConfig c) {
    std::string err;
    IqSweepSource src(dev, t);
    if (!src.open(err)) { std::lock_guard<std::mutex> l(P.mu); P.msg = err; P.running = false; return; }
    runSchedule(src, c, [&](const Sweep& s) {
        std::lock_guard<std::mutex> l(P.mu);
        P.last = s;
        return !P.cancel.load();
    }, err, &P.cancel);
    src.close();
    std::lock_guard<std::mutex> l(P.mu);
    if (!err.empty() && err != "cancelled") P.msg = err;
    P.running = false;
}

void stopWorker() {
    P.cancel = true;
    if (P.th.joinable()) P.th.join();
    P.running = false;
}

void begin(App& a) {
    stopWorker();
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size() || !a.devices[a.devIdx].isRadio()) { P.msg = tr("pick a radio first (the test signal and files cannot scan)"); return; }
    const DeviceInfo dev = a.devices[a.devIdx];
    const double centre = (a.engine.running() && a.engine.captureCentreHz() > 0) ? a.engine.captureCentreHz() : a.tune.centerHz;
    if (a.engine.running()) a.engine.stop();   // the radio is the scanner's now: nothing is decoded until a click or "Back to receiving"
    TuneSettings t = a.tune;
    if (dev.maxRateHz > 0) t.sampleRate = std::min(t.sampleRate, dev.maxRateHz);
    t.centerHz = centre;
    t.bandwidthMhz = t.sampleRate / 1e6;
    SweepConfig c = panoramaConfig(centre, kPanoramaSpansMhz[std::min(2, std::max(0, P.spanIdx))] * 1e6);
    { std::lock_guard<std::mutex> l(P.mu); P.last = Sweep(); P.msg.clear(); }
    P.cancel = false; P.running = true; P.on = true;
    P.th = dect2::namedThread("onair-panorama", worker, dev, t, c);
}

void finish(App& a, double tuneHz) {   // leaves the panorama and starts the receiver again (on tuneHz when it is > 0)
    stopWorker();
    P.on = false;
    if (tuneHz > 0) { a.freqMhz = tuneHz / 1e6; a.tune.centerHz = tuneHz; }
    startReceiver(a);
}
} // namespace

// Called at the top of the spectrum panel: the strip, when the panorama is on. It sits in the layout, so everything under it makes room.
void panoramaStrip(App& a) {
    if (!P.on) return;
    if (P.th.joinable() && !P.running.load()) P.th.join();
    std::vector<float> col; double x0 = 0, x1 = 0; float floorDb = -120;
    std::string msg;
    {
        std::lock_guard<std::mutex> l(P.mu);
        msg = P.msg;
        if (P.last.bins() > 0) { col = panoramaColumns(P.last, kCols); x0 = P.last.startHz / 1e6; x1 = (P.last.startHz + P.last.bins() * P.last.binHz) / 1e6; floorDb = P.last.floorOverallDb; }
    }
    ImGui::TextColored(pal::warnAmber(), "%s", tr("Panorama: the radio is scanning, so nothing is decoded right now. Click a signal to tune it."));
    ImGui::SameLine();
    static const char* kSpan[3] = {N_("5 MHz"), N_("10 MHz"), N_("20 MHz")};
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::BeginCombo("##panospan", tr(kSpan[P.spanIdx]))) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(tr(kSpan[i]), i == P.spanIdx)) { P.spanIdx = i; begin(a); }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Back to receiving"))) { finish(a, 0); return; }
    if (!msg.empty()) { ImGui::TextColored(pal::badRed(), "%s", msg.c_str()); }
    if (plt::BeginPlot("##panorama", ImVec2(-1, 90 * gUi), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("MHz", "");
        if (!col.empty()) {
            float mx = floorDb + 20; for (float v : col) mx = std::max(mx, v);
            plt::SetupAxisLimits(plt::X1, x0, x1, plt::Cond_Always);
            plt::SetupAxisLimits(plt::Y1, floorDb - 6, mx + 4, plt::Cond_Always);
            std::vector<double> xs(col.size());
            std::vector<double> ys(col.begin(), col.end());
            for (size_t i = 0; i < col.size(); i++) xs[i] = x0 + (x1 - x0) * (i + 0.5) / col.size();
            plt::Spec sp; sp.LineColor = pal::accent();
            plt::PlotLine("panorama", xs.data(), ys.data(), (int)xs.size(), sp);
            if (plt::IsPlotHovered() && ImGui::IsMouseClicked(0)) {
                const double hz = plt::GetPlotMousePos().x * 1e6;
                plt::EndPlot();
                finish(a, hz);
                return;
            }
        }
        plt::EndPlot();
    }
}

// The one button, at the top right of the spectrum (p = its top left, w = its width). Only for a radio that cannot see 10 MHz at once.
void panoramaButton(App& a, ImVec2 p, float w) {
    if (P.on) return;
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size() || !a.devices[a.devIdx].isRadio()) return;
    const double rate = a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate;
    if (rate <= 0 || rate >= 10e6) return;
    const float bw = 96 * gUi;
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - 4 * gUi, p.y + 4 * gUi));
    if (ImGui::SmallButton(tr("Panorama"))) begin(a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A 10 MHz overview made by scanning in steps. Decoding pauses while it scans; click a signal in it to tune there."));
}
