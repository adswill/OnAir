// DVB-C screens: status bar, the analysis row (constellation, equaliser taps, MER history, Reed-Solomon losses), the Receiver tab, the tuner
// (cable channel, symbol rate, QAM, annex) and the test signal options. The TV tab, services and the player are the shared ones.
#include "app.h"
#include "dect2/dvbc/dvbc_rx.h"
#include <cmath>
#include <deque>
#include <utility>

namespace {

struct State {
    uint64_t lastSeq = 0;
    bool wasRunning = false, loaded = false;
    double srMsym = 0;                                // 0 = measured
    int qam = -1;                                     // -1 automatic, 0..4
    bool annexC = false;
    double pushedSr = -1; int pushedQam = -2; int pushedAnnex = -1;
    std::deque<float> mer, loss;
    std::deque<std::pair<uint64_t, uint64_t>> blocks;
};
State S;

bool live(const App& a) { return a.rx.standard == 41; }

const char* kQam[5] = {"16-QAM", "32-QAM", "64-QAM", "128-QAM", "256-QAM"};
const ImVec4 kDim(0.6f, 0.64f, 0.68f, 1), kGood(0.35f, 0.90f, 0.45f, 1), kWarn(0.95f, 0.60f, 0.25f, 1), kBad(0.95f, 0.45f, 0.30f, 1), kText(0.93f, 0.95f, 0.97f, 1);
const char* kCableNote = "DVB-C needs a cable feed: connect the radio to the cable TV outlet (through a DC block and an attenuator), not to an antenna.";

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.srMsym = std::max(0.0, d.getD("dvbcSr", 0.0));
    S.qam = (int)std::max(-1L, std::min(4L, d.getI("dvbcQam", -1)));
    S.annexC = d.getI("dvbcAnnexC", 0) != 0;
}

void saveState(App& a) {
    plat::Prefs& d = plat::prefs();
    d.setD("dvbcSr", S.srMsym);
    d.setI("dvbcQam", S.qam);
    d.setI("dvbcAnnexC", S.annexC ? 1 : 0);
    savePrefs(a);
}

void push(std::deque<float>& d, float v) { d.push_back(v); if (d.size() > 600) d.pop_front(); }

float recentLoss() {
    if (S.blocks.size() < 2) return -1;
    const auto& f = S.blocks.front(); const auto& b = S.blocks.back();
    const uint64_t ok = b.first - f.first, bad = b.second - f.second;
    return ok + bad ? 100.f * (float)bad / (float)(ok + bad) : -1.f;
}

void tick(App& a) {
    loadState();
    const bool run = a.engine.running();
    if (run && (!S.wasRunning || S.pushedSr != S.srMsym || S.pushedQam != S.qam || S.pushedAnnex != (int)S.annexC)) {
        a.engine.dvbc().setSymbolRate(S.srMsym * 1e6);
        a.engine.dvbc().setQam(S.qam);
        a.engine.dvbc().setAnnexC(S.annexC);
        S.pushedSr = S.srMsym; S.pushedQam = S.qam; S.pushedAnnex = (int)S.annexC;
    }
    S.wasRunning = run;
    if (!live(a) || a.rx.seq == S.lastSeq) return;
    S.lastSeq = a.rx.seq;
    const DvbcTelemetry& t = a.rx.dvbc;
    if (t.lockCarrier) push(S.mer, t.merDb);
    S.blocks.push_back({t.blocksOk, t.blocksBad});
    if (S.blocks.size() > 17) S.blocks.pop_front();
    if (t.lockSync) { const float l = recentLoss(); if (l >= 0) push(S.loss, l); }
}

