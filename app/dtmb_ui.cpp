// DTMB screens: status bar, the analysis row (constellation, channel impulse response from the PN header, C/N and MER history, codeword loss),
// the Receiver tab, the decoder option and the options of the test signal. The TV tab, services and the player are the shared ones.
#include "app.h"
#include <cmath>
#include <deque>
#include <utility>

namespace {

struct State {
    uint64_t lastSeq = 0;
    bool wasRunning = false, loaded = false;
    int threads = 0;                                  // LDPC threads: 0 = automatic
    int pushedThreads = -1;
    std::deque<float> cn, mer, loss;                  // a point per report (about 4 a second)
    std::deque<std::pair<uint64_t, uint64_t>> blocks; // cumulative good and bad codewords at the last reports
};
State S;

bool live(const App& a) { return a.engine.running() && a.rx.standard == 8; }

const char* kHeader[3] = {"PN420", "PN595", "PN945"};
const char* kMap[5] = {"4QAM-NR", "4QAM", "16QAM", "32QAM", "64QAM"};
const char* kRate[3] = {"0.4", "0.6", "0.8"};
const int kHeaderLen[3] = {420, 595, 945};
const double kSymbolRate = 7.56e6;                    // symbols per second in the 8 MHz channel
const int kBodyLen = 3780;                            // symbols of the frame body

const ImVec4 kDim(0.6f, 0.64f, 0.68f, 1), kGood(0.35f, 0.90f, 0.45f, 1), kWarn(0.95f, 0.60f, 0.25f, 1), kBad(0.95f, 0.45f, 0.30f, 1), kText(0.93f, 0.95f, 0.97f, 1);

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.threads = (int)plat::prefs().getI("dtmbThreads", 0);
    if (S.threads < 0 || S.threads > 4) S.threads = 0;
}

void push(std::deque<float>& d, float v) { d.push_back(v); if (d.size() > 600) d.pop_front(); }

// the share of codewords lost over the last few seconds, or -1 when none came
float recentLoss() {
    if (S.blocks.size() < 2) return -1;
    const auto& f = S.blocks.front(); const auto& b = S.blocks.back();
    const uint64_t ok = b.first - f.first, bad = b.second - f.second;
    return ok + bad ? 100.f * (float)bad / (float)(ok + bad) : -1.f;
}

void tick(App& a) {
    loadState();
    const bool run = a.engine.running();
    if (run && (!S.wasRunning || S.pushedThreads != S.threads)) {
        if (S.threads > 0) a.engine.dtmb().setDecoderThreads(S.threads);
        S.pushedThreads = S.threads;
    }
    S.wasRunning = run;
    if (!live(a) || a.rx.seq == S.lastSeq) return;
    S.lastSeq = a.rx.seq;
    const DtmbTelemetry& t = a.rx.dtmb;
    if (t.state >= 1) push(S.cn, t.snrPnDb);
    if (t.siOk && t.merDb > 0) push(S.mer, t.merDb);
    S.blocks.push_back({t.blocksOk, t.blocksBad});
    if (S.blocks.size() > 17) S.blocks.pop_front();
    if (t.siOk) { const float l = recentLoss(); if (l >= 0) push(S.loss, l); }
}

std::string interleaverText(int i) { return i == 1 ? "mode 1" : i == 2 ? "mode 2" : "-"; }

