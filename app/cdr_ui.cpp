// CDR screens: the Radio tab (the picked service and the signal), the station list (every service tagged DRA+: the sound is not decoded),
// status lamps (Signal, Sync, SIC, SDC, Data), the analysis plots (SIC / SDC / MSD cells, channel, SNR), the Receiver tab and the test signal.
#include "app.h"
#include "dect2/cdr_defs.h"
#include "dect2/cdr_mux.h"
#include "dect2/cdr_tel.h"
#include <deque>

namespace {

constexpr int kStd = 23;   // the engine reports its standard code minus one

struct State {
    uint16_t sel = 0;                 // service id the user picked, 0: the first one
    uint64_t lastSeq = 0;
    std::deque<float> snr;
    uint64_t prevSiOk = 0, prevSiBad = 0, prevSdcOk = 0, prevSdcBad = 0, prevOk = 0, prevBad = 0;
    double siOkAt = -1e9, siBadAt = -1e9, sdcOkAt = -1e9, sdcBadAt = -1e9, okAt = -1e9, badAt = -1e9;
};
State S;

bool live(const App& a) { return a.engine.running() && a.rx.standard == kStd; }

const ImVec4 kGood(0.40f, 0.85f, 0.50f, 1), kWarn(0.95f, 0.60f, 0.25f, 1);
const char* kDraNote = "DRA+ sound, not decoded (the codec is patented and not openly specified)";

const char* rateText(int r) { static const char* n[] = {"1/4", "1/3", "1/2", "3/4"}; return r >= 0 && r < 4 ? n[r] : "-"; }
const char* modName(int m) { return m >= 0 && m <= 2 ? dect2::cdr::modText(m) : "-"; }

std::string serviceName(const dect2::CdrServiceInfo& s) {
    char b[48]; snprintf(b, sizeof b, "%s %04X", s.audio ? "Audio service" : s.data ? "Data service" : "Service", s.id); return b;
}

std::string channelsText(int c) { return c == 1 ? "mono" : c == 2 ? "stereo" : c == 3 ? "5.1" : "-"; }

std::string serviceFacts(const dect2::CdrServiceInfo& s) {
    char b[160];
    if (s.audio && !s.streams.empty()) {
        const auto& st = s.streams[0];
        snprintf(b, sizeof b, "DRA+ %.0f kbit/s, %g kHz %s", st.bitrate / 1000.0, st.sampleRate / 1000.0, channelsText(st.channelsCode).c_str());
    } else if (s.data) snprintf(b, sizeof b, "data %.1f kbit/s%s", s.kbps, s.text.empty() ? "" : ", text");
    else snprintf(b, sizeof b, "%.1f kbit/s", s.kbps);
    return b;
}

const dect2::CdrServiceInfo* picked(const dect2::CdrTelemetry& t) {
    for (const auto& s : t.services) if (s.id == S.sel) return &s;
    return t.services.empty() ? nullptr : &t.services[0];
}

int blockLamp(double okAt, double badAt, double window, double now) {
    const bool ok = now - okAt < window, bad = now - badAt < window;
    return ok && bad ? 2 : ok ? 1 : bad ? 3 : 0;
}

void push(std::deque<float>& d, float v) { d.push_back(v); if (d.size() > 600) d.pop_front(); }

void tick(App& a) {
    if (!live(a) || a.rx.seq == S.lastSeq) return;
    S.lastSeq = a.rx.seq;
    const dect2::CdrTelemetry& t = a.rx.cdr;
    const double now = ImGui::GetTime();
    auto count = [&](uint64_t cur, uint64_t& prev, double& at) { if (cur < prev) prev = 0; if (cur > prev) at = now; prev = cur; };
    count(t.siOk, S.prevSiOk, S.siOkAt); count(t.siBad, S.prevSiBad, S.siBadAt);
    count(t.sdcOk, S.prevSdcOk, S.sdcOkAt); count(t.sdcBad + t.tablesBad, S.prevSdcBad, S.sdcBadAt);
    count(t.blocksOk, S.prevOk, S.okAt); count(t.blocksBad, S.prevBad, S.badAt);
    if (t.state > 0) push(S.snr, t.snrDb);
}

void kvRow(const App& a, const char* k, const std::string& v, float x = 110.f) {
    ImGui::TextDisabled("%s", k); kvColumn(x * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

// ---------------------------------------------------------------- the Radio tab

void details(App& a, float w, float h) {
    const dect2::CdrTelemetry& t = a.rx.cdr;
    const bool on = live(a);
    const dect2::CdrServiceInfo* s = on ? picked(t) : nullptr;
    ImGui::BeginChild("##cdr_now", ImVec2(w, h), ImGuiChildFlags_Borders);
    if (!s) {
        ImGui::PushFont(a.ui, 22.f);
        { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(!a.engine.running() ? "stopped" : t.state > 0 ? "reading the multiplex" : "searching for a CDR signal"); ImGui::PopTextWrapPos(); }
        ImGui::PopFont();
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? t.status.c_str() : "Start the receiver. Test signal: pick the synthetic source."); ImGui::PopTextWrapPos(); }
        ImGui::EndChild();
        return;
    }
    ImGui::PushFont(a.ui, 26.f);
    ImGui::PushStyleColor(ImGuiCol_Text, pal::dev() ? pal::accent() : ImVec4(0.55f, 0.80f, 1.f, 1));
    { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(serviceName(*s).c_str()); ImGui::PopTextWrapPos(); }
    ImGui::PopStyleColor();
    ImGui::PopFont();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", t.network.empty() ? "network name not received yet" : t.network.c_str()); ImGui::PopTextWrapPos(); }
    ImGui::Spacing();
    char b[96];
    snprintf(b, sizeof b, "%04X  (multiplex frame %d, sub-frame %d)", s->id, s->smfId, s->subIndex); kvRow(a, "service id", b);
    kvRow(a, "kind", s->audio ? "audio" : s->data ? "data" : "-");
    snprintf(b, sizeof b, "%.1f kbit/s", s->kbps); kvRow(a, "share", b);
    for (const auto& st : s->streams) {
        snprintf(b, sizeof b, "DRA+ (algorithm code %d)", st.algo); kvRow(a, "codec", b);
        snprintf(b, sizeof b, "%.1f kbit/s", st.bitrate / 1000.0); kvRow(a, "bit rate", st.bitrate ? b : "-");
        snprintf(b, sizeof b, "%g kHz, %s", st.sampleRate / 1000.0, channelsText(st.channelsCode).c_str()); kvRow(a, "audio", st.sampleRate ? b : "-");
        kvRow(a, "language", st.language.empty() ? "-" : st.language);
    }
    if (s->audio) {
        snprintf(b, sizeof b, "%d units, %d bytes per logical frame", s->audioUnits, s->audioBytes); kvRow(a, "received", b);
        ImGui::TextDisabled("sound"); kvColumn(110 * gUi);
        { ImGui::PushTextWrapPos(0); ImGui::TextColored(kWarn, "%s", kDraNote); ImGui::PopTextWrapPos(); }
    }
    if (s->data) {
        std::string ty;
        for (int d : s->dataTypes) { if (!ty.empty()) ty += ", "; ty += std::to_string(d) + " " + dect2::cdr::dataUnitTypeText(d); }
        kvRow(a, "data units", ty.empty() ? "-" : ty);
    }
    snprintf(b, sizeof b, "%llu ok, %llu bad", (unsigned long long)s->subOk, (unsigned long long)s->subBad); kvRow(a, "sub-frames", b);
    ImGui::Spacing();
    sectionHeader(Ic::Radio, "Text");
    ImGui::PushTextWrapPos(0);
    if (!s->text.empty()) ImGui::TextUnformatted(s->text.c_str());
    else {
        const dect2::CdrServiceInfo* tx = nullptr;
        for (const auto& o : t.services) if (!o.text.empty()) { tx = &o; break; }
        if (tx) ImGui::Text("%04X: %s", tx->id, tx->text.c_str()); else ImGui::TextDisabled("no text service");
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Service names and programme types belong to the electronic service guide, which is not decoded.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

void signalPanel(App& a, float w, float h) {
    const dect2::CdrTelemetry& t = a.rx.cdr;
    const bool on = live(a) && t.state > 0;
    ImGui::BeginChild("##cdr_sig", ImVec2(w, h), ImGuiChildFlags_Borders);
    sectionHeader(Ic::Wave, "Signal");
    char b[96];
    snprintf(b, sizeof b, "%d", t.tm); kvRow(a, "mode", on ? b : "-", 100);
    const dect2::cdr::SpectrumMode* sm = dect2::cdr::spectrumMode(t.sm);
    snprintf(b, sizeof b, "%d  (%d00 kHz)", t.sm, sm ? sm->ni : 0); kvRow(a, "spectrum", on ? b : "-", 100);
    const bool si = on && t.siValid;
    snprintf(b, sizeof b, "%s, rate %s", modName(t.msdMod), rateText(t.rate)); kvRow(a, "service data", si ? b : "-", 100);
    kvRow(a, "description", si ? modName(t.sdiMod) : "-", 100);
    snprintf(b, sizeof b, "mode %d", t.alloc); kvRow(a, "allocation", si ? b : "-", 100);
    snprintf(b, sizeof b, "%.1f dB", t.snrDb); kvRow(a, "SNR", on ? b : "-", 100);
    snprintf(b, sizeof b, "%+.1f Hz", t.cfoHz); kvRow(a, "carrier offset", on ? b : "-", 108);
    ImGui::Spacing();
    sectionHeader(Ic::Layers, "Network");
    kvRow(a, "name", t.network.empty() ? "-" : t.network, 100);
    snprintf(b, sizeof b, "%llX", (unsigned long long)t.networkId); kvRow(a, "id", on && !t.network.empty() ? b : "-", 100);
    kvRow(a, "country", t.country.empty() ? "-" : t.country, 100);
    ImGui::EndChild();
}

void tab(App& a) {
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 8 * gUi;
    const float sw = std::min(std::max(250.f * gUi, W * 0.34f), W * 0.5f);
    details(a, W - sw - gap, H);
    ImGui::SameLine(0, gap);
    signalPanel(a, sw, H);
}

// ---------------------------------------------------------------- the station list

void list(App& a) {
    const dect2::CdrTelemetry& t = a.rx.cdr;
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see stations"); ImGui::PopTextWrapPos(); return; }
    if (t.services.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(t.state > 0 ? "reading the multiplex" : "searching for a CDR signal"); ImGui::PopTextWrapPos(); return; }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%zu service%s. DRA+ sound is not decoded.", t.services.size(), t.services.size() == 1 ? "" : "s");
    ImGui::PopTextWrapPos();
    const dect2::CdrServiceInfo* cur = picked(t);
    for (const auto& s : t.services) {
        ImGui::PushID(s.id);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float lh = ImGui::GetTextLineHeightWithSpacing();
        if (ImGui::Selectable("##svc", cur && cur->id == s.id, 0, ImVec2(0, lh * 2 + 2 * gUi))) S.sel = s.id;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(p.x + 4 * gUi, p.y + 1), ImGui::GetColorU32(ImGuiCol_Text), serviceName(s).c_str());
        dl->AddText(ImVec2(p.x + 4 * gUi, p.y + lh + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), serviceFacts(s).c_str());
        if (s.audio && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kDraNote);
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------- the analysis row

void squarePlot(const char* title, const char* id, const std::vector<cf32>& pts, float side, double lim) {
    ImGui::BeginGroup();
    captionFit(side, "%s (%zu)", title, pts.size());
    scatter(id, pts, ImVec2(side, side), lim, pal::accent(0.7f));
    ImGui::EndGroup();
}

void panels(App& a) {
    const dect2::CdrTelemetry& t = a.rx.cdr;
    const bool on = live(a);
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi;
    const float plotH = std::max(70.f, H - ImGui::GetTextLineHeightWithSpacing() - 8);
    const float side = std::min(plotH, std::max(80.f, W * 0.13f));
    const float colW = std::max(90.f, (W - 6 * gap - 3 * side) / 2.0f);
    const std::vector<cf32> none;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    squarePlot("SIC cells", "##cdr_si", on ? t.siConst : none, side, 1.6);
    ImGui::SameLine(0, gap);
    squarePlot("SDC cells", "##cdr_sdc", on ? t.sdcConst : none, side, 1.6);
    ImGui::SameLine(0, gap);
    squarePlot("MSD cells", "##cdr_msc", on ? t.mscConst : none, side, 1.6);
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Channel response (kHz)");
    if (plt::BeginPlot("##cdr_tf", ImVec2(colW, plotH), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes(nullptr, nullptr, 0, 0);
        const int n = (int)std::min(t.chanDb.size(), t.chanKhz.size());
        if (on && n > 1) {
            float lo = 1e9f, hi = -1e9f;
            for (int i = 0; i < n; i++) { lo = std::min(lo, t.chanDb[(size_t)i]); hi = std::max(hi, t.chanDb[(size_t)i]); }
            plt::SetupAxisLimits(plt::X1, t.chanKhz.front(), t.chanKhz[(size_t)n - 1], plt::Cond_Always);
            plt::SetupAxisLimits(plt::Y1, lo - 5, std::max(hi, lo + 10.f) + 5, plt::Cond_Always);
            plt::Spec sp; sp.LineColor = pal::accent(); sp.LineWeight = 1.2f;
            plt::PlotScatter("H", t.chanKhz.data(), t.chanDb.data(), n, sp);
        } else { plt::SetupAxisLimits(plt::X1, -100, 100, plt::Cond_Once); plt::SetupAxisLimits(plt::Y1, -20, 10, plt::Cond_Once); }
        plt::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "SNR (dB)");
    historyPlot("##cdr_snr", "dB", S.snr, ImVec2(colW, plotH));
    ImGui::EndGroup();
}

// ---------------------------------------------------------------- status bar and summary

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::CdrTelemetry& t = a.rx.cdr;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    const double now = ImGui::GetTime();
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Signal", !on ? 0 : t.state >= 1 ? 1 : t.levelDb > -90 ? 2 : 0); flowNext(12 * gUi);
    lamp("Sync", !on ? 0 : t.state >= 1 ? 1 : 0); flowNext(12 * gUi);
    lamp("SIC", !on ? 0 : blockLamp(S.siOkAt, S.siBadAt, 2.0, now)); flowNext(12 * gUi);
    lamp("SDC", !on ? 0 : blockLamp(S.sdcOkAt, S.sdcBadAt, 4.0, now)); flowNext(12 * gUi);
    lamp("Data", !on ? 0 : blockLamp(S.okAt, S.badAt, 3.0, now)); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[80];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    if (t.state == 0) { ro("State", "searching"); snprintf(b, sizeof b, "%.0f dBFS", t.levelDb); ro("Level", b); return; }
    snprintf(b, sizeof b, "%d / %d", t.tm, t.sm); ro("Mode", b);
    snprintf(b, sizeof b, "%.1f dB", t.snrDb); ro("SNR", b);
    snprintf(b, sizeof b, "%+.1f Hz", t.cfoHz); ro("CFO", b);
    snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro("LDPC ok/bad", b);
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "CDR";
    if (live(a)) l2 = dect2::cdrSummary(a.rx.cdr);
}

// ---------------------------------------------------------------- the Receiver tab

void receiver(App& a) {
    const dect2::CdrTelemetry& t = a.rx.cdr;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    ImGui::BeginChild("##cdr_rcv", ImVec2(0, 0));
    char b[200];
    auto kv = [&](const char* k, const std::string& v) { kvRow(a, k, v, 150); };
    auto kf = [&](const char* k, const char* fmt, auto... v) { snprintf(b, sizeof b, fmt, v...); kvRow(a, k, b, 150); };
    sectionHeader(Ic::Chip, "Receiver");
    kv("state", dect2::cdrStateText(t.state));
    kv("status", t.status.empty() ? "-" : t.status);
    kf("input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kf("signal time", "%.1f s", t.timeSec);
    sectionHeader(Ic::Wave, "Physical layer");
    kf("transmission mode", "%d", t.tm);
    kf("spectrum mode", "%d  (NI %d, %d .. %d kHz from the centre)", t.sm, t.ni, t.innerKhz, t.outerKhz);
    kf("beacon correlation", "%.2f", t.syncMetric);
    kf("sample clock offset", "%+.1f ppm", t.timingDriftPpm);
    kf("SNR", "%.1f dB", t.snrDb);
    kf("carrier offset", "%+.2f Hz", t.cfoHz);
    kf("sub-frames", "%llu", (unsigned long long)t.subframes);
    sectionHeader(Ic::Layers, "System information");
    if (t.siValid) {
        kf("frame / sub-frame", "%d / %d", t.frame, t.subframe);
        kf("allocation", "mode %d", t.alloc);
        kf("service description", "%s", modName(t.sdiMod));
        kf("service data", "%s, LDPC rate %s%s", modName(t.msdMod), rateText(t.rate), t.uniform ? "" : " (unequal)");
        kf("hierarchy", "%d", t.hier);
        kf("nominal frequency", "%d kHz", t.nominalKhz);
        kv("multi-frequency", t.multiFreq ? "signalled" : "no");
    } else kv("system information", "-");
    kf("SIC blocks", "%llu ok, %llu bad", (unsigned long long)t.siOk, (unsigned long long)t.siBad);
    sectionHeader(Ic::Layers, "Multiplex");
    kf("control frames", "%llu ok, %llu bad", (unsigned long long)t.sdcOk, (unsigned long long)t.sdcBad);
    kf("control tables", "%llu ok, %llu bad", (unsigned long long)t.tablesOk, (unsigned long long)t.tablesBad);
    kf("service frames", "%llu ok, %llu bad", (unsigned long long)t.muxOk, (unsigned long long)t.muxBad);
    kf("logical frames", "%llu", (unsigned long long)t.logicalFrames);
    kf("LDPC code words", "%llu ok, %llu bad  (%d per logical frame, %.1f iterations)", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.codewords, t.ldpcIterations);
    kf("capacity", "%d bytes per service multiplex frame", t.capacityBytes);
    kv("network", t.network.empty() ? "-" : t.network);
    kf("network id", "%llX", (unsigned long long)t.networkId);
    kv("country", t.country.empty() ? "-" : t.country);
    {
        std::string f;
        for (double m : t.freqsMhz) { snprintf(b, sizeof b, "%s%.3f MHz", f.empty() ? "" : ", ", m); f += b; }
        kv("frequencies", f.empty() ? "-" : f);
    }
    if (!t.otherTables.empty()) {
        std::string o;
        for (int x : t.otherTables) { snprintf(b, sizeof b, "%s0x%02X", o.empty() ? "" : ", ", x); o += b; }
        kv("tables not decoded", o);
    }
    sectionHeader(Ic::Radio, "Services");
    if (t.services.empty()) ImGui::TextDisabled("none yet");
    for (const auto& s : t.services) {
        ImGui::PushFont(a.mono, 0); ImGui::Text("%04X", s.id); ImGui::PopFont();
        ImGui::SameLine(0, 10 * gUi);
        ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s, %.1f kbit/s share, %s", s.audio ? "audio" : "data", s.kbps, serviceFacts(s).c_str()); ImGui::PopTextWrapPos();
    }
    sectionHeader(Ic::Warning, "Not supported");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s.", kDraNote);
    ImGui::TextDisabled("The electronic service guide (service names, programme types), hierarchical modulation and unequal protection are not decoded.");
    for (const auto& n : t.notes) ImGui::TextColored(kWarn, "%s", n.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- the test signal

bool combo(const char* id, float w, const char* const* items, int n, int cur, int& out) {
    bool ch = false;
    float tw = 0;
    for (int i = 0; i < n; i++) tw = std::max(tw, ImGui::CalcTextSize(items[i]).x);
    ImGui::SetNextItemWidth(std::max(w * gUi, tw + ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().FramePadding.x));
    if (ImGui::BeginCombo(id, items[std::max(0, std::min(n - 1, cur))])) {
        for (int i = 0; i < n; i++) if (ImGui::Selectable(items[i], cur == i)) { out = i; ch = true; }
        ImGui::EndCombo();
    }
    return ch;
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    static const char* tm[] = {"mode 1", "mode 1 ", "mode 2", "mode 3"};
    static const char* sm[] = {"spectrum 1", "spectrum 1 ", "spectrum 2", "spectrum 9", "spectrum 10", "spectrum 22", "spectrum 23"};
    static const char* mod[] = {"QPSK", "16QAM", "64QAM"};
    static const char* rate[] = {"rate 3/4", "rate 1/4", "rate 1/3", "rate 1/2", "rate 3/4 "};
    static const char* sdi[] = {"SDC QPSK", "SDC 16QAM", "SDC 64QAM"};
    static const char* al[] = {"allocation 1", "allocation 1 ", "allocation 2", "allocation 3"};
    int v = 0;
    ImGui::TextDisabled("test signal");
    flowNext();
    if (combo("##ct", 70, tm, 4, sc.modeOpt[0], v)) { sc.modeOpt[0] = v; changed = true; } flowNext(6 * gUi);
    if (combo("##cs", 100, sm, 7, sc.modeOpt[1], v)) { sc.modeOpt[1] = v; changed = true; } flowNext(6 * gUi);
    if (combo("##cm", 70, mod, 3, sc.modeOpt[2], v)) { sc.modeOpt[2] = v; changed = true; } flowNext(6 * gUi);
    if (combo("##cr", 80, rate, 5, sc.modeOpt[3], v)) { sc.modeOpt[3] = v; changed = true; } flowNext(6 * gUi);
    if (combo("##cd", 90, sdi, 3, sc.modeOpt[4], v)) { sc.modeOpt[4] = v; changed = true; } flowNext(6 * gUi);
    if (combo("##ca", 100, al, 4, sc.modeOpt[5], v)) { sc.modeOpt[5] = v; changed = true; } flowNext(8 * gUi);
    bool fm = sc.modeOpt[6] == 0, text = sc.modeOpt[7] == 0;
    if (ImGui::Checkbox("FM programme", &fm)) { sc.modeOpt[6] = fm ? 0 : 1; changed = true; } flowNext(8 * gUi);
    if (ImGui::Checkbox("text service", &text)) { sc.modeOpt[7] = text ? 0 : 1; changed = true; }
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const dect2::CdrTelemetry& t = a.rx.cdr;
    const uint64_t n = t.blocksOk + t.blocksBad;
    const double okPct = n ? 100.0 * (double)t.blocksOk / (double)n : 0;
    out.push_back({"SNR  dB", "%.1f", t.snrDb, 0, 40, t.state == 0 ? 0 : t.snrDb >= 15 ? 1 : t.snrDb >= 6 ? 2 : 3});
    out.push_back({"LDPC OK  %", "%.0f", okPct, 0, 100, n == 0 ? 0 : okPct > 99 ? 1 : okPct > 90 ? 2 : 3});
    out.push_back({"LEVEL  dBFS", "%.0f", t.levelDb, -100, 0, t.levelDb > -6 ? 3 : 0});
}

} // namespace

extern const ModeUi kCdrUi;
const ModeUi kCdrUi = {
    .sideTitle = "STATIONS",
    .tabName = "Radio",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .stream = false,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
