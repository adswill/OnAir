// DVB-S/S2 screens: lock lamps and readouts, constellation and quality plots, the receiver tab, symbol rate and standard controls,
// and the options of the test signal.
#include "app.h"
#include "dect2/dvbs_s2.h"
#include <cfloat>
#include <cmath>
#include <deque>

namespace {

struct State {
    bool loaded = false, wasRunning = false;
    double srMsym = 0;                  // manual symbol rate in Msym/s, 0 = find it from the spectrum
    int stdHint = 0;                    // 0 automatic, 1 DVB-S only, 2 DVB-S2 only
    double pushedSr = -1; int pushedStd = -1;
    uint64_t lastSeq = 0;
    std::deque<float> snr;              // Es/N0 of the reports, newest last
};
State S;

constexpr size_t kHist = 240;           // reports kept: about a minute

bool live(const App& a) { return a.engine.running() && a.rx.standard == 7; }

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.srMsym = std::max(0.0, d.getD("dvbsSr", 0.0));
    S.stdHint = (int)std::max(0L, std::min(2L, d.getI("dvbsStd", 0)));
}

void saveState(App& a) {
    plat::Prefs& d = plat::prefs();
    d.setD("dvbsSr", S.srMsym);
    d.setI("dvbsStd", S.stdHint);
    savePrefs(a);
}

const char* stdName(int s) { return s == 1 ? "DVB-S" : s == 2 ? "DVB-S2" : s == 3 ? "DVB-S2X" : "-"; }

std::string fmt(const char* f, double v) { char b[48]; snprintf(b, sizeof b, f, v); return b; }
std::string cnt(uint64_t v) { return std::to_string((unsigned long long)v); }

// How far the Es/N0 is above the quasi error free point of table 13 of EN 302 307-1 (DVB-S2) or tables 20a and 20c of EN 302 307-2 (DVB-S2X).
// False when it cannot be said.
bool margin(const DvbsTelemetry& t, double& m) {
    if (t.standard < 2 || t.modulation < 0 || !t.lockCarrier) return false;
    const int r = dvbs::s2RateFromName(t.modulation, t.codeRate.c_str(), t.frameSize == 2);
    if (r < 0) return false;
    const double q = dvbs::s2QefEsN0(t.modulation, r, t.frameSize == 2);
    if (q >= 90) return false;
    m = t.snrDb - q;
    return true;
}

const ImVec4 kDim(0.62f, 0.65f, 0.68f, 1);

// A lamp with a tooltip of its own (labels that other modes use, like "Frame", have theirs: this one wins)
void lampTip(const char* label, int state, const char* tip) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    lamp(label, state);
    if (ImGui::IsMouseHoveringRect(p, ImVec2(ImGui::GetItemRectMax().x, p.y + ImGui::GetTextLineHeight())) && ImGui::IsWindowHovered()) ImGui::SetTooltip("%s", tip);
}

struct Stages { int carrier, timing, frame, fec, ts; };
Stages stages(const App& a) {
    const DvbsTelemetry& t = a.rx.dvbs;
    const bool on = live(a);
    auto st = [&](bool ok, bool searching) { return !on ? 0 : ok ? 1 : searching ? 2 : 0; };
    Stages s;
    s.carrier = st(t.lockCarrier, t.lockSpectrum);
    s.timing = st(t.lockTiming, t.lockSpectrum);
    s.frame = st(t.lockFrame, t.lockCarrier && t.lockTiming);
    s.fec = st(t.lockFec, t.lockFrame);
    s.ts = st(t.tsLock, t.lockFec);
    return s;
}

const char* kTipCarrier = "Carrier: the spectrum shows a carrier and its phase is tracked. Amber: carrier found, still locking.";
const char* kTipTiming = "Symbol timing: the clock recovery loop has settled on the symbol rate.";
const char* kTipFrame = "Framing: DVB-S2 PLHEADERs are found where they should be; DVB-S: the packet sync bytes are found.";
const char* kTipFec = "Error correction: LDPC and BCH (DVB-S2) or Viterbi and Reed-Solomon (DVB-S) deliver good blocks.";
const char* kTipTs = "Transport stream: packets are flowing to the demultiplexer.";

// ---------------------------------------------------------------------------------------------- tick