// ---------------------------------------------------------------- the status bar

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const DtmbTelemetry& t = a.rx.dtmb;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    const float loss = recentLoss();
    int ldpc = 0;
    if (on && t.siOk) ldpc = (loss >= 0 && loss > 20) ? 3 : t.dataValid ? 1 : t.blocksBad > 0 ? 3 : 2;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("PN sync", !on ? 0 : t.state >= 2 ? 1 : t.state == 1 ? 2 : 0); flowNext(12 * gUi);
    lamp("SI", !on ? 0 : t.siOk ? 1 : t.state >= 1 ? 2 : 0); flowNext(12 * gUi);
    lamp("Carrier", !on ? 0 : t.carriers > 0 ? 1 : t.state >= 1 ? 2 : 0); flowNext(12 * gUi);
    lamp("LDPC", ldpc); flowNext(12 * gUi);
    lamp("TS lock", !on ? 0 : t.tsLock ? 1 : t.siOk ? 2 : 0, (int)Ic::Layers); flowNext(10 * gUi);
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
    else if (t.siOk) ro("State", "Decoding", ImVec4(0.95f, 0.85f, 0.35f, 1));
    else if (t.state >= 1) ro("State", "Syncing", ImVec4(0.95f, 0.85f, 0.35f, 1));
    else ro("State", "Searching", kDim);
    if (on && t.header >= 0) ro("Header", kHeader[t.header]);
    if (on && t.siOk) {
        ro("Mod", kMap[t.mapping]);
        ro("Rate", kRate[t.rate]);
        ro("Interleaver", interleaverText(t.interleaver));
        if (t.carriers == 1) ro("Carrier", "single");
    }
    flowBreak();
    if (on && t.state >= 1) {
        snprintf(b, sizeof b, "%.1f dB", t.snrPnDb); ro("C/N", b);
        if (t.siOk && t.merDb > 0) { snprintf(b, sizeof b, "%.1f dB", t.merDb); ro("MER", b); }
        snprintf(b, sizeof b, "%+.0f Hz", t.cfoHz); ro("CFO", b);
        if (t.siOk) {
            snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro("LDPC ok/bad", b, t.blocksBad && loss > 5 ? kWarn : kText);
            snprintf(b, sizeof b, "%llu", (unsigned long long)t.packets); ro("Packets", b);
        }
    }
    if (run) { snprintf(b, sizeof b, "%.3f Msps", a.engine.sampleRate() / 1e6); ro("fs", b); }
    ImGui::AlignTextToFramePadding();
    iconInline(Ic::Gauge, iconDim(), 0.9f); ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("Level"); ImGui::SameLine(0, 5 * gUi);
    {
        const ImU32 col = adc == AdcStatus::Overload ? IM_COL32(176, 66, 58, 255) : adc == AdcStatus::Good ? pal::remap(IM_COL32(40, 112, 150, 255)) : IM_COL32(176, 130, 48, 255);
        snprintf(b, sizeof b, run ? "%.1f dBFS" : "-", st.rmsDbfs);
        gaugePill(130, run ? (st.rmsDbfs + 60.f) / 60.f : 0.f, col, b);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("ADC level of the whole captured band (rms). %s\npeak %.2f   clip %.3f%%   DC %+.3f / %+.3f", adcAdvice(adc).c_str(), st.peak, st.clipFraction * 100, st.dcI, st.dcQ);
    }
    flowNext(15 * gUi);
    if (run) { const SampleLoss l = a.engine.sampleLoss(); ro("dropped", lossText(l), lossColour(l)); lossTooltip(l); }
    if (on && (t.cwDropped || t.cwSkipped)) {
        snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.cwDropped, (unsigned long long)t.cwSkipped);
        ro("cw dropped/skipped", b, t.cwDropped ? kWarn : kText);
    }
    if (run && a.devices[a.devIdx].kind != DeviceInfo::File) {
        ImGui::AlignTextToFramePadding();
        if (adc == AdcStatus::Overload) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1), "ADC OVERLOAD - %s", a.agcOn ? "AGC is lowering the gain" : "reduce the gain (or enable AGC)"); ImGui::PopTextWrapPos(); }
        else if (adc == AdcStatus::Low) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC level low - raise the gain"); ImGui::PopTextWrapPos(); }
        else if (adc == AdcStatus::NoSignal) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC sees almost nothing - antenna / gain?"); ImGui::PopTextWrapPos(); }
        else flowBreak();
    } else flowBreak();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    const DtmbTelemetry& t = a.rx.dtmb;
    char b[128];
    l1 = "DTMB";
    if (!live(a)) return;
    if (t.siOk && t.mapping >= 0 && t.rate >= 0) {
        snprintf(b, sizeof b, "%s %s %s  C/N %.0f dB  %s", kHeader[t.header], kMap[t.mapping], kRate[t.rate], t.snrPnDb, t.tsLock ? "locked" : "decoding");
    } else if (t.state >= 1) {
        snprintf(b, sizeof b, "%s  C/N %.0f dB  syncing", t.header >= 0 ? kHeader[t.header] : "PN ?", t.snrPnDb);
    } else snprintf(b, sizeof b, "searching");
    l2 = b;
}

// ---------------------------------------------------------------- the analysis row

void cirPlot(const DtmbTelemetry& t, bool on, ImVec2 sz) {
    if (!plt::BeginPlot("##dtmbcir", sz, plt::Flags_NoLegend | plt::Flags_NoTitle)) return;
    plt::SetupAxes("delay (us)", "dB");
    plt::SetupAxisLimits(plt::Y1, -70, 5, plt::Cond_Always);
    const bool has = on && t.cirDb.size() > 1;
    const double us = 1e6 / kSymbolRate;   // one symbol
    if (has) {
        const int n = (int)t.cirDb.size();
        std::vector<float> x((size_t)n), y((size_t)n);
        for (int i = 0; i < n; i++) { x[(size_t)i] = (float)((t.cirFirst + i) * us); y[(size_t)i] = t.cirDb[(size_t)i]; }
        plt::SetupAxisLimits(plt::X1, x.front(), x.back(), plt::Cond_Always);
        plt::Spec sp; sp.LineColor = pal::accent(); sp.LineWeight = 1.2f;
        plt::PlotLine("cir", x.data(), y.data(), n, sp);
    } else plt::SetupAxisLimits(plt::X1, -5, 5, plt::Cond_Always);
    plt::EndPlot();
}