// ---------------------------------------------------------------- the status bar

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const DvbcTelemetry& t = a.rx.dvbc;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Carrier", !on ? 0 : t.lockCarrier ? 1 : t.lockSpectrum ? 2 : 0); flowNext(12 * gUi);
    lamp("Timing", !on ? 0 : t.lockTiming ? 1 : t.lockSpectrum ? 2 : 0); flowNext(12 * gUi);
    lamp("Equaliser", !on ? 0 : t.lockCarrier ? 1 : t.lockSpectrum ? 2 : 0); flowNext(12 * gUi);
    lamp("RS", !on ? 0 : t.lockSync ? (t.tsLock ? 1 : 3) : 0); flowNext(12 * gUi);
    lamp("TS lock", !on ? 0 : t.tsLock ? 1 : t.lockSync ? 2 : 0, (int)Ic::Layers); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val, ImVec4 col = kText) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(col, "%s", val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[96];
    if (!run) ro("State", "stopped", kDim);
    else if (a.engine.radioLost()) ro("State", "radio disconnected", kBad);
    else if (!on) ro("State", "starting", kDim);
    else if (t.tsLock) ro("State", "Locked", kGood);
    else if (t.state >= 1) ro("State", "Syncing", ImVec4(0.95f, 0.85f, 0.35f, 1));
    else ro("State", "Searching", kDim);
    if (on && t.qam >= 0) ro("QAM", kQam[t.qam]);
    if (on && t.symbolRate > 0) { snprintf(b, sizeof b, "%.4f Msym/s", t.symbolRate / 1e6); ro("SR", b); }
    flowBreak();
    if (on && t.lockCarrier) {
        snprintf(b, sizeof b, "%.1f dB", t.merDb); ro("MER", b);
        snprintf(b, sizeof b, "%+.0f Hz", t.cfoHz); ro("CFO", b);
        snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro("RS ok/bad", b, t.blocksBad ? kWarn : kText);
    }
    if (run) { snprintf(b, sizeof b, "%.3f Msps", a.engine.sampleRate() / 1e6); ro("fs", b); }
    if (run) { const SampleLoss l = a.engine.sampleLoss(); ro("dropped", lossText(l), lossColour(l)); lossTooltip(l); }
    flowBreak();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    const DvbcTelemetry& t = a.rx.dvbc;
    char b[128];
    l1 = t.annexC ? "DVB-C (annex C)" : "DVB-C";
    if (!live(a)) return;
    if (t.lockSync) snprintf(b, sizeof b, "%s %.4f Msym/s  MER %.0f dB  %s", t.qam >= 0 ? kQam[t.qam] : "", t.symbolRate / 1e6, t.merDb, t.tsLock ? "locked" : "decoding");
    else snprintf(b, sizeof b, "%s", t.stage.c_str());
    l2 = b;
}

// ---------------------------------------------------------------- the analysis row

void history(const char* id, const char* ylab, const std::deque<float>& d, ImVec2 sz, bool fixed, double lo, double hi) {
    if (!plt::BeginPlot(id, sz, plt::Flags_NoLegend | plt::Flags_NoTitle)) return;
    plt::SetupAxes("reports (~4/s)", ylab, 0, fixed ? 0 : plt::AxisFlags_AutoFit);
    plt::SetupAxisLimits(plt::X1, 0, 600, plt::Cond_Always);
    if (fixed) plt::SetupAxisLimits(plt::Y1, lo, hi, plt::Cond_Always);
    if (!d.empty()) { std::vector<float> v(d.begin(), d.end()); plt::Spec sp; sp.LineColor = pal::accent(); sp.LineWeight = 1.4f; plt::PlotLine("a", v.data(), (int)v.size(), sp); }
    plt::EndPlot();
}

void tapsPlot(const DvbcTelemetry& t, bool on, ImVec2 sz) {
    if (!plt::BeginPlot("##dvbctaps", sz, plt::Flags_NoLegend | plt::Flags_NoTitle)) return;
    plt::SetupAxes("delay (us)", "dB");
    plt::SetupAxisLimits(plt::Y1, -50, 5, plt::Cond_Always);
    if (on && t.eqDb.size() > 1 && t.symbolRate > 0) {
        const int n = (int)t.eqDb.size();
        std::vector<float> x((size_t)n), y((size_t)n);
        const double us = 0.5e6 / t.symbolRate;   // half a symbol
        for (int i = 0; i < n; i++) { x[(size_t)i] = (float)((t.eqCentre - i) * us); y[(size_t)i] = t.eqDb[(size_t)i]; }
        plt::SetupAxisLimits(plt::X1, std::min(x.front(), x.back()), std::max(x.front(), x.back()), plt::Cond_Always);
        plt::Spec sp; sp.LineColor = pal::accent(); sp.LineWeight = 1.2f;
        plt::PlotLine("taps", x.data(), y.data(), n, sp);
    } else plt::SetupAxisLimits(plt::X1, -2, 2, plt::Cond_Always);
    plt::EndPlot();
}

void panels(App& a) {
    const DvbcTelemetry& t = a.rx.dvbc;
    const bool on = live(a);
    const float availW = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi;
    const float plotH = std::max(90.f, availH - 26.f - ImGui::GetFrameHeight());
    const float sqW = std::min(plotH, std::max(120.f, availW * 0.25f));
    const float colW = std::max(120.f, (availW - 5 * gap - sqW) / 3.f);
    const ImVec2 sz(colW, plotH), sq(sqW, sqW);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap);
    ImGui::BeginGroup();
    captionFit(sqW, "Constellation (%zu symbols)", on ? t.cells.size() : (size_t)0);
    scatter("##dvbccon", on ? t.cells : std::vector<cf32>(), sq, 1.5, pal::accent(0.40f));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Equaliser taps (echoes)");
    tapsPlot(t, on, sz);
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "MER (dB)");
    history("##dvbcmer", "dB", S.mer, sz, false, 0, 0);
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Packets lost (%%)");
    history("##dvbcloss", "%", S.loss, sz, true, 0, 100);
    ImGui::EndGroup();
}

