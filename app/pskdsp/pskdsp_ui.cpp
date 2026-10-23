// PSK demodulator and analyser screens (family 202, not PSK31): the search-span spectrum (a click picks the signal), the constellation, the
// blind analysis (symbol rate candidates, modulation trial fit, pulse shape, quality) and the bits (differential decoding, descrambler, sync word,
// hex dump). EXPERIMENTAL: only generated signals have been tested.
#include "app.h"
#include "dect2/pskdsp/pskdsp_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using dect2::PskdTelemetry;
using dect2::pskd::Diff;
using dect2::pskd::Order;
using dect2::pskd::Scrambler;

struct State {
    bool loaded = false, dirty = true, wasRunning = false;
    int order = 0;          // Auto, BPSK, QPSK, OQPSK, 8PSK (the pskd::Order values)
    char baud[24] = {};     // "" or 0: blind
    int alpha = 0;          // index into kAlpha
    bool snap = true;
    int search = 0;         // index into kSearch
    int diff = 0, scr = 0;
    char sync[80] = {};
    bool invert = false, autoAmb = true;
    bool manual = false; double manualHz = 0, manualBw = 0;
};
State S;

const struct { const char* label; double v; } kAlpha[] = {{N_("auto"), -2}, {N_("rectangular"), -1}, {"0.20", 0.2}, {"0.25", 0.25}, {"0.35", 0.35}, {"0.50", 0.5}, {"0.75", 0.75}, {"1.00", 1.0}};
const struct { const char* label; double hz; } kSearch[] = {{N_("auto"), 0}, {"25 kHz", 25e3}, {"50 kHz", 50e3}, {"100 kHz", 100e3}, {"250 kHz", 250e3}, {"500 kHz", 500e3}, {"1 MHz", 1e6}, {"2.4 MHz", 2.4e6}};
const char* const kOrderNames[] = {N_("auto"), "BPSK", "QPSK", "OQPSK", "8PSK"};
const char* const kDiffNames[] = {N_("none"), N_("symbol (DPSK)"), N_("NRZ-M"), N_("NRZ-S / NRZI")};
const char* const kScrNames[] = {N_("none"), "G3RUH", "CCSDS"};

bool live(const App& a) { return dect2::pskd::matchesMode(a.family, a.rx.standard); }

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.order = (int)d.getI("pskdOrder", 0);
    snprintf(S.baud, sizeof S.baud, "%s", d.getS("pskdBaud", "").c_str());
    S.alpha = std::clamp((int)d.getI("pskdAlpha", 0), 0, 7);
    S.snap = d.getB("pskdSnap", true);
    S.search = std::clamp((int)d.getI("pskdSearch", 0), 0, 7);
    S.diff = std::clamp((int)d.getI("pskdDiff", 0), 0, 3);
    S.scr = std::clamp((int)d.getI("pskdScr", 0), 0, 2);
    snprintf(S.sync, sizeof S.sync, "%s", d.getS("pskdSync", "").c_str());
    S.invert = d.getB("pskdInvert", false);
    S.autoAmb = d.getB("pskdAmb", true);
}

void saveState() {
    plat::Prefs& d = plat::prefs();
    d.setI("pskdOrder", S.order); d.setS("pskdBaud", S.baud); d.setI("pskdAlpha", S.alpha); d.setB("pskdSnap", S.snap); d.setI("pskdSearch", S.search);
    d.setI("pskdDiff", S.diff); d.setI("pskdScr", S.scr); d.setS("pskdSync", S.sync); d.setB("pskdInvert", S.invert); d.setB("pskdAmb", S.autoAmb);
}

void pushSettings(App& a) {
    dect2::PskdReceiver& r = a.engine.pskdemod();
    r.setSearchHz(kSearch[S.search].hz);
    r.setOrder((Order)S.order);
    r.setBaud(std::max(0.0, atof(S.baud)));
    r.setAlpha(kAlpha[S.alpha].v);
    r.setSnapStandard(S.snap);
    r.setManualCarrier(S.manual, S.manualHz, S.manualBw);
    r.setBits((Diff)S.diff, (Scrambler)S.scr, S.sync, S.invert, S.autoAmb, 0, false);
    S.dirty = false;
}

void tick(App& a) {
    loadState();
    const bool run = a.engine.running();
    if (run && (!S.wasRunning || S.dirty)) pushSettings(a);
    S.wasRunning = run;
}

template <class F> void combo(const char* id, int& v, int n, F name, float w) {
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo(id, tr(name(v)))) {
        for (int i = 0; i < n; i++) if (ImGui::Selectable(tr(name(i)), i == v)) { v = i; S.dirty = true; saveState(); }
        ImGui::EndCombo();
    }
}