void tick(App& a) {
    loadState();
    const bool run = a.engine.running();
    if (run && (!S.wasRunning || S.pushedSr != S.srMsym || S.pushedStd != S.stdHint)) {
        a.engine.dvbs().setSymbolRate(S.srMsym * 1e6);
        a.engine.dvbs().setStandardHint(S.stdHint);
        S.pushedSr = S.srMsym; S.pushedStd = S.stdHint;
    }
    S.wasRunning = run;
    if (live(a) && a.rx.seq != S.lastSeq) {
        S.lastSeq = a.rx.seq;
        const DvbsTelemetry& t = a.rx.dvbs;
        if (t.lockCarrier) { S.snr.push_back(t.snrDb); if (S.snr.size() > kHist) S.snr.pop_front(); }
    }
}

// ---------------------------------------------------------------------------------------------- status bar and top bar

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const DvbsTelemetry& t = a.rx.dvbs;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    const Stages s = stages(a);
    const float gap = 12 * gUi;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(gap);
    lampTip("Carrier", s.carrier, kTipCarrier); flowNext(gap);
    lampTip("Timing", s.timing, kTipTiming); flowNext(gap);
    lampTip("Framing", s.frame, kTipFrame); flowNext(gap);
    lampTip("FEC", s.fec, kTipFec); flowNext(gap);
    lampTip("TS lock", s.ts, kTipTs); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val, ImVec4 col = ImVec4(0.93f, 0.95f, 0.97f, 1)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(col, "%s", val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    if (!on) { ro("State", run ? "starting" : "stopped", kDim); return; }
    if (t.tsLock) ro("State", "Locked", pal::okGreen());
    else if (t.lockSpectrum) ro("State", "Locking", pal::accent());
    else ro("State", "Searching", kDim);
    if (t.standard) ro("Standard", stdName(t.standard));
    if (t.symbolRate > 0) ro("Symbol rate", fmt("%.3f Msym/s", t.symbolRate / 1e6) + (t.symbolRateManual ? " (set)" : ""));
    if (t.standard && !t.modulationName.empty()) {
        std::string m = t.modulationName + " " + t.codeRate;
        if (t.standard >= 2 && t.frameSize == 2) m += " short";
        if (t.pilots) m += " pilots";
        if (t.vcm) m += " VCM";
        ro("Mode", m);
    }
    flowBreak();
    if (t.lockCarrier) {
        ro("Es/N0", fmt("%.1f dB", t.snrDb));
        double m;
        if (margin(t, m)) ro("Margin", fmt("%+.1f dB", m), m >= 1.0 ? pal::okGreen() : m >= 0 ? pal::accent() : pal::badRed());
        ro("Offset", fmt("%+.0f kHz", t.cfoHz / 1e3));
    }
    if (t.standard >= 2) {
        ro("Frames", cnt(t.blocksOk) + " ok  " + cnt(t.blocksBad) + " bad", t.blocksBad ? ImVec4(0.95f, 0.55f, 0.35f, 1) : ImVec4(0.93f, 0.95f, 0.97f, 1));
    } else if (t.standard == 1) {
        ro("RS", cnt(t.rsClean + t.rsCorrected) + " ok  " + cnt(t.rsFailed) + " failed", t.rsFailed ? ImVec4(0.95f, 0.55f, 0.35f, 1) : ImVec4(0.93f, 0.95f, 0.97f, 1));
    }
    if (t.packets) ro("Packets", cnt(t.packets));
    if (t.netBitrate > 0) ro("TS", fmt("%.2f Mbit/s", t.netBitrate / 1e6));
    if (run) { const SampleLoss l = a.engine.sampleLoss(); ro("dropped", lossText(l), lossColour(l)); lossTooltip(l); }
}

void summary(const App& a, std::string& l1, std::string& l2) {
    const DvbsTelemetry& t = a.rx.dvbs;
    l1 = "DVB-S/S2";
    if (!live(a)) return;
    if (t.standard) {
        l1 = stdName(t.standard);
        if (!t.modulationName.empty()) l1 += " " + t.modulationName + " " + t.codeRate;
    }
    char b[96];
    if (t.tsLock) snprintf(b, sizeof b, "locked  %.2f Msym/s  Es/N0 %.1f dB", t.symbolRate / 1e6, t.snrDb);
    else if (t.lockSpectrum) snprintf(b, sizeof b, "locking  %.2f Msym/s", t.symbolRate / 1e6);
    else snprintf(b, sizeof b, "searching for a carrier");
    l2 = b;
}

