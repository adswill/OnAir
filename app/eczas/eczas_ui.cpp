// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// e-Czas screens (Poland's national time service, 225 kHz long wave, GUM): the time and date of the last time packet with the local offset, the
// summer-time and leap-second flags, the transmitter state, lock / sync / time lamps, the carrier level and SNR, the computer clock against the
// received time (and its history), the frame counters with the error counts, the phase plot of the carrier and the signal the bit slicer sees,
// and the frame log. Long wave needs a radio that tunes to 225 kHz (an SDRplay, Airspy HF+, an RTL-SDR in direct sampling mode, or an upconverter
// in front of an HF radio): a note says so when the radio chosen cannot.
#include "app.h"
#include "dect2/eczas/eczas_live.h"
#include "dect2/eczas/eczas_proto.h"
#include "dect2/eczas/eczas_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                 // 0 time, 1 signal, 2 frames
    bool loaded = false;
    bool audio = false;           // the file is a recording of the radio's audio (a BFO recording), not IQ
    int tone = 1000;              // the carrier in that audio, Hz
    int pushedAudio = -1, pushedTone = -1;
};
State S;

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.audio = plat::prefs().getB("eczasAudio", false);
    S.tone = (int)std::max(50L, std::min(20000L, (long)plat::prefs().getI("eczasTone", 1000)));
}
void saveState() {
    plat::prefs().setB("eczasAudio", S.audio);
    plat::prefs().setI("eczasTone", S.tone);
    plat::prefs().flush();
}

bool live(const App& a) { return a.rx.standard == 86; }   // the engine reports its standard code minus one

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::string dateText(int64_t sec, bool date = true) {
    int y, mo, d, h, mi, s;
    dect2::eczas::civilFromUnix(sec, y, mo, d, h, mi, s);
    char b[48];
    if (date) snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
    else snprintf(b, sizeof b, "%02d:%02d:%02d", h, mi, s);
    return b;
}

const char* weekdayName(int64_t sec) {
    static const char* wd[] = {N_("Sunday"), N_("Monday"), N_("Tuesday"), N_("Wednesday"), N_("Thursday"), N_("Friday"), N_("Saturday")};
    int64_t days = sec / 86400;
    if (sec % 86400 < 0) days--;
    return wd[(int)(((days + 4) % 7 + 7) % 7)];
}

// The radio cannot tune 225 kHz (it is below what most SDRs reach): the same range logic as the radio picker's warning, with what to do about it
std::string rangeNote(const App& a) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return "";
    const std::string w = radioRangeWarning(a.devices[(size_t)a.devIdx], a.freqMhz, &a.tune.radio);
    if (w.empty()) return "";
    return std::string(tr("Your radio cannot tune 225 kHz.")) + " " + w + " " +
           tr("Long wave needs an SDRplay, an Airspy HF+, an RTL-SDR in direct sampling mode, or an upconverter in front of a radio that starts at a few MHz (set it under the radio's settings), "
              "and a long wire or a ferrite rod antenna. A BFO recording of the radio's audio also plays: tick Audio, not IQ in the Receiver tab.");
}

void notes(const App& a) {
    const std::string n = rangeNote(a);
    if (!n.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::warnAmber(), "%s", n.c_str()); ImGui::PopTextWrapPos(); }
}

void plot(const char* id, const char* label, const std::vector<float>& v, float lo, float hi, const char* unit) {
    if (v.size() < 2) return;
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine();
    float last = NAN;
    for (auto it = v.rbegin(); it != v.rend(); ++it) if (!std::isnan(*it)) { last = *it; break; }
    if (!std::isnan(last)) ImGui::Text("%.1f %s", last, unit);
    ImGui::PlotLines(id, v.data(), (int)v.size(), 0, nullptr, lo, hi, ImVec2(ImGui::GetContentRegionAvail().x, 46 * gUi));
}

void minMax(const std::vector<float>& v, float& lo, float& hi, float pad) {
    lo = 1e30f; hi = -1e30f;
    for (float x : v) if (!std::isnan(x)) { lo = std::min(lo, x); hi = std::max(hi, x); }
    if (lo > hi) { lo = 0; hi = 1; }
    lo -= pad; hi += pad;
}

