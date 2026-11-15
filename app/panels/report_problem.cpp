// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Help > Report a problem (report_problem.h): the troubleshooter and the radio/mode text of the report window (debug_report.cpp).
// Everything here runs on this computer: no network, no recordings, no settings file. Log lines are shortened (personal folder
// paths become "~" or the folder name) and a line that may hold a key, token, password or serial is left out.
#include "report_problem.h"
#include "../i18n/i18n.h"
#include "../ui/scale.h"
#include "dect2/engine/crash_report.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kNumQ = 5;

struct Question { const char* ask; const char* fix; };
const Question kQuestions[kNumQ] = {
    {N_("Does the radio show in the Source list?"),
     N_("Unplug the radio and plug it into another USB port with a data cable. On Linux, check the radio's udev rules (Help > Troubleshooting). Then pick the radio again in the Source list.")},
    {N_("Is the antenna connected to the radio's receive port?"),
     N_("Connect the antenna to the port the radio labels for receive, and tighten the connector. Try another antenna cable if you have one.")},
    {N_("Does the spectrum show signal when you tune to a station you know is on the air?"),
     N_("Check the frequency and the band for the mode, then raise the gain a little. Make sure the sample rate is Auto.")},
    {N_("Does the radio accept the sample rate the mode needs?"),
     N_("Set the sample rate to Auto. A radio cannot run at every rate, so OnAir then picks the nearest rate the radio offers.")},
    {N_("Do you hear sound when Listen is on?"),
     N_("Turn Listen on, raise the volume, and choose the output device in the audio output popup.")},
};

struct ReportState {
    int answers[kNumQ] = {0, 0, 0, 0, 0};   // 0 not answered, 1 yes, 2 no
};

ReportState& state() { static ReportState s; return s; }

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
    return s;
}

std::string homeDir() {
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE");
#else
    const char* h = std::getenv("HOME");
#endif
    return h ? h : "";
}

// Any token that looks like a path is cut down to its last part (the file or folder name).
std::string shortenPaths(const std::string& line) {
    std::string out, tok;
    auto flush = [&] {
        const bool isPath = tok.size() > 1 &&
            (tok[0] == '/' || tok[0] == '~' || (tok.size() > 2 && tok[1] == ':' && (tok[2] == '\\' || tok[2] == '/'))) &&
            tok.find_first_of("/\\", 1) != std::string::npos;
        if (isPath) {
            const std::string base = tok.substr(tok.find_last_of("/\\") + 1);
            out += base.empty() ? std::string("<folder>") : base;
        } else out += tok;
        tok.clear();
    };
    for (char c : line) {
        if (c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '(' || c == ')' || c == '[' || c == ']' || c == '<' || c == '>' || c == ',' || c == '=') {
            flush();
            out += c;
        } else tok += c;
    }
    flush();
    return out;
}

// Free text from a radio or a file (its name, its board): folder paths shortened.
std::string clean(const std::string& text) {
    std::string s = replaceAll(text, dect2::crash::logDir(), "<OnAir folder>");
    const std::string home = homeDir();
    if (home.size() > 1) s = replaceAll(s, home, "~");
    return shortenPaths(s);
}

bool mayHoldSecret(const std::string& line) {
    std::string l;
    for (char c : line) l += (char)std::tolower((unsigned char)c);
    for (const char* w : {"key", "token", "password", "passwd", "secret", "auth", "bearer", "cookie", "credential", "serial"})
        if (l.find(w) != std::string::npos) return true;
    return false;
}

const char* kindName(DeviceInfo::Kind k) {
    switch (k) {
        case DeviceInfo::Synthetic: return "built-in test signal";
        case DeviceInfo::File: return "file playback (the file name and folder are left out)";
        case DeviceInfo::HackRF: return "HackRF";
        case DeviceInfo::Soapy: return "SoapySDR radio";
        case DeviceInfo::Native: return "radio with its own driver";
        case DeviceInfo::Audio: return "sound card";
        case DeviceInfo::Hamlib: return "rig control (Hamlib)";
    }
    return "other";
}

