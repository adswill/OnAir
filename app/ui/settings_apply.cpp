// What Settings does outside its own window (4.0, docs/SETTINGS-4.0.md): the start-up frequency and mode, starting the receiver at launch,
// the crash-restore choice, Confirm before quitting, the IQ recording's split size and free-space guard, and the Record IQ key.
// Every value is read from the preferences keys of the Settings rows (settings_table.h), so nothing here has a switch of its own.
#include "../app.h"
#include "../i18n/i18n.h"
#include "../platform/platform.h"
#include "../state/recdir.h"
#include "../state/workspace.h"
#include "../panels/automation_internal.h"
#include "dect2/source/iq_record.h"
#include "scale.h"
#include <algorithm>
#include <cstdint>
#include <string>

namespace {
bool gPendingMode = false, gPendingStart = false, gExitAsk = false, gExitOk = false, gRestoreDone = false;
double gLastRecCheck = 0;

// the IQ file of the radio, as the Record IQ button names it (diag_ui.cpp)
bool startIq(App& a, std::string& err) {
    const dect2::FileFormat f = (dect2::FileFormat)recFileFormatOf(a.recFormat);
    const std::string name = a.engine.recordingName(f);
    const std::string path = recordingsDir() + "/" + (a.recFormat == 2 ? dect2::blueRecordingName(name) : name);
    if (!a.engine.startRecording(path, f, err)) return false;
    a.recDonePath.clear();
    return true;
}

std::string folderOf(const std::string& path) {
    const size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? std::string(".") : path.substr(0, s);
}

bool busy(App& a) { return a.engine.running() || a.engine.recordingStats().active || a.engine.player().recording().active; }

// Confirm before quitting: the window's close (button, Alt+F4, the menu) is held back and asked about
void exitGuard(App& a) {
    const int when = (int)plat::prefs().getI("confirmExit", 0);
    if (gWindow && glfwWindowShouldClose(gWindow) && !gExitOk && (when == 2 || (when == 1 && busy(a)))) {
        glfwSetWindowShouldClose(gWindow, GLFW_FALSE);
        gExitAsk = true;
    }
    const char* id = trw("Quit OnAir?##exitask");
    if (gExitAsk) { ImGui::OpenPopup(id); gExitAsk = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (a.engine.recordingStats().active || a.engine.player().recording().active) ImGui::TextUnformatted(tr("A recording is running: it ends when OnAir quits."));
        else if (a.engine.running()) ImGui::TextUnformatted(tr("The receiver is running."));
        ImGui::TextUnformatted(tr("Quit OnAir?"));
        ImGui::TextDisabled("%s", tr("Settings > General > Confirm before quitting"));
        if (ImGui::Button(tr("Quit"), ImVec2(110 * gUi, 0))) { gExitOk = true; ImGui::CloseCurrentPopup(); if (gWindow) glfwSetWindowShouldClose(gWindow, GLFW_TRUE); }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"), ImVec2(110 * gUi, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// the IQ recording's split size and the free-space guard, every 2 s
void recordingGuard(App& a) {
    const double now = ImGui::GetTime();
    if (now - gLastRecCheck < 2.0) return;
    gLastRecCheck = now;
    const dect2::RecordingStats st = a.engine.recordingStats();
    if (!st.active) return;
    const long guardMb = plat::prefs().getI("recMinFreeMb", 200), splitMb = plat::prefs().getI("recSplitMb", 0);
    if (guardMb > 0) {
        const unsigned long long fb = recdir::freeBytes(folderOf(st.path));
        if (fb != UINT64_MAX && fb < (unsigned long long)guardMb * 1024ull * 1024ull) {
            a.engine.stopRecording();
            const std::string msg = std::string(tr("IQ recording stopped: the disk is almost full")) + " (" + folderOf(st.path) + ")";
            a.engine.log(msg);
            automationapp::alertNow(a, "OnAir", msg, false);
            return;
        }
    }
    if (splitMb > 0 && st.bytes >= (uint64_t)splitMb * 1024ull * 1024ull) {
        a.engine.stopRecording();
        std::string err;
        if (!startIq(a, err)) a.engine.log("recording: the next part could not start: " + err);
        else a.engine.log("recording: next part " + a.engine.recordingStats().path);
    }
}
}  // namespace

int sessionRestoreChoice() { return (int)std::max(0L, std::min(2L, plat::prefs().getI("sessionRestore", 0))); }

void settingsStartup(App& a, bool plainStart) {
    dect2::setRecordingNamePattern(plat::prefs().getS("recNamePattern", ""));
    if (!plainStart) return;   // a start with flags (--device, a file, a test run) keeps what the flags say
    if (plat::prefs().getI("startFreqMode", 0) == 1) {
        const double f = plat::prefs().getD("startFreqMhz", 100);
        if (f > 0) { a.freqMhz = f; a.tune.centerHz = f * 1e6; }
    }
    gPendingMode = plat::prefs().getI("startFamily", -1) >= 0;
    gPendingStart = plat::prefs().getB("autoStartRx", false);
}

void settingsFrame(App& a) {
    const int frame = ImGui::GetFrameCount();
    if (gPendingMode && frame >= 2) {
        gPendingMode = false;
        const int fam = (int)plat::prefs().getI("startFamily", -1);
        const std::string sub = plat::prefs().getS("startSub", "");
        bool known = false;
        for (const auto& m : allModes()) if (m.family == fam) known = true;
        if (known) {
            selectMode(a, fam, sub.empty() ? nullptr : sub.c_str());
            if (plat::prefs().getI("startFreqMode", 0) == 1) {   // the fixed frequency wins over the mode's own
                const double f = plat::prefs().getD("startFreqMhz", 100);
                if (f > 0) { a.freqMhz = f; a.tune.centerHz = f * 1e6; }
            }
        }
    }
    if (gPendingStart && frame >= 30 && !gPendingMode) {   // after the radio list and the mode have settled
        gPendingStart = false;
        if (!a.engine.running() && a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].isRadio()) {
            a.engine.log("Settings: starting the receiver at launch");
            startReceiver(a);
        }
    }
    if (!gRestoreDone && frame >= 3) {
        gRestoreDone = true;
        if (sessionRestoreChoice() == 1 && workspaceRestorePending()) {
            std::string why;
            workspaceRestoreAnswer(a, true, why);
            if (!why.empty()) a.engine.log("restore last session: " + why);
        }
    }
    exitGuard(a);
    recordingGuard(a);
}

void toggleIqRecording(App& a) {
    if (a.engine.recordingStats().active) { a.engine.stopRecording(); return; }
    if (!a.engine.running()) { a.engine.log("Record IQ: start the radio first"); return; }
    if (a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].kind == DeviceInfo::File) { a.engine.log("Record IQ: a file playback is already a recording"); return; }
    std::string err;
    if (!startIq(a, err)) a.engine.log("recording failed: " + err);
}
