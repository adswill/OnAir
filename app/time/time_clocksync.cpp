// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The "Sync computer clock" section and the optional NTP server (clocksync_ui.h). Nothing here sets the clock by itself: only a click on
// "Sync now" runs the operating system's administrator prompt, and only for a locked, plausible measurement (core/src/time_ntp.cpp).
#include "dect2/common/thread_name.h"
#include "panels/clocksync_ui.h"
#include "dect2/time/time_ntp.h"
#include "dect2/time/time_proto.h"
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {
using namespace dect2;

struct Job {
    std::atomic<bool> running{false};
    std::mutex mu;
    std::string message;
    bool ok = false;
};

struct State {
    bool loaded = false;
    int source = ClockGps;
    bool ntpOn = false;                // never remembered: off at every start
    std::string ntpMsg;
    std::shared_ptr<Job> job = std::make_shared<Job>();
    ntp::Server server;
};
State S;

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.source = plat::prefs().getI("clockSyncSource", ClockGps) == ClockStation ? ClockStation : ClockGps;
}

const char* sourceName(int s) { return s == ClockGps ? "GPS" : "time station"; }

double stddev(const std::vector<float>& v) {
    double m = 0, q = 0;
    int n = 0;
    for (float x : v) if (!std::isnan(x)) { m += x; n++; }
    if (n < 3) return 0;
    m /= n;
    for (float x : v) if (!std::isnan(x)) q += (x - m) * (x - m);
    return std::sqrt(q / n);
}

void row(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void wrapped(const ImVec4* col, const char* fmt, ...) {
    char b[768];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushTextWrapPos(0);
    if (col) ImGui::TextColored(*col, "%s", b); else ImGui::TextDisabled("%s", b);
    ImGui::PopTextWrapPos();
}

const char* stationTag(int s) {
    switch (s) {
    case timesig::Dcf77: return "DCF77"; case timesig::Msf: return "MSF"; case timesig::Wwvb: return "WWVB"; case timesig::Jjy40: case timesig::Jjy60: return "JJY";
    case timesig::Wwv: return "WWV"; case timesig::Chu: return "WWV"; case timesig::Bpm: case timesig::BpmC: return "BPM"; case timesig::Bpc: return "BPC"; case timesig::Hla: return "HLA"; default: return "TIME";
    }
}

} // namespace

void clockPublishGps(const App& a) {
    ClockMeasurement m;
    const GnssTelemetry& t = a.rx.gnss;
    if (a.rx.standard == 13 && a.engine.running() && t.clockValid && t.fix.valid) {
        m.valid = true;
        m.offsetMs = t.clockOffsetAvgMs;
        m.accuracyMs = 10 + stddev(t.offsetHist);   // the radio's USB and driver buffering is not known better than this
        m.label = "GPS";
    }
    ClockDiscipline::get().publish(ClockGps, m);
}

void clockPublishStation(const App& a) {
    ClockMeasurement m;
    const TimeTelemetry& t = a.rx.timesig;
    if (a.rx.standard == 59 && a.engine.running() && t.clockValid && t.confirmed) {
        m.valid = true;
        m.offsetMs = t.clockOffsetAvgMs;
        // the radio's buffering, the path (a sky wave is only known to a few ms) and the tick jitter
        m.accuracyMs = 10 + (timesig::stationIsHf(t.station) ? 5 : 1) + 3 * t.tickJitterMs;
        m.label = stationTag(t.station);
    }
    ClockDiscipline::get().publish(ClockStation, m);
}

void clockMinMax(const std::vector<float>& v, float& lo, float& hi, float pad) {
    lo = 1e30f; hi = -1e30f;
    for (float x : v) if (!std::isnan(x)) { lo = std::min(lo, x); hi = std::max(hi, x); }
    if (lo > hi) { lo = 0; hi = 1; }
    lo -= pad; hi += pad;
}

