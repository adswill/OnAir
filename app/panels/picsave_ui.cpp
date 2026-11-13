// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "picsave_ui.h"
#include "../app.h"
#include "../state/picsave.h"
#include "../platform/platform.h"
#include "../ui/ui_common.h"
#include "../ui/theme.h"
#include "../gfx/gfx.h"
#include "dect2/engine/updater.h"
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace picui {
namespace {
std::string gStatus, gLastPath;
bool gStatusBad = false;
double gStatusAt = -1e9;
fs::path P(const std::string& u8) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(u8.data()), u8.size())); }
std::string home() {
    const char* h = getenv("HOME");
    if (!h || !*h) h = getenv("USERPROFILE");
    return h ? h : "";
}
void say(const std::string& s, bool bad, const std::string& path = "") { gStatus = s; gStatusBad = bad; gLastPath = path; gStatusAt = ImGui::GetTime(); }
// makes <folder>/<mode> and picks a free name; "" with the status set when the folder cannot be used
std::string target(const char* mode, time_t t) {
    const std::string dir = picsave::folderFor(folder(), mode);
    std::error_code ec;
    fs::create_directories(P(dir), ec);
    if (ec || !fs::is_directory(P(dir), ec)) { say(picsave::failText(folder(), true), true); return ""; }
    return picsave::uniquePath(picsave::buildPath(folder(), mode, t), [](const std::string& p) { std::error_code e; return fs::exists(P(p), e); });
}
}  // namespace

std::string folder() {
    const std::string f = plat::prefs().getS("picSaveDir", "");
    return f.empty() ? picsave::defaultFolder(home()) : f;
}

std::string saveRgba(const char* mode, time_t t, const uint8_t* rgba, int w, int h) {
    const std::string path = target(mode, t);
    if (path.empty()) return "";
    if (w <= 0 || h <= 0 || !gfx::writePng(path.c_str(), rgba, w, h)) { say(picsave::failText(folder(), false), true); return ""; }
    say(picsave::savedText(path), false, path);
    return path;
}

std::string saveCopy(const char* mode, time_t t, const std::string& pngFile) {
    const std::string path = target(mode, t);
    if (path.empty()) return "";
    std::error_code ec;
    fs::copy_file(P(pngFile), P(path), fs::copy_options::none, ec);
    if (ec) { say(picsave::failText(folder(), false), true); return ""; }
    say(picsave::savedText(path), false, path);
    return path;
}

void tooltipFor(const char* mode) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", picsave::tooltipText(folder(), mode).c_str());
}

void settingRow() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Save pictures to"));
    ImGui::SameLine(0, 6 * gUi);
    const std::string f = folder();
    ImGui::TextUnformatted(ellipsize(f, std::max(60.f * gUi, ImGui::GetContentRegionAvail().x - 190 * gUi), ImGui::GetFontSize()).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.c_str());
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::SmallButton(tr("Choose..."))) {
        const std::string d = plat::openFolderDialog();
        if (!d.empty()) plat::prefs().setS("picSaveDir", d);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Pictures you save go to a folder per mode inside this one, named by date, time and mode."));
    ImGui::SameLine(0, 4 * gUi);
    if (ImGui::SmallButton(tr("Open folder##picsave"))) {
        std::error_code ec; fs::create_directories(P(f), ec);
        dect2::openUrl(f);
    }
}

void statusLine() {
    if (gStatus.empty() || ImGui::GetTime() - gStatusAt > 30.0) return;
    ImGui::PushTextWrapPos(0);
    if (gStatusBad) ImGui::TextColored(pal::warnAmber(), "%s", gStatus.c_str()); else ImGui::TextDisabled("%s", gStatus.c_str());
    ImGui::PopTextWrapPos();
    if (!gStatusBad && !gLastPath.empty()) {
        ImGui::SameLine(0, 6 * gUi);
        if (ImGui::SmallButton(tr("Open folder##picsaved"))) dect2::openUrl(picsave::parentOf(gLastPath));
    }
}

}  // namespace picui
