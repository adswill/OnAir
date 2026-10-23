// DVB-S/S2 screens: lock lamps and readouts, constellation and quality plots, the receiver tab, symbol rate and standard controls,
// and the options of the test signal.
#include "app.h"
#include "dect2/dvbs/dvbs_s2.h"
#include <cfloat>
#include <cmath>
#include <deque>

namespace {

struct State {
    bool loaded = false, wasRunning = false;
    double srMsym = 0;                  // manual symbol rate in Msym/s, 0 = find it from the spectrum
    int stdHint = 0;                    // 0 automatic, 1 DVB-S only, 2 DVB-S2 only
    int txPol = 0, lnbPol = 0;          // polarisation of the wanted transponder / of the LNB supply (0 not set, 1 vertical, 2 horizontal): only used for a warning
    double searchSince = -1;            // ImGui time when the receiver started looking without finding a carrier
    double pushedSr = -1; int pushedStd = -1;
    uint64_t lastSeq = 0;
    std::deque<float> snr;              // Es/N0 of the reports, newest last
};
State S;

constexpr size_t kHist = 240;           // reports kept: about a minute

bool live(const App& a) { return a.rx.standard == 7; }

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.srMsym = std::max(0.0, d.getD("dvbsSr", 0.0));
    S.stdHint = (int)std::max(0L, std::min(2L, d.getI("dvbsStd", 0)));
    S.txPol = (int)std::max(0L, std::min(2L, d.getI("dvbsTxPol", 0)));
    S.lnbPol = (int)std::max(0L, std::min(2L, d.getI("dvbsLnbPol", 0)));
}

void saveState(App& a) {
    plat::Prefs& d = plat::prefs();
    d.setD("dvbsSr", S.srMsym);
    d.setI("dvbsStd", S.stdHint);
    d.setI("dvbsTxPol", S.txPol);
    d.setI("dvbsLnbPol", S.lnbPol);
    savePrefs(a);
}

const char* stdName(int s) { return s == 1 ? "DVB-S" : s == 2 ? "DVB-S2" : s == 3 ? "DVB-S2X" : "-"; }

std::string fmt(const char* f, double v) { char b[48]; snprintf(b, sizeof b, f, v); return b; }
std::string cnt(uint64_t v) { return std::to_string((unsigned long long)v); }
// a symbol rate in the unit that reads best: kSym/s below 1 MSym/s (narrow carriers, down to about 10 kSym/s), MSym/s above
std::string rateStr(double hz, int msymDigits = 3) {
    char b[48];
    if (hz < 1e6) snprintf(b, sizeof b, hz < 100e3 ? "%.2f kSym/s" : "%.1f kSym/s", hz / 1e3); else snprintf(b, sizeof b, "%.*f MSym/s", msymDigits, hz / 1e6);
    return b;
}