void controls(App& a) {
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Modulation")); ImGui::SameLine(0, 5 * gUi);
    combo("##pdord", S.order, 5, [](int i) { return kOrderNames[i]; }, 90 * gUi);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("auto tries BPSK, QPSK, OQPSK and 8PSK on the signal and keeps the one whose constellation fits with every point used"));
    flowNext(8 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Baud")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::InputTextWithHint("##pdbaud", tr("auto"), S.baud, sizeof S.baud, ImGuiInputTextFlags_CharsDecimal)) { S.dirty = true; saveState(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Symbol rate in baud; empty measures it blindly (75 baud and up to hundreds of kilobaud)"));
    flowNext(8 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Roll-off")); ImGui::SameLine(0, 5 * gUi);
    combo("##pdalpha", S.alpha, 8, [](int i) { return kAlpha[i].label; }, 100 * gUi);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Pulse shape: root-raised-cosine roll-off, or rectangular pulses. auto tries the usual ones and keeps the best fit"));
    flowNext(8 * gUi);
    if (ImGui::Checkbox(tr("snap to standard rates"), &S.snap)) { S.dirty = true; saveState(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Round the measured rate to 75 / 150 / ... 9600 baud, 1200 x n and the other usual rates when within 0.6 %"));
    flowNext(8 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Search width")); ImGui::SameLine(0, 5 * gUi);
    combo("##pdsrch", S.search, 8, [](int i) { return kSearch[i].label; }, 90 * gUi);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How much of the band around the dial is searched for the signal"));
    flowNext(8 * gUi);
    if (ImGui::Button(tr("Analyse now"))) { a.engine.pskdemod().analyseNow(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Run the blind analysis again at once (it repeats every few seconds by itself)"));
    if (S.manual) {
        flowNext(8 * gUi);
        if (ImGui::Button(tr("Find the signal by itself"))) { S.manual = false; S.dirty = true; }
    }
    flowEnd();
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Differential")); ImGui::SameLine(0, 5 * gUi);
    combo("##pddiff", S.diff, 4, [](int i) { return kDiffNames[i]; }, 120 * gUi);
    flowNext(8 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Descrambler")); ImGui::SameLine(0, 5 * gUi);
    combo("##pdscr", S.scr, 3, [](int i) { return kScrNames[i]; }, 90 * gUi);
    flowNext(8 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Sync word")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(150 * gUi);
    if (ImGui::InputTextWithHint("##pdsync", tr("hex or bits"), S.sync, sizeof S.sync)) { S.dirty = true; saveState(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A known frame marker, hex (1ACFFC1D) or bits (0101...). It resolves the phase ambiguity (rotation and spectral inversion) and marks the frames below"));
    flowNext(8 * gUi);
    if (ImGui::Checkbox(tr("invert"), &S.invert)) { S.dirty = true; saveState(); }
    flowNext(8 * gUi);
    if (ImGui::Checkbox(tr("find phase ambiguity"), &S.autoAmb)) { S.dirty = true; saveState(); }
    flowNext(8 * gUi);
    if (ImGui::Button(tr("Clear bits"))) a.engine.pskdemod().clearResults();
    flowEnd();
}

void spectrum(App& a, const PskdTelemetry& t, bool on, float w, float h) {
    if (plt::BeginPlot("##pd_spec", ImVec2(w, h), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes(tr("Hz from the dial"), "dB", plt::AxisFlags_AutoFit, plt::AxisFlags_AutoFit);
        if (on && !t.spectrumDb.empty()) {
            std::vector<float> x(t.spectrumDb.size());
            for (size_t i = 0; i < x.size(); i++) x[i] = (float)(t.specLoHz + (double)i * t.specStepHz);
            plt::PlotLine("spec", x.data(), t.spectrumDb.data(), (int)x.size());
            if (t.haveAnalysis && t.signal) {
                const double v[2] = {t.carrierHz - t.bwHz / 2, t.carrierHz + t.bwHz / 2};
                plt::Spec sp; sp.LineColor = ImVec4(0.95f, 0.70f, 0.30f, 0.8f);
                plt::PlotInfLines("band", v, 2, sp);
            }
            if (plt::IsPlotHovered() && ImGui::IsMouseClicked(0)) {
                const plt::Point p = plt::GetPlotMousePos();
                S.manual = true; S.manualHz = p.x; S.manualBw = std::max(t.bwHz, t.baud * 1.5);
                S.dirty = true;
            }
        }
        plt::EndPlot();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Click on a signal to analyse that one instead of the strongest"));
}

void constellation(const PskdTelemetry& t, bool on, float w, float h) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float s = std::min(w, h);
    const ImVec2 c(p0.x + w / 2, p0.y + h / 2);
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(8, 9, 10, 255));
    dl->AddLine(ImVec2(c.x - s / 2, c.y), ImVec2(c.x + s / 2, c.y), IM_COL32(60, 64, 68, 255));
    dl->AddLine(ImVec2(c.x, c.y - s / 2), ImVec2(c.x, c.y + s / 2), IM_COL32(60, 64, 68, 255));
    dl->AddCircle(c, s / 2 * 0.62f, IM_COL32(60, 64, 68, 255), 48);
    if (on) {
        float rms = 0;
        for (size_t i = 0; i + 1 < t.constellation.size(); i += 2) rms += t.constellation[i] * t.constellation[i] + t.constellation[i + 1] * t.constellation[i + 1];
        const size_t n = t.constellation.size() / 2;
        rms = n ? std::sqrt(rms / (float)n) : 1.f;
        const float k = s / 2 * 0.62f / std::max(1e-6f, rms);
        for (size_t i = 0; i < n; i++)
            dl->AddCircleFilled(ImVec2(c.x + t.constellation[2 * i] * k, c.y - t.constellation[2 * i + 1] * k), 1.8f * gUi, IM_COL32(120, 220, 150, 170));
    }
    ImGui::Dummy(ImVec2(w, h));
}

void kvRows(App& a, const PskdTelemetry& t) {
    auto kv = [&](const char* k, const char* fmt, auto... v) {
        ImGui::TextDisabled("%s", tr(k)); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont();
    };
    if (!t.haveAnalysis) { ImGui::TextDisabled("%s", tr("analysing...")); return; }
    kv("status", "%s", t.status.c_str());
    if (!t.signal) return;
    kv("carrier", "%+.1f Hz %s", t.carrierHz, tr("from the dial"));
    kv("bandwidth", "%.0f Hz, %.1f dB %s", t.bwHz, t.analysisSnrDb, tr("over the noise"));
    if (t.rateFound) {
        kv("symbol rate", "%.6g baud %s", t.baud, t.baudSnapped ? tr("(standard rate)") : tr("(measured)"));
        kv("modulation", "%s", dect2::pskd::orderName((Order)t.order));
        kv("pulse shape", "%s", t.shape.c_str());
        kv("bit rate", "%.6g bit/s", t.bitRate);
        kv("quality", "EVM %.3f, MER %.1f dB, Es/N0 %.1f dB, Eb/N0 %.1f dB", t.evm, t.merDb, t.esn0Db, t.ebn0Db);
        kv("Doppler", "%+.2f Hz/s", t.dopplerHzPerSec);
    }
    kv("lines", "|x|^2 %.0f dB, x^2 %.0f dB, x^4 %.0f dB, x^8 %.0f dB", t.lineEnv, t.line2, t.line4, t.line8);
    for (const auto& e : t.orderEvm) kv("trial fit", "%s: EVM %.3f", dect2::pskd::orderName((Order)e.first), e.second);
}

void bitsView(App& a, const PskdTelemetry& t, bool on) {
    ImGui::TextDisabled("%s", tr("BITS"));
    ImGui::SameLine(0, 10 * gUi);
    ImGui::PushFont(a.mono, 0);
    if (on) ImGui::Text("%llu %s, %llu %s, %s %d%s%s", (unsigned long long)t.bits, tr("bits"), (unsigned long long)t.syncs, tr("sync words"), tr("rotation"), t.rotation * 90 / (t.order == (int)Order::Psk8 ? 2 : 1), t.conj ? " conj" : "", t.resolved ? "" : " ?");
    ImGui::PopFont();
    if (!on || t.rows.empty()) { ImGui::TextDisabled("%s", tr("no bits yet")); return; }
    ImGui::PushFont(a.mono, 0);
    for (const auto& r : t.rows) ImGui::TextUnformatted(r.c_str());
    ImGui::PopFont();
    if (!t.frames.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("after the sync word"));
        ImGui::PushFont(a.mono, 0);
        for (const auto& f : t.frames) ImGui::TextUnformatted(f.c_str());
        ImGui::PopFont();
    }
}

void tab(App& a) {
    loadState();
    const bool on = live(a);
    const PskdTelemetry& t = a.rx.pskdm;
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", tr("Experimental: tested with generated signals only, no real signal yet."));
    controls(a);
    const float W = ImGui::GetContentRegionAvail().x, H = std::max(200.f * gUi, ImGui::GetContentRegionAvail().y);
    const float topH = std::min(H * 0.45f, 260.f * gUi);
    spectrum(a, t, on, W * 0.62f, topH);
    ImGui::SameLine(0, 6 * gUi);
    constellation(t, on, W * 0.38f - 6 * gUi, topH);
    if (ImGui::BeginChild("##pdlow", ImVec2(0, 0), false)) {
        if (on) kvRows(a, t); else ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver"));
        ImGui::Separator();
        bitsView(a, t, on);
    }
    ImGui::EndChild();
}

void panels(App& a) {
    const PskdTelemetry& t = a.rx.pskdm;
    const bool on = live(a);
    const float w = (ImGui::GetContentRegionAvail().x - 8 * gUi) / 2, h = std::max(100.f * gUi, ImGui::GetContentRegionAvail().y);
    ImGui::BeginGroup();
    captionFit(w, "%s", tr("Envelope spectrum |x|^2: a line at the symbol rate"));
    if (plt::BeginPlot("##pd_env", ImVec2(w, h - ImGui::GetTextLineHeightWithSpacing()), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("Hz", "dB", plt::AxisFlags_AutoFit, plt::AxisFlags_AutoFit);
        if (on && !t.envSpecDb.empty() && t.envHzPerBin > 0) {
            const size_t n = std::min<size_t>(t.envSpecDb.size(), 4096);
            std::vector<float> x(n);
            for (size_t i = 0; i < n; i++) x[i] = (float)((double)i * t.envHzPerBin);
            plt::PlotLine("env", x.data(), t.envSpecDb.data(), (int)n);
        }
        plt::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, 8 * gUi);
    ImGui::BeginGroup();
    captionFit(w, "%s", tr("Power-law spectrum x^M: a line at M times the carrier"));
    if (plt::BeginPlot("##pd_pow", ImVec2(w, h - ImGui::GetTextLineHeightWithSpacing()), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("Hz", "dB", plt::AxisFlags_AutoFit, plt::AxisFlags_AutoFit);
        if (on && !t.powSpecDb.empty() && t.powHzPerBin > 0) {
            std::vector<float> x(t.powSpecDb.size());
            const double c = (double)t.powSpecDb.size() / 2;
            for (size_t i = 0; i < x.size(); i++) x[i] = (float)(((double)i - c) * t.powHzPerBin);
            plt::PlotLine("pow", x.data(), t.powSpecDb.data(), (int)x.size());
        }
        plt::EndPlot();
    }
    ImGui::EndGroup();
}

void list(App& a) {
    const PskdTelemetry& t = a.rx.pskdm;
    if (!live(a) || t.rates.empty()) { ImGui::TextDisabled("%s", tr("no symbol rate candidates yet")); return; }
    ImGui::TextDisabled("%s", tr("Click a rate to use it"));
    for (size_t i = 0; i < t.rates.size(); i++) {
        char b[64];
        snprintf(b, sizeof b, "%.6g %s   %.0f %%##pdc%zu", t.rates[i].first, tr("baud"), t.rates[i].second * 100, i);
        if (ImGui::Selectable(b)) { snprintf(S.baud, sizeof S.baud, "%.6g", t.rates[i].first); S.dirty = true; saveState(); }
    }
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const PskdTelemetry& t = a.rx.pskdm;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", tr(k)); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", dect2::pskdSummary(t).c_str());
    kv("search span", "%.0f kHz", t.searchHz / 1e3);
    kv("input", "%.3f Msps", t.inputRate / 1e6);
    kv("following", "%.6g baud, EVM %.3f, lock %.2f", t.liveBaud, t.liveEvm, t.liveLock);
    kv("bits", "%llu", (unsigned long long)t.bits);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Signal"), !on ? 0 : a.rx.pskdm.signal ? 1 : 0); flowNext(12 * gUi);
    lamp(tr("Lock"), !on ? 0 : a.rx.pskdm.state == 2 ? 1 : a.rx.pskdm.state == 1 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.mono, 0);
    ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::pskdSummary(a.rx.pskdm).c_str());
    ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "PSK demodulator";
    if (live(a)) l2 = dect2::pskdSummary(a.rx.pskdm);
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const PskdTelemetry& t = a.rx.pskdm;
    out.push_back({"Eb/N0  dB", "%.1f", t.ebn0Db, -5, 30, t.ebn0Db >= 8 ? 1 : t.ebn0Db >= 3 ? 2 : 3});
    out.push_back({"BITS", "%.0f", (double)t.bits, 0, 100000, t.bits > 0 ? 1 : 0});
}

} // namespace

extern const ModeUi kPskdUi;
const ModeUi kPskdUi = {
    .sideTitle = "RATES",
    .tabName = "PSK analyser",
    .tabIcon = Ic::Wave,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .tick = tick,
    .meters = meters,
};
