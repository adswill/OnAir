// DRM screens: the Radio tab (now playing, text message, time, signal summary), the services list, status lamps, the analysis plots (FAC / SDC / MSC cells,
// channel, SNR), the Receiver tab with all telemetry and the options of the test signal.
#include "app.h"
#include <cmath>
#include <deque>

namespace {

constexpr int kStd = 11;   // RxTelemetry::standard of DRM (the tuning table says 12, the engine reports 11)

struct State {
    bool wasRunning = false;
    float pushedVol = -1;
    bool pushedMute = false;
    int sel = -1;                     // short id the user picked, -1: the first audio service
    int pushedSel = -2;
    uint64_t lastSeq = 0;
    std::deque<float> snr, audio;     // SNR of the cells and share of good audio frames, a point per report
    uint64_t prevFacOk = 0, prevFacBad = 0, prevSdcOk = 0, prevSdcBad = 0, prevAudOk = 0, prevAudBad = 0;
    double facOkAt = -1e9, facBadAt = -1e9, sdcOkAt = -1e9, sdcBadAt = -1e9, audOkAt = -1e9, audBadAt = -1e9;   // when the last good / bad block was counted
};
State S;

bool live(const App& a) { return a.engine.running() && a.rx.standard == kStd; }

void push(std::deque<float>& d, float v) { d.push_back(v); if (d.size() > 600) d.pop_front(); }

const ImVec4 kGood(0.40f, 0.85f, 0.50f, 1), kWarn(0.95f, 0.60f, 0.25f, 1), kBad(0.95f, 0.40f, 0.35f, 1);

// ---- text helpers

const char* modeText(int m) { static const char* n[] = {"A", "B", "C", "D", "E"}; return m >= 0 && m < 5 ? n[m] : "-"; }

std::string khz(float k) { char b[24]; snprintf(b, sizeof b, "%g kHz", (double)k); return b; }

std::string upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }

std::string serviceName(const DrmService& s) {
    if (!s.label.empty()) return s.label;
    char b[40]; snprintf(b, sizeof b, "service %d (%06X)", s.shortId + 1, s.id & 0xFFFFFF); return b;
}

std::string serviceFacts(const DrmService& s) {
    std::string r = s.audio ? (s.programmeName.empty() ? "audio" : s.programmeName) : "data";
    if (!s.languageName.empty() && s.languageName != "-") r += ", " + s.languageName;
    if (!s.country.empty()) r += ", " + upper(s.country);
    return r;
}

const DrmService* playing(const DrmTelemetry& t) {
    for (const auto& s : t.services) if (s.shortId == t.serviceAudioSelected) return &s;
    for (const auto& s : t.services) if (s.audio) return &s;
    return t.services.empty() ? nullptr : &t.services[0];
}