// the carrier's phase: points of the last two seconds (one per 8 ms) around a unit circle; the two keying states sit 36 degrees apart
void phasePlot(const dect2::EczasTelemetry& t) {
    const float side = std::min(ImGui::GetContentRegionAvail().x, 190 * gUi);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(side, side));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p0.x + side * 0.5f, p0.y + side * 0.5f);
    const float r = side * 0.36f;
    const ImU32 grid = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.35f);
    dl->AddRectFilled(p0, ImVec2(p0.x + side, p0.y + side), ImGui::GetColorU32(ImGuiCol_FrameBg));
    dl->AddCircle(c, r, grid, 48, 1.f);
    dl->AddLine(ImVec2(p0.x, c.y), ImVec2(p0.x + side, c.y), grid);
    dl->AddLine(ImVec2(c.x, p0.y), ImVec2(c.x, p0.y + side), grid);
    const float a36 = 36.f * 3.14159265f / 180;
    for (float a : {0.f, a36}) dl->AddLine(c, ImVec2(c.x + r * 1.15f * std::cos(a), c.y - r * 1.15f * std::sin(a)), ImGui::GetColorU32(ImGuiCol_PlotLines, 0.5f));
    const size_t n = std::min(t.constelI.size(), t.constelQ.size());
    for (size_t i = 0; i < n; i++) {
        const float f = (float)i / (float)std::max<size_t>(1, n - 1);
        const ImU32 col = ImGui::GetColorU32(ImVec4(0.45f, 0.78f, 0.98f, 0.15f + 0.8f * f));
        dl->AddCircleFilled(ImVec2(c.x + t.constelI[i] * r, c.y - t.constelQ[i] * r), 2.0f * gUi, col);
    }
}

// what the bit slicer sees (the phase change over each 20 ms bit): +-1 is a full 36 degree step, the dashed lines are half of it
void tracePlot(const dect2::EczasTelemetry& t) {
    const ImVec2 sz(ImGui::GetContentRegionAvail().x, 74 * gUi);
    std::vector<float> v(t.trace.size());
    for (size_t i = 0; i < v.size(); i++) v[i] = (float)t.trace[i] / 16384.f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PlotLines("##ecztr", v.data(), (int)v.size(), 0, nullptr, -1.5f, 1.5f, sz);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (float lvl : {-0.5f, 0.5f}) {
        const float y = p0.y + sz.y * (0.5f - lvl / 3.f);
        for (float x = p0.x; x < p0.x + sz.x; x += 8 * gUi) dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + 4 * gUi, p0.x + sz.x), y), ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.5f));
    }
}

const char* stateText(const dect2::EczasTelemetry& t) {
    return t.state == 2 ? (t.confirmed ? tr("time decoded, two packets agree") : tr("time decoded (one packet)"))
         : t.state == 1 ? tr("carrier found, waiting for a time packet") : tr("searching for the carrier");
}