// Eb/N0 per information bit: Es/N0 less 10 log10 of the bits a symbol carries (modulation times code rate; DVB-S also times 188/204 for the Reed-Solomon code)
bool ebN0(const DvbsTelemetry& t, double& out) {
    int bps = 0;
    const std::string& m = t.modulationName;
    if (m.find("BPSK") != std::string::npos) bps = 1;
    else if (m.find("QPSK") != std::string::npos) bps = 2;
    else if (m.find("8PSK") != std::string::npos) bps = 3;
    else if (m.find("256APSK") != std::string::npos) bps = 8;
    else if (m.find("128APSK") != std::string::npos) bps = 7;
    else if (m.find("64APSK") != std::string::npos) bps = 6;
    else if (m.find("32APSK") != std::string::npos) bps = 5;
    else if (m.find("16APSK") != std::string::npos) bps = 4;
    int a = 0, b = 0;
    if (!bps || sscanf(t.codeRate.c_str(), "%d/%d", &a, &b) != 2 || a <= 0 || b <= 0) return false;
    double r = (double)a / b;
    if (t.standard == 1) r *= 188.0 / 204.0;
    out = t.snrDb - 10.0 * std::log10(bps * r);
    return true;
}

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
Stages stages(const DvbsTelemetry& t, bool on) {
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

// The lock lamps and the readouts of a DVB-S/S2 report (state, standard, symbol rate, MODCOD, Es/N0, margin, counters), wrapping with flowNext():
// the status bar of the mode, and the panel of QO-100 under its wideband view (the decoder as the second mode). withCarrierOffset: a carrier
// offset from the centre means nothing for a carrier picked out of a wider capture, nor do the radio's dropped samples belong to it (standalone)
void statusRows(App& a, const DvbsTelemetry& t, bool on, bool run, bool standalone) {
    const Stages s = stages(t, on);
    const float gap = 12 * gUi;
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
    if (t.symbolRate > 0) ro("Symbol rate", rateStr(t.symbolRate) + (t.symbolRateManual ? " (set)" : ""));
    if (t.standard && !t.modulationName.empty()) {
        std::string m = t.modulationName + " " + t.codeRate;
        if (t.standard >= 2 && t.frameSize == 2) m += " short";
        else if (t.standard == 3 && t.frameSize == 3) m += " medium";
        if (t.pilots) m += " pilots";
        if (t.vcm) m += " VCM";
        ro("Mode", m);
    }
    flowBreak();
    // what the automatic search is doing, or why it does not lock, and how long a narrow carrier takes (a DVB-S2 frame at 10 kSym/s lasts over 3 s)
    if (!t.tsLock && !t.searchStatus.empty()) {
        std::string w = t.searchStatus;
        if (t.huntSecsExpected > 1.5 && t.huntSecs < t.huntSecsExpected) w += fmt("  (about %.0f s left)", t.huntSecsExpected - t.huntSecs);
        ro("Search", w, kDim);
        flowBreak();
    }
    if (t.lockCarrier) {
        ro("Es/N0", fmt("%.1f dB", t.snrDb));
        double eb;
        if (ebN0(t, eb)) ro("Eb/N0", fmt("%.1f dB", eb));
        double m;
        if (margin(t, m)) ro("Margin", fmt("%+.1f dB", m), m >= 1.0 ? pal::okGreen() : m >= 0 ? pal::accent() : pal::badRed());
        if (standalone) ro("Offset", fmt("%+.0f kHz", t.cfoHz / 1e3));
    }
    if (t.standard >= 2) {
        ro("Frames", cnt(t.blocksOk) + " ok  " + cnt(t.blocksBad) + " bad", t.blocksBad ? ImVec4(0.95f, 0.55f, 0.35f, 1) : ImVec4(0.93f, 0.95f, 0.97f, 1));
    } else if (t.standard == 1) {
        ro("RS", cnt(t.rsClean + t.rsCorrected) + " ok  " + cnt(t.rsFailed) + " failed", t.rsFailed ? ImVec4(0.95f, 0.55f, 0.35f, 1) : ImVec4(0.93f, 0.95f, 0.97f, 1));
    }
    if (t.packets) ro("Packets", cnt(t.packets));
    if (t.netBitrate > 0) ro("TS", fmt("%.2f Mbit/s", t.netBitrate / 1e6));
    if (run && standalone) { const SampleLoss l = a.engine.sampleLoss(); ro("dropped", lossText(l), lossColour(l)); lossTooltip(l); }
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    statusRows(a, a.rx.dvbs, on, run, true);
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
    if (t.tsLock) snprintf(b, sizeof b, "locked  %s  Es/N0 %.1f dB", rateStr(t.symbolRate, 2).c_str(), t.snrDb);
    else if (t.lockSpectrum) snprintf(b, sizeof b, "locking  %s", rateStr(t.symbolRate, 2).c_str());
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
    const Stages s = stages(t, on);
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

// the input streams of the carrier: what each ISI carries, at which MODCODs and how fast (a multistream carrier interleaves several)
void streamsSection(App& a, const DvbsTelemetry& t) {
    if (t.streams.empty() || (t.streams.size() == 1 && t.streams[0].content == "transport stream")) return;
    ImGui::Spacing();
    ImGui::TextColored(pal::accent(), t.streams.size() > 1 ? "INPUT STREAMS (MULTISTREAM)" : "INPUT STREAM");
    if (ImGui::BeginTable("##dvbsstreams", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX)) {
        ImGui::TableSetupColumn("ISI"); ImGui::TableSetupColumn("carries"); ImGui::TableSetupColumn("mode"); ImGui::TableSetupColumn("frames"); ImGui::TableSetupColumn("MODCODs"); ImGui::TableSetupColumn("rate"); ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        ImGui::PushFont(a.mono, 0);
        for (const auto& d : t.streams) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); if (d.single) ImGui::TextUnformatted("-"); else ImGui::Text("%d", d.isi);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d.content.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s%s%s", d.ccm ? "CCM" : "ACM/VCM", d.issyi ? ", ISSY" : "", d.hem ? ", HEM" : "");
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)d.frames);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d.modcods.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f kbit/s", d.bps / 1e3);
            ImGui::TableNextColumn(); if (d.decoded) ImGui::TextColored(pal::accent(), "decoded as TS");
        }
        ImGui::PopFont();
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------------------------- survey of the carriers, symbol file
// A wide capture can hold many carriers: the list shows every one found in the spectrum; "Decode" makes the receiver work on that one. The symbol file is for other tools.
bool g_symDumpOn = false;

