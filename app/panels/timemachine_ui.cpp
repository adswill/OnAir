// "Rewind" (the time machine, core/include/dect2/source/time_machine.h): a rolling memory of the radio's last seconds of raw samples. One button
// in the diagnostics row opens its window: switch it on, how far back it reaches, the memory it may take, "Save the last N s" with a slider over the
// ring (the bar shows how much history there is), how long to keep recording after the trigger, and what triggers a save: the button, the squelch of
// the General receiver opening, or an automation rule with the "Save the rewind" action. The settings are kept in the preferences.
#include "../app.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "dect2/source/time_machine.h"
#include "dect2/engine/updater.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

struct Settings {
    bool on = false;
    int seconds = 60;     // how far back the ring reaches
    int decim = 1;        // 1, 2, 4, 8, 16
    int memMB = 512;      // memory limit
    int pre = 30;         // seconds before the trigger that a save keeps
    int post = 10;        // seconds after it
    bool squelch = false; // a save when the General receiver's squelch opens
    bool loaded = false;
};
Settings S;
bool gWasSaving = false, gSquelchWas = false;
double gLastTrigger = -100;
std::string gMsg;

void persist() {
    plat::Prefs& d = plat::prefs();
    d.setB("tmOn", S.on); d.setI("tmSec", S.seconds); d.setI("tmDecim", S.decim); d.setI("tmMB", S.memMB);
    d.setI("tmPre", S.pre); d.setI("tmPost", S.post); d.setB("tmSquelch", S.squelch);
}

int fixDecim(int d) { return d >= 16 ? 16 : d >= 8 ? 8 : d >= 4 ? 4 : d >= 2 ? 2 : 1; }

void apply(App& a) { a.engine.setTimeMachine(S.on ? S.seconds : 0, S.decim, S.memMB); }

std::string timeText(double s) {
    char b[32];
    if (s >= 3600) snprintf(b, sizeof b, "%.1f h", s / 3600);
    else if (s >= 120) snprintf(b, sizeof b, "%.0f min", s / 60);
    else snprintf(b, sizeof b, "%.0f s", s);
    return b;
}

bool saveNow(App& a, double post, std::string& note) {
    if (!S.on) { note = "the rewind memory is off"; return false; }
    const std::string path = recordingsDir() + "/" + a.engine.timeMachineName();
    if (!a.engine.timeMachineSaveTo(path, S.pre, post, note)) return false;
    gLastTrigger = ImGui::GetTime();
    note = "saving " + timeText(S.pre) + " before and " + timeText(post) + " after now";
    return true;
}

}  // namespace

void timeMachineLoad(App& a) {
    plat::Prefs& d = plat::prefs();
    S.on = d.getB("tmOn", false);
    S.seconds = std::max(5, std::min(24 * 3600, (int)d.getI("tmSec", 60)));
    S.decim = fixDecim((int)d.getI("tmDecim", 1));
    S.memMB = std::max(16, std::min(65536, (int)d.getI("tmMB", 512)));
    S.pre = std::max(0, std::min(S.seconds, (int)d.getI("tmPre", 30)));
    S.post = std::max(0, std::min(600, (int)d.getI("tmPost", 10)));
    S.squelch = d.getB("tmSquelch", false);
    S.loaded = true;
    apply(a);
}

bool timeMachineSaveNow(App& a, double postSeconds, std::string& note) { return saveNow(a, postSeconds, note); }

// every frame: the General receiver's squelch opening is a trigger; the end of a save goes to the log
void timeMachineTick(App& a) {
    if (!S.loaded) return;
    const dect2::TimeMachineStats st = a.engine.timeMachineStats();
    if (gWasSaving && !st.saving) {
        if (!st.error.empty()) a.engine.log("rewind save failed: " + st.error);
        else {
            char b[96];
            snprintf(b, sizeof b, "rewind: saved %.1f s to ", st.lastSeconds);
            a.engine.log(std::string(b) + st.lastPath);
            if (st.lastMissing) a.engine.log("rewind: the disk was too slow, part of the window had been overwritten");
        }
    }
    gWasSaving = st.saving;
    const bool open = a.engine.running() && a.rx.general.state == 2 && a.rx.general.squelchOpen;
    if (S.squelch && S.on && open && !gSquelchWas && !st.saving && ImGui::GetTime() - gLastTrigger > S.post + 5) {
        std::string note;
        if (saveNow(a, S.post, note)) a.engine.log("rewind: squelch opened, " + note);
    }
    gSquelchWas = open;
}

