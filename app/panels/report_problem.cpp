// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Help > Report a problem (report_problem.h). The zip is written only when the user presses Save, to a file the user picks.
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
constexpr size_t kLogLines = 200;

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
    int builtAnswers[kNumQ] = {-1, -1, -1, -1, -1};
    bool built = false;
    std::string diag;    // the diagnostics.txt text, as it will be saved
    std::string status;  // the result of the last Save
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

std::string readmeText() {
    return "OnAir problem report\n"
           "\n"
           "This zip was made on your computer by Help > Report a problem. Nothing was uploaded.\n"
           "\n"
           "Files:\n"
           "  diagnostics.txt  the OnAir version, this computer, the selected source, the active mode,\n"
           "                   the sample rate, the enabled features, your troubleshooter answers and the\n"
           "                   last 200 log lines (folder paths shortened, lines that may hold a secret left out)\n"
           "  README.txt       this file\n"
           "\n"
           "To report the problem, attach this zip to a new issue on GitHub and say what you were doing.\n"
           "Recordings, settings, keys and passwords are not in this zip.\n";
}

std::string buildDiagnostics(App& a, const int* answers, bool withLog = true) {   // answers nullptr: no troubleshooter part
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

    if (answers) s += "\nTroubleshooter answers:\n";
    for (int i = 0; answers && i < kNumQ; i++) {
        const char* ans = answers[i] == 1 ? "yes" : answers[i] == 2 ? "no" : "not answered";
        s += std::string("  ") + kQuestions[i].ask + " " + ans + "\n";
    }

    if (!withLog) return s;
    size_t total = 0;
    std::vector<std::string> lines = a.engine.logSnapshot(total);
    const size_t from = lines.size() > kLogLines ? lines.size() - kLogLines : 0;
    s += "\nLog (last " + std::to_string(lines.size() - from) + " lines):\n";
    for (size_t i = from; i < lines.size(); i++) {
        if (mayHoldSecret(lines[i])) { s += "[line left out: it may hold a key, a token or a password]\n"; continue; }
        s += clean(lines[i]) + "\n";
    }
    return s;
}

}  // namespace

std::string reportCleanText(const std::string& text) { return clean(text); }
bool reportMayHoldSecret(const std::string& line) { return mayHoldSecret(line); }
std::string reportRadioAndMode(App& a) { return buildDiagnostics(a, nullptr, false); }

void reportProblemWindow(App& a) {
    ReportState& st = state();
    if (!a.reportOpen) { st.built = false; return; }

    // the text is rebuilt when the window opens, when an answer changes, and on Refresh
    bool answersChanged = false;
    for (int i = 0; i < kNumQ; i++) if (st.answers[i] != st.builtAnswers[i]) answersChanged = true;
    if (!st.built || answersChanged) {
        st.diag = buildDiagnostics(a, st.answers);
        for (int i = 0; i < kNumQ; i++) st.builtAnswers[i] = st.answers[i];
        st.built = true;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(760 * gUi, vp->WorkSize.x * 0.95f), std::min(720 * gUi, vp->WorkSize.y * 0.9f)), ImGuiCond_Appearing);
    if (!ImGui::Begin(trw("Report a problem"), &a.reportOpen)) { ImGui::End(); return; }

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(tr("This writes one zip file on this computer that you can attach to a GitHub issue. Nothing is uploaded. Check the list below before you save."));
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    ImGui::SeparatorText(tr("Included in the zip"));
    ImGui::BulletText("%s", tr("diagnostics.txt: the version, this computer, the selected source and its device info, the active mode, the sample rate, the enabled features, your troubleshooter answers and the last 200 log lines"));
    ImGui::BulletText("%s", tr("README.txt: what the zip is and how to attach it"));
    ImGui::BulletText("%s", tr("Folder paths are shortened. Log lines that may hold a key, a token or a password are left out."));
    ImGui::Spacing();
    ImGui::SeparatorText(tr("Left out"));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(tr("Recordings, settings files, keys, tokens, passwords, serial numbers, device addresses, file paths beyond the folder name, and the map server login."));
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    ImGui::SeparatorText(tr("Troubleshooter"));
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

    ImGui::SeparatorText(tr("Preview of diagnostics.txt"));
    {
        const size_t lines = (size_t)std::count(st.diag.begin(), st.diag.end(), '\n');
        const std::string info = std::to_string(lines) + " " + tr("lines") + ", " + std::to_string(st.diag.size()) + " " + tr("bytes");
        ImGui::TextDisabled("%s", info.c_str());
    }
    ImGui::BeginChild("##reportpreview", ImVec2(0, 200 * gUi), true);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(st.diag.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    if (ImGui::Button(tr("Refresh preview"))) st.built = false;
    ImGui::SameLine();
    if (ImGui::Button(tr("Save report zip..."))) {
        const std::string path = plat::saveFileDialog("onair-report.zip");
        if (!path.empty()) {
            std::vector<std::pair<std::string, std::string>> files = {{"diagnostics.txt", st.diag}, {"README.txt", readmeText()}};
            st.status = writeZipFile(path, files)
                ? tr("Saved. Nothing was uploaded: attach this file to your GitHub issue.")
                : tr("Could not write the zip file.");
        }
    }
    if (!st.status.empty()) { ImGui::SameLine(); ImGui::TextUnformatted(st.status.c_str()); }

    ImGui::End();
}
