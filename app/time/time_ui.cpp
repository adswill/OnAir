// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Time signal screens: the received time in large type with the date, time zone, summer time, leap second and DUT1 flags, the computer
// clock against it (after the path delay from the shared receiver position), the seconds tick, the symbols of the minute, the decoded
// minute log, level / SNR / tick / offset histories, and the station and frequency pickers. LF stations need a radio that tunes below
// 1 MHz: a note says so when the radio chosen cannot.
#include "app.h"
#include "panels/clocksync_ui.h"
#include "dect2/time/time_proto.h"
#include "dect2/time/time_rx.h"
#include "dect2/time/time_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include "../ui/font_size.h"

namespace {
using namespace dect2::timesig;

struct State {
    int view = 0;                 // 0 time, 1 log, 2 clock
    bool loaded = false;
    int station = -1;             // -1: from the frequency
    int pref60 = Msf;
    bool listen = false; int pushedListen = -1; float pushedVol = -1; int pushedMute = -1;
    bool audio = false; int tone = 500; float clockPct = 0;   // a recording of the demodulated audio, the BPC tone in it, the recorder's clock error
    int pushedAudio = -1, pushedTone = -1; float pushedScale = -1;
};
State S;

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.station = std::max(-1, std::min((int)kNumStations - 1, (int)plat::prefs().getI("timeStation", -1)));
    if (S.station == Chu) S.station = Wwv;   // CHU removed (#150)
    S.pref60 = (int)plat::prefs().getI("timePref60", Msf);
    if (S.pref60 != Msf && S.pref60 != Wwvb && S.pref60 != Jjy60) S.pref60 = Msf;
    S.listen = plat::prefs().getB("timeListen", false);
    S.audio = plat::prefs().getB("timeAudio", false);
    S.tone = (int)std::max(50L, std::min(20000L, (long)plat::prefs().getI("timeTone", 500)));
    S.clockPct = (float)std::max(-10.0, std::min(10.0, (double)plat::prefs().getI("timeClockPermille", 0) / 10.0));
}
void saveState() {
    plat::prefs().setI("timeStation", S.station); plat::prefs().setI("timePref60", S.pref60);
    plat::prefs().setB("timeAudio", S.audio); plat::prefs().setI("timeTone", S.tone); plat::prefs().setI("timeClockPermille", (long)std::lround(S.clockPct * 10));
    plat::prefs().flush();
}

bool live(const App& a) { return a.rx.standard == 59; }   // the engine reports its standard code minus one

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::string utcText(double u, bool date = true) {
    if (u <= 0) return "-";
    const int64_t s = (int64_t)std::floor(u);
    int y, m, d;
    civilFromDays(s / 86400, y, m, d);
    const int64_t r = s % 86400;
    char b[48];
    if (date) snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", y, m, d, (int)(r / 3600), (int)(r / 60 % 60), (int)(r % 60));
    else snprintf(b, sizeof b, "%02d:%02d:%02d", (int)(r / 3600), (int)(r / 60 % 60), (int)(r % 60));
    return b;
}

std::string clock(int64_t t) { return utcText((double)t, false); }

// a station below what the radio is specified for (a HackRF starts at 1 MHz): a note, nothing is blocked; "" when it can or the range is not known
std::string rangeNote(const App& a) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return "";
    const DeviceInfo& d = a.devices[(size_t)a.devIdx];
    if (!d.isRadio() || d.minFreqHz <= 0) return "";
    if (a.freqMhz * 1e6 >= d.minFreqHz) return "";
    char b[640];
    snprintf(b, sizeof b, "This radio is specified from %.6g MHz: OnAir still tries %.4g kHz, but the driver may not reach it or the signal may be weak or absent. The LF stations (DCF77, MSF, WWVB, JJY) need a radio that "
             "tunes below 1 MHz: an SDRplay, an Airspy HF+, or an RTL-SDR in direct sampling mode, with an LF antenna (a ferrite rod or a long wire). "
             "BPC (68.5 kHz) is below the HackRF and most HF radios too: it needs an LF-capable radio or an upconverter in front of the radio. "
         "WWV / WWVH, BPM and HLA are on HF and work with any radio that reaches 2.5 - 15 MHz.", d.minFreqHz / 1e6, a.freqMhz * 1e3);
    return b;
}

void notes(const App& a) {
    const std::string n = rangeNote(a);
    if (!n.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::warnAmber(), "%s", n.c_str()); ImGui::PopTextWrapPos(); }
}