// ---------------------------------------------------------------------------------------------- analysis row

// One line plot with the newest sample at the right edge. xspan = how many samples the axis shows.
void linePlot(const char* id, const std::vector<float>& v, ImVec2 size, const char* xlabel, const char* ylabel, int xspan) {
    if (plt::BeginPlot(id, size, plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes(xlabel, ylabel, 0, 0);
        plt::SetupAxisLimits(plt::X1, 0, xspan, plt::Cond_Always);
        float lo = 0, hi = 30;   // the range of the data with a little room, at least 4 dB high
        if (!v.empty()) {
            lo = *std::min_element(v.begin(), v.end()); hi = *std::max_element(v.begin(), v.end());
            const float mid = 0.5f * (lo + hi), half = std::max(2.f, 0.5f * (hi - lo) + 0.5f);
            lo = mid - half; hi = mid + half;
        }
        plt::SetupAxisLimits(plt::Y1, lo, hi, plt::Cond_Always);
        if (!v.empty()) {
            const int n = (int)v.size();
            std::vector<float> x((size_t)n);
            for (int i = 0; i < n; i++) x[(size_t)i] = (float)(xspan - n + i);
            plt::Spec sp; sp.LineColor = pal::accent(); sp.LineWeight = 1.2f;
            plt::PlotLine("h", x.data(), v.data(), n, sp);
        }
        plt::EndPlot();
    }
}

void panels(App& a) {
    const DvbsTelemetry& t = a.rx.dvbs;
    const bool on = live(a);
    const float availW = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi;
    const float plotH = std::max(70.f, availH - 26.f - ImGui::GetFrameHeight());
    const float sqW = std::min(plotH, std::max(110.f, availW * 0.22f));
    const bool wide = availW > 760 * gUi;                  // the per-frame MER plot only when there is room for it
    const float readW = std::max(150.f * gUi, std::min(250.f * gUi, availW * 0.26f));
    const int nPlots = wide ? 2 : 1;
    const float colW = std::max(100.f, (availW - (nPlots + 2) * gap - sqW - readW) / nPlots);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    ImGui::BeginGroup();
    captionFit(sqW, "Symbols (%zu)", on ? t.cells.size() : (size_t)0);
    scatter("##dvbscst", on ? t.cells : std::vector<cf32>(), ImVec2(sqW, plotH), 1.6, pal::accent(0.45f));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Es/N0 (dB), last minute");
    linePlot("##dvbssnr", std::vector<float>(S.snr.begin(), S.snr.end()), ImVec2(colW, plotH), "reports (~4/s)", "dB", (int)kHist);
    ImGui::EndGroup();
    if (wide) {
        ImGui::SameLine(0, gap);
        ImGui::BeginGroup();
        captionFit(colW, "MER per frame (dB)");
        const int xs = (int)std::max<size_t>(60, t.merHistory.size());
        std::vector<float> mer;   // the first entries, before the carrier loop has settled, are zero
        if (on) for (float m : t.merHistory) if (m > 0 || !mer.empty()) mer.push_back(m);
        linePlot("##dvbsmer", mer, ImVec2(colW, plotH), "frames", "dB", xs);
        ImGui::EndGroup();
    }
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(readW, "Lock stages and counters");
    ImGui::BeginChild("##dvbsrd", ImVec2(readW, plotH + ImGui::GetFrameHeight() * 0.5f));   // scrolls when the row is short
    const Stages s = stages(a);
    const float g2 = 8 * gUi;
    lampTip("Carrier", s.carrier, kTipCarrier); flowNext(g2);   // the lamps wrap in a narrow readout
    lampTip("Timing", s.timing, kTipTiming); flowNext(g2);
    lampTip("Framing", s.frame, kTipFrame);
    flowEnd();
    lampTip("FEC", s.fec, kTipFec); flowNext(g2);
    lampTip("TS lock", s.ts, kTipTs);
    flowEnd();
    auto kv = [&](const char* k, const std::string& v) {
        ImGui::TextDisabled("%s", k); kvColumn(84 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
    };
    if (on) {
        if (t.standard >= 2) {
            kv("frames", cnt(t.blocksOk) + " / " + cnt(t.blocksBad));
            kv("LDPC iter", fmt("%.1f", t.ldpcIterAvg));
        } else if (t.standard == 1) {
            kv("RS fixed", cnt(t.rsCorrected));
            kv("RS failed", cnt(t.rsFailed));
        }
        if (t.preFecBer >= 0) { char b[24]; snprintf(b, sizeof b, "%.1e", t.preFecBer); kv("pre-FEC BER", b); }
        kv("packets", cnt(t.packets));
        kv("bad packets", cnt(t.packetsBad));
    }
    ImGui::EndChild();
    ImGui::EndGroup();
}

// ---------------------------------------------------------------------------------------------- Receiver tab

void receiver(App& a) {
    const DvbsTelemetry& t = a.rx.dvbs;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    struct KV { std::string k, v; };
    std::vector<KV> L, R;
    auto head = [](std::vector<KV>& v, const char* h) { if (!v.empty()) v.push_back({"", ""}); v.push_back({h, "#"}); };
    char b[96];
    const char* stateTxt = t.tsLock ? "locked, transport stream flowing" : t.lockFec ? "error correction locks" : t.lockFrame ? "frames found"
                         : t.lockCarrier ? "carrier tracked" : t.lockSpectrum ? "carrier found" : "searching";

    head(L, "SIGNAL");
    L.push_back({"state", stateTxt});
    L.push_back({"standard", stdName(t.standard)});
    L.push_back({"modulation", t.modulation >= 0 ? t.modulationName : "-"});
    L.push_back({"code rate", t.codeRate.empty() ? "-" : t.codeRate});
    L.push_back({"carrier offset", t.lockSpectrum ? fmt("%+.1f kHz", t.cfoHz / 1e3) : "-"});
    L.push_back({"spectrum", t.inverted ? "inverted (high-side LNB oscillator)" : t.lockSpectrum ? "normal" : "-"});
    if (t.carrierHiHz > t.carrierLoHz) { snprintf(b, sizeof b, "%+.2f to %+.2f MHz  (%.2f MHz wide)", t.carrierLoHz / 1e6, t.carrierHiHz / 1e6, (t.carrierHiHz - t.carrierLoHz) / 1e6); L.push_back({"carrier -3 dB", b}); }
    L.push_back({"spectrum SNR", t.lockSpectrum ? fmt("%.1f dB", t.spectrumSnrDb) : "-"});
    if (t.lockSpectrum) L.push_back({"spectrum fit", fmt("%.2f", t.spectrumFitRms) + (t.spectrumFitRms < 0.3f ? "  (looks like a carrier)" : t.spectrumFitRms < 1.f ? "" : "  (does not look like a carrier)")});

    head(L, "SYMBOL RATE");
    L.push_back({"in use", t.symbolRate > 0 ? fmt("%.4f Msym/s", t.symbolRate / 1e6) : "-"});
    L.push_back({"source", t.symbolRateManual ? "set by hand" : "from the spectrum"});
    L.push_back({"from spectrum", t.symbolRateSpectrum > 0 ? fmt("%.4f Msym/s", t.symbolRateSpectrum / 1e6) : "-"});
    L.push_back({"timing loop", t.symbolRateLoop > 0 ? fmt("%.4f Msym/s", t.symbolRateLoop / 1e6) : "-"});
    if (t.symbolRate > 0 && t.symbolRateLoop > 0) L.push_back({"loop vs used", fmt("%+.0f ppm", (t.symbolRateLoop / t.symbolRate - 1) * 1e6)});
    {
        const char* src = t.rollOffSource == 2 ? "signalled" : t.rollOffSource == 1 ? "measured" : "assumed";
        L.push_back({"roll-off", t.rollOff > 0 ? fmt("%.2f", t.rollOff) + " (" + src + ")" : "-"});
    }

    head(L, "DVB-S2");
    if (t.standard >= 2) {
        L.push_back({"MODCOD", t.modcod > 0 ? std::to_string(t.modcod) : t.modcod == 0 ? "0 (dummy frame)" : "-"});
        L.push_back({"frame", t.frameSize == 1 ? "normal, 64800 bits" : t.frameSize == 2 ? "short, 16200 bits" : "-"});
        L.push_back({"pilots", t.pilots ? "on" : "off"});
        L.push_back({"VCM / ACM", t.vcm ? "yes, the MODCOD changes" : "no"});
        L.push_back({"stream (ISI)", t.isi >= 0 ? std::to_string(t.isi) : "single stream"});
        L.push_back({"PL scrambling", "code " + std::to_string(t.plScramblingCode)});
        L.push_back({"PLFRAMEs seen", cnt(t.framesSeen) + " (" + cnt(t.framesDummy) + " dummy)"});
    } else L.push_back({"", "not a DVB-S2 signal"});

    head(R, "QUALITY");
    R.push_back({"Es/N0", t.lockCarrier ? fmt("%.1f dB", t.snrDb) : "-"});
    R.push_back({"MER", t.lockCarrier ? fmt("%.1f dB", t.merDb) : "-"});
    {
        double m;
        if (margin(t, m)) {
            const double q = t.snrDb - m;
            snprintf(b, sizeof b, "%+.1f dB  (table 13: %.1f dB)", m, q);
            R.push_back({"margin to QEF", b});
        } else R.push_back({"margin to QEF", t.standard == 1 ? "DVB-S2 only" : "-"});
    }
    if (t.preFecBer >= 0) { snprintf(b, sizeof b, "%.2e", t.preFecBer); R.push_back({"pre-FEC BER", b}); } else R.push_back({"pre-FEC BER", "-"});
    if (t.standard >= 2) R.push_back({"LDPC iterations", fmt("%.1f", t.ldpcIterAvg) + " per frame"});

    head(R, "ERROR CORRECTION");
    if (t.standard == 1) {
        R.push_back({"RS clean", cnt(t.rsClean)});
        R.push_back({"RS corrected", cnt(t.rsCorrected)});
        R.push_back({"RS failed", cnt(t.rsFailed)});
    } else {
        R.push_back({"frames ok", cnt(t.blocksOk)});
        R.push_back({"frames bad", cnt(t.blocksBad)});
        R.push_back({"BCH ok / bad", cnt(t.bchOk) + " / " + cnt(t.bchBad)});
    }

    head(R, "TRANSPORT STREAM");
    R.push_back({"lock", t.tsLock ? "yes, for " + fmt("%.0f s", t.secsSinceLock) : "no"});
    R.push_back({"packets", cnt(t.packets)});
    R.push_back({"with error flag", cnt(t.packetsBad)});
    R.push_back({"bit rate", t.netBitrate > 0 ? fmt("%.3f Mbit/s", t.netBitrate / 1e6) : "-"});
    if (t.standard >= 2) {
        R.push_back({"user packet CRC", cnt(t.crcErrors) + " errors"});
        R.push_back({"generic stream", cnt(t.gseFrames) + " frames (not converted)"});
    }

    ImGui::BeginChild("##dvbsrx", ImVec2(0, 0), 0);
    const size_t rows = std::max(L.size(), R.size());
    if (ImGui::BeginTable("##dvbsrxt", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("k1", ImGuiTableColumnFlags_WidthFixed, 112 * gUi);
        ImGui::TableSetupColumn("v1", ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn("k2", ImGuiTableColumnFlags_WidthFixed, 128 * gUi);
        ImGui::TableSetupColumn("v2", ImGuiTableColumnFlags_WidthStretch, 1.f);
        auto cell = [&](const std::vector<KV>& v, size_t i) {
            ImGui::TableNextColumn();
            if (i < v.size() && v[i].v == "#") { ImGui::TextColored(pal::accent(), "%s", v[i].k.c_str()); ImGui::TableNextColumn(); return; }
            if (i < v.size()) ImGui::TextDisabled("%s", v[i].k.c_str());
            ImGui::TableNextColumn();
            if (i < v.size() && !v[i].v.empty()) { ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v[i].v.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont(); }   // wraps in a narrow column
        };
        for (size_t i = 0; i < rows; i++) { ImGui::TableNextRow(); cell(L, i); cell(R, i); }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextColored(pal::accent(), "NOTES");
    ImGui::PushTextWrapPos(0);
    if (!t.signalNote.empty()) ImGui::TextUnformatted(t.signalNote.c_str());
    ImGui::TextDisabled("DVB-S2X: the MODCODs of EN 302 307-2 table 17a are decoded; VL-SNR frames, superframes and bundled channels are only followed, not decoded.");
    if (t.standard == 3) ImGui::TextUnformatted("A DVB-S2X signal was recognised.");
    ImGui::TextDisabled("Margin is measured against the ideal-demodulator Es/N0 of EN 302 307-1 table 13 (DVB-S2X: EN 302 307-2 tables 20a and 20c, 50 LDPC iterations): a real receiver needs a little more.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------------------------- tuner

void tuner(App& a, bool& retune) {
    (void)retune;   // the symbol rate and the standard are handed to the running receiver, no retune needed
    loadState();
    const double fs = a.engine.running() ? a.engine.sampleRate() : dvbsTuning().sampleRate;
    const double maxMsym = std::floor(dvbsMaxSymbolRate(fs) / 1e3) / 1e3;
    bool ch = false;
    ImGui::TextDisabled("Symbol rate");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Auto measures the width of the carrier in the spectrum. Set it by hand for weak or narrow carriers, or when Auto is a few percent off.\n\n"
                          "A HackRF sees about 17 MHz of band at most (20 Msps), which is a carrier of up to about %.1f Msym/s.\n"
                          "At %.0f Msps the limit is %.1f Msym/s. Wide transponders (27.5 Msym/s and more) do not fit: use a recording or a wider radio.",
                          dvbsMaxSymbolRate(20e6) / 1e6, fs / 1e6, dvbsMaxSymbolRate(fs) / 1e6);
    sameLineIf(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("Auto").x, 6 * gUi);
    bool autoSr = S.srMsym <= 0;
    if (ImGui::Checkbox("Auto##dvbssr", &autoSr)) {
        if (autoSr) S.srMsym = 0;
        else {
            const DvbsTelemetry& t = a.rx.dvbs;
            S.srMsym = live(a) && t.symbolRate > 0 ? std::round(t.symbolRate / 1e3) / 1e3 : 5.0;
        }
        ch = true;
    }
    sameLineIf(104 * gUi, 6 * gUi);
    ImGui::BeginDisabled(autoSr);
    ImGui::SetNextItemWidth(std::min(104 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    double v = autoSr ? (live(a) ? a.rx.dvbs.symbolRate / 1e6 : 0.0) : S.srMsym;
    if (ImGui::InputDouble("##dvbssrv", &v, 0, 0, autoSr ? "%.3f Msym/s" : "%.3f Msym/s")) {
        if (!autoSr) { S.srMsym = std::max(0.05, std::min(maxMsym, v)); ch = true; }
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Standard");
    ImGui::SameLine(0, 6 * gUi);
    static const char* names[] = {"Auto", "DVB-S only", "DVB-S2 only"};
    ImGui::SetNextItemWidth(std::min(120 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    if (ImGui::BeginCombo("##dvbsstd", names[S.stdHint])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(names[i], S.stdHint == i)) { S.stdHint = i; ch = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Auto tries DVB-S2 and DVB-S. Choose one to skip the other search on a weak signal.");
    if (ch) saveState(a);
}

// ---------------------------------------------------------------------------------------------- test signal

// Rates the combination offers, in the generator's numbering (S2: index into s2RateName; S: 0..4 = 1/2 2/3 3/4 5/6 7/8; S2X: the position among
// the MODCODs of the modulation and frame size, dvbs_gen.h)
const char* kS1Rates[] = {"1/2", "2/3", "3/4", "5/6", "7/8"};
const char* kRoll[] = {"0.35", "0.25", "0.20", "0.15", "0.10", "0.05"};

bool isS2x(const SynthConfig& sc) { return sc.modeOpt[0] == 2; }
int s2xMod(const SynthConfig& sc) { return std::max(0, std::min(dvbs::kS2Mods - 1, sc.modeOpt[1])); }
// the frame size the generator sends for S2X: 64APSK and up have normal frames only
bool s2xShort(const SynthConfig& sc) { return sc.modeOpt[4] != 0 && dvbs::s2xRateCount(s2xMod(sc), true) > 0; }
// S2X: the rate combo item of a MODCOD, its code rate and "8APSK" where the modulation is not the one of the combo
std::string s2xRateLabel(int rate) {
    std::string l = dvbs::s2RateName(rate);
    for (int m = 0; m < dvbs::kS2Mods; m++)
        if (dvbs::s2Modcod(m, rate) >= 0 && std::string(dvbs::s2ModNameFor(m, rate)) != dvbs::s2ModName(m)) l += std::string(" ") + dvbs::s2ModNameFor(m, rate);
    return l;
}

bool rateOk(const SynthConfig& sc, int rate) {
    if (sc.modeOpt[0] == 1) return rate >= 0 && rate <= 4;
    if (isS2x(sc)) return rate >= 0 && rate < dvbs::s2xRateCount(s2xMod(sc), s2xShort(sc));
    return dvbs::s2Dims(std::max(0, std::min(3, sc.modeOpt[1])), rate, sc.modeOpt[4] != 0).ok;
}
int curRate(const SynthConfig& sc) { return sc.modeOpt[2] ? sc.modeOpt[2] - 1 : (sc.modeOpt[0] == 1 ? 1 : isS2x(sc) ? 0 : 5); }

// Some combinations do not exist (32APSK 1/2, short 9/10 ...): move to the nearest rate that does, so the generator never gets one
void fixRate(SynthConfig& sc) {
    if (!isS2x(sc)) sc.modeOpt[1] = std::max(0, std::min(3, sc.modeOpt[1]));     // 64APSK and up are S2X only
    int r = curRate(sc);
    if (rateOk(sc, r)) return;
    const int n = sc.modeOpt[0] == 1 ? 5 : isS2x(sc) ? dvbs::s2xRateCount(s2xMod(sc), s2xShort(sc)) : dvbs::kS2Rates;
    for (int d = 1; d < n; d++) {
        if (r + d < n && rateOk(sc, r + d)) { r += d; break; }
        if (r - d >= 0 && rateOk(sc, r - d)) { r -= d; break; }
    }
    sc.modeOpt[2] = r + 1;
}

// the options wrap onto more lines in a narrow window (flowNext() between them)
void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    const bool s1 = sc.modeOpt[0] == 1;
    ImGui::SetNextItemWidth(78 * gUi);
    {
        static const char* nm[] = {"DVB-S2", "DVB-S", "DVB-S2X"};
        const int sd = std::max(0, std::min(2, sc.modeOpt[0]));
        if (ImGui::BeginCombo("##dvstd", nm[sd])) {
            for (int i = 0; i < 3; i++) if (ImGui::Selectable(nm[i], i == sd)) { sc.modeOpt[0] = i; fixRate(sc); changed = true; }
            ImGui::EndCombo();
        }
    }
    flowNext(10 * gUi);
    ImGui::BeginDisabled(s1);
    ImGui::SetNextItemWidth(74 * gUi);
    {
        const int nMod = isS2x(sc) ? dvbs::kS2Mods : 4;
        const int m = s1 ? 0 : std::max(0, std::min(nMod - 1, sc.modeOpt[1]));
        if (ImGui::BeginCombo("##dvmod", dvbs::s2ModName(m))) {
            for (int i = 0; i < nMod; i++) if (ImGui::Selectable(dvbs::s2ModName(i), i == m)) { sc.modeOpt[1] = i; fixRate(sc); changed = true; }
            ImGui::EndCombo();
        }
    }
    ImGui::EndDisabled();
    flowNext(10 * gUi);
    ImGui::SetNextItemWidth(62 * gUi);
    {
        const int r = curRate(sc);
        const bool x = isS2x(sc);
        auto label = [&](int i) -> std::string {
            if (s1) return kS1Rates[std::max(0, std::min(4, i))];
            if (x) return s2xRateLabel(dvbs::s2xRate(s2xMod(sc), s2xShort(sc), i));
            return dvbs::s2RateName(std::max(0, std::min(10, i)));
        };
        if (ImGui::BeginCombo("##dvrate", label(r).c_str())) {
            const int n = s1 ? 5 : x ? dvbs::s2xRateCount(s2xMod(sc), s2xShort(sc)) : dvbs::kS2Rates;
            for (int i = 0; i < n; i++) {
                if (!rateOk(sc, i)) continue;
                if (ImGui::Selectable(label(i).c_str(), i == r)) { sc.modeOpt[2] = i + 1; changed = true; }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Code rate. Only the combinations the standard defines are listed.");
    }
    flowNext(10 * gUi);
    ImGui::TextDisabled("Rs");
    ImGui::SameLine(0, 4 * gUi);
    ImGui::SetNextItemWidth(98 * gUi);
    {
        double rs = sc.modeVal[0] > 0 ? sc.modeVal[0] / 1e6 : 5.0;
        if (ImGui::InputDouble("##dvsr", &rs, 0, 0, "%.3f Msym/s")) {
            const double mx = dvbsMaxSymbolRate(dvbsTuning().sampleRate) / 1e6;
            sc.modeVal[0] = std::max(0.1, std::min(mx, rs)) * 1e6;
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Symbol rate of the test signal. Press Enter to apply. The limit is set by the sample rate (%.1f Msym/s at %.0f Msps).",
                                                      dvbsMaxSymbolRate(dvbsTuning().sampleRate) / 1e6, dvbsTuning().sampleRate / 1e6);
    }
    flowNext(10 * gUi);
    ImGui::TextDisabled("RO");
    ImGui::SameLine(0, 4 * gUi);
    ImGui::SetNextItemWidth(60 * gUi);
    {
        const int ro = std::max(0, std::min(5, sc.modeOpt[3]));
        if (ImGui::BeginCombo("##dvro", kRoll[ro])) {
            for (int i = 0; i < 6; i++) if (ImGui::Selectable(kRoll[i], i == ro)) { sc.modeOpt[3] = i; changed = true; }
            ImGui::EndCombo();
        }
    }
    flowNext(10 * gUi);
    ImGui::BeginDisabled(s1);
    {
        bool sh = !s1 && sc.modeOpt[4] != 0;
        if (ImGui::Checkbox("short", &sh)) { sc.modeOpt[4] = sh ? 1 : 0; fixRate(sc); changed = true; }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Short frames (16200 bits) instead of normal ones (64800). Short frames have no 9/10.");
        ImGui::SameLine(0, 8 * gUi);
        bool pl = !s1 && sc.modeOpt[5] != 0;
        if (ImGui::Checkbox("pilots", &pl)) { sc.modeOpt[5] = pl ? 1 : 0; changed = true; }
    }
    ImGui::EndDisabled();
    flowNext(10 * gUi);
    {
        bool inv = sc.modeOpt[6] != 0;
        if (ImGui::Checkbox("inverted", &inv)) { sc.modeOpt[6] = inv ? 1 : 0; changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turn the spectrum around, as an LNB with a high-side oscillator does.");
    }
    flowNext(10 * gUi);
    ImGui::BeginDisabled(s1);
    ImGui::SetNextItemWidth(118 * gUi);
    {
        static const char* nm[] = {"clean channel", "VCM demo", "LNB noise, typical", "LNB noise, critical"};
        const int e = s1 ? 0 : std::max(0, std::min(3, sc.modeOpt[7]));
        if (ImGui::BeginCombo("##dvex", nm[e])) {
            for (int i = 0; i < 4; i++) if (ImGui::Selectable(nm[i], i == e)) { sc.modeOpt[7] = i; changed = true; }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("VCM demo: the MODCOD changes from frame to frame.\nLNB noise: oscillator phase noise of EN 302 307-1 annex H.8 (DVB-S2 only).");
    }
    ImGui::EndDisabled();
    flowNext(10 * gUi);
    ImGui::TextDisabled("Es/N0");
    ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(70 * gUi);
    { float snr = (float)sc.snrDb; if (ImGui::SliderFloat("##dvsnr", &snr, 0, 30, "%.0f dB")) { sc.snrDb = snr; changed = true; } }
    flowNext(10 * gUi);
    ImGui::TextDisabled("CFO");
    ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(92 * gUi);
    { float cfo = (float)(sc.cfoHz / 1e3); if (ImGui::SliderFloat("##dvcfo", &cfo, -2000, 2000, "%.0f kHz")) { sc.cfoHz = cfo * 1e3; changed = true; } }
}

// ---------------------------------------------------------------------------------------------- meters

void meters(const App& a, std::vector<ModeMeter>& out) {
    const DvbsTelemetry& t = a.rx.dvbs;
    double m = 0;
    const bool hasM = margin(t, m);
    out.push_back({"Es/N0  dB", "%.1f", t.lockCarrier ? t.snrDb : 0, 0, 30, !t.lockCarrier ? 0 : hasM ? (m >= 1 ? 1 : m >= 0 ? 2 : 3) : 0});
    out.push_back({"MER  dB", "%.1f", t.lockCarrier ? t.merDb : 0, 0, 30, 0});
    out.push_back({"MARGIN  dB", "%+.1f", hasM ? m : 0, -3, 10, !hasM ? 0 : m >= 1 ? 1 : m >= 0 ? 2 : 3});
    out.push_back({"TS  Mbit/s", "%.2f", t.netBitrate / 1e6, 0, 60, t.tsLock ? 1 : 0});
}

} // namespace

extern const ModeUi kDvbsUi;
const ModeUi kDvbsUi = {
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
