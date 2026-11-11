// DVB-SH (EN 302 583, OFDM / SH-A) screens: status bar, the analysis row (SNR history), the Receiver tab with the TPS parameters and the
// time interleaver, the tuner (channel width: automatic on top, a fixed width under Advanced; HP / LP stream) and the test signal options.
// The TV tab, services and the player are the shared ones (the decoded transport stream goes there like DVB-T's).
#include "app.h"
#include "dect2/dvbsh/dvbsh_engine.h"
#include <cmath>
#include <deque>

namespace {

struct State {
    uint64_t lastSeq = 0;
    bool wasRunning = false;
    int width = 0;          // 0 automatic, 1 8 MHz, 2 7, 3 6, 4 5, 5 1.7
    bool lp = false;
    int pushedWidth = -1;
    int pushedLp = -1;
    int waveform = 0;       // 0 automatic, 1 OFDM (SH-A), 2 TDM (SH-B)
    int pushedWaveform = -1;
    int gold = 0, pushedGold = -1;
    std::deque<float> snr;
};
State S;

bool live(const App& a) { return a.rx.standard == 747; }

const ImVec4 kDim(0.6f, 0.64f, 0.68f, 1), kGood(0.35f, 0.90f, 0.45f, 1), kWarn(0.95f, 0.60f, 0.25f, 1), kBad(0.95f, 0.45f, 0.30f, 1), kText(0.93f, 0.95f, 0.97f, 1);
const char* kNote = N_("DVB-SH (satellite and terrestrial mobile TV, S band 2170 - 2200 MHz, also UHF and L band): the OFDM form (SH-A) and the TDM satellite form (SH-B) "
                       "with the 3GPP2 turbo code and the long time interleaver. Written from ETSI EN 302 583 and tried only on generated test signals.");

void tick(App& a) {
    const bool run = a.engine.running();
    static const double widths[6] = {0, 8, 7, 6, 5, 1.7};
    if (run && (!S.wasRunning || S.pushedWidth != S.width)) { a.engine.dvbsh().setBandwidth(widths[S.width]); S.pushedWidth = S.width; }
    if (run && (!S.wasRunning || S.pushedLp != (int)S.lp)) { a.engine.dvbsh().setLowPriority(S.lp); S.pushedLp = S.lp; }
    if (run && (!S.wasRunning || S.pushedWaveform != S.waveform)) { a.engine.dvbsh().setWaveform(S.waveform); S.pushedWaveform = S.waveform; }
    if (run && (!S.wasRunning || S.pushedGold != S.gold)) { a.engine.dvbsh().setTdmGoldCode(S.gold); S.pushedGold = S.gold; }
    S.wasRunning = run;
    if (!live(a) || a.rx.seq == S.lastSeq) return;
    S.lastSeq = a.rx.seq;
    if (a.rx.dvbsh.locked) { S.snr.push_back(a.rx.dvbsh.snrDb); if (S.snr.size() > 600) S.snr.pop_front(); }
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const DvbshTelemetry& t = a.rx.dvbsh;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("OFDM", !on ? 0 : t.state >= 3 ? 1 : 0); flowNext(12 * gUi);
    lamp("TPS", !on ? 0 : t.locked ? 1 : t.state >= 3 ? 2 : 0); flowNext(12 * gUi);
    lamp("Turbo", !on ? 0 : t.blocksOk + t.blocksBad == 0 ? 0 : t.blocksBad * 10 > t.blocksOk + t.blocksBad ? 3 : t.blocksBad ? 2 : 1); flowNext(12 * gUi);
    lamp("TS", !run ? 0 : a.ts.services.empty() ? 0 : 1, (int)Ic::Layers); flowNext(12 * gUi);
    const PlayerStats ps = a.engine.player().stats();
    const bool pl = run && ps.active;
    lamp("Video", !pl || !ps.hasVideo ? 0 : (ps.shown > 0 && ps.videoQueue > 2) ? 1 : 2, (int)Ic::Tv); flowNext(12 * gUi);
    lamp("Audio", !pl || !ps.hasAudio ? 0 : (ps.audioBufferMs > 150) ? 1 : 2, (int)Ic::Speaker); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val, ImVec4 col = kText) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(col, "%s", val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[96];
    if (!run) ro(tr("State"), tr("stopped"), kDim);
    else if (a.engine.radioLost()) ro(tr("State"), tr("radio disconnected"), kBad);
    else if (!on) ro(tr("State"), tr("starting"), kDim);
    else if (!a.rx.rateOk) ro(tr("State"), tr("sample rate too low"), kBad);
    else if (t.state >= 4) ro(tr("State"), tr("Locked"), kGood);
    else if (t.locked) ro(tr("State"), tr("TPS decoded"), ImVec4(0.95f, 0.85f, 0.35f, 1));
    else if (t.state >= 3) ro(tr("State"), tr("OFDM found"), ImVec4(0.95f, 0.85f, 0.35f, 1));
    else ro(tr("State"), tr("Searching"), kDim);
    if (on && t.state >= 3) {
        snprintf(b, sizeof b, "%.1f MHz", t.bwMhz); ro(tr("Channel"), b);
        snprintf(b, sizeof b, "%.1f dB", t.snrDb); ro("SNR", b);
        snprintf(b, sizeof b, "%+.0f Hz", t.cfoHz); ro("CFO", b);
    }
    if (on && t.locked) {
        snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro(tr("Codewords ok/bad"), b, t.blocksBad ? kWarn : kText);
    }
    if (run) { snprintf(b, sizeof b, "%.3f Msps", a.engine.sampleRate() / 1e6); ro("fs", b); }
    if (run) { const SampleLoss l = a.engine.sampleLoss(); ro(tr("dropped"), lossText(l), lossColour(l)); lossTooltip(l); }
    flowBreak();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "DVB-SH";
    if (!live(a)) return;
    l2 = dect2::dvbshSummary(a.rx.dvbsh);
}

void panels(App& a) {
    const float availW = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi;
    const float plotH = std::max(90.f, availH - 26.f - ImGui::GetFrameHeight());
    const float colW = std::max(120.f, availW - 2 * gap);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap);
    ImGui::BeginGroup();
    captionFit(colW, "%s", tr("SNR from the continual pilots (dB)"));
    if (plt::BeginPlot("##dvbshsnr", ImVec2(colW, plotH), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes(tr("reports"), "dB", 0, plt::AxisFlags_AutoFit);
        plt::SetupAxisLimits(plt::X1, 0, 600, plt::Cond_Always);
        if (!S.snr.empty()) { std::vector<float> v(S.snr.begin(), S.snr.end()); plt::Spec sp; sp.LineColor = pal::accent(); sp.LineWeight = 1.4f; plt::PlotLine("a", v.data(), (int)v.size(), sp); }
        plt::EndPlot();
    }
    ImGui::EndGroup();
}

void receiver(App& a) {
    const DvbshTelemetry& t = a.rx.dvbsh;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("DVB-SH OFDM (SH-A): 1K, 2K, 4K or 8K carriers, QPSK or 16-QAM (also hierarchical), the 3GPP2 turbo code at rates 1/5 to 2/3, a bit "
                                 "interleaver and a 48-branch time interleaver that can spread each codeword over seconds. The receiver finds the FFT size and guard from "
                                 "the cyclic prefix, the carrier offset from the pilots, the code rate and interleaver from the TPS, and checks the CRC-16 of every packet."));
    ImGui::TextColored(kWarn, "%s", tr(kNote));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("Start the receiver with DVB-SH selected.")); return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(190 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    const auto& p = t.params;
    kv(tr("channel width"), "%.1f MHz", t.bwMhz);
    kv(tr("carrier offset"), "%+.1f Hz", t.cfoHz);
    kv("SNR", "%.1f dB", t.snrDb);
    if (t.tdm) {
        kv(tr("waveform"), "%s", tr("TDM (SH-B)"));
        if (t.tdmMod >= 0) kv(tr("TDM"), "%s, %.4f Msym/s, clock %+.1f ppm, %llu frames", dect2::dvbsh::tdmModName(t.tdmMod), t.symbolRate / 1e6, t.clockPpm, (unsigned long long)t.tdmFrames);
        if (t.locked) {
            kv(tr("code rate"), "%s (pattern %d), frame width %d CU", dect2::dvbsh::rateName(t.sf.pat), t.sf.pat, t.sf.frameWidthCu);
            kv(tr("time interleaver"), "common multiplier %d, late taps %d, slices %d, slice distance %d, non-late increment %d: %.2f s%s", t.sf.ti.commonMultiplier, t.sf.ti.lateTaps,
               t.sf.ti.slices, t.sf.ti.sliceDistance, t.sf.ti.nonLateIncrement, t.tiSeconds, t.tiTooLarge ? " (more memory than allowed)" : "");
            if (t.sf.llPresent) kv(tr("low latency"), "%s", tr("signalled; the low latency bursts (annex B) are not separated"));
        }
    } else if (t.locked) {
        kv(tr("OFDM"), "%s, guard 1/%d, %s%s", dect2::dvbsh::modeName(p.mode), 32 >> p.guard, p.mod ? "16-QAM" : "QPSK", p.hier ? (p.hier == 1 ? ", hierarchical alpha 1" : p.hier == 2 ? ", hierarchical alpha 2" : ", hierarchical alpha 4") : "");
        kv(tr("code rate"), "HP %s (pattern %d)%s", dect2::dvbsh::rateName(p.patHp), p.patHp, p.hier ? (std::string(", LP ") + dect2::dvbsh::rateName(p.patLp)).c_str() : "");
        kv(tr("time interleaver"), "common multiplier %d, late taps %d, slices %d, slice distance %d, non-late increment %d: %.2f s%s", p.ti.commonMultiplier, p.ti.lateTaps,
           p.ti.slices, p.ti.sliceDistance, p.ti.nonLateIncrement, t.tiSeconds, t.tiTooLarge ? " (more memory than allowed)" : "");
        kv(tr("cell id"), "0x%04X", (unsigned)p.cellId);
    }
    if (!t.tdm) kv(tr("TPS frames"), "%llu", (unsigned long long)t.tpsFrames);
    kv(tr("codewords"), "%llu good, %llu bad, %.2f turbo iterations", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.avgIterations);
    kv(tr("packets"), "%llu good, %llu with CRC errors", (unsigned long long)t.packetsOk, (unsigned long long)t.packetsBad);
    kv(tr("input rate"), "%s", a.rx.rateOk ? tr("ok") : tr("too low for DVB-SH"));
}