// ---------------------------------------------------------------- the Receiver tab

void receiver(App& a) {
    const DvbcTelemetry& t = a.rx.dvbc;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("DVB-C (EN 300 429 / ITU-T J.83 annex A, and annex C): symbol rate from the spectrum, matched filter and timing loop, blind then "
                        "decision-directed equaliser with a carrier loop, QAM order found from the stream, Reed-Solomon (204,188), energy dispersal.");
    ImGui::TextColored(kWarn, "%s", kCableNote);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "Start the receiver with DVB-C selected."); return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(190 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", t.stage.c_str());
    if (!t.note.empty()) kv("note", "%s", t.note.c_str());
    kv("annex", "%s, roll-off %.2f", t.annexC ? "C (6 MHz)" : "A (8 MHz)", t.rollOff);
    kv("QAM", "%s%s", t.qam >= 0 ? kQam[t.qam] : "not known yet", t.qamManual ? " (manual)" : "");
    kv("symbol rate", "%.4f Msym/s%s", t.symbolRate / 1e6, t.symbolRateManual ? " (manual)" : "");
    kv("  measured", "%.4f Msym/s (envelope line), carrier width %.3f MHz", t.symbolRateMeasured / 1e6, t.width3dBHz / 1e6);
    kv("symbol clock", "%+.1f ppm", t.clockPpm);
    kv("carrier offset", "%+.1f Hz", t.cfoHz);
    kv("spectrum", "%s", t.inverted ? "inverted" : "normal");
    kv("input level", "%.1f dBFS", t.levelDbfs);
    kv("MER", "%.1f dB", t.merDb);
    kv("payload rate", "%.2f Mbit/s", t.netMbps);
    ImGui::Spacing();
    kv("Reed-Solomon", "%llu clean, %llu corrected, %llu failed", (unsigned long long)t.rsClean, (unsigned long long)t.rsCorrected, (unsigned long long)t.rsFailed);
    kv("bytes corrected", "%.2e of the bytes", t.preRsByteErr);
    kv("transport stream", "%llu packets, %llu with errors, %s", (unsigned long long)t.packets, (unsigned long long)t.packetsBad, t.tsLock ? "locked" : "no lock");
    kv("searches", "%llu", (unsigned long long)t.reacquisitions);
    kv("input rate", "%s", a.rx.rateOk ? "ok" : "too low for DVB-C (7 Msps or more)");
}

// ---------------------------------------------------------------- tuner, decoder and test signal

