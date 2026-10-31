// The File menu part of workspaces (save, load, delete) and the two dialogs: the save name and "Restore last session?".
// The logic is in workspace.cpp. English text only.
#include "workspace.h"
#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct SaveDialog {
    bool request = false;
    char name[64] = {};
};
SaveDialog gSave;
bool gRestoreAsked = false;

std::string trimmed(const char* s) {
    std::string t = s;
    const size_t b = t.find_first_not_of(" \t");
    if (b == std::string::npos) return std::string();
    const size_t e = t.find_last_not_of(" \t");
    return t.substr(b, e - b + 1);
}

std::string nextDefaultName(const std::vector<Workspace>& list) {
    for (int n = (int)list.size() + 1; n < 1000; n++) {
        const std::string cand = "Workspace " + std::to_string(n);
        bool used = false;
        for (const Workspace& w : list) used = used || w.name == cand;
        if (!used) return cand;
    }
    return "Workspace";
}

void saveNamed(App& a, const std::string& name) {
    std::vector<Workspace> list = workspaceList();
    const Workspace w = workspaceCapture(a, name);
    bool replaced = false;
    for (Workspace& o : list) if (o.name == name) { o = w; replaced = true; }   // the same name is saved over
    if (!replaced) list.push_back(w);
    workspaceStore(list);
    a.engine.log(std::string(replaced ? "Workspace updated: " : "Workspace saved: ") + name);
}

}  // namespace

void workspaceMenu(App& a) {
    const bool running = a.engine.running();
    const std::vector<Workspace> list = workspaceList();
    if (ImGui::MenuItem(tr("Save workspace..."))) {
        gSave.request = true;
        snprintf(gSave.name, sizeof gSave.name, "%s", nextDefaultName(list).c_str());
    }
    if (ImGui::BeginMenu(tr("Load workspace"), !list.empty())) {
        if (running) ImGui::TextDisabled("%s", tr("The receiver stops, loads it and starts again"));
        for (const Workspace& w : list) {
            if (ImGui::MenuItem(w.name.c_str())) {   // a running receiver is stopped for the load and started again (it used to grey the whole list out)
                std::string why;
                if (running) a.engine.stop();
                if (!workspaceApply(a, w, why)) a.engine.log("Workspace not loaded: " + why);
                else if (!why.empty()) a.engine.log("Workspace loaded with a note: " + why);
                else a.engine.log("Workspace loaded: " + w.name);
                if (running) startReceiver(a);
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Delete workspace"), !list.empty())) {
        for (const Workspace& w : list) {
            if (ImGui::MenuItem(w.name.c_str())) {
                std::vector<Workspace> keep;
                for (const Workspace& o : list) if (o.name != w.name) keep.push_back(o);
                workspaceStore(keep);
                a.engine.log("Workspace deleted: " + w.name);
            }
        }
        ImGui::EndMenu();
    }
}

void workspaceDialogs(App& a) {
    if (gSave.request) { ImGui::OpenPopup(trw("Save workspace")); gSave.request = false; }
    if (ImGui::BeginPopupModal(trw("Save workspace"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(tr("Name this workspace. It keeps the source, mode, frequency,\nsample rate, gains and open windows."));
        ImGui::SetNextItemWidth(280 * gUi);
        const bool enter = ImGui::InputText("##wsname", gSave.name, sizeof gSave.name, ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string name = trimmed(gSave.name);
        if (name.empty()) ImGui::BeginDisabled();
        if (ImGui::Button(tr("Save")) || (enter && !name.empty())) {
            saveNamed(a, name);
            ImGui::CloseCurrentPopup();
        }
        if (name.empty()) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Another popup (the data-sharing consent, a crash note) opened at the same level closes this one without answering it: keep asking
    // while the answer is pending, but only when no other popup is up, so the two dialogs wait for each other instead of one erasing the other.
    if (workspaceRestorePending() && !ImGui::IsPopupOpen(trw("Restore last session?")) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        ImGui::OpenPopup(trw("Restore last session?"));
        gRestoreAsked = true;
    }
    if (ImGui::BeginPopupModal(trw("Restore last session?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!workspaceRestorePending()) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        const Workspace& w = workspaceRecovered();
        ImGui::TextUnformatted(tr("OnAir did not close cleanly last time."));
        ImGui::Text(tr("Last session: %s, %.3f MHz"), w.devName.empty() ? tr("no radio") : w.devName.c_str(), w.freqMhz);
        if (!w.panels.empty()) ImGui::Text(tr("Windows open: %s"), w.panels.c_str());
        ImGui::TextUnformatted(tr("Restore last session? The settings come back; press Start to listen again."));
        if (ImGui::Button(tr("Restore"))) {
            std::string why;
            workspaceRestoreAnswer(a, true, why);
            if (!why.empty()) a.engine.log("Last session: " + why);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Start fresh"))) {
            std::string why;
            workspaceRestoreAnswer(a, false, why);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