void timeView(App& a) {
    notes(a);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to read the time service")); return; }
    const dect2::EczasTelemetry& t = a.rx.eczas;
    ImGui::PushFont(a.mono, 30 * gUi);
    if (t.timeValid) ImGui::TextColored(t.confirmed ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s UTC", dateText((int64_t)t.utcNow).c_str());
    else ImGui::TextDisabled("----.--.-- --:--:-- UTC");
    ImGui::PopFont();
    kvRow(a, tr("state"), "%s", stateText(t));
    if (t.timeValid) {
        const dect2::eczas::TimePacket& p = t.pkt;
        const int64_t now = (int64_t)t.utcNow;
        kvRow(a, tr("local time"), "%s, %s (UTC+%d)", dateText(now + 3600 * p.tzHours).c_str(), tr(weekdayName(now + 3600 * p.tzHours)), p.tzHours);
        kvRow(a, tr("last packet"), "%.0f s ago, stamp %s UTC", t.ageSec, dateText(p.utcSec).c_str());
        kvRow(a, tr("summer time"), "%s", p.dstChangeSoon ? tr("the local time changes next Sunday at 01:00 UTC (summer / standard time)") : tr("no change announced"));
        kvRow(a, tr("leap second"), "%s", p.leapAnnounce ? (p.leapSubtract ? tr("one is to be removed at the end of the quarter") : tr("one is to be added at the end of the quarter")) : tr("none announced"));
        kvRow(a, tr("transmitter"), "%s (Solec Kujawski)", dect2::eczas::txStateText(p.txState));
    }
    ImGui::Spacing();
    if (t.clockValid) {
        const double o = t.clockOffsetAvgMs;
        const ImVec4 c = std::fabs(o) < 50 ? pal::okGreen() : std::fabs(o) < 500 ? pal::warnAmber() : pal::badRed();
        ImGui::TextDisabled("%s", tr("computer clock")); kvColumn(130 * gUi);
        ImGui::PushFont(a.mono, 0);
        ImGui::TextColored(c, "%+.1f ms %s", o, o >= 0 ? tr("ahead") : tr("behind"));
        ImGui::PopFont();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("The computer clock when each time packet began, minus the stamp in the packet and the path delay, averaged over the last packets. It includes the "
                                       "radio's buffering (the earliest arrival is used). The frame timing of the service is from its description (GUM, v1.0) and not yet checked "
                                       "against a signal with a known clock: use it as an indication."));
        kvRow(a, tr("last packet"), "%+.1f ms", t.clockOffsetMs);
    } else {
        kvRow(a, tr("computer clock"), "%s", t.timeValid ? tr("nothing to compare (a recording)") : tr("once the time is decoded"));
    }
    kvRow(a, tr("path"), "%.0f km from Solec Kujawski, %.2f ms%s", t.distanceKm, t.propDelayMs, t.locationSet ? "" : tr("; set your position in ADS-B (Receiver position) for the right delay"));
    ImGui::Spacing();
    float lo, hi;
    minMax(t.offsetHist, lo, hi, 5);
    plot("##eczo", tr("clock offset"), t.offsetHist, lo, hi, "ms");
    plot("##eczs", "SNR", t.snrHist, 0, 50, "dB");
    minMax(t.levelHist, lo, hi, 3);
    plot("##eczl", tr("level"), t.levelHist, lo, hi, "dBFS");
}