std::string rangeLine(double lo, double hi, double scale, const char* unit) {
    if (lo <= 0 && hi <= 0) return "unknown";
    char b[96];
    std::snprintf(b, sizeof b, "%.6g to %.6g %s", lo / scale, hi / scale, unit);
    return b;
}

std::string buildDiagnostics(App& a) {
    std::string os, cpu, gpu;
    systemInfoText(os, cpu, gpu);
    std::string s = "OnAir problem report\n";
    s += std::string("Version: ") + ONAIR_VERSION + "\n";
    s += "OS: " + clean(os) + "\n";
    s += "CPU: " + clean(cpu) + "\n";
    s += "Graphics: " + clean(gpu) + "\n\n";

    s += "Source:\n";
    if (a.devIdx >= 0 && a.devIdx < (int)a.devices.size()) {
        const DeviceInfo& d = a.devices[a.devIdx];
        s += "  Name: " + clean(d.name) + "\n";
        s += "  Kind: " + std::string(kindName(d.kind)) + "\n";
        s += "  Board: " + (d.board.empty() ? std::string("-") : clean(d.board)) + "\n";
        s += "  Sample rate range: " + rangeLine(d.minRateHz, d.maxRateHz, 1e6, "Msps") + "\n";
        s += "  Frequency range: " + rangeLine(d.minFreqHz, d.maxFreqHz, 1e6, "MHz") + "\n";
        s += "  Gain range: " + rangeLine(d.gainMinDb, d.gainMaxDb, 1, "dB") + "\n";
        s += "  (serial numbers, device arguments and addresses are left out)\n";
    } else s += "  none selected\n";
    s += std::string("Receiver: ") + (a.engine.running() ? "running" : "stopped") + "\n";

    std::string mode = "none";
    for (const auto& m : allModes()) if (modeSelected(a, m)) { mode = m.name; break; }
    s += "Active mode: " + clean(mode) + "\n";

    char rates[128];
    std::snprintf(rates, sizeof rates, "%.3f Msps (radio %.3f Msps)\n", a.engine.sampleRate() / 1e6, a.engine.radioRate() / 1e6);
    s += std::string("Sample rate: ") + rates;

    s += "\nEnabled features:\n";
#ifdef DECT2_HAVE_SOAPY
    s += "  SoapySDR radios\n";
#endif
#ifdef DECT2_HAVE_HAMLIB
    s += "  Hamlib rig control\n";
#endif
#ifdef DECT2_HAVE_RFNM
    s += "  RFNM radio\n";
#endif
#ifdef DECT2_HAVE_WHISPER
    s += "  speech to text (Whisper)\n";
#endif
#ifdef DECT2_NO_SIMD
    s += "  portable build (no SIMD)\n";
#endif
    if (a.rx.gpuAvailable) s += "  GPU LDPC decoder\n";
    if (a.netOn) s += "  remote control API\n";

    return s;
}

}  // namespace

std::string reportCleanText(const std::string& text) { return clean(text); }
bool reportMayHoldSecret(const std::string& line) { return mayHoldSecret(line); }
std::string reportRadioAndMode(App& a) { return buildDiagnostics(a); }

void reportTroubleshooterReset() {
    for (int& v : state().answers) v = 0;
}

void reportTroubleshooter() {
    ReportState& st = state();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(tr("Answer what you can. Each No shows one thing to try."));
    ImGui::PopTextWrapPos();
    for (int i = 0; i < kNumQ; i++) {
        ImGui::PushID(i);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(tr(kQuestions[i].ask));
        ImGui::PopTextWrapPos();
        ImGui::RadioButton(tr("Yes"), &st.answers[i], 1);
        ImGui::SameLine();
        ImGui::RadioButton(tr("No"), &st.answers[i], 2);
        if (st.answers[i] == 2) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.78f, 0.25f, 1));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(tr(kQuestions[i].fix));
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
        ImGui::Spacing();
    }
}

std::string reportTroubleshooterAnswers() {
    std::string s;
    for (int i = 0; i < kNumQ; i++) {
        const int v = state().answers[i];
        if (v != 0) s += std::string("  ") + kQuestions[i].ask + " " + (v == 1 ? "yes" : "no") + "\n";
    }
    return s;
}