const char* dstText(const Fields& f, int st) {
    if (f.dstCode >= 0) {
        static const char* c[] = {"standard time", "DST ends today", "DST begins today", "DST in effect"};
        return c[f.dstCode & 3];
    }
    if (f.dst < 0) return "not sent";
    if (st == Dcf77) return f.dst ? "CEST (summer time)" : "CET";
    if (st == Msf) return f.dst ? "BST (summer time)" : "GMT";
    return f.dst ? "summer time" : "standard time";
}

void plot(const char* id, const char* label, const std::vector<float>& v, float lo, float hi, const char* unit) {
    if (v.size() < 2) return;
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine();
    float last = NAN;
    for (auto it = v.rbegin(); it != v.rend(); ++it) if (!std::isnan(*it)) { last = *it; break; }
    if (!std::isnan(last)) ImGui::Text("%.2f %s", last, unit);
    std::vector<float> w(v);
    for (float& x : w) if (std::isnan(x)) x = lo;
    ImGui::PlotLines(id, w.data(), (int)w.size(), 0, nullptr, lo, hi, ImVec2(ImGui::GetContentRegionAvail().x, 46 * gUi));
}

void minMax(const std::vector<float>& v, float& lo, float& hi, float pad) {
    lo = 1e30f; hi = -1e30f;
    for (float x : v) if (!std::isnan(x)) { lo = std::min(lo, x); hi = std::max(hi, x); }
    if (lo > hi) { lo = 0; hi = 1; }
    lo -= pad; hi += pad;
}

