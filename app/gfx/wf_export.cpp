// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Waterfall export: the waterfall written as a sequence of PNG files (one per frame) into a folder the user picks.
// Two ways to take the rows: Frames (a snapshot of the newest rows every few seconds) or Timelapse (one row every few
// seconds, 512 rows to a frame). There is no video encoder here: the user turns the PNG files into a video with their own tool.
#include "../app.h"
#include "../platform/platform.h"
#include "gfx.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {
constexpr int kOutW = Waterfall::W / 4;   // 2048 pixels across: each pixel is the highest level of the history columns it covers
constexpr size_t kRowBytes = (size_t)kOutW * 4;

struct Export {
    bool on = false;
    bool timelapse = false;
    float frameSec = 5.f;        // Frames: a frame every this many seconds (a 1024 x 512 PNG is about 2 MB, stored uncompressed)
    float tlSec = 5.f;           // Timelapse: a row kept every this many seconds
    float everySec = 1.f;        // the value in use for the running export
    std::string dir, prefix;
    long frames = 0;
    double lastT = 0;            // when the last frame, or the last kept row, was taken
    std::vector<uint8_t> block;  // Timelapse: kept rows waiting for a frame, oldest first
    int kept = 0;
    std::string status = "Stopped";
};
Export& st() { static Export e; return e; }

// the row `age` rows back (0 = the newest) as kOutW RGBA pixels, through the waterfall's colour table (0xAABBGGRR)
void rowPixels(const Waterfall& wf, int age, uint8_t* out) {
    const int per = std::max(1, wf.cols / kOutW);   // the history width is a power of two, at least 8192
    const uint8_t* l = wf.lv.data() + (size_t)((wf.writeRow + age) % Waterfall::H) * wf.cols;
    for (int x = 0; x < kOutW; x++) {
        uint8_t m = 0;
        for (int k = 0; k < per; k++) m = std::max(m, l[x * per + k]);
        const uint32_t c = wf.lut[m];
        uint8_t* p = out + (size_t)x * 4;
        p[0] = (uint8_t)(c & 255); p[1] = (uint8_t)((c >> 8) & 255); p[2] = (uint8_t)((c >> 16) & 255); p[3] = 255;
    }
}

// one PNG file, the next number in the folder; rows are top to bottom, newest first
bool writeFrame(const std::vector<uint8_t>& px, int h) {
    Export& e = st();
    char num[32];
    snprintf(num, sizeof num, "_%06ld.png", e.frames + 1);
    const std::string path = e.dir + "/" + e.prefix + num;
    if (!gfx::writePng(path.c_str(), px.data(), kOutW, h)) {
        e.status = "Could not write " + path;
        e.on = false;
        return false;
    }
    e.frames++;
    return true;
}

// Timelapse: the kept rows (stored oldest first) as one frame, newest at the top
void flushBlock() {
    Export& e = st();
    if (e.kept == 0) return;
    const int h = e.kept;
    std::vector<uint8_t> out(kRowBytes * (size_t)h);
    for (int i = 0; i < h; i++) memcpy(out.data() + (size_t)(h - 1 - i) * kRowBytes, e.block.data() + (size_t)i * kRowBytes, kRowBytes);
    writeFrame(out, h);
    e.block.clear();
    e.kept = 0;
}
}

// called for each spectrum row the waterfall takes (app_state.cpp)
void wfExportRow(App& a, double now) {
    Export& e = st();
    if (!e.on) return;
    const Waterfall& wf = a.wf;
    if (wf.lv.size() != (size_t)wf.cols * Waterfall::H || wf.filled <= 0) return;
    if (now - e.lastT < e.everySec) return;
    e.lastT = now;
    if (!e.timelapse) {
        const int n = std::min(wf.filled, (int)Waterfall::H);
        std::vector<uint8_t> px(kRowBytes * (size_t)n);
        for (int age = 0; age < n; age++) rowPixels(wf, age, px.data() + (size_t)age * kRowBytes);
        writeFrame(px, n);
        return;
    }
    e.block.resize(kRowBytes * (size_t)(e.kept + 1));
    rowPixels(wf, 0, e.block.data() + (size_t)e.kept * kRowBytes);
    e.kept++;
    if (e.kept == Waterfall::H) flushBlock();
}

// the Start / Stop block of the waterfall popup
void wfExportPopup(App& a) {
    (void)a;
    Export& e = st();
    static char dirBuf[1024] = "";
    ImGui::Separator();
    ImGui::TextDisabled("Export frames (PNG files)");
    ImGui::BeginDisabled(e.on);
    if (ImGui::RadioButton("Frames", !e.timelapse)) e.timelapse = false;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A PNG of the newest rows every few seconds");
    ImGui::SameLine(0, 6);
    if (ImGui::RadioButton("Timelapse", e.timelapse)) e.timelapse = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("One row every few seconds, 512 rows to a frame: a long span in a short video");
    ImGui::SetNextItemWidth(170 * gUi);
    if (e.timelapse) ImGui::SliderFloat("##wfxtl", &e.tlSec, 1.f, 60.f, "one row every %.0f s");
    else ImGui::SliderFloat("##wfxfr", &e.frameSec, 1.f, 30.f, "a frame every %.0f s");
    if (ImGui::Button("Choose folder...")) {
        const std::string d = plat::openFolderDialog();
        if (!d.empty()) snprintf(dirBuf, sizeof dirBuf, "%s", d.c_str());
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The folder for the PNG files (you can also type the path below)");
    ImGui::SetNextItemWidth(260 * gUi);
    ImGui::InputText("##wfxdir", dirBuf, sizeof dirBuf);
    ImGui::EndDisabled();

    if (!e.on) {
        ImGui::BeginDisabled(dirBuf[0] == 0);
        if (ImGui::Button("Start")) {
            std::error_code ec;   // the folder is created when it is missing
            std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(dirBuf)), ec);
            if (ec) e.status = "Could not create the folder";
            else {
                char stamp[32] = "";
                const time_t t = time(nullptr);
                const struct tm* tmv = localtime(&t);
                if (tmv) strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", tmv);
                e.dir = dirBuf;
                e.prefix = std::string("wf_") + stamp;
                e.everySec = e.timelapse ? e.tlSec : e.frameSec;
                e.frames = 0;
                e.lastT = 0;
                e.block.clear();
                e.kept = 0;
                e.on = true;
                e.status = "Recording";
            }
        }
        ImGui::EndDisabled();
    } else if (ImGui::Button("Stop")) {
        if (e.timelapse) flushBlock();   // the last, shorter frame
        e.on = false;
        if (e.status == "Recording") e.status = "Stopped";   // keeps a write error shown
    }
    ImGui::SameLine(0, 10);
    ImGui::Text("Frames: %ld", e.frames);
    if (dirBuf[0]) ImGui::TextWrapped("Folder: %s", dirBuf);
    else ImGui::TextDisabled("No folder chosen yet.");
    ImGui::TextDisabled("%s", e.status.c_str());
    ImGui::TextWrapped("Each frame is a PNG file, up to 1024 x 512 pixels, newest row at the top. OnAir does not make a video: "
                       "turn the PNG files into one with your own tool, for example ffmpeg with an input pattern like wf_<start time>_%%06d.png.");
}
