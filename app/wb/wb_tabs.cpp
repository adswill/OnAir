// Signal workbench: the Measure, Demodulate, Bits and Export tabs, the background jobs behind them and the listening.
#include "dect2/common/thread_name.h"
#include "wb_state.h"
#include "dect2/signallab/signallab.h"
#include "ui/scale.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace dect2::wb;
using dect2::cf32;

namespace wbui {

static MeasureOptions gMo;
static int gMoFft = 3;   // 2048 << ?

// ---- jobs ----------------------------------------------------------------------------------------------------------------------------

void startMeasure() {
    State& s = S();
    if (!s.src || s.mJob.running) return;
    uint64_t t0, t1; double f0, f1;
    selection(t0, t1, f0, f1);
    auto res = std::make_shared<Measurement>();
    s.mRes = res;
    s.mT0 = (double)t0; s.mT1 = (double)t1; s.mF0 = f0; s.mF1 = f1;
    MeasureOptions o = gMo;
    o.fftSize = 1 << (11 + gMoFft);
    auto src = s.src;
    Job* job = &s.mJob;
    s.mJob.cancel = false; s.mJob.finished = false; s.mJob.running = true; s.mJob.prog = 0;
    s.mJob.th = dect2::namedThread("onair-wbm", [=]() {
        measure(*src, t0, t1, f0, f1, o, *res, [job](double p) { job->prog = p; return !job->cancel.load(); });
        job->finished = true;
    });
}

void startDemod() {
    State& s = S();
    if (!s.src || s.dJob.running) return;
    uint64_t t0, t1; double f0, f1;
    selection(t0, t1, f0, f1);
    DemodParams p = s.dp;
    if (s.bwFromSel && f1 > f0) { p.centreHz = 0.5 * (f0 + f1); p.bwHz = f1 - f0; s.dp.centreHz = p.centreHz; s.dp.bwHz = p.bwHz; }
    else if (s.bwFromSel) { p.bwHz = 0; p.centreHz = 0; }
    auto res = std::make_shared<DemodResult>();
    s.dRes = res;
    auto src = s.src;
    Job* job = &s.dJob;
    s.dJob.cancel = false; s.dJob.finished = false; s.dJob.running = true; s.dJob.prog = 0;
    stopAudio();
    s.dJob.th = dect2::namedThread("onair-wbd", [=]() {
        *res = demodulate(*src, t0, t1, p);
        job->finished = true;
    });
}

// ---- small widgets -------------------------------------------------------------------------------------------------------------------

static void kv(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(150 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

static void selectionNote() {
    State& s = S();
    uint64_t t0, t1; double f0, f1;
    selection(t0, t1, f0, f1);
    const double r = rateHz();
    char b[256];
    if (f1 > f0) snprintf(b, sizeof b, "Selection: %s to %s, band %s to %s", fmtTime((double)t0 / r).c_str(), fmtTime((double)t1 / r).c_str(), fmtFreq(f0 + s.src->centerHz(), 3).c_str(), fmtFreq(f1 + s.src->centerHz(), 3).c_str());
    else snprintf(b, sizeof b, "Selection: %s to %s, the whole band (drag a rectangle in the spectrogram to pick a band)", fmtTime((double)t0 / r).c_str(), fmtTime((double)t1 / r).c_str());
    ImGui::TextDisabled("%s", b);
}

// ---- Measure -------------------------------------------------------------------------------------------------------------------------

void drawMeasure(App& a) {
    State& s = S();
    if (!s.src) { noSource("measure"); return; }
    selectionNote();
    ImGui::BeginDisabled(s.mJob.running);
    if (ImGui::Button("Measure selection")) { s.note.clear(); startMeasure(); }
    ImGui::EndDisabled();
    if (s.mJob.running) { ImGui::SameLine(0, 8 * gUi); ImGui::ProgressBar((float)s.mJob.prog.load(), ImVec2(120 * gUi, 0)); ImGui::SameLine(); if (ImGui::Button("Cancel")) s.mJob.cancel = true; }
    ImGui::SameLine(0, 12 * gUi);
    ImGui::TextDisabled("FFT"); ImGui::SameLine(0, 4 * gUi);
    ImGui::SetNextItemWidth(70 * gUi);
    static const char* ff[] = {"2048", "4096", "8192", "16384", "32768", "65536"};
    ImGui::Combo("##wbmfft", &gMoFft, ff, 6);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Resolution of the averaged spectrum the peaks and widths come from");
    ImGui::SameLine(0, 8 * gUi);
    ImGui::SetNextItemWidth(60 * gUi);
    ImGui::SliderInt("##wbtop", &gMo.topN, 3, 40, "top %d");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How many peaks to list");
    ImGui::SameLine(0, 8 * gUi);
    ImGui::Checkbox("modulation", &gMo.ident);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Run the signal identifier: modulation class, symbol rate, candidates (a few seconds on a long selection)");
    ImGui::SameLine(0, 4 * gUi);
    ImGui::Checkbox("symbol rate", &gMo.symbolRate);
    ImGui::SameLine(0, 4 * gUi);
    ImGui::Checkbox("bursts", &gMo.bursts);
    if (!s.haveMeas) {
        if (!s.note.empty()) ImGui::TextColored(pal::warnAmber(), "%s", s.note.c_str());
        ImGui::TextDisabled("Select a time range and a band on the Signal tab (or none: the whole file), then press Measure selection.");
        return;
    }
    const Measurement& m = s.meas;
    ImGui::Spacing();
    const double c = s.src->centerHz();
    kv(a, "band measured", "%s to %s (%s)", fmtFreq(m.f0 + c).c_str(), fmtFreq(m.f1 + c).c_str(), fmtFreq(m.f1 - m.f0).c_str());
    kv(a, "level", "peak %.1f dBFS in one bin, band power %.1f dBFS, noise floor %.1f dB per bin (%.1f Hz bins)", m.peakDb, m.bandPowerDb, m.noiseFloorDb, m.binHz);
    kv(a, "SNR", "%.1f dB (signal inside the 99%% band over the noise in it)", m.snrDb);
    kv(a, "bandwidth", "-3 dB: %s    -20 dB: %s%s    99%% occupied: %s", fmtFreq(m.bw3Hz, 2).c_str(), fmtFreq(m.bw20Hz, 2).c_str(), m.bw20Limited ? " (the noise hides the -20 dB point)" : "", fmtFreq(m.obwHz, 2).c_str());
    kv(a, "occupied band", "%s to %s, centre %s", fmtFreq(m.obwLoHz + c, 3).c_str(), fmtFreq(m.obwHiHz + c, 3).c_str(), fmtFreq(m.centreHz + c, 3).c_str());
    if (!m.peaks.empty()) kv(a, "carrier / strongest line", "%s, %+.1f Hz from the centre of the occupied band", fmtFreq(m.carrierHz + c, 3).c_str(), m.carrierOffsetHz);
    if (m.sym.baud > 0) {
        kv(a, "symbol rate", "%.1f Bd (%s line, %.1f dB over its surroundings)", m.sym.baud, m.sym.method.c_str(), m.sym.strengthDb);
        ImGui::SameLine(0, 8 * gUi);
        if (ImGui::SmallButton("use as baud")) { s.dp.baud = snapBaud(m.sym.baud); s.tab = 2; }
        if (m.sym.cands.size() > 1) {
            std::string o;
            char b[64];
            for (size_t i = 1; i < m.sym.cands.size(); i++) { snprintf(b, sizeof b, "%s%.1f (%.0f dB)", i > 1 ? ", " : "", m.sym.cands[i].first, m.sym.cands[i].second); o += b; }
            kv(a, "other lines", "%s", o.c_str());
        }
    } else if (gMo.symbolRate) kv(a, "symbol rate", "no clear line (a carrier, noise or an analogue signal)");
    if (m.identValid) {
        kv(a, "modulation hint", "%s", m.ident.summary.c_str());
        int shown = 0;
        for (const auto& cd : m.ident.cands) {
            if (shown++ >= 4) break;
            ImGui::TextDisabled("    "); ImGui::SameLine(0, 0);
            ImGui::Text("%-14s %3.0f%%  %s", cd.name.c_str(), cd.score, cd.why.c_str());
        }
    }
    kv(a, "bursts", "%zu, on %.0f %% of the time%s", m.bursts.size(), m.duty * 100, m.bursts.empty() ? " (continuous)" : "");
    // spectrum plot
    ImGui::Spacing();
    const float plotH = std::max(150.f * gUi, std::min(260.f * gUi, ImGui::GetContentRegionAvail().y * 0.34f));
    if (!m.psd.empty() && plt::BeginPlot("##wbpsd", ImVec2(-1, plotH), plt::Flags_NoLegend)) {
        plt::SetupAxes(c != 0 ? "frequency, kHz" : "offset from the centre, kHz", "dBFS per bin");
        std::vector<float> xs(m.psd.size());
        for (size_t i = 0; i < xs.size(); i++) xs[i] = (float)((m.psdF0 + (double)i * m.binHz + c) / 1e3);
        plt::SetupAxisLimits(plt::X1, (m.f0 + c) / 1e3, (m.f1 + c) / 1e3, plt::Cond_Once);
        float lo = 0, hi = -300;
        for (float v : m.psd) { lo = std::min(lo, v); hi = std::max(hi, v); }
        plt::SetupAxisLimits(plt::Y1, std::max(-160.0, (double)m.noiseFloorDb - 15), hi + 5, plt::Cond_Once);
        plt::Spec sp; sp.LineColor = ImVec4(0.42f, 0.70f, 0.95f, 1);
        plt::PlotLine("spectrum", xs.data(), m.psd.data(), (int)xs.size(), sp);
        std::vector<float> px, py;
        for (const Peak& p : m.peaks) { px.push_back((float)((p.freqHz + c) / 1e3)); py.push_back((float)p.levelDb); }
        if (!px.empty()) { plt::Spec ps; ps.Marker = plt::Marker_Circle; ps.MarkerSize = 4; ps.MarkerFillColor = ImVec4(1, 0.7f, 0.2f, 1); ps.MarkerLineColor = ImVec4(1, 0.7f, 0.2f, 1); ps.LineColor = ImVec4(1, 0.7f, 0.2f, 0); plt::PlotScatter("peaks", px.data(), py.data(), (int)px.size(), ps); }
        const float nfl = (float)m.noiseFloorDb;
        const float fl[2] = {nfl, nfl};
        const float fx[2] = {xs.front(), xs.back()};
        plt::Spec nf; nf.LineColor = ImVec4(0.6f, 0.6f, 0.6f, 0.8f);
        plt::PlotLine("floor", fx, fl, 2, nf);
        const float eb[2] = {(float)((m.obwLoHz + c) / 1e3), (float)((m.obwHiHz + c) / 1e3)};
        plt::Spec ob; ob.LineColor = ImVec4(0.4f, 0.9f, 0.5f, 0.7f);
        plt::PlotInfLines("99 % band", eb, 2, ob);
        plt::EndPlot();
    }
    // peaks table
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "PEAKS");
    if (ImGui::BeginTable("##wbpeaks", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, std::min(180.f * gUi, 24.f * gUi * (float)(m.peaks.size() + 1))))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#"); ImGui::TableSetupColumn("Frequency", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("From DC"); ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Width (-3 dB)");
        ImGui::TableHeadersRow();
        int i = 1;
        for (const Peak& p : m.peaks) {
            ImGui::TableNextRow(); ImGui::PushID(i);
            ImGui::TableNextColumn();
            if (ImGui::Selectable(std::to_string(i).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) { s.curOn = true; s.curF = p.freqHz; if (s.curT == 0) s.curT = (s.v0 + s.v1) / 2; }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(fmtFreq(p.freqHz + c, 3).c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%+.1f Hz", p.freqHz);
            ImGui::TableNextColumn(); ImGui::Text("%.1f dBFS", p.levelDb);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(fmtFreq(p.widthHz, 1).c_str());
            ImGui::PopID(); i++;
        }
        ImGui::EndTable();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click a row to put the cursor on that frequency");
    // power over time and bursts
    if (!m.power.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(pal::heading(), "POWER OVER TIME AND BURSTS");
        if (plt::BeginPlot("##wbpow", ImVec2(-1, 130 * gUi), plt::Flags_NoLegend)) {
            plt::SetupAxes("seconds", "dBFS in the band");
            const double dt = m.powerStepSec;
            std::vector<float> xs(m.power.size());
            for (size_t i = 0; i < xs.size(); i++) xs[i] = (float)(m.powerT0Sec + (double)i * dt);
            plt::SetupAxisLimits(plt::X1, xs.front(), xs.back(), plt::Cond_Once);
            float lo = 0, hi = -300;
            for (float v : m.power) { lo = std::min(lo, v); hi = std::max(hi, v); }
            lo = *std::min_element(m.power.begin(), m.power.end());
            plt::SetupAxisLimits(plt::Y1, lo - 3, hi + 3, plt::Cond_Once);
            plt::Spec sp; sp.LineColor = ImVec4(0.95f, 0.65f, 0.3f, 1);
            plt::PlotLine("power", xs.data(), m.power.data(), (int)xs.size(), sp);
            if (!m.bursts.empty()) { const float th[2] = {(float)m.burstThresholdDb, (float)m.burstThresholdDb}; const float tx[2] = {xs.front(), xs.back()}; plt::Spec ts; ts.LineColor = ImVec4(0.6f, 0.9f, 0.6f, 0.8f); plt::PlotLine("threshold", tx, th, 2, ts); }
            plt::EndPlot();
        }
        if (!m.bursts.empty() && ImGui::BeginTable("##wbbursts", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, std::min(170.f * gUi, 24.f * gUi * (float)(m.bursts.size() + 1))))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#"); ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Length"); ImGui::TableSetupColumn("Peak"); ImGui::TableSetupColumn("Mean");
            ImGui::TableHeadersRow();
            int i = 1;
            for (const Burst& b : m.bursts) {
                ImGui::TableNextRow(); ImGui::PushID(1000 + i);
                ImGui::TableNextColumn();
                if (ImGui::Selectable(std::to_string(i).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {   // select the burst
                    const double r = rateHz();
                    setSelection(b.startSec * r, (b.startSec + b.lenSec) * r, s.selF, s.sF0, s.sF1);
                    s.v0 = std::max(0.0, b.startSec * r - b.lenSec * r * 0.25); s.v1 = std::min((double)s.src->length(), (b.startSec + b.lenSec) * r + b.lenSec * r * 0.25);
                }
                ImGui::TableNextColumn(); ImGui::TextUnformatted(fmtTime(b.startSec).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(fmtTime(b.lenSec).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.1f dB", b.peakDb);
                ImGui::TableNextColumn(); ImGui::Text("%.1f dB", b.meanDb);
                ImGui::PopID(); i++;
            }
            ImGui::EndTable();
            ImGui::TextDisabled("Click a burst to select it and zoom the Signal tab to it.");
        }
    }
}

// ---- audio ---------------------------------------------------------------------------------------------------------------------------

void stopAudio() {
    State& s = S();
    s.playing = false;
    if (s.aud) s.aud->flush();
}

void audioTick(App& a) {
    State& s = S();
    if (!s.playing || !s.haveDemod || s.dr.audio.empty()) return;
    if (!s.aud) {
        s.aud = std::make_unique<dect2::AudioOut>();
        if (!s.aud->start(s.dr.audio.empty() ? 48000 : 48000)) { s.note = "no audio output device"; s.playing = false; return; }
        s.aud->setStartThreshold(4800);
    }
    s.aud->setVolume(a.volume);
    s.aud->setMuted(a.muted);
    // keep about half a second queued
    while (s.aud->bufferedFrames() < 24000 && s.playPos < s.dr.audio.size()) {
        const size_t n = std::min<size_t>(2400, s.dr.audio.size() - s.playPos);
        std::vector<float> st(n * 2);
        for (size_t i = 0; i < n; i++) st[2 * i] = st[2 * i + 1] = s.dr.audio[s.playPos + i];
        s.aud->write(st.data(), (int)n);
        s.playPos += n;
    }
    if (s.playPos >= s.dr.audio.size() && s.aud->bufferedFrames() < 100) s.playing = false;
}

// ---- Demodulate ------------------------------------------------------------------------------------------------------------------------

struct AfskPreset { const char* name; double mark, space, baud; };
static const AfskPreset kAfsk[] = {
    {"Bell 202 (1200 / 2200 Hz, 1200 Bd): packet radio, APRS", 1200, 2200, 1200},
    {"Bell 103 originate (1270 / 1070 Hz, 300 Bd)", 1270, 1070, 300},
    {"Bell 103 answer (2225 / 2025 Hz, 300 Bd)", 2225, 2025, 300},
    {"Kansas City (2400 / 1200 Hz, 300 Bd)", 2400, 1200, 300},
    {"V.23 (1300 / 2100 Hz, 1200 Bd)", 1300, 2100, 1200},
    {"Your own tones", 0, 0, 0},
};

static void autoBaud(App&) {
    State& s = S();
    uint64_t t0, t1; double f0, f1;
    selection(t0, t1, f0, f1);
    double bw = f1 > f0 ? f1 - f0 : (s.src->real() ? 0.96 * s.src->rate() / 2 : 0.96 * s.src->rate());
    double centre = f1 > f0 ? 0.5 * (f0 + f1) : (s.src->real() ? s.src->rate() / 4 : 0);
    Extracted e;
    t1 = std::min<uint64_t>(t1, t0 + (uint64_t)(1u << 25));
    if (!extractBand(*s.src, t0, t1, centre, bw, std::min(std::max(bw * 2.5, 4000.0), s.src->rate()), e, 1u << 20) || e.x.size() < 4096) { s.note = "the selection is too short to find a symbol rate"; return; }
    SymEstimate est;
    if (!estimateSymbolRate(e.x.data(), e.x.size(), e.rate, est)) { s.note = "no clear symbol-rate line in the selection (is it a burst of data? is the band right?)"; return; }
    s.dp.baud = snapBaud(est.baud);
    char b[128]; snprintf(b, sizeof b, "symbol rate %.1f Bd (%s line, %.0f dB), snapped to %.1f", est.baud, est.method.c_str(), est.strengthDb, s.dp.baud);
    s.note = b;
}

// One button for the Demodulate tab: modulation, then symbol rate, from the selection (Signal Lab, core/src/signallab). Only what it is sure of is applied.
static void autoAll(App& a) {
    State& s = S();
    uint64_t t0, t1; double f0, f1;
    selection(t0, t1, f0, f1);
    double bw = f1 > f0 ? f1 - f0 : (s.src->real() ? 0.96 * s.src->rate() / 2 : 0.96 * s.src->rate());
    double centre = f1 > f0 ? 0.5 * (f0 + f1) : (s.src->real() ? s.src->rate() / 4 : 0);
    Extracted e;
    t1 = std::min<uint64_t>(t1, t0 + (uint64_t)(1u << 25));
    if (!extractBand(*s.src, t0, t1, centre, bw, std::min(std::max(bw * 2.5, 4000.0), s.src->rate()), e, 1u << 20) || e.x.size() < 4096) { s.note = tr("the selection is too short"); return; }
    const auto mod = dect2::signallab::classify(e.x, e.rate, e.bwHz);
    const auto g = mod.best();
    namespace L = dect2::signallab;
    int kind = -1;
    if (g.confidence >= 0.35) {
        if (g.mod == L::Mod::OOK || g.mod == L::Mod::ASK) kind = KAsk;
        else if (g.mod == L::Mod::FSK) kind = g.order >= 4 ? KFsk4 : KFsk2;
        else if (g.mod == L::Mod::MSK) kind = KMsk;
        else if (g.mod == L::Mod::GMSK) kind = KGmsk;
        else if (g.mod == L::Mod::PSK) kind = g.order >= 8 ? KPsk8 : g.order >= 4 ? KQpsk : KBpsk;
    }
    char b[200];
    if (kind < 0) { snprintf(b, sizeof b, "%s", tr("Auto: the modulation is not clear (select one burst of data, or set it by hand)")); s.note = b; return; }
    s.dp.kind = kind;
    SymEstimate est;
    const bool haveRate = estimateSymbolRate(e.x.data(), e.x.size(), e.rate, est);
    if (haveRate) s.dp.baud = snapBaud(est.baud);
    snprintf(b, sizeof b, tr("Auto: %s, %.0f%% sure; symbol rate %s"), kindName(kind), g.confidence * 100, haveRate ? (std::to_string((int)std::lround(s.dp.baud)) + " Bd").c_str() : tr("not found"));
    s.note = b;
    (void)a;
}

static void eyePlot(const DemodResult& r, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##wbeye", size);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(8, 9, 11, 255));
    if (r.eyeTraces > 0 && r.eyeLen > 1) {
        float lim = 1.15f;
        for (float v : r.eye) lim = std::max(lim, std::fabs(v) * 1.05f);
        lim = std::min(lim, 3.f);
        dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
        for (int t = 0; t < r.eyeTraces; t++) {
            const float* e = &r.eye[(size_t)t * r.eyeLen];
            for (int i = 0; i + 1 < r.eyeLen; i++)
                dl->AddLine(ImVec2(p0.x + size.x * i / (r.eyeLen - 1), p0.y + size.y * (0.5f - 0.5f * e[i] / lim)), ImVec2(p0.x + size.x * (i + 1) / (r.eyeLen - 1), p0.y + size.y * (0.5f - 0.5f * e[i + 1] / lim)), IM_COL32(90, 170, 240, 45));
        }
        dl->AddLine(ImVec2(p0.x + size.x / 2, p0.y), ImVec2(p0.x + size.x / 2, p0.y + size.y), IM_COL32(255, 255, 255, 50));
        dl->AddLine(ImVec2(p0.x, p0.y + size.y / 2), ImVec2(p0.x + size.x, p0.y + size.y / 2), IM_COL32(255, 255, 255, 30));
        dl->PopClipRect();
    }
    dl->AddRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(70, 76, 84, 255));
    dl->AddText(ImVec2(p0.x + 4, p0.y + 2), IM_COL32(130, 140, 150, 255), "eye (two symbols)");
}

static void histPlot(const DemodResult& r, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##wbhist", size);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(8, 9, 11, 255));
    if (!r.soft.empty()) {
        float lo = 1e30f, hi = -1e30f;
        for (float v : r.soft) { lo = std::min(lo, v); hi = std::max(hi, v); }
        if (hi > lo) {
            const int nb = std::max(16, (int)(size.x / 3));
            std::vector<int> h((size_t)nb, 0);
            int mx = 1;
            for (float v : r.soft) { const int b = std::min(nb - 1, (int)((v - lo) / (hi - lo) * nb)); mx = std::max(mx, ++h[(size_t)b]); }
            for (int i = 0; i < nb; i++) {
                const float x = p0.x + size.x * i / nb, y = p0.y + size.y - 4 - (size.y - 18) * h[(size_t)i] / mx;
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + size.x / nb - 0.5f, p0.y + size.y - 4), IM_COL32(110, 180, 120, 220));
            }
            for (float l : r.levels) { const float x = p0.x + size.x * (l - lo) / (hi - lo); dl->AddLine(ImVec2(x, p0.y + 14), ImVec2(x, p0.y + size.y), IM_COL32(255, 200, 80, 200)); }
        }
    }
    dl->AddRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(70, 76, 84, 255));
    dl->AddText(ImVec2(p0.x + 4, p0.y + 2), IM_COL32(130, 140, 150, 255), "soft symbols (decision levels in orange)");
}

static void timePlot(const char* id, const char* ylabel, const std::vector<float>& y, double rate, float h, ImVec4 col, double yCentre = 0, bool sameLimits = false) {
    (void)yCentre; (void)sameLimits;
    if (y.empty()) return;
    ImGui::TextDisabled("%s", id);
    if (plt::BeginPlot(id, ImVec2(-1, h), plt::Flags_NoLegend)) {
        plt::SetupAxes("seconds", ylabel);
        std::vector<float> xs(y.size());
        for (size_t i = 0; i < xs.size(); i++) xs[i] = (float)((double)i / rate);
        plt::SetupAxisLimits(plt::X1, 0, xs.back(), plt::Cond_Once);
        float lo = *std::min_element(y.begin(), y.end()), hi = *std::max_element(y.begin(), y.end());
        const float m = std::max(1e-6f, (hi - lo) * 0.08f);
        plt::SetupAxisLimits(plt::Y1, lo - m, hi + m, plt::Cond_Once);
        plt::Spec sp; sp.LineColor = col;
        plt::PlotLine(id, xs.data(), y.data(), (int)y.size(), sp);
        plt::EndPlot();
    }
}

void drawDemod(App& a) {
    State& s = S();
    if (!s.src) { noSource("demodulate"); return; }
    selectionNote();
    DemodParams& p = s.dp;
    // the demodulator
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Demodulator"); ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button(tr("Auto"))) autoAll(a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Finds the modulation and symbol rate of the selection and fills them in. You can change them afterwards."));
    ImGui::SameLine(0, 8 * gUi);
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::BeginCombo("##wbkind", kindName(p.kind))) {
        static const int order[] = {KAm, KFm, KUsb, KLsb, KCw, KAsk, KFsk2, KFsk4, KAfsk, KBpsk, KQpsk, KPsk8, KMsk, KGmsk};
        for (int k : order) if (ImGui::Selectable(kindName(k), p.kind == k)) { p.kind = k; if (k == KAfsk) { p.markHz = kAfsk[s.afskPreset].mark ? kAfsk[s.afskPreset].mark : p.markHz; } }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("AM, FM, USB, LSB, CW give audio (and the plots below). ASK/OOK, 2-FSK, 4-FSK, AFSK, BPSK, QPSK, 8PSK, MSK and GMSK also recover the bit stream.");
    const bool bits = kindHasBits(p.kind);
    if (bits) {
        ImGui::SameLine(0, 10 * gUi);
        ImGui::TextDisabled("Baud"); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(90 * gUi);
        ImGui::InputDouble("##wbbaud", &p.baud, 0, 0, "%.2f", ImGuiInputTextFlags_AutoSelectAll);
        p.baud = std::max(1.0, p.baud);
        ImGui::SameLine(0, 4 * gUi);
        if (ImGui::Button("Auto-detect")) autoBaud(a);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Find the symbol rate of the selection from the lines of its envelope and frequency steps and use the nearest standard rate. Select a burst of data first.");
    }
    ImGui::SameLine(0, 10 * gUi);
    ImGui::Checkbox("band from selection", &s.bwFromSel);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Demodulate the band selected in the spectrogram. Off: type the centre and filter width below.");
    if (!s.bwFromSel) {
        ImGui::SameLine(0, 6 * gUi);
        ImGui::SetNextItemWidth(100 * gUi);
        double cHz = p.centreHz + s.src->centerHz();
        if (ImGui::InputDouble("##wbdc", &cHz, 0, 0, "%.0f Hz", ImGuiInputTextFlags_EnterReturnsTrue)) p.centreHz = cHz - s.src->centerHz();
        ImGui::SameLine(0, 4 * gUi);
        ImGui::SetNextItemWidth(90 * gUi);
        ImGui::InputDouble("##wbdb", &p.bwHz, 0, 0, "%.0f Hz wide", ImGuiInputTextFlags_EnterReturnsTrue);
    }
    // the options of the chosen demodulator
    if (kindIsPsk(p.kind)) {
        ImGui::Checkbox("carrier recovery", &p.carrierRecovery);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Find the frequency offset from the x^M line (M = 2, 4, 8) and track the phase with a loop. Off: the constellation turns.");
        ImGui::SameLine(0, 8 * gUi);
        ImGui::Checkbox("differential", &p.differential);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The data is in the phase step between symbols (DBPSK, DQPSK, D8PSK): also removes the phase ambiguity of the carrier loop.");
        ImGui::SameLine(0, 8 * gUi);
        ImGui::TextDisabled("roll-off"); ImGui::SameLine(0, 4 * gUi);
        ImGui::SetNextItemWidth(80 * gUi);
        float ro = (float)p.rolloff;
        if (ImGui::SliderFloat("##wbro", &ro, 0.f, 1.f, "%.2f")) p.rolloff = ro;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Root-raised-cosine matched filter with this roll-off; 0: a boxcar over one symbol");
    }
    if (p.kind == KFsk2 || p.kind == KFsk4 || p.kind == KMsk || p.kind == KGmsk) {
        bool inv = p.fskInvert != 0;
        if (ImGui::Checkbox("swap which side is 1", &inv)) p.fskInvert = inv ? 1.0 : 0.0;
    }
    if (p.kind == KAfsk) {
        ImGui::TextDisabled("Tones"); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(280 * gUi);
        if (ImGui::BeginCombo("##wbaf", kAfsk[s.afskPreset].name)) {
            for (int i = 0; i < (int)(sizeof kAfsk / sizeof kAfsk[0]); i++)
                if (ImGui::Selectable(kAfsk[i].name, s.afskPreset == i)) { s.afskPreset = i; if (kAfsk[i].mark) { p.markHz = kAfsk[i].mark; p.spaceHz = kAfsk[i].space; p.baud = kAfsk[i].baud; } }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 6 * gUi);
        ImGui::TextDisabled("mark"); ImGui::SameLine(0, 3 * gUi); ImGui::SetNextItemWidth(70 * gUi); ImGui::InputDouble("##wbmk", &p.markHz, 0, 0, "%.0f");
        ImGui::SameLine(0, 4 * gUi);
        ImGui::TextDisabled("space"); ImGui::SameLine(0, 3 * gUi); ImGui::SetNextItemWidth(70 * gUi); ImGui::InputDouble("##wbsp", &p.spaceHz, 0, 0, "%.0f");
        ImGui::SameLine(0, 8 * gUi);
        ImGui::Checkbox("FM first", &p.fmFirst);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The selection is an FM carrier carrying the tones (a recording of the radio signal): demodulate FM first, the tones are in its audio. Off: the selection holds the tones themselves (an audio recording).");
    }
    if (p.kind == KCw) {
        ImGui::TextDisabled("pitch"); ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(80 * gUi);
        ImGui::InputDouble("##wbcw", &p.cwPitchHz, 0, 0, "%.0f Hz");
    }
    if (bits) {
        ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Clock recovery"); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(170 * gUi);
        static const char* cl[] = {"best phase only", "Gardner timing loop"};
        ImGui::Combo("##wbclk", &p.clock, cl, 2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Best phase: sample at the best instant of the first symbols and keep the rate fixed. Gardner: a loop that follows a small difference between the real symbol rate and the one typed in.");
        ImGui::SameLine(0, 8 * gUi);
        ImGui::TextDisabled("loop"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(90 * gUi);
        float lb = (float)p.loopBw;
        if (ImGui::SliderFloat("##wblb", &lb, 0.002f, 0.1f, "%.3f", ImGuiSliderFlags_Logarithmic)) p.loopBw = lb;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bandwidth of the timing and carrier loops, as a fraction of the symbol rate: narrower is quieter, wider follows faster");
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(s.dJob.running);
    if (ImGui::Button("Demodulate selection")) { s.note.clear(); startDemod(); savePrefs(); }
    ImGui::EndDisabled();
    if (s.dJob.running) { ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("working..."); }
    if (!s.note.empty()) { ImGui::SameLine(0, 10 * gUi); ImGui::TextColored(pal::warnAmber(), "%s", s.note.c_str()); }
    if (!s.haveDemod) { ImGui::TextDisabled("Select a burst (a time range and, to keep other signals out, a band) and press Demodulate selection."); return; }
    const DemodResult& r = s.dr;
    ImGui::Spacing();
    ImGui::Separator();
    if (bits) {
        char b[300];
        snprintf(b, sizeof b, "%zu symbols at %.2f Bd (%d bit%s each), %.2f samples per symbol, SNR %.1f dB, offset %+.1f Hz%s", r.symbols, r.baud, r.bitsPerSymbol, r.bitsPerSymbol > 1 ? "s" : "", r.sps, r.snrDb, r.cfoHz,
                 r.devHz > 0 ? (std::string(", deviation ") + fmtFreq(r.devHz, 1)).c_str() : "");
        ImGui::TextDisabled("%s", b);
    }
    // listening
    if (!r.audio.empty()) {
        if (!s.playing) { if (ImGui::Button("Listen")) { s.playing = true; s.playPos = 0; if (s.aud) s.aud->flush(); } }
        else if (ImGui::Button("Stop")) stopAudio();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play the demodulated audio through OnAir's audio output (volume and mute are the app's)");
        ImGui::SameLine(0, 8 * gUi);
        ImGui::SetNextItemWidth(90 * gUi);
        ImGui::SliderFloat("##wbvol", &a.volume, 0.f, 1.f, "vol %.2f");
        ImGui::SameLine(0, 6 * gUi);
        ImGui::Checkbox("mute", &a.muted);
        ImGui::SameLine(0, 8 * gUi);
        ImGui::TextDisabled("%.1f s of audio", (double)r.audio.size() / 48000);
    }
    // the plots
    const float h = std::max(70.f * gUi, std::min(110.f * gUi, ImGui::GetContentRegionAvail().y / 6));
    if (!r.instFreq.empty()) {
        timePlot("Instantaneous frequency (Hz)", "Hz", r.instFreq, r.plotRate, h, ImVec4(0.45f, 0.75f, 0.95f, 1));
        timePlot("Phase (rad)", "rad", r.instPhase, r.plotRate, h * 0.8f, ImVec4(0.95f, 0.65f, 0.35f, 1));
        timePlot("Magnitude", "|x|", r.magnitude, r.plotRate, h * 0.8f, ImVec4(0.55f, 0.85f, 0.5f, 1));
    }
    if (bits) {
        const float sz = std::max(150.f * gUi, std::min(260.f * gUi, ImGui::GetContentRegionAvail().x / 2 - 12 * gUi));
        eyePlot(r, ImVec2(sz, sz * 0.75f));
        ImGui::SameLine(0, 8 * gUi);
        if (kindIsPsk(p.kind)) {
            std::vector<cf32> pts(r.constellation.begin(), r.constellation.begin() + (long)std::min<size_t>(r.constellation.size(), 6000));
            scatter("##wbconst", pts, ImVec2(sz * 0.75f, sz * 0.75f), 1.6, ImVec4(0.5f, 0.8f, 1, 0.5f));
        } else histPlot(r, ImVec2(sz, sz * 0.75f));
        ImGui::Spacing();
        ImGui::TextColored(pal::heading(), "BITS");
        ImGui::SameLine(0, 8 * gUi);
        ImGui::TextDisabled("%zu bits as demodulated; the Bits tab has the line codes, views, search and export", r.bits.size());
        ImGui::PushFont(a.mono, 0);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(bitsToString(r.bits, 0, 320).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        if (ImGui::Button("Open in Bits tab")) s.tab = 3;
    }
}

// ---- Bits ----------------------------------------------------------------------------------------------------------------------------

void recomputeBits() {
    State& s = S();
    s.manchErrors = 0;
    BitOpts o = s.bo;
    o.skip = (size_t)s.bskip;
    s.shown = applyBits(s.haveDemod ? s.dr.bits : Bits(), o, &s.manchErrors);
    s.hits.clear(); s.hitsInv.clear();
    Bits pat;
    if (parsePattern(s.pat, pat)) s.hits = findPattern(s.shown, pat, s.bInvertAlso, s.patMis, 5000, &s.hitsInv);
    const size_t bytesPage = (size_t)s.bpl * 192;
    const size_t pages = std::max<size_t>(1, (s.shown.size() / 8 + bytesPage - 1) / bytesPage);
    s.bpage = std::max(0, std::min((int)pages - 1, s.bpage));
    s.bitsText = formatBits(s.shown, s.bview, s.bpl, s.blsb, bytesPage, (size_t)s.bpage * bytesPage);
    s.bitsDirty = false;
}

void drawBits(App& a) {
    State& s = S();
    if (!s.haveDemod || !kindHasBits(s.dp.kind) || s.dr.bits.empty()) {
        ImGui::TextDisabled("No bit stream yet: choose a digital demodulator on the Demodulate tab and run it on a burst.");
        return;
    }
    bool ch = s.bitsDirty;
    ImGui::TextDisabled("Line code and bit order");
    ch |= ImGui::Checkbox("invert", &s.bo.invert); ImGui::SameLine(0, 8 * gUi);
    ch |= ImGui::Checkbox("differential decode", &s.bo.diffDecode);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("out = in XOR the previous in (undo differential encoding at the bit level; PSK has its own option on the Demodulate tab)");
    ImGui::SameLine(0, 8 * gUi);
    ch |= ImGui::Checkbox("NRZI", &s.bo.nrzi);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("NRZI decode: a change of level is 0, no change is 1 (HDLC / USB / AX.25). Invert for the other convention.");
    ImGui::SameLine(0, 8 * gUi);
    ImGui::SetNextItemWidth(130 * gUi);
    static const char* mc[] = {"no Manchester", "Manchester IEEE 802.3", "Manchester G.E. Thomas"};
    ch |= ImGui::Combo("##wbman", &s.bo.manchester, mc, 3);
    if (s.bo.manchester) { ImGui::SameLine(0, 4 * gUi); int ph = s.bo.manchesterPhase; ImGui::SetNextItemWidth(60 * gUi); if (ImGui::SliderInt("##wbmp", &ph, 0, 1, "phase %d")) { s.bo.manchesterPhase = ph; ch = true; } ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("%zu invalid pairs", s.manchErrors); }
    ImGui::SameLine(0, 8 * gUi);
    ch |= ImGui::Checkbox("reverse bits in each byte", &s.bo.bitReverse);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Byte alignment"); ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(70 * gUi);
    ch |= ImGui::SliderInt("##wbsk", &s.bskip, 0, 7, "skip %d");
    ImGui::SameLine(0, 10 * gUi);
    ch |= ImGui::Checkbox("LSB first in bytes", &s.blsb);
    ImGui::SameLine(0, 10 * gUi);
    ImGui::SetNextItemWidth(100 * gUi);
    static const char* vw[] = {"hex", "binary", "ASCII"};
    ch |= ImGui::Combo("##wbvw", &s.bview, vw, 3);
    ImGui::SameLine(0, 6 * gUi);
    ImGui::SetNextItemWidth(70 * gUi);
    ch |= ImGui::SliderInt("##wbbpl", &s.bpl, 4, 32, "%d / line");
    if (ch) recomputeBits();
    // search
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Find"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(170 * gUi);
    bool sc = ImGui::InputText("##wbpat", s.pat, sizeof s.pat);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A bit pattern: 0x7E, 7E, 1011 0010, 0b1010 ...");
    ImGui::SameLine(0, 6 * gUi);
    sc |= ImGui::Checkbox("also inverted", &s.bInvertAlso);
    ImGui::SameLine(0, 6 * gUi);
    ImGui::SetNextItemWidth(80 * gUi);
    sc |= ImGui::SliderInt("##wbmm", &s.patMis, 0, 6, "%d wrong");
    if (sc) recomputeBits();
    Bits pat;
    const bool patOk = parsePattern(s.pat, pat);
    ImGui::SameLine(0, 8 * gUi);
    if (!patOk) ImGui::TextColored(pal::warnAmber(), "not a pattern");
    else ImGui::TextDisabled("%zu found%s", s.hits.size(), s.bInvertAlso ? (", " + std::to_string(s.hitsInv.size()) + " inverted").c_str() : "");
    // numbers
    size_t ones = 0;
    for (uint8_t v : s.shown) ones += v & 1;
    ImGui::TextDisabled("%zu bits, %.1f %% ones, %zu bytes", s.shown.size(), s.shown.empty() ? 0.0 : 100.0 * (double)ones / (double)s.shown.size(), s.shown.size() / 8);
    // pages
    const size_t bytesPage = (size_t)s.bpl * 192;
    const int pages = (int)std::max<size_t>(1, (s.shown.size() / 8 + bytesPage - 1) / bytesPage);
    ImGui::SameLine(0, 12 * gUi);
    if (ImGui::SmallButton("<") && s.bpage > 0) { s.bpage--; recomputeBits(); }
    ImGui::SameLine(0, 3 * gUi); ImGui::TextDisabled("page %d / %d", s.bpage + 1, pages); ImGui::SameLine(0, 3 * gUi);
    if (ImGui::SmallButton(">") && s.bpage + 1 < pages) { s.bpage++; recomputeBits(); }
    ImGui::SameLine(0, 12 * gUi);
    if (ImGui::Button("Copy bits")) {
        std::string t = bitsToString(s.shown, 0, 1u << 20);
        ImGui::SetClipboardText(t.c_str());
        s.note = "bits copied";
    }
    ImGui::SameLine(0, 4 * gUi);
    if (ImGui::Button("Copy hex")) { ImGui::SetClipboardText(formatBits(s.shown, ViewHex, 32, s.blsb, 1u << 20).c_str()); s.note = "hex copied"; }
    ImGui::SameLine(0, 4 * gUi);
    if (ImGui::Button("Save text...")) { const std::string p = saveFileDialog("bits.txt"); if (!p.empty()) { std::string e; s.note = writeBitsText(p, s.shown, e) ? "saved " + p : e; } }
    ImGui::SameLine(0, 4 * gUi);
    if (ImGui::Button("Save binary...")) { const std::string p = saveFileDialog("bits.bin"); if (!p.empty()) { std::string e; s.note = writeBitsBin(p, s.shown, s.blsb, e) ? "saved " + p : e; } }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The bits packed into bytes (MSB first, or LSB first when that is ticked); the last byte is padded with zeros");
    if (!s.note.empty()) { ImGui::SameLine(0, 8 * gUi); ImGui::TextColored(pal::okGreen(), "%s", s.note.c_str()); }
    // matches
    if (patOk && (!s.hits.empty() || !s.hitsInv.empty())) {
        ImGui::TextDisabled("at bit:");
        int n = 0;
        auto jump = [&](size_t bit) { s.bpage = (int)((bit / 8) / bytesPage); recomputeBits(); };
        for (size_t h : s.hits) { if (n++ >= 14) break; ImGui::SameLine(0, 6 * gUi); char b[32]; snprintf(b, sizeof b, "%zu##h%zu", h, h); if (ImGui::SmallButton(b)) jump(h); }
        for (size_t h : s.hitsInv) { if (n++ >= 20) break; ImGui::SameLine(0, 6 * gUi); char b[32]; snprintf(b, sizeof b, "~%zu##i%zu", h, h); if (ImGui::SmallButton(b)) jump(h); }
    }
    ImGui::Separator();
    ImGui::PushFont(a.mono, 0);
    const ImVec2 sz(-1, std::max(120.f * gUi, ImGui::GetContentRegionAvail().y - 6 * gUi));
    ImGui::InputTextMultiline("##wbbits", (char*)s.bitsText.c_str(), s.bitsText.size() + 1, sz, ImGuiInputTextFlags_ReadOnly);
    ImGui::PopFont();
}

// ---- Export --------------------------------------------------------------------------------------------------------------------------

void drawExport(App& a) {
    State& s = S();
    (void)a;
    if (!s.src) { noSource("export"); return; }
    selectionNote();
    static int fmt = ExCf32;
    ImGui::AlignTextToFramePadding(); ImGui::TextColored(pal::heading(), "SELECTION AS A RECORDING");
    ImGui::TextDisabled("Format"); ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(250 * gUi);
    static const char* ef[] = {"WAV (float, two channels I / Q; mono for a real recording)", "cf32 (raw float32 I/Q)", "SigMF (cf32 data + meta, with the centre frequency)"};
    ImGui::Combo("##wbef", &fmt, ef, 3);
    ImGui::SameLine(0, 8 * gUi);
    if (ImGui::Button("Export selection...")) {
        uint64_t t0, t1; double f0, f1;
        selection(t0, t1, f0, f1);
        const std::string p = saveFileDialog(fmt == ExWav ? "selection.wav" : fmt == ExCf32 ? "selection.cf32" : "selection.sigmf-data");
        if (!p.empty()) {
            std::string err;
            s.note = exportSelection(*s.src, t0, t1, f0, f1, p, fmt, err) ? "saved " + p : "export failed: " + err;
        }
    }
    ImGui::TextDisabled("With a band selected only that band is written, moved to baseband and at a lower sample rate (its centre goes into the SigMF file).");
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "RESULTS");
    ImGui::BeginDisabled(!s.haveDemod || s.dr.audio.empty());
    if (ImGui::Button("Demodulated audio (.wav)...")) {
        const std::string p = saveFileDialog("demodulated.wav");
        if (!p.empty()) { std::string err; s.note = writeWavMono16(p, s.dr.audio, 48000, err) ? "saved " + p : err; }
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, 6 * gUi);
    ImGui::BeginDisabled(!s.haveDemod || s.dr.bits.empty());
    if (ImGui::Button("Bits (.txt)...")) { const std::string p = saveFileDialog("bits.txt"); if (!p.empty()) { std::string err; s.note = writeBitsText(p, s.shown.empty() ? s.dr.bits : s.shown, err) ? "saved " + p : err; } }
    ImGui::SameLine(0, 4 * gUi);
    if (ImGui::Button("Bits (.bin)...")) { const std::string p = saveFileDialog("bits.bin"); if (!p.empty()) { std::string err; s.note = writeBitsBin(p, s.shown.empty() ? s.dr.bits : s.shown, s.blsb, err) ? "saved " + p : err; } }
    ImGui::EndDisabled();
    ImGui::SameLine(0, 6 * gUi);
    ImGui::BeginDisabled(!s.haveMeas || s.meas.peaks.empty());
    if (ImGui::Button("Peaks (.csv)...")) {
        const std::string p = saveFileDialog("peaks.csv");
        if (!p.empty()) { std::string err; s.note = writeTextFile(p, peaksCsv(s.meas, s.src->centerHz()), err) ? "saved " + p : err; }
    }
    ImGui::EndDisabled();
    if (!s.haveMeas) ImGui::TextDisabled("(the peaks need a measurement, the audio and bits a demodulation)");
    if (!s.note.empty()) ImGui::TextColored(pal::okGreen(), "%s", s.note.c_str());
}

} // namespace wbui