void historyTwo(const char* id, const char* ylab, const std::deque<float>& a1, const std::deque<float>& a2, ImVec2 sz, bool fixed, double lo, double hi) {
    if (!plt::BeginPlot(id, sz, plt::Flags_NoLegend | plt::Flags_NoTitle)) return;
    plt::SetupAxes("reports (~4/s)", ylab, 0, fixed ? 0 : plt::AxisFlags_AutoFit);
    plt::SetupAxisLimits(plt::X1, 0, 600, plt::Cond_Always);
    if (fixed) plt::SetupAxisLimits(plt::Y1, lo, hi, plt::Cond_Always);
    if (!a1.empty()) { std::vector<float> v(a1.begin(), a1.end()); plt::Spec sp; sp.LineColor = pal::accent(); sp.LineWeight = 1.4f; plt::PlotLine("a", v.data(), (int)v.size(), sp); }
    if (!a2.empty()) { std::vector<float> v(a2.begin(), a2.end()); plt::Spec sp; sp.LineColor = ImVec4(0.40f, 0.85f, 0.50f, 1); sp.LineWeight = 1.4f; plt::PlotLine("b", v.data(), (int)v.size(), sp); }
    plt::EndPlot();
}

void panels(App& a) {
    const DtmbTelemetry& t = a.rx.dtmb;
    const bool on = live(a);
    const float availW = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi;
    const float plotH = std::max(90.f, availH - 26.f - ImGui::GetFrameHeight());
    const float sqW = std::min(plotH, std::max(120.f, availW * 0.25f));
    const float colW = std::max(120.f, (availW - 5 * gap - sqW) / 3.f);
    const ImVec2 sz(colW, plotH), sq(sqW, sqW);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap);
    ImGui::BeginGroup();
    captionFit(sqW, "Constellation (%zu cells)", on && t.siOk ? t.cells.size() : (size_t)0);   // the captions are never wider than their plots
    scatter("##dtmbcon", on && t.siOk ? t.cells : std::vector<cf32>(), sq, 1.7, pal::accent(0.40f));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    if (on && t.cirDb.size() > 1) captionFit(colW, "Impulse response (echo span %.2f us)", t.echoSpanUs); else captionFit(colW, "Impulse response from the PN header");
    cirPlot(t, on, sz);
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "C/N and MER (dB)");
    historyTwo("##dtmbcn", "dB", S.cn, S.mer, sz, false, 0, 0);
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Codewords lost (%%)");
    historyTwo("##dtmbloss", "%", S.loss, std::deque<float>(), sz, true, 0, 100);
    ImGui::EndGroup();
}

// ---------------------------------------------------------------- the Receiver tab