template <size_t N> bool combo(const char* id, float w, int& v, const char* const (&names)[N]) {
    bool ch = false;
    ImGui::SetNextItemWidth(std::min(w * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo(id, tr(names[(size_t)v < N ? (size_t)v : 0]))) {
        for (size_t i = 0; i < N; i++) if (ImGui::Selectable(tr(names[i]), (size_t)v == i)) { v = (int)i; ch = true; }
        ImGui::EndCombo();
    }
    return ch;
}

void tuner(App& a, bool& retune) {
    (void)retune;
    static const char* const widths[] = {N_("Automatic"), "8 MHz", "7 MHz", "6 MHz", "5 MHz", "1.7 MHz"};
    ImGui::TextDisabled("%s", tr("Channel width"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Automatic tries 5, 8, 7, 6 and 1.7 MHz until OFDM symbols are found."));
    sameLineIf(90 * gUi, 5 * gUi);
    static const char* const waves[] = {N_("OFDM and TDM"), N_("OFDM (SH-A)"), N_("TDM (SH-B)")};
    if (ImGui::CollapsingHeader(trw("Advanced"))) {
        combo("##dvbshbw", 120, S.width, widths);
        flowNext(8 * gUi);
        combo("##dvbshwave", 130, S.waveform, waves);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Automatic tries each channel width as OFDM and then as TDM (the satellite form, whose symbol rate is found from the table of the standard)."));
        flowNext(8 * gUi);
        ImGui::TextDisabled("%s", tr("TDM scrambling code"));
        ImGui::SameLine(0, 4 * gUi);
        ImGui::SetNextItemWidth(80 * gUi);
        ImGui::InputInt("##dvbshgold", &S.gold, 0);
        S.gold = std::max(0, std::min(255, S.gold));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The physical layer scrambling sequence number n of the TDM waveform; 0 is the default of a single-beam system."));
    } else {
        if (S.width != 0) { ImGui::SameLine(); ImGui::TextDisabled("%s", widths[S.width]); }
        if (S.waveform != 0) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr(waves[S.waveform])); }
    }
    flowNext(8 * gUi);
    ImGui::Checkbox(tr("Low-priority stream"), &S.lp);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Hierarchical 16-QAM carries two streams: the robust high-priority one (default) and the low-priority one."));
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(kWarn, "%s", tr(kNote));
    ImGui::PopTextWrapPos();
    (void)a;
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    static const char* const bw[] = {"5 MHz", "8 MHz", "7 MHz", "6 MHz", "1.7 MHz"};
    static const char* const fft[] = {"2K", "8K", "4K", "1K"};
    static const char* const mod[] = {"QPSK", "16-QAM"};
    static const char* const rate[] = {"1/3", "1/5", "2/9", "1/4", "2/7", "1/3", "1/3 c", "2/5", "2/5 c", "1/2", "1/2 c", "2/3", "2/3 c"};
    if (combo("##shbw", 90, sc.modeOpt[0], bw)) changed = true;
    flowNext(8 * gUi);
    if (combo("##shfft", 60, sc.modeOpt[1], fft)) changed = true;
    flowNext(8 * gUi);
    if (combo("##shmod", 80, sc.modeOpt[2], mod)) changed = true;
    flowNext(8 * gUi);
    if (combo("##shrate", 70, sc.modeOpt[3], rate)) changed = true;
    static const char* const form[] = {"OFDM", "TDM QPSK", "TDM 8PSK", "TDM 16APSK"};
    flowNext(8 * gUi);
    if (combo("##shform", 110, sc.modeOpt[5], form)) changed = true;
    flowNext(12 * gUi); ImGui::TextDisabled("SNR"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(90 * gUi);
    float snr = (float)sc.snrDb; if (ImGui::SliderFloat("##shsnr", &snr, 0, 40, "%.0f dB")) { sc.snrDb = snr; changed = true; }
    flowNext(8 * gUi); ImGui::TextDisabled("CFO"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(110 * gUi);
    float cfo = (float)(sc.cfoHz / 1e3); if (ImGui::SliderFloat("##shcfo", &cfo, -100, 100, "%.0f kHz")) { sc.cfoHz = cfo * 1e3; changed = true; }
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const bool on = live(a);
    const DvbshTelemetry& t = a.rx.dvbsh;
    out.push_back({"SNR  dB", "%.1f", on && t.locked ? t.snrDb : 0, 0, 30, !on || !t.locked ? 0 : t.snrDb >= 8 ? 1 : t.snrDb >= 3 ? 2 : 3});
    out.push_back({"BAD  codewords", "%.0f", on ? (double)t.blocksBad : 0, 0, 1000, !on || t.blocksOk + t.blocksBad == 0 ? 0 : t.blocksBad == 0 ? 1 : 3});
    out.push_back({"CFO  Hz", "%+.0f", on ? t.cfoHz : 0, -200000, 200000, 0});
}

} // namespace

extern const ModeUi kDvbshUi;
const ModeUi kDvbshUi = {
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