// 0 Sunday: the SDC gives a date, not a weekday
const char* weekday(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    static const char* n[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    if (m < 1 || m > 12) return "";
    if (m < 3) y--;
    return n[(y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7];
}

std::string timeText(const DrmTelemetry& t) {
    if (!t.timeValid) return "-";
    char b[96];
    snprintf(b, sizeof b, "%s %04d-%02d-%02d %02d:%02d UTC", weekday(t.year, t.month, t.day), t.year, t.month, t.day, t.hour, t.minute);
    std::string r = b;
    if (t.hasLocalOffset) {
        const int o = t.localOffsetHalfHours, m = std::abs(o) * 30;
        snprintf(b, sizeof b, "  (local %c%d:%02d)", o < 0 ? '-' : '+', m / 60, m % 60);
        r += b;
    }
    return r;
}

const char* stateText(int s) { return s == 2 ? "decoding" : s == 1 ? "synchronised, reading the FAC" : "searching"; }

// one lamp from the times of the last good and bad block: green while good ones keep coming, amber when bad ones mix in, red when only bad ones come
int blockLamp(double okAt, double badAt, double window, double now) {
    const bool ok = now - okAt < window, bad = now - badAt < window;
    return ok && bad ? 2 : ok ? 1 : bad ? 3 : 0;
}

// ---------------------------------------------------------------- tick

void tick(App& a) {
    const bool run = a.engine.running();
    if (run && a.rx.standard == kStd) {
        if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
            a.engine.drm().setVolume(a.volume);
            a.engine.drm().setMuted(a.muted);
            S.pushedVol = a.volume; S.pushedMute = a.muted;
        }
    }
    if (run && (!S.wasRunning || S.sel != S.pushedSel)) { a.engine.drm().selectService(S.sel); S.pushedSel = S.sel; }
    S.wasRunning = run;
    if (!live(a) || a.rx.seq == S.lastSeq) return;
    S.lastSeq = a.rx.seq;
    const DrmTelemetry& t = a.rx.drm;
    const double now = ImGui::GetTime();
    auto count = [&](uint64_t cur, uint64_t& prev, double& at) { if (cur < prev) prev = 0; if (cur > prev) at = now; prev = cur; };
    count(t.facOk, S.prevFacOk, S.facOkAt); count(t.facBad, S.prevFacBad, S.facBadAt);
    count(t.sdcOk, S.prevSdcOk, S.sdcOkAt); count(t.sdcBad, S.prevSdcBad, S.sdcBadAt);
    const uint64_t okBefore = S.prevAudOk, badBefore = S.prevAudBad;
    count(t.blocksOk, S.prevAudOk, S.audOkAt); count(t.blocksBad, S.prevAudBad, S.audBadAt);
    if (t.snrDb > 0 || t.state > 0) push(S.snr, t.snrDb);
    const uint64_t dOk = S.prevAudOk >= okBefore ? S.prevAudOk - okBefore : 0, dBad = S.prevAudBad >= badBefore ? S.prevAudBad - badBefore : 0;
    if (dOk + dBad > 0) push(S.audio, 100.f * (float)dOk / (float)(dOk + dBad));
}

// ---------------------------------------------------------------- the Radio tab

void kvRow(const App& a, const char* k, const std::string& v, float x = 110.f) {
    ImGui::TextDisabled("%s", k); ImGui::SameLine(x * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(v.c_str()); ImGui::PopFont();
}

void nowPlaying(App& a, float w, float h) {
    const DrmTelemetry& t = a.rx.drm;
    const bool on = live(a);
    const DrmService* s = on ? playing(t) : nullptr;
    ImGui::BeginChild("##drm_now", ImVec2(w, h), ImGuiChildFlags_Borders);
    if (!on || !s) {
        ImGui::PushFont(a.ui, 22.f);
        ImGui::TextUnformatted(!a.engine.running() ? "stopped" : t.mode >= 0 ? "reading the multiplex" : "searching for a DRM signal");
        ImGui::PopFont();
        ImGui::TextDisabled("%s", a.engine.running() ? (t.status.empty() ? "" : t.status.c_str()) : "Start the receiver. Test signal: pick the synthetic source.");
        ImGui::EndChild();
        return;
    }
    ImGui::PushFont(a.ui, 26.f);
    ImGui::TextColored(pal::dev() ? pal::accent() : ImVec4(0.55f, 0.80f, 1.f, 1), "%s", serviceName(*s).c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("%s", s->audio ? "audio service" : "data service (not decoded)");
    ImGui::Spacing();
    kvRow(a, "programme", s->programmeName.empty() ? "-" : s->programmeName);
    kvRow(a, "language", s->languageName.empty() ? "-" : s->languageName);
    kvRow(a, "country", s->country.empty() ? "-" : upper(s->country));
    char b[64];
    snprintf(b, sizeof b, "%06X", s->id & 0xFFFFFF);
    kvRow(a, "service id", b);
    kvRow(a, "audio", s->codecText.empty() ? "-" : s->codecText);
    if (s->bitrateBps > 0) { snprintf(b, sizeof b, "%.1f kbit/s", s->bitrateBps / 1000.0); kvRow(a, "stream rate", b); }
    ImGui::TextDisabled("state"); ImGui::SameLine(110 * gUi);
    const ImVec4 col = t.audioState == 2 ? kGood : t.audioState == 1 ? kWarn : ImVec4(0.62f, 0.65f, 0.68f, 1);
    ImGui::TextColored(col, "%s", t.audioState == 2 ? "playing" : t.audioState == 1 ? "frames received, cannot decode" : s->audio ? "waiting for audio" : "no audio");
    ImGui::PushTextWrapPos(0);
    if (!t.audioInfo.empty() && t.audioState != 0 && t.audioInfo != s->codecText) ImGui::TextDisabled("%s", t.audioInfo.c_str());
    if (t.hierarchicalUnsupported) ImGui::TextColored(kWarn, "The signal uses a hierarchical mapping, which this receiver does not decode.");
    if (s->conditionalAccess) ImGui::TextColored(kWarn, "Conditional access is flagged: the stream may be scrambled.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    sectionHeader(Ic::Radio, "Text message");
    ImGui::PushTextWrapPos(0);
    if (!s->textMessage) ImGui::TextDisabled("this service sends none");
    else if (t.textMessage.empty()) ImGui::TextDisabled("waiting for the first segment");
    else ImGui::TextUnformatted(t.textMessage.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

void signalPanel(App& a, float w, float h) {
    const DrmTelemetry& t = a.rx.drm;
    const bool on = live(a) && t.mode >= 0;
    ImGui::BeginChild("##drm_sig", ImVec2(w, h), ImGuiChildFlags_Borders);
    sectionHeader(Ic::Wave, "Signal");
    char b[96];
    kvRow(a, "mode", on ? std::string("robustness ") + modeText(t.mode) : "-", 100);
    kvRow(a, "occupancy", on && t.occupancy >= 0 ? khz(t.bandwidthKhz) : "-", 100);
    kvRow(a, "interleaver", on && t.longInterleave >= 0 ? (t.longInterleave ? "long (2 s)" : "short (400 ms)") : "-", 100);
    if (on && t.mscQam) snprintf(b, sizeof b, "%d-QAM  (SDC %d-QAM)", t.mscQam, t.sdcQam); else snprintf(b, sizeof b, "-");
    kvRow(a, "MSC", b, 100);
    if (on && t.numStreams > 0) {
        const bool two = t.streamLenA[0] > 0;   // part A exists: unequal error protection
        if (two && t.protRateA > 0) snprintf(b, sizeof b, "A %d (%d%%), B %d (%d%%)", t.protA, t.protRateA, t.protB, t.protRateB);
        else if (two) snprintf(b, sizeof b, "A %d, B %d", t.protA, t.protB);
        else if (t.protRateB > 0) snprintf(b, sizeof b, "level %d (rate %d%%)", t.protB, t.protRateB);
        else snprintf(b, sizeof b, "level %d", t.protB);
    } else snprintf(b, sizeof b, "-");
    kvRow(a, "protection", b, 100);
    snprintf(b, sizeof b, on ? "%.1f dB" : "-", t.snrDb); kvRow(a, "SNR", b, 100);
    snprintf(b, sizeof b, on ? "%+.1f Hz" : "-", t.cfoHz); kvRow(a, "carrier offset", b, 108);
    ImGui::Spacing();
    sectionHeader(Ic::Clock, "Time from the signal");
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(on ? timeText(t).c_str() : "-"); ImGui::PopFont();
    ImGui::EndChild();
}

void tab(App& a) {
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 8 * gUi;
    const float sw = std::min(std::max(250.f * gUi, W * 0.34f), W * 0.5f);
    nowPlaying(a, W - sw - gap, H);
    ImGui::SameLine(0, gap);
    signalPanel(a, sw, H);
}

// ---------------------------------------------------------------- the services list

void list(App& a) {
    const DrmTelemetry& t = a.rx.drm;
    if (!live(a)) { ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see services"); return; }
    if (t.services.empty()) { ImGui::TextDisabled(t.mode >= 0 ? "reading the multiplex" : "searching for a DRM signal"); return; }
    ImGui::TextDisabled("%zu service%s, click to play", t.services.size(), t.services.size() == 1 ? "" : "s");
    const DrmService* cur = playing(t);
    for (const auto& s : t.services) {
        ImGui::PushID(s.shortId);
        const bool isCur = cur && cur->shortId == s.shortId;
        if (!s.audio) ImGui::BeginDisabled();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float lh = ImGui::GetTextLineHeightWithSpacing();
        if (ImGui::Selectable("##svc", isCur, 0, ImVec2(0, lh * 2 + 2 * gUi))) S.sel = s.shortId;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(p.x + 4 * gUi, p.y + 1), ImGui::GetColorU32(ImGuiCol_Text), serviceName(s).c_str());
        dl->AddText(ImVec2(p.x + 4 * gUi, p.y + lh + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), serviceFacts(s).c_str());
        if (!s.audio) ImGui::EndDisabled();
        if (!s.audio && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Data services are listed but not decoded.");
        if (s.audio && ImGui::IsItemHovered() && !s.codecText.empty()) ImGui::SetTooltip("%s", s.codecText.c_str());
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------- the analysis row

void squarePlot(const char* title, const char* id, const std::vector<cf32>& pts, float side, double lim) {
    ImGui::BeginGroup();
    ImGui::TextDisabled("%s (%zu)", title, pts.size());
    scatter(id, pts, ImVec2(side, side), lim, pal::accent(0.7f));
    ImGui::EndGroup();
}

void panels(App& a) {
    const DrmTelemetry& t = a.rx.drm;
    const bool on = live(a);
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi;
    const float plotH = std::max(70.f, H - ImGui::GetTextLineHeightWithSpacing() - 8);
    const float side = std::min(plotH, std::max(80.f, W * 0.13f));
    const float colW = std::max(90.f, (W - 7 * gap - 3 * side) / 3.6f);
    const ImVec2 sz(colW, plotH);
    const std::vector<cf32> none;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    squarePlot("FAC cells", "##drm_fac", on ? t.facConst : none, side, 1.6);
    ImGui::SameLine(0, gap);
    squarePlot("SDC cells", "##drm_sdc", on ? t.sdcConst : none, side, 1.6);
    ImGui::SameLine(0, gap);
    squarePlot("MSC cells", "##drm_msc", on ? t.mscConst : none, side, t.mscQam == 64 ? 1.6 : 1.8);
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("Channel response (kHz)");
    if (plt::BeginPlot("##drm_tf", ImVec2(colW, plotH), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes(nullptr, nullptr, 0, 0);
        const bool have = on && !t.chanDb.empty() && t.chanSpacingHz > 0;
        std::vector<float> x;
        if (have) {
            const int n = (int)t.chanDb.size();
            x.resize((size_t)n);
            for (int i = 0; i < n; i++) x[(size_t)i] = (float)((t.chanFirstCarrier + i * t.chanCarrierStep) * t.chanSpacingHz / 1000.0);
            float lo = 1e9f, hi = -1e9f;
            for (float v : t.chanDb) { lo = std::min(lo, v); hi = std::max(hi, v); }
            plt::SetupAxisLimits(plt::X1, x.front(), x.back(), plt::Cond_Always);
            plt::SetupAxisLimits(plt::Y1, lo - 5, std::max(hi, lo + 10.f) + 5, plt::Cond_Always);
            plt::Spec sp; sp.LineColor = pal::accent(); sp.LineWeight = 1.2f;
            plt::PlotLine("H", x.data(), t.chanDb.data(), n, sp);
        } else { plt::SetupAxisLimits(plt::X1, -5, 5, plt::Cond_Once); plt::SetupAxisLimits(plt::Y1, -20, 10, plt::Cond_Once); }
        plt::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("Impulse response (ms)");
    if (plt::BeginPlot("##drm_ir", ImVec2(colW, plotH), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes(nullptr, nullptr, 0, 0);
        if (on && !t.cirDb.empty() && t.cirStepMs > 0) {
            const int n = (int)t.cirDb.size();
            std::vector<float> x((size_t)n);
            for (int i = 0; i < n; i++) x[(size_t)i] = t.cirStartMs + i * t.cirStepMs;
            float mx = -200;
            for (float v : t.cirDb) mx = std::max(mx, v);
            plt::SetupAxisLimits(plt::X1, x.front(), x.back(), plt::Cond_Always);
            plt::SetupAxisLimits(plt::Y1, mx - 50, mx + 5, plt::Cond_Always);
            plt::Spec sp; sp.LineColor = ImVec4(0.95f, 0.65f, 0.30f, 1); sp.LineWeight = 1.2f;
            plt::PlotLine("h", x.data(), t.cirDb.data(), n, sp);
        } else { plt::SetupAxisLimits(plt::X1, -1, 5, plt::Cond_Once); plt::SetupAxisLimits(plt::Y1, -50, 5, plt::Cond_Once); }
        plt::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("SNR (dB)");
    historyPlot("##drm_snr", "dB", S.snr, ImVec2(colW * 0.8f, plotH));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("Audio frames ok (%%)");
    historyPlot("##drm_aud", "%", S.audio, ImVec2(colW * 0.8f, plotH));
    ImGui::EndGroup();
}

// ---------------------------------------------------------------- status bar and summary

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const DrmTelemetry& t = a.rx.drm;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    const double now = ImGui::GetTime();
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight() * 2.f + ImGui::GetStyle().ItemSpacing.y * 2.f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - 4, p.y - 2), ImVec2(p.x + ImGui::GetContentRegionAvail().x + 4, p.y + h), IM_COL32(22, 23, 25, 255), 3.f);
    }
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); ImGui::SameLine(0, 12 * gUi);
    lamp("Signal", !on ? 0 : t.mode >= 0 ? 1 : 0); ImGui::SameLine(0, 12 * gUi);
    lamp("Frame", !on ? 0 : t.state >= 1 ? 1 : t.mode >= 0 ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
    lamp("FAC", !on ? 0 : blockLamp(S.facOkAt, S.facBadAt, 3.0, now)); ImGui::SameLine(0, 12 * gUi);
    lamp("SDC", !on ? 0 : blockLamp(S.sdcOkAt, S.sdcBadAt, 6.0, now)); ImGui::SameLine(0, 12 * gUi);
    lamp("Audio", !on ? 0 : t.audioState == 2 ? blockLamp(S.audOkAt, S.audBadAt, 3.0, now) : t.audioState == 1 ? 2 : 0); ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("|"); ImGui::SameLine(0, 10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        ImGui::SameLine(0, 15 * gUi);
    };
    char b[80];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    if (t.mode < 0) { ro("State", "searching"); snprintf(b, sizeof b, "%.0f dBFS", t.levelDbfs); ro("Level", b); return; }
    snprintf(b, sizeof b, "%s  %s", modeText(t.mode), t.occupancy >= 0 ? khz(t.bandwidthKhz).c_str() : "-"); ro("Mode", b);
    snprintf(b, sizeof b, "%.1f dB", t.snrDb); ro("SNR", b);
    snprintf(b, sizeof b, "%+.1f Hz", t.cfoHz); ro("CFO", b);
    snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro("Audio ok/bad", b);
}

void summary(const App& a, std::string& l1, std::string& l2) {
    const DrmTelemetry& t = a.rx.drm;
    l1 = "DRM";
    if (!live(a)) return;
    char b[160];
    if (t.mode < 0) { snprintf(b, sizeof b, "searching  %.0f dBFS", t.levelDbfs); l2 = b; return; }
    const DrmService* s = playing(t);
    snprintf(b, sizeof b, "%s  mode %s %s  SNR %.0f dB", s && !s->label.empty() ? s->label.c_str() : stateText(t.state), modeText(t.mode), t.occupancy >= 0 ? khz(t.bandwidthKhz).c_str() : "", t.snrDb);
    l2 = b;
}

// ---------------------------------------------------------------- the Receiver tab

void receiver(App& a) {
    const DrmTelemetry& t = a.rx.drm;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    ImGui::BeginChild("##drm_rcv", ImVec2(0, 0));
    char b[200];
    auto kv = [&](const char* k, const std::string& v) { kvRow(a, k, v, 150); };
    auto kf = [&](const char* k, const char* fmt, auto... v) { snprintf(b, sizeof b, fmt, v...); kvRow(a, k, b, 150); };
    sectionHeader(Ic::Chip, "Receiver");
    kv("state", stateText(t.state));
    kv("status", t.status.empty() ? "-" : t.status);
    kf("level", "%.1f dBFS in the 48 kHz channel", t.levelDbfs);
    kf("quality", "%.0f %%", t.quality * 100.0);
    sectionHeader(Ic::Wave, "Signal");
    kv("robustness mode", t.mode >= 0 ? modeText(t.mode) : "-");
    kv("occupancy", t.occupancy >= 0 ? khz(t.bandwidthKhz) + "  (code " + std::to_string(t.occupancy) + ")" : "-");
    kv("interleaver", t.longInterleave < 0 ? "-" : t.longInterleave ? "long (2 s)" : "short (400 ms)");
    kf("MSC / SDC mapping", "%d-QAM / %d-QAM", t.mscQam, t.sdcQam);
    if (t.protRateB > 0) kf("protection", "A %d (%d%%)  B %d (%d%%)", t.protA, t.protRateA, t.protB, t.protRateB);
    else kf("protection", "level A %d, level B %d  (part A is %s)", t.protA, t.protB, t.streamLenA[0] > 0 ? "used" : "empty");
    snprintf(b, sizeof b, "%d:", t.numStreams);
    {
        std::string s = b;
        for (int i = 0; i < t.numStreams && i < 4; i++) { snprintf(b, sizeof b, " [%d] A %d B %d", i, t.streamLenA[i], t.streamLenB[i]); s += b; }
        kv("streams (bytes)", t.numStreams ? s + "  per logical frame" : "-");
    }
    kf("frame identity", "%d", t.identity);
    kv("reconfiguration", t.reconfiguration ? "announced" : "none");
    kv("time", timeText(t));
    sectionHeader(Ic::Gauge, "Measurements");
    kf("SNR", "%.1f dB (pilot MER)", t.snrDb);
    kf("carrier offset", "%+.2f Hz", t.cfoHz);
    kf("sample clock offset", "%+.1f ppm", t.sroPpm);
    kf("Doppler spread", "%.2f Hz", t.dopplerHz);
    kf("delay spread", "%.3f ms", t.delaySpreadMs);
    kf("strongest path at", "%+.3f ms", t.timingMs);
    kf("guard correlation", "%.2f", t.correlation);
    sectionHeader(Ic::Layers, "Counters");
    kf("FAC blocks", "%llu ok, %llu bad", (unsigned long long)t.facOk, (unsigned long long)t.facBad);
    kf("SDC blocks", "%llu ok, %llu bad", (unsigned long long)t.sdcOk, (unsigned long long)t.sdcBad);
    kf("multiplex frames", "%llu ok, %llu bad", (unsigned long long)t.mscFramesOk, (unsigned long long)t.mscFramesBad);
    kf("audio frames", "%llu ok, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kf("text segments", "%llu ok, %llu bad", (unsigned long long)t.textSegmentsOk, (unsigned long long)t.textSegmentsBad);
    sectionHeader(Ic::Speaker, "Audio");
    kv("decoder", t.audioState == 2 ? "decoding" : t.audioState == 1 ? "frames received, no usable decoder" : "no audio stream");
    kv("detail", t.audioInfo.empty() ? "-" : t.audioInfo);
    kf("samples out", "%llu  (%.1f s at 48 kHz)", (unsigned long long)t.audioSamples, t.audioSamples / 48000.0);
    sectionHeader(Ic::Layers, "Services");
    if (t.services.empty()) ImGui::TextDisabled("none yet");
    for (const auto& s : t.services) {
        snprintf(b, sizeof b, "%d  %s", s.shortId + 1, serviceName(s).c_str());
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(b); ImGui::PopFont();
        ImGui::SameLine(0, 10 * gUi);
        ImGui::TextDisabled("%06X  %s%s%s", s.id & 0xFFFFFF, serviceFacts(s).c_str(), s.codecText.empty() ? "" : ", ", s.codecText.c_str());
        if (s.conditionalAccess) { ImGui::SameLine(0, 6 * gUi); ImGui::TextColored(kWarn, "CA"); }
    }
    sectionHeader(Ic::Warning, "Not supported");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("AAC with SBR or parametric stereo: only the AAC core plays (no SBR for 960-sample frames in the audio library), so the sound is band-limited and mono for PS services.");
    ImGui::TextDisabled("xHE-AAC: the frames are received, but whether sound comes out depends on the audio library's USAC decoder; the state above says what happened.");
    ImGui::TextDisabled("Data services (Journaline, packet mode), alternative frequencies, hierarchical mappings and DRM+ (robustness mode E) are not decoded.");
    ImGui::TextDisabled("Below 30 MHz a HackRF needs an upconverter or a long antenna.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- the test signal

bool combo(const char* id, float w, const char* const* items, int n, int cur, int& out, const bool* disabled = nullptr) {
    bool ch = false;
    ImGui::SetNextItemWidth(w * gUi);
    if (ImGui::BeginCombo(id, items[std::max(0, std::min(n - 1, cur))])) {
        for (int i = 0; i < n; i++) {
            if (disabled && disabled[i]) ImGui::BeginDisabled();
            if (ImGui::Selectable(items[i], cur == i)) { out = i; ch = true; }
            if (disabled && disabled[i]) ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    return ch;
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    static const char* modes[] = {"mode A", "mode B", "mode C", "mode D"};
    static const char* occ[] = {"usual (10 kHz)", "4.5 kHz", "5 kHz", "9 kHz", "10 kHz", "18 kHz", "20 kHz"};
    static const char* qam[] = {"64-QAM", "16-QAM"};
    static const char* prot[] = {"usual protection", "protection 0", "protection 1", "protection 2", "protection 3"};
    static const char* il[] = {"long interleaver", "short interleaver"};
    static const char* aud[] = {"melody", "1 kHz tone", "silence", "noise-like", "not AAC"};
    static const char* chn[] = {"no channel", "AWGN", "Rice, delay", "US consortium", "CCIR poor", "channel 5", "channel 6"};
    const int mode = sc.modeOpt[0] == 0 ? 1 : std::max(0, std::min(3, sc.modeOpt[0] - 1));
    int v = 0;
    ImGui::TextDisabled("test signal");
    ImGui::SameLine();
    if (combo("##dm", 78, modes, 4, mode, v)) { sc.modeOpt[0] = v + 1; if (v >= 2 && sc.modeOpt[1] != 0 && sc.modeOpt[1] != 4 && sc.modeOpt[1] != 6) sc.modeOpt[1] = 0; changed = true; }
    ImGui::SameLine(0, 6 * gUi);
    bool dis[7] = {};
    if (mode >= 2) for (int i : {1, 2, 3, 5}) dis[i] = true;   // modes C and D only have 10 and 20 kHz
    if (combo("##do", 110, occ, 7, sc.modeOpt[1], v, dis)) { sc.modeOpt[1] = v; changed = true; }
    ImGui::SameLine(0, 6 * gUi);
    if (combo("##dq", 76, qam, 2, sc.modeOpt[2], v)) { sc.modeOpt[2] = v; changed = true; }
    ImGui::SameLine(0, 6 * gUi);
    if (combo("##dp", 120, prot, 5, sc.modeOpt[3], v)) { sc.modeOpt[3] = v; changed = true; }
    ImGui::SameLine(0, 6 * gUi);
    if (combo("##di", 130, il, 2, sc.modeOpt[4], v)) { sc.modeOpt[4] = v; changed = true; }
    ImGui::SameLine(0, 6 * gUi);
    if (combo("##da", 100, aud, 5, sc.modeOpt[5], v)) { sc.modeOpt[5] = v; changed = true; }
    ImGui::SameLine(0, 6 * gUi);
    if (combo("##dc", 110, chn, 7, sc.modeOpt[6], v)) { sc.modeOpt[6] = v; changed = true; }
    ImGui::SameLine(0, 8 * gUi);
    bool text = sc.modeOpt[7] == 0;
    if (ImGui::Checkbox("text message", &text)) { sc.modeOpt[7] = text ? 0 : 1; changed = true; }
}

// ---------------------------------------------------------------- the meter bank

void meters(const App& a, std::vector<ModeMeter>& out) {
    const DrmTelemetry& t = a.rx.drm;
    const uint64_t n = t.blocksOk + t.blocksBad;
    const double okPct = n ? 100.0 * (double)t.blocksOk / (double)n : 0;
    out.push_back({"SNR  dB", "%.1f", t.snrDb, 0, 40, t.state == 0 ? 0 : t.snrDb >= 15 ? 1 : t.snrDb >= 8 ? 2 : 3});
    out.push_back({"QUALITY  %", "%.0f", t.quality * 100.0, 0, 100, t.state == 0 ? 0 : t.quality > 0.9 ? 1 : t.quality > 0.5 ? 2 : 3});
    out.push_back({"AUDIO OK  %", "%.0f", okPct, 0, 100, n == 0 ? 0 : okPct > 95 ? 1 : okPct > 70 ? 2 : 3});
    out.push_back({"LEVEL  dBFS", "%.0f", t.levelDbfs, -100, 0, t.levelDbfs > -6 ? 3 : 0});
}

} // namespace

extern const ModeUi kDrmUi;
const ModeUi kDrmUi = {
    .sideTitle = "SERVICES",
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