void clockPlot(const char* id, const char* label, const std::vector<float>& v, float lo, float hi, const char* unit) {
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

void clockSyncPanel(App& a) {
    loadState();
    ClockDiscipline& cd = ClockDiscipline::get();
    const double now = systemUnixNow();
    ImGui::TextUnformatted("Sync computer clock");
    ImGui::Spacing();
    // the source
    ImGui::TextDisabled("Source"); kvColumn(130 * gUi);
    for (int s = 0; s < kClockSources; s++) {
        if (s) ImGui::SameLine(0, 12 * gUi);
        if (ImGui::RadioButton(sourceName(s), S.source == s)) { S.source = s; plat::prefs().setI("clockSyncSource", s); plat::prefs().flush(); }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("GPS: the satellites' UTC, read in the GNSS mode.\nTime station: DCF77, MSF, WWVB, JJY, WWV, BPM, BPC or HLA, read in the Time signals mode.\nThe radio runs one mode at a time: switch to the mode of the source and wait for it to lock.");
    ClockMeasurement m;
    const bool have = cd.latest(S.source, m, 30);
    if (have) {
        const double o = m.offsetMs;
        const ImVec4 c = std::fabs(o) < 50 ? pal::okGreen() : std::fabs(o) < 1000 ? pal::warnAmber() : pal::badRed();
        ImGui::TextDisabled("measured offset"); kvColumn(130 * gUi);
        ImGui::PushFont(a.mono, 0);
        ImGui::TextColored(c, "%+.1f ms %s (+- %.0f ms)", o, o >= 0 ? "ahead" : "behind", m.accuracyMs);
        ImGui::PopFont();
        row(a, "from", "%s%s%s", sourceName(S.source), S.source == ClockStation ? ": " : "", S.source == ClockStation ? m.label.c_str() : "");
    } else {
        ImGui::TextDisabled("measured offset"); kvColumn(130 * gUi);
        wrapped(nullptr, S.source == ClockGps ? "none: open the GNSS mode, start the receiver and wait for a fix with the date known"
                                              : "none: open the Time signals mode, start the receiver and wait until two decoded minutes agree");
    }
    { const ImVec4 amber = pal::warnAmber();
      wrapped(&amber, "Accuracy: the offset includes the radio's USB and driver buffering (the earliest arrival over half a minute is used), typically 5 - 30 ms; "
                      "with a time station also the path (a few ms). It tells whether the clock is seconds or hundreds of ms off, not whether it is within 1 ms."); }
    ImGui::Spacing();
    // the sync
    const ClockOs os = clockThisOs();
    std::string refuse = have ? clockSyncRefusal(m, now) : "no locked measurement from this source";
    if (refuse.empty()) refuse = clockSetLimit(os, m.offsetMs);
    if (S.job->running.load()) refuse = "waiting for the administrator prompt";
    const bool can = refuse.empty();
    if (!can) ImGui::BeginDisabled();
    if (ImGui::Button("Sync now")) {
        const double off = m.offsetMs;
        auto job = S.job;
        { std::lock_guard<std::mutex> lk(job->mu); job->message.clear(); }
        job->running = true;
        dect2::namedThread("onair-clksync", [job, off] {
            std::string msg;
            const bool ok = clockSyncRun(off, msg);
            { std::lock_guard<std::mutex> lk(job->mu); job->message = msg; job->ok = ok; }
            job->running = false;
        }).detach();
    }
    if (!can) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", can ? "Moves the computer clock by the measured offset. The system asks for the administrator password (macOS), a UAC prompt (Windows) or a polkit prompt (Linux).\nNothing happens until you click, and it is never done automatically."
                                    : refuse.c_str());
    ImGui::SameLine(0, 10 * gUi);
    ImGui::AlignTextToFramePadding();
    if (!can) ImGui::TextDisabled("not possible: %s", refuse.c_str());
    else ImGui::TextDisabled("sets the clock %s by %.0f ms", m.offsetMs > 0 ? "back" : "forward", std::fabs(m.offsetMs));
    {
        std::lock_guard<std::mutex> lk(S.job->mu);
        if (!S.job->message.empty()) {
            const ImVec4 c = S.job->ok ? pal::okGreen() : pal::warnAmber();
            wrapped(&c, "%s", S.job->message.c_str());
        }
    }
    // the command, for anyone who prefers to run it by hand (Windows: or a UAC relaunch with the button above)
    const double shown = have ? m.offsetMs : 0;
    static char cmd[512];
    snprintf(cmd, sizeof cmd, "%s", clockSetCommand(os, shown).c_str());
    ImGui::TextDisabled("Command (run as administrator)");
    ImGui::SetNextItemWidth(std::max(120.f, ImGui::GetContentRegionAvail().x - 70 * gUi));
    ImGui::InputText("##csyncCmd", cmd, sizeof cmd, ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button("Copy##csync")) ImGui::SetClipboardText(cmd);
    if (!have) wrapped(nullptr, "The command above is for an offset of 0 until a source is locked.");
    if (os == OsWindows) wrapped(nullptr, "Windows: the button asks for elevation (UAC) and runs the same PowerShell command; you can also paste it into an administrator PowerShell.");

    // ---------------------------------------------------------------- NTP
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    bool on = S.ntpOn && S.server.running();
    if (ImGui::Checkbox("Act as NTP server (LAN)", &on)) {
        if (on) {
            std::string msg;
            S.ntpOn = S.server.start(1123, msg);
            S.ntpMsg = msg;
        } else {
            S.server.stop();
            S.ntpOn = false;
            S.ntpMsg = "NTP server stopped";
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Serves the computer clock corrected by the measured offset to other computers on your network, on UDP port 123 (1123 without administrator rights).\nOff at every start. Only private (LAN) addresses are answered, at most %d requests per client in %.0f s.", ntp::RateLimiter::kPerClient, ntp::RateLimiter::kWindowS);
    { const ImVec4 amber = pal::warnAmber();
      wrapped(&amber, "For hobby use only. The accuracy is that of a USB radio and an ordinary computer clock (tens of ms), the source is only measured while this mode runs, "
                      "and nothing here is a certified time service. Do not use it for anything that depends on exact time, and do not expose it to the internet."); }
    if (!S.ntpMsg.empty()) wrapped(nullptr, "%s", S.ntpMsg.c_str());
    if (S.server.running()) {
        ClockMeasurement b;
        int src = ClockGps;
        const bool fresh = cd.best(b, src, 600);
        const ntp::ServerStats st = S.server.stats();
        row(a, "serving", "UDP %d, %s", S.server.port(), fresh ? (src == ClockGps ? "stratum 1, reference GPS" : "stratum 2, from a time station") : "not synchronised (stratum 16): no fresh measurement");
        row(a, "requests", "%llu answered, %llu rate limited, %llu refused (not LAN or not a client request)", (unsigned long long)st.requests, (unsigned long long)st.limited, (unsigned long long)st.refused);
        if (st.lastClient) row(a, "last client", "%s", ntp::ipText(st.lastClient).c_str());
        if (S.server.port() != 123) wrapped(nullptr, "Clients must use port %d (chrony: server HOST port %d), or forward UDP 123 to it.", S.server.port(), S.server.port());
    } else {
        S.ntpOn = false;
    }
}