void receiver(App& a) {
    const DtmbTelemetry& t = a.rx.dtmb;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("DTMB (GB 20600) receiver: PN header search, carrier and clock loops, channel estimate from the PN, system information, LDPC and BCH.");
    ImGui::TextDisabled("Not supported: DTMB-A (APSK modulations and the extra LDPC rates), and channels other than 8 MHz at 7.56 Msymbol/s.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "Start the receiver with DTMB selected."); ImGui::PopTextWrapPos(); } return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(190 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", t.state == 2 ? "locked" : t.state == 1 ? "frames tracked" : "searching for PN headers");
    kv("frame header", "%s", t.header >= 0 ? kHeader[t.header] : "not known yet");
    if (t.header >= 0) {
        const int sym = kHeaderLen[t.header] + kBodyLen;
        kv("signal frame", "%d symbols, %.1f us (header %d + body %d)", sym, sym / kSymbolRate * 1e6, kHeaderLen[t.header], kBodyLen);
    }
    kv("PN phase", "%s", t.header < 0 ? "-" : t.header == 1 ? (t.phaseRotates ? "changes from frame to frame" : "fixed") : (t.phaseRotates ? "rotating in the super-frame" : "fixed"));
    kv("carriers", "%s", t.carriers == 3780 ? "3780 (multi-carrier)" : t.carriers == 1 ? "1 (single carrier)" : "not known yet");
    kv("system information", "%s", t.siOk ? "decoded" : "not decoded");
    if (t.siOk) kv("  information word", "%d of 3..24, correlation %.2f", t.siIndex, t.siScore);
    else if (t.siScore > 0) kv("  information word", "best correlation %.2f", t.siScore);
    if (t.siOk) {
        kv("modulation", "%s", kMap[t.mapping]);
        kv("LDPC code rate", "%s", kRate[t.rate]);
        kv("time interleaver", "%s (%s)", interleaverText(t.interleaver).c_str(), t.interleaver == 2 ? "M = 720, about 253 ms" : "M = 240, about 84 ms");
        kv("payload rate", "%.2f Mbit/s", t.netMbps);
    }
    ImGui::Spacing();
    kv("carrier offset", "%+.1f Hz", t.cfoHz);
    kv("sample clock", "%+.1f ppm corrected", t.clockPpm);
    kv("input level", "%.1f dBFS", t.levelDbfs);
    kv("C/N (PN header)", "%.1f dB", t.snrPnDb);
    if (t.siOk && t.merDb > 0) kv("MER (data carriers)", "%.1f dB", t.merDb); else kv("MER (data carriers)", "-");
    kv("echo span", "%.2f us", t.echoSpanUs);
    kv("strongest path", "%+.2f us from nominal", t.peakOffsetUs);
    kv("frames", "%llu, %.1f %% recently without a usable header", (unsigned long long)t.frames, t.frameLossPct);
    ImGui::Spacing();
    kv("LDPC codewords", "%llu ok, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("LDPC iterations", "%.1f per codeword (recent mean)", t.ldpcIter);
    kv("BCH corrections", "%llu codewords with one bit corrected", (unsigned long long)t.bchCorrected);
    kv("dropped codewords", "%llu (decoder behind)", (unsigned long long)t.cwDropped);
    kv("skipped codewords", "%llu (not tried after a run of failures)", (unsigned long long)t.cwSkipped);
    kv("transport stream", "%llu packets, %s", (unsigned long long)t.packets, t.tsLock ? "sync bytes good" : "no lock");
    kv("input rate", "%s", t.rateOk ? "ok" : "too low for DTMB (8 Msps or more)");
}

// ---------------------------------------------------------------- decoder and test signal

// the channel width: 8 MHz (China, Hong Kong, Macau) or 6 MHz (Cuba, in the American 6 MHz raster); the signal is the same, 7.56 or 5.67 Msym/s
void tuner(App& a, bool& retune) {
    (void)retune;
    ImGui::TextDisabled("Channel");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("DTMB channel width: 8 MHz in China, Hong Kong and Macau, 6 MHz in Cuba.\nA receiver set to the wrong width finds nothing.");
    sameLineIf(90 * gUi, 5 * gUi);
    static const char* const kW[] = {"8 MHz", "6 MHz (Cuba)"};
    int cur = a.dtmbBwMhz == 6 ? 1 : 0;
    ImGui::SetNextItemWidth(std::min(std::max(90 * gUi, ImGui::CalcTextSize(kW[1]).x + ImGui::GetFrameHeight() + 8 * gUi), ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("##dtmbbw", &cur, kW, 2)) {
        a.dtmbBwMhz = cur == 1 ? 6 : 8;
        applyBandwidth(a);
        savePrefs(a);
        if (a.engine.running()) startReceiver(a);   // the receiver takes the width when it starts
    }
}

void decoder(App& a, bool&) {
    loadState();
    ImGui::TextDisabled("LDPC threads"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(80 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    char cur[16]; if (S.threads == 0) snprintf(cur, sizeof cur, "auto"); else snprintf(cur, sizeof cur, "%d", S.threads);
    if (ImGui::BeginCombo("##dtmbthr", cur)) {
        for (int i = 0; i <= 4; i++) {
            char n[16]; if (i == 0) snprintf(n, sizeof n, "auto"); else snprintf(n, sizeof n, "%d", i);
            if (ImGui::Selectable(n, S.threads == i)) { S.threads = i; plat::prefs().setI("dtmbThreads", i); savePrefs(a); }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Threads of the LDPC decoder. Auto uses about half of the cores, at most 4. Takes effect at the next lock.");
}

template <size_t N> bool combo(const char* id, float w, int& v, const char* const (&names)[N]) {
    bool ch = false;
    ImGui::SetNextItemWidth(w * gUi);
    if (ImGui::BeginCombo(id, names[(size_t)v < N ? (size_t)v : 0])) {
        for (size_t i = 0; i < N; i++) if (ImGui::Selectable(names[i], (size_t)v == i)) { v = (int)i; ch = true; }
        ImGui::EndCombo();
    }
    return ch;
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    static const char* const hdr[] = {"PN945", "PN595", "PN420"};
    static const char* const mod[] = {"64QAM", "32QAM", "16QAM", "4QAM", "4QAM-NR"};
    static const char* const rate[] = {"0.6", "0.4", "0.8"};
    static const char* const il[] = {"mode 1", "mode 2"};
    static const char* const car[] = {"C=3780", "C=1"};
    static const char* const phase[] = {"PN rotating", "PN fixed"};
    const bool single = sc.modeOpt[4] == 1;
        ImGui::BeginDisabled(single);
    if (combo("##tdh", 70, sc.modeOpt[0], hdr)) changed = true;
    ImGui::EndDisabled();
    flowNext(6 * gUi);
    if (combo("##tdm", 80, sc.modeOpt[1], mod)) changed = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("32QAM and 4QAM-NR exist at code rate 0.8 only: that rate is used for them.");
    flowNext(6 * gUi);
    if (combo("##tdr", 56, sc.modeOpt[2], rate)) changed = true;
    flowNext(6 * gUi);
    if (combo("##tdi", 72, sc.modeOpt[3], il)) changed = true;
    flowNext(8 * gUi);
    if (combo("##tdc", 72, sc.modeOpt[4], car)) changed = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("C=1: single carrier. It always uses PN595 with a fixed phase.");
    flowNext(8 * gUi);
    ImGui::BeginDisabled(single);
    if (combo("##tdp", 96, sc.modeOpt[5], phase)) changed = true;
    ImGui::EndDisabled();
    flowNext(12 * gUi); ImGui::TextDisabled("SNR"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(90 * gUi);
    float snr = (float)sc.snrDb; if (ImGui::SliderFloat("##tdsnr", &snr, 0, 40, "%.0f dB")) { sc.snrDb = snr; changed = true; }
    flowNext(8 * gUi); ImGui::TextDisabled("CFO"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(100 * gUi);
    float cfo = (float)(sc.cfoHz / 1e3); if (ImGui::SliderFloat("##tdcfo", &cfo, -40, 40, "%.2f kHz")) { sc.cfoHz = cfo * 1e3; changed = true; }
    flowNext(8 * gUi); ImGui::TextDisabled("echo"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(90 * gUi);
    float ec = (float)sc.echoDb; if (ImGui::SliderFloat("##tdec", &ec, 0, 30, ec == 0 ? "off" : "-%.0f dB")) { sc.echoDb = ec; changed = true; }
    flowNext(8 * gUi); ImGui::TextDisabled("delay"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(90 * gUi);
    int dl = sc.echoDelay; if (ImGui::SliderInt("##tded", &dl, 5, 600, "%d smp")) { sc.echoDelay = dl; changed = true; }
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const bool on = live(a);
    const DtmbTelemetry& t = a.rx.dtmb;
    const float loss = recentLoss();
    out.push_back({"C/N  dB", "%.1f", on ? t.snrPnDb : 0, 0, 40, !on ? 0 : t.snrPnDb >= 20 ? 1 : t.snrPnDb >= 12 ? 2 : 3});
    out.push_back({"MER  dB", "%.1f", on && t.siOk ? t.merDb : 0, 0, 40, !on || !t.siOk ? 0 : t.merDb >= 20 ? 1 : t.merDb >= 12 ? 2 : 3});
    out.push_back({"CODEWORDS LOST  %", "%.1f", on && loss > 0 ? loss : 0, 0, 100, !on || !t.siOk ? 0 : loss <= 0 ? 1 : loss < 5 ? 2 : 3});
    out.push_back({"CFO  Hz", "%+.0f", on ? t.cfoHz : 0, -20000, 20000, 0});
    out.push_back({"LEVEL  dBFS", "%.1f", on ? t.levelDbfs : -120, -80, 0, 0});
    out.push_back({"PAYLOAD  Mbit/s", "%.2f", on && t.siOk ? t.netMbps : 0, 0, 32, 0});
}

} // namespace

void scanTabCommon(App& a);   // scan_outputs.cpp: the scan tab of the TV modes (it sets the DTMB flag from the family)
namespace { void scan(App& a) { scanTabCommon(a); } }

extern const ModeUi kDtmbUi;
const ModeUi kDtmbUi = {
    .sideTitle = "SERVICES",
    .receiver = receiver,
    .stream = true,
    .panels = panels,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .decoder = decoder,
    .synth = synth,
    .scan = scan,
    .tick = tick,
    .meters = meters,
};
