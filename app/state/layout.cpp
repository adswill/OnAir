// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Per-mode window layout files. See layout.h.
#include "layout.h"
#include "../app.h"
#include "../platform/platform.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <filesystem>
#include <system_error>

namespace {
std::string gCur;   // the ini file in use; empty before the first frame
std::string dirPath() { return plat::dataDir() + "/layouts"; }
}  // namespace

void layoutTick(App& a, bool enabled) {
    dockRegisterIni();   // the dock layout (ui2_dock.cpp) is kept in the same file
    ImGuiIO& io = ImGui::GetIO();
    if (!enabled) { io.IniFilename = nullptr; gCur.clear(); return; }
    std::string subId;
    if (const auto* sm = curSub(a)) subId = sm->id;
    const std::string path = dirPath() + "/" + layoutFileName(a.family, subId);
    if (path == gCur) return;
    std::error_code ec;
    std::filesystem::create_directories(dirPath(), ec);
    if (!gCur.empty()) ImGui::SaveIniSettingsToDisk(gCur.c_str());   // the old mode's layout is kept before the new one is read
    ImGui::ClearIniSettings();
    gCur = path;
    ImGui::LoadIniSettingsFromDisk(gCur.c_str());
    io.IniFilename = gCur.c_str();   // ImGui rewrites it every few seconds while a window moved
}

void layoutSaveNow() {
    if (!gCur.empty()) ImGui::SaveIniSettingsToDisk(gCur.c_str());
}

void layoutReset() {
    std::error_code ec;
    std::filesystem::remove_all(dirPath(), ec);
    ImGui::ClearIniSettings();
    ImGui::GetIO().IniFilename = nullptr;
    gCur.clear();   // the next frame starts an empty file for the current mode
}