template <size_t N> bool combo(const char* id, float w, int& v, const char* const (&names)[N]) {
    bool ch = false;
    ImGui::SetNextItemWidth(std::min(w * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo(id, names[(size_t)v < N ? (size_t)v : 0])) {
        for (size_t i = 0; i < N; i++) if (ImGui::Selectable(names[i], (size_t)v == i)) { v = (int)i; ch = true; }
        ImGui::EndCombo();
    }
    return ch;
}

void tuner(App& a, bool& retune) {
    (void)retune;
    loadState();
    // the European cable raster: channel centres 114 + 8 k MHz up to 858 MHz
    ImGui::TextDisabled("Channel");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("European cable raster, 8 MHz channels from 114 to 862 MHz.\n%s", kCableNote);
    sameLineIf(90 * gUi, 5 * gUi);
    char cur[48];
    snprintf(cur, sizeof cur, "%.0f MHz", dvbcChannelCentreMhz(a.freqMhz));
    ImGui::SetNextItemWidth(std::min(110 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##dvbcch", cur)) {
        for (int k = 0; k <= 93; k++) {
            const double f = 114.0 + 8.0 * k;
            char n[32]; snprintf(n, sizeof n, "%.0f MHz", f);
            if (ImGui::Selectable(n, std::fabs(a.freqMhz - f) < 0.5)) tuneFreq(a, f);
        }
        ImGui::EndCombo();
    }
    flowNext(8 * gUi);
    ImGui::TextDisabled("Symbol rate");
    sameLineIf(90 * gUi, 5 * gUi);
    static const double kRates[] = {0, 6.952, 6.900, 6.875, 6.111, 5.361, 5.274};
    static const char* const kRateNames[] = {"auto", "6.952", "6.900", "6.875", "6.111", "5.361 (C)", "5.274 (C)"};
    int ri = 0;
    for (int i = 1; i < 7; i++) if (std::fabs(S.srMsym - kRates[i]) < 1e-6) ri = i;
    if (combo("##dvbcsr", 96, ri, kRateNames)) { S.srMsym = kRates[ri]; saveState(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Msym/s. Auto measures it from the signal; a manual rate that does not lock gives way to the measured one.");
    {   // 3.4: any symbol rate, not only the list
        flowNext(4 * gUi);
        static float typed = 0;
        static double synced = -1;
        if (synced != S.srMsym) { typed = (float)S.srMsym; synced = S.srMsym; }   // follows the list, not the typing
        ImGui::SetNextItemWidth(70 * gUi);
        if (ImGui::InputFloat("Msym/s##dvbcsrfree", &typed, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) { S.srMsym = typed > 0 ? (double)typed : 0.0; saveState(a); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Type any symbol rate in Msym/s and press Enter (0 = auto).");
    }
    flowNext(8 * gUi);
    ImGui::TextDisabled("QAM");
    sameLineIf(50 * gUi, 5 * gUi);
    static const char* const kQamNames[] = {"auto", "16-QAM", "32-QAM", "64-QAM", "128-QAM", "256-QAM"};
    int qi = S.qam + 1;
    if (combo("##dvbcqam", 90, qi, kQamNames)) { S.qam = qi - 1; saveState(a); }
    flowNext(8 * gUi);
    bool c = S.annexC;
    if (ImGui::Checkbox("Annex C", &c)) { S.annexC = c; saveState(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("ITU-T J.83 annex C (Japan, 6 MHz channels): roll-off 0.13 instead of 0.15.");
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(kWarn, "%s", kCableNote);
    ImGui::PopTextWrapPos();
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    static const char* const qam[] = {"64-QAM", "16-QAM", "32-QAM", "128-QAM", "256-QAM"};
    static const char* const annex[] = {"annex A", "annex C"};
    static const char* const inv[] = {"normal", "inverted"};
    if (combo("##tcq", 84, sc.modeOpt[0], qam)) changed = true;
    flowNext(6 * gUi);
    if (combo("##tca", 80, sc.modeOpt[1], annex)) changed = true;
    flowNext(6 * gUi);
    if (combo("##tci", 80, sc.modeOpt[2], inv)) changed = true;
    flowNext(12 * gUi); ImGui::TextDisabled("SNR"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(90 * gUi);
    float snr = (float)sc.snrDb; if (ImGui::SliderFloat("##tcsnr", &snr, 10, 45, "%.0f dB")) { sc.snrDb = snr; changed = true; }
    flowNext(8 * gUi); ImGui::TextDisabled("CFO"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(100 * gUi);
    float cfo = (float)(sc.cfoHz / 1e3); if (ImGui::SliderFloat("##tccfo", &cfo, -100, 100, "%.1f kHz")) { sc.cfoHz = cfo * 1e3; changed = true; }
    flowNext(8 * gUi); ImGui::TextDisabled("clock"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(90 * gUi);
    float ppm = (float)sc.modeVal[1]; if (ImGui::SliderFloat("##tcppm", &ppm, -100, 100, "%.0f ppm")) { sc.modeVal[1] = ppm; changed = true; }
    flowNext(8 * gUi); ImGui::TextDisabled("echo"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(90 * gUi);
    float ec = (float)sc.echoDb; if (ImGui::SliderFloat("##tcec", &ec, 0, 30, ec == 0 ? "off" : "-%.0f dB")) { sc.echoDb = ec; changed = true; }
    flowNext(8 * gUi); ImGui::TextDisabled("delay"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(90 * gUi);
    int dl = sc.echoDelay; if (ImGui::SliderInt("##tced", &dl, 1, 40, "%d smp")) { sc.echoDelay = dl; changed = true; }
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const bool on = live(a);
    const DvbcTelemetry& t = a.rx.dvbc;
    const float loss = recentLoss();
    out.push_back({"MER  dB", "%.1f", on && t.lockCarrier ? t.merDb : 0, 0, 45, !on || !t.lockCarrier ? 0 : t.merDb >= 30 ? 1 : t.merDb >= 22 ? 2 : 3});
    out.push_back({"PACKETS LOST  %", "%.1f", on && loss > 0 ? loss : 0, 0, 100, !on || !t.lockSync ? 0 : loss <= 0 ? 1 : loss < 5 ? 2 : 3});
    out.push_back({"CFO  Hz", "%+.0f", on ? t.cfoHz : 0, -100000, 100000, 0});
    out.push_back({"CLOCK  ppm", "%+.1f", on ? t.clockPpm : 0, -100, 100, 0});
    out.push_back({"LEVEL  dBFS", "%.1f", on ? t.levelDbfs : -120, -80, 0, 0});
    out.push_back({"PAYLOAD  Mbit/s", "%.2f", on && t.lockSync ? t.netMbps : 0, 0, 60, 0});
}

} // namespace

extern const ModeUi kDvbcUi;
const ModeUi kDvbcUi = {
    .sideTitle = "SERVICES",
    .receiver = receiver,
    .stream = true,
    .panels = panels,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