void timeMachineUi(App& a) {
    if (!S.loaded) return;
    const dect2::TimeMachineStats st = a.engine.timeMachineStats();
    const bool isFile = a.devices[a.devIdx].kind == dect2::DeviceInfo::File;
    std::string label = tr("Rewind");
    if (st.enabled) label += "  " + timeText(st.filledSeconds);
    label += "###rewind";
    if (st.saving) ImGui::PushStyleColor(ImGuiCol_Text, pal::badRed());
    if (ImGui::SmallButton(label.c_str())) ImGui::OpenPopup("##rewindpop");
    if (st.saving) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Keeps the last seconds of the radio's raw samples in memory, so the signal from just before something happened can be saved too"));
    if (!ImGui::BeginPopup("##rewindpop")) return;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 420 * gUi);
    bool ch = false;
    if (isFile) ImGui::TextDisabled("%s", tr("A file source is already there: nothing to keep"));
    ImGui::BeginDisabled(isFile);
    if (ImGui::Checkbox(tr("Keep a rolling memory of the raw samples"), &S.on)) { ch = true; apply(a); }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(S.on);   // the ring is built when it is switched on
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::SliderInt(tr("Reaches back (s)"), &S.seconds, 5, 1800, "%d", ImGuiSliderFlags_Logarithmic)) { S.pre = std::min(S.pre, S.seconds); ch = true; }
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::SliderInt(tr("Memory limit (MB)"), &S.memMB, 16, 8192, "%d", ImGuiSliderFlags_Logarithmic)) ch = true;
    ImGui::SetNextItemWidth(220 * gUi);
    {
        auto dtext = [](int d) { return d == 1 ? std::string(tr("Full rate")) : "1 / " + std::to_string(d); };
        if (ImGui::BeginCombo(tr("Samples kept"), dtext(S.decim).c_str())) {
            for (int d : {1, 2, 4, 8, 16}) {
                const std::string b = dtext(d);
                if (ImGui::Selectable(b.c_str(), d == S.decim)) { S.decim = d; ch = true; }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("1 / 4 averages every 4 samples: the memory then reaches 4 times as far back, but only the middle quarter of the band is kept. The settings can be changed while it is off."));
    if (st.enabled) {
        char b[200];
        snprintf(b, sizeof b, tr("Reaches back %s using %d MB; %s in memory now (%.3f Msps)"), timeText(st.windowSeconds).c_str(), (int)(st.bytes / 1048576), timeText(st.filledSeconds).c_str(), st.storedRateHz / 1e6);
        ImGui::TextDisabled("%s", b);
        if (st.windowSeconds + 0.5 < S.seconds) ImGui::TextColored(pal::warnAmber(), "%s", tr("The memory limit shortens the window; raise it or keep fewer samples."));
    }
    ImGui::Separator();
    // the rewind slider over the ring: how much of what is in memory a save keeps
    const double have = st.enabled ? st.filledSeconds : 0;
    const int maxPre = std::max(1, (int)std::floor(std::min<double>(st.enabled ? st.windowSeconds : S.seconds, std::max(1.0, have))));
    S.pre = std::max(0, std::min(S.pre, st.enabled ? maxPre : S.seconds));
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::SliderInt(tr("Save the last (s)"), &S.pre, 0, maxPre)) ch = true;
    {   // the bar: the ring's fill, and the part a save keeps
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = 260 * gUi, h = 8 * gUi;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const double win = st.enabled ? std::max(1.0, st.windowSeconds) : std::max(1.0, (double)S.seconds);
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(60, 64, 72, 255), 2);
        const float fill = (float)std::min(1.0, have / win), keep = (float)std::min<double>(fill, S.pre / win);
        dl->AddRectFilled(ImVec2(p.x + w * (1 - fill), p.y), ImVec2(p.x + w, p.y + h), IM_COL32(90, 130, 190, 255), 2);
        dl->AddRectFilled(ImVec2(p.x + w * (1 - keep), p.y), ImVec2(p.x + w, p.y + h), IM_COL32(110, 200, 130, 255), 2);
        ImGui::Dummy(ImVec2(w, h + 4 * gUi));
    }
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::SliderInt(tr("Then keep recording (s)"), &S.post, 0, 600)) ch = true;
    if (ImGui::Checkbox(tr("Save when the General receiver's squelch opens"), &S.squelch)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("In the General receiver mode; for other events use an automation rule with the action \"Save the rewind\""));
    ImGui::BeginDisabled(!st.enabled || st.saving);
    if (ImGui::Button(tr("Save now"))) { std::string n; saveNow(a, S.post, n); gMsg = n; }
    ImGui::EndDisabled();
    if (st.saving) { ImGui::SameLine(); ImGui::TextDisabled("%.0f %%", st.savePct); }
    else if (!st.error.empty()) { ImGui::SameLine(); ImGui::TextColored(pal::badRed(), "%s", st.error.c_str()); }
    else if (!st.lastPath.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Show recording"))) dect2::openUrl(st.lastPath.substr(0, st.lastPath.find_last_of("/\\")));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st.lastPath.c_str());
    }
    if (!gMsg.empty() && !st.saving && st.error.empty() && st.lastPath.empty()) ImGui::TextDisabled("%s", gMsg.c_str());
    ImGui::PopTextWrapPos();
    if (ch) persist();
    ImGui::EndPopup();
}