void carriersSection(App& a, const DvbsTelemetry& t) {
    if (t.carriers.empty() && !g_symDumpOn && t.symbolDumpPath.empty()) return;
    ImGui::Spacing();
    ImGui::TextColored(pal::accent(), "CARRIERS IN THE SPECTRUM");
    if (t.carriers.size() > 1 || t.carrierPicked) {
        if (ImGui::BeginTable("##dvbscarriers", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX)) {
            ImGui::TableSetupColumn("offset"); ImGui::TableSetupColumn("symbol rate"); ImGui::TableSetupColumn("roll-off"); ImGui::TableSetupColumn("level"); ImGui::TableSetupColumn("C/N"); ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            ImGui::PushFont(a.mono, 0);
            int id = 0;
            for (const auto& c : t.carriers) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%+.3f MHz", c.centerHz / 1e6);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(rateStr(c.rateHz).c_str());
                ImGui::TableNextColumn(); if (c.rollOff > 0) ImGui::Text("%.2f", c.rollOff); else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn(); ImGui::Text("%.1f dB", c.levelDb);
                ImGui::TableNextColumn(); ImGui::Text("%.1f dB", c.snrDb);
                ImGui::TableNextColumn();
                ImGui::PushID(id++);
                if (c.selected) ImGui::TextColored(pal::accent(), "decoding");
                else if (ImGui::SmallButton("Decode")) a.engine.dvbs().selectCarrier(true, c.centerHz);
                ImGui::PopID();
            }
            ImGui::PopFont();
            ImGui::EndTable();
        }
        if (t.carrierPicked && ImGui::SmallButton("Back to the carrier nearest the centre")) a.engine.dvbs().selectCarrier(false, 0);
    }
    bool on = g_symDumpOn;
    if (ImGui::Checkbox("Write the symbols to a file", &on)) {
        g_symDumpOn = on;
        a.engine.dvbs().setSymbolDump(on ? plat::dataDir() + "/dvbs_symbols.cf32" : std::string());
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Every recovered symbol (after timing and carrier recovery) as interleaved float32 I/Q, up to 512 MB, to dvbs_symbols.cf32 in the data folder. Open it in GNU Radio, inspectrum or numpy to look at an unknown modulation.");
    if (!t.symbolDumpPath.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s  %.1f Msym", t.symbolDumpPath.c_str(), (double)t.symbolDumpCount / 1e6); }
}

// ---------------------------------------------------------------------------------------------- carrier identification (DVB-CID, ETSI TS 103 129)
bool g_cidOn = false;

void cidSection(App& a, const DvbsTelemetry& t) {
    if (!live(a) || !t.lockSpectrum) return;
    ImGui::Spacing();
    ImGui::TextColored(pal::accent(), "CARRIER ID (DVB-CID)");
    bool on = g_cidOn;
    if (ImGui::Checkbox("Look for the carrier identification signal", &on)) { g_cidOn = on; a.engine.dvbs().setCidSearch(on); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "A low-level spread-spectrum signal under the carrier that names the uplink modulator (TS 103 129). It needs a long observation (a frame lasts 36 s at 112 kchip/s) and a steady signal; it uses some processing.");
    if (!t.cid.on) return;
    const DvbsCid& c = t.cid;
    if (!c.acquired && !c.haveGuid) { ImGui::TextDisabled("searching (up to a few minutes)"); return; }
    ImGui::Text("%s  %+.0f Hz from the carrier centre, %.1f dB per bit, %llu bits, %llu frames%s", c.acquired ? "tracking" : "lost", c.offsetHz, c.snrDb, (unsigned long long)c.bits, (unsigned long long)c.frames, c.badFrames ? (", " + std::to_string(c.badFrames) + " not decoded").c_str() : "");
    if (c.haveGuid) {
        ImGui::PushFont(a.mono, 0);
        ImGui::Text("unique identifier  %s", c.guid.c_str());
        if (!c.mac.empty()) ImGui::Text("MAC address        %s", c.mac.c_str());
        if (c.havePosition) ImGui::Text("position           %.4f, %.4f", c.latitude, c.longitude);
        if (!c.telephone.empty()) ImGui::Text("telephone          %s", c.telephone.c_str());
        if (!c.text.empty()) ImGui::Text("text               %s", c.text.c_str());
        ImGui::PopFont();
    }
}

// ---------------------------------------------------------------------------------------------- IP data (GSE, MPE, ULE)
// What the IP traffic of the signal looks like: hosts, ports, protocols, volume, the terminals of an ACM outbound. Payloads are never shown or decoded; a flow whose
// UDP checksums fail and PDUs that read as random bytes are marked as encrypted. Optionally every IP packet is written to a PCAP file (link type RAW).
bool g_ipPcapOn = false;
std::string g_ipSaved;

void ipSection(App& a, const DvbsTelemetry& t) {
    const DvbsIpTel& ip = t.ip;
    if (!ip.active && ip.gseFields == 0 && t.gseFrames == 0) return;
    ImGui::Spacing();
    ImGui::TextColored(pal::accent(), "IP DATA");
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(pal::warnAmber(), "Satellite IP links can carry other people's private traffic. You are responsible for what the law where you live allows you to receive and keep. "
                                        "Nothing encrypted or scrambled is decoded here, and payloads are never shown: only addresses, ports, protocols and counts.");
    ImGui::PopTextWrapPos();
    char b[200];
    snprintf(b, sizeof b, "%s  |  %llu packets, %.1f MB  (IPv4 %llu, IPv6 %llu)  |  %llu GSE data fields%s", ip.source.empty() ? "no IP found yet" : ip.source.c_str(), (unsigned long long)ip.packets, (double)ip.bytes / 1e6,
             (unsigned long long)ip.ipv4, (unsigned long long)ip.ipv6, (unsigned long long)ip.gseFields, ip.reassembled ? ("  |  " + std::to_string(ip.reassembled) + " IPv4 datagrams reassembled").c_str() : "");
    ImGui::TextUnformatted(b);
    bool on = g_ipPcapOn;
    if (ImGui::Checkbox("Write a PCAP file", &on)) {
        g_ipPcapOn = on;
        a.engine.dvbs().setIpPcap(on ? plat::dataDir() + "/dvbs_ip.pcap" : std::string());
        g_ipSaved = on ? plat::dataDir() + "/dvbs_ip.pcap" : std::string();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Every IP packet found is appended to dvbs_ip.pcap in the data folder (libpcap, link type RAW), also with this window hidden. Open it in Wireshark.");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) a.engine.dvbs().clearIp();
    if (!ip.pcapPath.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s  %llu packets, %.1f MB", ip.pcapPath.c_str(), (unsigned long long)ip.pcapPackets, (double)ip.pcapBytes / 1e6); }
    if (!ip.protocols.empty()) {
        std::string l = "protocols: ";
        for (size_t i = 0; i < ip.protocols.size() && i < 8; i++) l += (i ? ", " : "") + ip.protocols[i].first + " " + std::to_string(ip.protocols[i].second);
        ImGui::TextDisabled("%s", l.c_str());
    }
    auto ports = [&](const char* nm, const std::vector<std::pair<std::string, uint64_t>>& v) {
        if (v.empty()) return;
        std::string l = std::string(nm) + " destination ports: ";
        for (size_t i = 0; i < v.size() && i < 8; i++) l += (i ? ", " : "") + v[i].first + " (" + std::to_string(v[i].second) + ")";
        ImGui::TextDisabled("%s", l.c_str());
    };
    ports("UDP", ip.udpPorts); ports("TCP", ip.tcpPorts);
    if (ip.encryptedFlows) ImGui::TextColored(pal::warnAmber(), "%zu of %zu flows have good headers but failing UDP checksums: encrypted in transit, not decoded.", ip.encryptedFlows, ip.flowCount);
    if (!ip.otherPdus.empty()) {
        std::string l = "other GSE PDUs by protocol type: ";
        for (size_t i = 0; i < ip.otherPdus.size() && i < 6; i++) l += (i ? ", " : "") + ip.otherPdus[i].first + "h x" + std::to_string(ip.otherPdus[i].second);
        ImGui::TextDisabled("%s", l.c_str());
        if (ip.otherEntropy >= 0) {
            if (ip.otherEncrypted) ImGui::TextColored(pal::warnAmber(), "They read as random bytes (%.2f bits per byte): link encryption, not decoded.", ip.otherEntropy);
            else ImGui::TextDisabled("%.2f bits per byte: a format, not ciphertext.", ip.otherEntropy);
        }
    }
    if (!ip.flows.empty() && ImGui::BeginTable("##dvbsipflows", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX, ImVec2(0, 160 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("source"); ImGui::TableSetupColumn("destination"); ImGui::TableSetupColumn("proto"); ImGui::TableSetupColumn("packets"); ImGui::TableSetupColumn("bytes"); ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        ImGui::PushFont(a.mono, 0);
        for (const auto& f : ip.flows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); if (f.sport >= 0) ImGui::Text("%s:%d", f.src.c_str(), f.sport); else ImGui::TextUnformatted(f.src.c_str());
            ImGui::TableNextColumn(); if (f.dport >= 0) ImGui::Text("%s:%d", f.dst.c_str(), f.dport); else ImGui::TextUnformatted(f.dst.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(f.protocol.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)f.packets);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)f.bytes);
            ImGui::TableNextColumn(); if (f.encrypted) ImGui::TextColored(pal::warnAmber(), "encrypted");
        }
        ImGui::PopFont();
        ImGui::EndTable();
    }
    if (!ip.media.empty()) {
        ImGui::TextDisabled("multicast streams in the traffic: %zu", ip.media.size());
        if (ImGui::BeginTable("##dvbsipmedia", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX)) {
            ImGui::TableSetupColumn("address"); ImGui::TableSetupColumn("kind"); ImGui::TableSetupColumn("codec"); ImGui::TableSetupColumn("packets"); ImGui::TableSetupColumn("rate"); ImGui::TableSetupColumn("what it carries");
            ImGui::TableHeadersRow();
            ImGui::PushFont(a.mono, 0);
            for (const auto& m : ip.media) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%s:%d", m.dst.c_str(), m.port);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(m.kind.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(m.codec.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%llu%s", (unsigned long long)m.packets, m.lost ? (" (" + std::to_string(m.lost) + " lost)").c_str() : "");
                ImGui::TableNextColumn(); ImGui::Text("%.0f kbit/s", m.kbps);
                ImGui::TableNextColumn();
                std::string w;
                for (size_t i = 0; i < m.services.size() && i < 6; i++) w += (i ? ", " : "") + m.services[i];
                if (m.scrambled) w += (w.empty() ? "" : "; ") + std::string("scrambled: only labelled");
                ImGui::TextUnformatted(w.c_str());
            }
            ImGui::PopFont();
            ImGui::EndTable();
        }
    }
    for (const auto& d : ip.sdp) ImGui::TextDisabled("announcement: \"%s\"%s%s  %s  %s  (%llu times, from %s)", d.name.c_str(), d.info.empty() ? "" : " - ", d.info.c_str(), d.address.c_str(), d.media.c_str(), (unsigned long long)d.seen, d.source.c_str());
    if (!ip.labels.empty()) {
        ImGui::TextDisabled("GSE labels (terminals of an outbound): %zu, with the MODCODs each is sent in", ip.labelCount);
        if (ImGui::BeginTable("##dvbsiplabels", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX, ImVec2(0, 140 * gUi))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("label"); ImGui::TableSetupColumn("PDUs"); ImGui::TableSetupColumn("bytes"); ImGui::TableSetupColumn("addresses"); ImGui::TableSetupColumn("MODCODs");
            ImGui::TableHeadersRow();
            ImGui::PushFont(a.mono, 0);
            for (const auto& l : ip.labels) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(l.label.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)l.pdus);
                ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)l.bytes);
                ImGui::TableNextColumn(); { std::string s; for (size_t i = 0; i < l.addrs.size(); i++) s += (i ? ", " : "") + l.addrs[i]; ImGui::TextUnformatted(s.c_str()); }
                ImGui::TableNextColumn(); ImGui::TextUnformatted(l.modcods.c_str());
            }
            ImGui::PopFont();
            ImGui::EndTable();
        }
    }
}

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
    L.push_back({"in use", t.symbolRate > 0 ? rateStr(t.symbolRate, 4) : "-"});
    L.push_back({"source", t.symbolRateManual ? "set by hand" : "from the spectrum"});
    L.push_back({"from spectrum", t.symbolRateSpectrum > 0 ? rateStr(t.symbolRateSpectrum, 4) : "-"});
    L.push_back({"timing loop", t.symbolRateLoop > 0 ? rateStr(t.symbolRateLoop, 4) : "-"});
    if (t.symbolRate > 0 && t.symbolRateLoop > 0) L.push_back({"loop vs used", fmt("%+.0f ppm", (t.symbolRateLoop / t.symbolRate - 1) * 1e6)});
    {
        const char* src = t.rollOffSource == 2 ? "signalled" : t.rollOffSource == 1 ? "measured" : "assumed";
        L.push_back({"roll-off", t.rollOff > 0 ? fmt("%.2f", t.rollOff) + " (" + src + ")" : "-"});
    }

    head(L, "DVB-S2");
    if (t.standard >= 2) {
        L.push_back({"MODCOD", t.modcod > 0 ? std::to_string(t.modcod) : t.modcod == 0 ? "0 (dummy frame)" : "-"});
        L.push_back({"frame", t.frameSize == 1 ? "normal, 64800 bits" : t.frameSize == 2 ? "short, 16200 bits" : t.frameSize == 3 ? "medium, 32400 bits" : "-"});
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
    carriersSection(a, t);
    cidSection(a, t);
    streamsSection(a, t);
    ipSection(a, t);
    ImGui::Spacing();
    ImGui::TextColored(pal::accent(), "NOTES");
    ImGui::PushTextWrapPos(0);
    if (live(a) && !t.lockSpectrum) { if (S.searchSince < 0) S.searchSince = ImGui::GetTime(); } else S.searchSince = -1;
    if (S.searchSince >= 0 && ImGui::GetTime() - S.searchSince > 10)
        ImGui::TextColored(pal::warnAmber(), "No carrier found. Most often the LNB is on the other polarisation (13 V vertical, 18 V horizontal) or the other band (22 kHz tone), or the dish is off target.");
    if (!t.signalNote.empty()) ImGui::TextUnformatted(t.signalNote.c_str());
    if (!t.superframe.empty()) ImGui::TextUnformatted(t.superframe.c_str());
    ImGui::TextDisabled("DVB-S2X: the MODCODs of EN 302 307-2 table 17a and the VL-SNR frames of table 18a (set 1 and 2, PLS codes 129 and 131) are decoded; the low roll-offs 0.15, 0.10 and 0.05 are signalled and followed. Superframes (Annex E) and bundled channels are not decoded.");
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
        ImGui::SetTooltip("Auto (the default) finds the symbol rate, roll-off and standard by itself, from about 10 kSym/s up to what the sample rate carries.\n"
                          "Narrow carriers take longer to lock: the receiver needs about 150 000 symbols (15 s at 10 kSym/s). Untick Auto only to force a rate (kSym/s).\n\n"
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
    // in kSym/s, so that narrow carriers (10 kSym/s) and wide ones (27500 kSym/s) are typed the same way
    double v = (autoSr ? (live(a) ? a.rx.dvbs.symbolRate / 1e6 : 0.0) : S.srMsym) * 1e3;
    if (ImGui::InputDouble("##dvbssrv", &v, 0, 0, "%.1f kSym/s")) {
        if (!autoSr) { S.srMsym = std::max(0.005, std::min(maxMsym, v / 1e3)); ch = true; }
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
    {   // polarisation: the radio cannot switch the LNB (13 V vertical, 18 V horizontal), so this only warns when the two do not match
        static const char* pn[] = {"not set", "vertical", "horizontal"};
        ImGui::SetNextItemWidth(std::min(110 * gUi, ImGui::GetContentRegionAvail().x));
        ImGui::TextDisabled("Transponder");
        ImGui::SameLine(0, 6 * gUi);
        ImGui::SetNextItemWidth(std::min(100 * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##dvbstxpol", pn[S.txPol])) {
            for (int i = 0; i < 3; i++) if (ImGui::Selectable(pn[i], S.txPol == i)) { S.txPol = i; ch = true; }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The polarisation of the transponder you want (from the satellite's channel list, e.g. 11.727 GHz V).");
        ImGui::TextDisabled("LNB supply");
        ImGui::SameLine(0, 6 * gUi);
        ImGui::SetNextItemWidth(std::min(100 * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##dvbslnbpol", pn[S.lnbPol])) {
            for (int i = 0; i < 3; i++) if (ImGui::Selectable(pn[i], S.lnbPol == i)) { S.lnbPol = i; ch = true; }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("What your LNB is switched to: about 13 V is vertical, about 18 V is horizontal.\nOnAir cannot change this itself: the radio's bias-tee is only about 5 V. Use a power inserter or a switch.");
        if (S.txPol && S.lnbPol && S.txPol != S.lnbPol) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(pal::warnAmber(), "This transponder is %s but the LNB is set to %s. Switch the LNB supply (%s) or this channel will not be received.",
                               pn[S.txPol], pn[S.lnbPol], S.txPol == 1 ? "13 V" : "18 V");
            ImGui::PopTextWrapPos();
        }
    }
    {   // where the error correction runs (the same choice as the DVB-T2 decoder has): LDPC for DVB-S2, Viterbi for DVB-S
        static const char* modes[] = {"CPU", "GPU", "Auto"};
        const DvbsTelemetry& t = a.rx.dvbs;
        const bool gpuOk = GpuLdpc::instance().available();
        char lbl[48];
        snprintf(lbl, sizeof lbl, "%s", a.computeMode == 2 ? (live(a) && t.onGpu ? "Auto (GPU)" : "Auto (CPU)") : modes[a.computeMode]);
        ImGui::TextDisabled("Compute");
        ImGui::SameLine(0, 6 * gUi);
        ImGui::SetNextItemWidth(std::min(120 * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##dvbscompute", lbl)) {
            for (int i = 0; i < 3; i++) {
                const bool dis = i == 1 && !gpuOk;
                if (dis) ImGui::BeginDisabled();
                if (ImGui::Selectable(modes[i], a.computeMode == i)) { a.computeMode = i; a.engine.setComputeMode(i); savePrefs(a); }
                if (dis) ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Where the error correction runs: the LDPC decoder of DVB-S2 and the Viterbi decoder of DVB-S.\n"
                              "The decoded stream is the same on either.\n"
                              "CPU: all cores. GPU: %s.\n"
                              "Auto: the GPU when there is one and it is faster, measured while it runs; otherwise the CPU.\n"
                              "Graphics: %s",
#ifdef __APPLE__
                              "Metal compute",
#else
                              "Direct3D 11 compute (Windows)",
#endif
                              gpuOk ? GpuLdpc::instance().deviceName() : "none usable");
    }
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

// The spectrum explorer hands a carrier over: its symbol rate (Msym/s, 0 = automatic) becomes the manual setting
double dvbsSymbolRateMsym() { loadState(); return S.srMsym; }

void dvbsSetSymbolRate(App& a, double msym) {
    loadState();
    S.srMsym = std::max(0.0, msym);
    saveState(a);
}

// The lamps and readouts of a DVB-S/S2 report that is not the first mode's (QO-100's decoder panel)
void dvbsStatusRows(App& a, const DvbsTelemetry& t, bool on, bool run) { statusRows(a, t, on, run, false); }

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