void signalView(App& a) {
    notes(a);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the signal")); return; }
    const dect2::EczasTelemetry& t = a.rx.eczas;
    kvRow(a, tr("carrier"), "%.1f dBFS, SNR %.1f dB in 100 Hz, %+.2f Hz from the dial", t.levelDb, t.snrDb, t.cfoHz);
    ImGui::TextDisabled("%s", tr("phase of the carrier"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The last two seconds of the carrier's phase, one point per 8 ms, newest brightest. The service keys the phase between two states 36 degrees apart (50 bit/s); a clean signal shows two tight clusters."));
    phasePlot(t);
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("signal at the bit slicer"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The phase change over each 20 ms bit: a step of +-1 is a bit that changes the keying state, 0 one that does not; the slicer decides at half (dashed). The last two seconds."));
    tracePlot(t);
}

void framesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the frames")); return; }
    const dect2::EczasTelemetry& t = a.rx.eczas;
    if (ImGui::Button(tr("Copy"))) {
        std::string txt;
        for (const auto& l : t.log) txt += dateText(l.wallTime, false) + "\t" + l.hex + "\t" + l.text + "\n";
        ImGui::SetClipboardText(txt.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear"))) a.engine.eczas().clearResults();
    ImGui::SameLine();
    ImGui::TextDisabled("%llu syncs, %llu frames, %llu time packets, %llu CRC failures, %llu other slots, %llu false syncs",
                        (unsigned long long)t.syncs, (unsigned long long)t.frames, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
                        (unsigned long long)t.otherType, (unsigned long long)t.falseSync);
    if (!ImGui::BeginTable("##eczlog", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr(dect2::timefmt::headComputer())); ImGui::TableSetupColumn(tr("Frame"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(tr("Result"));
    ImGui::TableHeadersRow();
    for (const auto& l : t.log) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dateText(l.wallTime, false).c_str());
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(l.hex.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn();
        if (l.ok) ImGui::TextColored(pal::okGreen(), "%s", l.text.c_str());
        else if (l.kind == 2) ImGui::TextDisabled("%s", l.text.c_str());
        else ImGui::TextColored(pal::warnAmber(), "%s", l.text.c_str());
    }
    ImGui::EndTable();
    if (t.log.empty()) ImGui::TextDisabled("%s", tr("no frame yet: a time packet comes every few slots of the minute frame"));
}

// ---------------------------------------------------------------- hooks

void pushSettings(App& a) {
    double lat, lon;
    receiverLocation(lat, lon);
    a.engine.eczas().setReceiverLocation(lat, lon, receiverLocationSet());
    if ((int)S.audio != S.pushedAudio || S.tone != S.pushedTone) { a.engine.eczas().setAudioInput(S.audio, S.tone); S.pushedAudio = S.audio; S.pushedTone = S.tone; }
}

void tick(App& a) { loadState(); pushSettings(a); }

void tab(App& a) {
    subNav("ecz", S.view, {tr("Time"), tr("Signal"), tr("Frames")});
    if (S.view == 0) timeView(a);
    else if (S.view == 1) signalView(a);
    else framesView(a);
}

void list(App& a) {
    notes(a);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::EczasTelemetry& t = a.rx.eczas;
    ImGui::TextUnformatted("e-Czas 225 kHz");
    ImGui::PushFont(a.mono, 0);
    ImGui::TextUnformatted(t.timeValid ? dateText((int64_t)t.utcNow + 3600 * t.pkt.tzHours, false).c_str() : "--:--:--");
    if (t.clockValid) ImGui::Text("%+.1f ms", t.clockOffsetAvgMs);
    ImGui::PopFont();
}

void receiver(App& a) {
    loadState();
    notes(a);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::EczasTelemetry& t = a.rx.eczas;
    kvRow(a, tr("state"), "%s", dect2::eczasSummary(t).c_str());
    kvRow(a, tr("carrier"), "%.1f dBFS, SNR %.1f dB, offset %+.2f Hz", t.levelDb, t.snrDb, t.cfoHz);
    kvRow(a, tr("frames"), "%llu syncs, %llu frames, %llu time packets, %llu CRC failures, %llu other slots, %llu false syncs",
          (unsigned long long)t.syncs, (unsigned long long)t.frames, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
          (unsigned long long)t.otherType, (unsigned long long)t.falseSync);
    kvRow(a, tr("input"), "%.3f Msps%s", t.inputRate / 1e6, t.audioInput ? tr(", audio recording") : "");
    kvRow(a, tr("signal time"), "%.1f s", t.timeSec);
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Recording of the radio's audio"));
    if (ImGui::Checkbox(tr("Audio, not IQ"), &S.audio)) saveState();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The file is a recording of a receiver's audio (SSB / CW) tuned a little off 225 kHz, so the carrier is a tone (a WAV with the audio on the left channel and silence on the right; a mono WAV: eczastool rx reads it directly). Not for IQ files or radios."));
    if (S.audio) {
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::InputInt(tr("carrier tone Hz"), &S.tone, 10, 100)) { S.tone = std::max(50, std::min(20000, S.tone)); saveState(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The pitch of the carrier in the recording: 1000 Hz when the radio's dial was 224 kHz (upper sideband)."));
    }
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Not used: the Reed-Solomon bytes of the frame (a frame with a wrong check byte is dropped, not repaired)."));
    ImGui::PopTextWrapPos();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Carrier"), !on ? 0 : a.rx.eczas.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Sync"), !on ? 0 : a.rx.eczas.frames > 0 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Time"), !on ? 0 : a.rx.eczas.dataValid ? (a.rx.eczas.confirmed ? 1 : 2) : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::eczasSummary(a.rx.eczas).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "e-Czas 225 kHz";
    if (live(a)) l2 = dect2::eczasSummary(a.rx.eczas);
}

void synth(App&, bool&) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("The test signal carries the computer's time with a time packet every 12 s: the clock offset should stay near zero (the simulator's own buffering shows in it)."));
    ImGui::PopTextWrapPos();
}

} // namespace

extern const ModeUi kEczasUi;
const ModeUi kEczasUi = {
    .sideTitle = "TIME",
    .tabName = "e-Czas",
    .tabIcon = Ic::Clock,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .synth = synth,
    .tick = tick,
};