void timeView(App& a) {
    notes(a);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to read the time station"); return; }
    const dect2::TimeTelemetry& t = a.rx.timesig;
    const Fields& f = t.fields;
    // the received time, large
    ImGui::PushFont(a.mono, 30.f);
    if (t.timeValid) ImGui::TextColored(t.confirmed ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s UTC", utcText(t.utcNow).c_str());
    else ImGui::TextDisabled("----.--.-- --:--:-- UTC");
    ImGui::PopFont();
    if (t.timeValid && f.utcOffsetMin) {
        ImGui::SameLine(0, 12 * gUi);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("station time %s (UTC%+d:%02d)", utcText(t.utcNow + f.utcOffsetMin * 60, false).c_str(), f.utcOffsetMin / 60, std::abs(f.utcOffsetMin % 60));
    }
    kvRow(a, "station", "%s%s, %s", stationName(t.station), t.station == Wwv ? (t.hawaii ? " (WWVH, 1200 Hz ticks)" : " (WWV, 1000 Hz ticks)") : "", stationTimeZone(t.station));
    kvRow(a, "state", "%s", t.state == 2 ? (t.confirmed ? "time decoded, two minutes agree" : "time decoded (one minute)") : t.state == 1 ? "seconds found, waiting for a whole minute" : "searching for the seconds");
    if (t.timeValid) {
        static const char* wd[] = {"", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
        kvRow(a, "date", "%04d-%02d-%02d, %s, day %d of the year", f.year, f.month, f.day, f.weekday >= 1 && f.weekday <= 7 ? wd[f.weekday] : "-", f.doy);
        kvRow(a, "summer time", "%s%s", dstText(f, t.station), f.dstAnnounce ? ", the change is announced for the end of this hour" : "");
        kvRow(a, "leap second", "%s", f.leapAnnounce ? (f.leapSign < 0 ? "one is to be removed" : "one is announced") : t.station == Msf ? "not sent (DUT1 only)" : "none announced");
        if (f.dut1 != 99) kvRow(a, "DUT1", "%+.1f s (UT1 - UTC)", f.dut1 / 10.0);
        if (t.station == BpmC) {
            if (f.hasDut1Ms) kvRow(a, "DUT1", "%+.*f ms (UT1 - UTC)%s", f.dut1Tenth >= 0 ? 1 : 0, f.dut1Ms, f.dut1Tenth >= 0 ? "" : ", the 0.1 ms digit comes with the next minute frame");
            kvRow(a, "TAI - UTC", f.leapCount >= 0 ? "%d s" : "not read yet (comes with the minute frame)", f.leapCount);
            kvRow(a, "parity", "PC1 - PC7 of the second frame%s pass", f.leapCount >= 0 ? ", PC8 - PC10 of the minute frame" : "");
        }
        if (t.station == Bpc) kvRow(a, "frame", "%d of 3 in the minute (starts at second %d); parity P3 and P4 pass; AM / PM from P3", f.frameIndex + 1, f.second);
        if (t.station == Hla) kvRow(a, "checks", "marker positions, the unused seconds and the ranges (HLA sends no parity); the year is not sent: this year is assumed");
        if (t.station == Bpm) kvRow(a, "BPM seconds", "%s", f.ut1 ? "UT1 (the ticks of this minute follow UT1; the offset allows for DUT1)" : "UTC");
        if (f.callBit) kvRow(a, "call bit", "set: DCF77 runs on its reserve antenna");
        kvRow(a, "last minute", "%.0f s ago, %llu decoded, %llu rejected", t.secsSinceFrame, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    }
    ImGui::Spacing();
    // the computer clock
    if (t.clockValid) {
        const double o = t.clockOffsetAvgMs;
        const ImVec4 c = std::fabs(o) < 20 ? pal::okGreen() : std::fabs(o) < 500 ? pal::warnAmber() : pal::badRed();
        ImGui::TextDisabled("computer clock"); kvColumn(130 * gUi);
        ImGui::PushFont(a.mono, 0);
        ImGui::TextColored(c, "%+.1f ms %s", o, o >= 0 ? "ahead" : "behind");
        ImGui::PopFont();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The computer clock when each second arrived, minus the received second and the path delay, averaged over the last minute.\n"
                              "It includes the radio's USB buffering (the earliest arrival is used), typically a few ms.");
        kvRow(a, "last second", "%+.1f ms", t.clockOffsetMs);
    } else {
        kvRow(a, "computer clock", "%s", t.timeValid ? "nothing to compare (a recording)" : "once the time is decoded");
    }
    kvRow(a, "path", "%.0f km from %s, %.2f ms %s%s", t.distanceKm, stationName(t.station), t.propDelayMs, stationIsHf(t.station) ? "(sky wave estimate)" : "(ground wave)",
          t.locationSet ? "" : "; set your position in ADS-B (Receiver position) for the right delay");
    kvRow(a, "seconds tick", "at %.3f ms of the signal second, %.2f ms rms jitter", t.secPhaseMs, t.tickJitterMs);
    kvRow(a, "second", "%s", t.secondOfMinute >= 0 ? std::to_string(t.secondOfMinute).c_str() : "-");
    ImGui::TextDisabled("this minute"); kvColumn(130 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(t.bitsNow.empty() ? "-" : t.bitsNow.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("One symbol a second: 0 / 1 bits (MSF: 0 - 3 = A + 2 B), M a marker, - no keying, ? not read");
    ImGui::Spacing();
    float lo, hi;
    minMax(t.offsetHist, lo, hi, 1);
    plot("##tmo", "clock offset", t.offsetHist, lo, hi, "ms");
    minMax(t.tickHist, lo, hi, 0.5f);
    plot("##tmt", "tick", t.tickHist, lo, hi, "ms");
    plot("##tms", "SNR", t.snrHist, 0, 60, "dB");
    minMax(t.levelHist, lo, hi, 3);
    plot("##tml", "level", t.levelHist, lo, hi, "dBFS");
}

void logView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the minutes decoded"); return; }
    const auto& log = a.rx.timesig.log;
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (const auto& l : log) txt += clock(l.wallTime) + "\t" + (l.ok ? "ok" : "rejected") + "\t" + l.text + "\t" + l.bits + "\n";
        ImGui::SetClipboardText(txt.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) a.engine.timesig().clearLog();
    if (!ImGui::BeginTable("##tmlog", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Computer UTC"); ImGui::TableSetupColumn("Minute"); ImGui::TableSetupColumn("Symbols", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& l : log) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(l.wallTime).c_str());
        ImGui::TableNextColumn();
        if (l.ok) ImGui::TextColored(pal::okGreen(), "%s", l.text.c_str()); else ImGui::TextColored(pal::warnAmber(), "%s", l.text.c_str());
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(l.bits.c_str()); ImGui::PopFont();
    }
    ImGui::EndTable();
    if (log.empty()) ImGui::TextDisabled("no minute yet: a whole minute after the seconds are found");
}

// ---------------------------------------------------------------- hooks

void pushSettings(App& a) {
    a.engine.timesig().setStation(S.station);
    a.engine.timesig().setPref60(S.pref60);
    double lat, lon;
    receiverLocation(lat, lon);
    a.engine.timesig().setReceiverLocation(lat, lon, receiverLocationSet());
    const float scale = 1.f + S.clockPct / 100.f;
    if ((int)S.audio != S.pushedAudio || S.tone != S.pushedTone) { a.engine.timesig().setAudioInput(S.audio, S.tone); S.pushedAudio = S.audio; S.pushedTone = S.tone; }
    if (scale != S.pushedScale) { a.engine.timesig().setClockScale(scale); S.pushedScale = scale; }
    if ((int)S.listen != S.pushedListen) { a.engine.timesig().setListen(S.listen); S.pushedListen = S.listen; }
    if (a.volume != S.pushedVol || (int)a.muted != S.pushedMute) { a.engine.timesig().setVolume(a.volume); a.engine.timesig().setMuted(a.muted); S.pushedVol = a.volume; S.pushedMute = a.muted; }
}

void tick(App& a) { loadState(); pushSettings(a); clockPublishStation(a); }

void tuner(App& a, bool& retune) {
    loadState();
    ImGui::TextDisabled("Station");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(150 * gUi, ImGui::GetContentRegionAvail().x));
    const char* cur = S.station < 0 ? "from the frequency" : stationName(S.station);
    if (ImGui::BeginCombo("##tmst", cur)) {
        if (ImGui::Selectable("from the frequency", S.station < 0)) { S.station = -1; saveState(); }
        for (int s = 0; s < kNumStations; s++) {
            if (s == Chu) continue;
            char b[64];
            snprintf(b, sizeof b, "%-11s %g kHz", stationName(s), stationKhz(s));
            if (ImGui::Selectable(b, S.station == s)) {
                S.station = s;
                if (s == Msf || s == Wwvb || s == Jjy60) S.pref60 = s;
                saveState();
                if (stationForKhz(a.freqMhz * 1e3, S.pref60) != s) { tuneFreq(a, stationKhz(s) / 1e3); retune = true; }
                a.tune.synth.modeOpt[0] = s + 1;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("DCF77 Germany, MSF UK, WWVB and WWV USA, WWVH Hawaii, JJY Japan, BPM China (ticks, and the coded signal), BPC China (68.5 kHz, needs an LF radio or an upconverter), HLA Korea, RWM Russia.\nIn Europe DCF77 and MSF are heard across the continent at night and in the day.");
    const int st = S.station >= 0 ? S.station : stationForKhz(a.freqMhz * 1e3, S.pref60);
    if (st == Wwv || st == Bpm || st == BpmC || st == Hla || st == Rwm) {
        ImGui::SameLine(0, 8 * gUi);
        ImGui::SetNextItemWidth(std::min(100 * gUi, ImGui::GetContentRegionAvail().x));
        char lb[32];
        snprintf(lb, sizeof lb, "%.3g MHz", a.freqMhz);
        if (ImGui::BeginCombo("##tmfq", lb)) {
            const std::vector<double> fs = st == Wwv ? std::vector<double>{2.5, 5, 10, 15, 20} : st == Bpm ? std::vector<double>{2.5, 5, 10, 15} : st == BpmC ? std::vector<double>{2.5, 5} : st == Rwm ? std::vector<double>{4.996, 9.996, 14.996} : std::vector<double>{5};
            for (double f : fs) {
                char b[24];
                snprintf(b, sizeof b, "%g MHz", f);
                if (ImGui::Selectable(b, std::fabs(a.freqMhz - f) < 1e-3)) { tuneFreq(a, f); retune = true; }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st == Wwv ? "WWV and WWVH share 2.5, 5, 10 and 15 MHz (WWV also 20 MHz); the 1000 / 1200 Hz ticks tell them apart" : st == Bpm ? "BPM (China, Pucheng) shares 2.5, 5, 10 and 15 MHz with WWV: choose the station by hand, the frequency cannot tell them apart" : st == BpmC ? "BPM's coded time signal (2.5 and 5 MHz, in the 5 minute pure carrier slots of every half hour)" : st == Rwm ? "RWM (Russia, Moscow region): 4.996, 9.996 and 14.996 MHz. No time of day is sent: the seconds, the minute pulse and DUT1 are read, the minute comes from the computer clock" : "HLA (Korea) 5 MHz");
    }
    if (st < 0) { ImGui::SameLine(0, 8 * gUi); ImGui::TextColored(pal::warnAmber(), "no station at this frequency"); }
    ImGui::SameLine(0, 8 * gUi);
    listenControls(a, S.listen, "timeListen", "time", "Hear the station: the AM audio (ticks, voice) of WWV, a tone keyed by the carrier for the LF stations (DCF77, MSF, WWVB, JJY); decoding is the same either way");
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("simulated station");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(130 * gUi, ImGui::GetContentRegionAvail().x));
    int s = sc.modeOpt[0] > 0 ? sc.modeOpt[0] - 1 : Dcf77;
    if (ImGui::BeginCombo("##tmsy", stationName(s))) {
        for (int k = 0; k < kNumStations; k++) {
            if (k == Chu) continue;
            if (ImGui::Selectable(stationName(k), k == s)) { sc.modeOpt[0] = k + 1; changed = true; }
        }
        ImGui::EndCombo();
    }
    if (s == Wwv) {
        flowNext();
        bool h = sc.modeOpt[1] == 1;
        if (ImGui::Checkbox("WWVH", &h)) { sc.modeOpt[1] = h ? 1 : 0; changed = true; }
    }
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("It carries the computer's time, so the clock offset shows the delay of the path only."); ImGui::PopTextWrapPos(); }
}

void tab(App& a) {
    subNav("tmv", S.view, {"Time", "Minutes", "Clock"});
    if (S.view == 0) timeView(a);
    else if (S.view == 1) logView(a);
    else { notes(a); if (!live(a)) ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to measure the computer clock"); ImGui::BeginChild("##tmclk", ImVec2(0, 0)); clockSyncPanel(a); ImGui::EndChild(); }
}

void list(App& a) {
    notes(a);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::TimeTelemetry& t = a.rx.timesig;
    ImGui::TextUnformatted(stationName(t.station));
    ImGui::PushFont(a.mono, 0);
    ImGui::TextUnformatted(t.timeValid ? utcText(t.utcNow, false).c_str() : "--:--:--");
    if (t.clockValid) ImGui::Text("%+.1f ms", t.clockOffsetAvgMs);
    ImGui::PopFont();
}

void receiver(App& a) {
    notes(a);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::TimeTelemetry& t = a.rx.timesig;
    kvRow(a, "state", "%s", dect2::timeSummary(t).c_str());
    kvRow(a, "carrier", "%.1f dBFS, offset %+.2f Hz from the dial", t.levelDb, t.cfoHz);
    kvRow(a, stationIsHf(t.station) ? "ticks" : "keying", stationIsHf(t.station) ? "%.2f of the tick above the background, SNR %.1f dB" : "depth %.2f, SNR %.1f dB", t.depth, t.snrDb);
    if (t.station == Wwv) kvRow(a, "tick tones", "1000 Hz (WWV) %.1f dB, 1200 Hz (WWVH) %.1f dB", t.tick1000Db, t.tick1200Db);
    kvRow(a, "seconds tick", "%.3f ms, jitter %.2f ms rms", t.secPhaseMs, t.tickJitterMs);
    kvRow(a, "minutes", "%llu decoded, %llu rejected", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kvRow(a, "input", "%.3f Msps", t.inputRate / 1e6);
    ImGui::Spacing();
    ImGui::TextDisabled("Recording of demodulated audio");
    if (ImGui::Checkbox("Audio, not IQ", &S.audio)) saveState();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The file holds the station's audio (a WAV with the audio on the left channel and silence on the right): HLA, BPM and the other HF stations as heard, BPC as the tone its carrier became");
    if (S.audio && !stationIsHf(t.station)) {
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::InputInt("tone Hz", &S.tone, 10, 100)) { S.tone = std::max(50, std::min(20000, S.tone)); saveState(); }
    }
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::InputFloat("recorder clock error %", &S.clockPct, 0.1f, 1.0f, "%.2f")) { S.clockPct = std::max(-10.f, std::min(10.f, S.clockPct)); saveState(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A recording whose sample clock ran slow shows the 1 kHz of BPM at 975 Hz and the seconds 1.0255 s long: enter 2.55. 0 for a good recording or a radio");
    kvRow(a, "signal time", "%.1f s", t.timeSec);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    if (t.dcfPhaseLocked) ImGui::TextDisabled("DCF77 phase code: locked, correlation %.2f, last bit %d", t.dcfPhaseCorr, t.dcfPhaseBit);
    if (t.wwvbPhaseLocked) ImGui::TextDisabled("WWVB phase code: minute %lld (UTC seconds)", (long long)t.wwvbPhaseUtc);
    ImGui::TextDisabled("Not used: the WWV voice announcements.");
    ImGui::PopTextWrapPos();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Seconds", !on ? 0 : a.rx.timesig.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("Time", !on ? 0 : a.rx.timesig.timeValid ? (a.rx.timesig.confirmed ? 1 : 2) : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::timeSummary(a.rx.timesig).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Time signals";
    if (live(a)) l2 = dect2::timeSummary(a.rx.timesig);
}

} // namespace

extern const ModeUi kTimeUi;
const ModeUi kTimeUi = {
    .sideTitle = "TIME",
    .tabName = "Time",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .synth = synth,
    .tick = tick,
};
