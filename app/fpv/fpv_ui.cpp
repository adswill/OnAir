// Analog FPV video screens: the live picture (with snapshot and recording), the channel grid with the scan, and the capture's spectrum.
// The receiver (FpvReceiver) measures the channels and makes the pictures; the telemetry carries the levels and the numbers. The on-screen display
// of the flight controller is part of the picture: it is shown, not decoded.
#include "app.h"
#include "../d40/d40_ui.h"
#include "dect2/atv/atv_std.h"
#include "dect2/fpv/fpv_chan.h"
#include "dect2/fpv/fpv_live.h"
#include "dect2/fpv/fpv_tel.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

using dect2::FpvChannelInfo;
using dect2::FpvTelemetry;

struct State {
    int view = 0;                               // 0 video, 1 channels, 2 spectrum
    bool loaded = false, wasRunning = false;
    gfx::Image* img = nullptr;
    int imgW = 0, imgH = 0;
    uint64_t frameSeq = 0;
    std::shared_ptr<const AtvFrame> frame;
    bool frameDirty = false;
    int deint = 0;                              // 0 weave, 1 bob
    int sysHint = -1;                           // -1 automatic, 0 PAL, 1 NTSC
    bool colourOn = true;
    float saturation = 1.f;
    int sub = 0;                                // 0 automatic, 1 6.0 MHz, 2 6.5 MHz
    int pushedDeint = -1, pushedSys = -2, pushedSub = -1;
    bool pushedColour = false;
    float pushedSat = -1.f, pushedVol = -1.f;
    bool pushedMute = false, pushedAudio = false;
    // the scan
    bool scanning = false, scanBest = true;
    std::vector<double> scanList;               // centre frequencies, MHz
    size_t scanIdx = 0;
    double scanT0 = 0;
    double scanFrom = 0;                        // the frequency to go back to
    int scanPlan = 0;                           // 0 5.8 GHz, 1 1.2 GHz
    // recording
    bool rec = false;
    std::string recDir;
    int recN = 0;
    double recLast = 0;
    std::string note;
    double noteT = 0;
};
State S;

bool live(const App& a) { return a.rx.standard == 102; }

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    if (const char* v = std::getenv("ONAIR_FPV_VIEW")) S.view = std::max(0, std::min(2, std::atoi(v)));   // dev: the screen to open on (screenshots)
    S.deint = (int)plat::prefs().getI("fpvDeint", 0) == 1 ? 1 : 0;
    S.sub = (int)std::max<long long>(0, std::min<long long>(2, (long long)plat::prefs().getI("fpvSub", 0)));
}

void say(const std::string& s) { S.note = s; S.noteT = ImGui::GetTime(); }
void sayf(const char* fmt, const char* arg) { char b[700]; snprintf(b, sizeof b, fmt, arg); say(b); }

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void nextIf(float w) { sameLineIf(w * gUi, 8 * gUi); }
float fit(float want) { return std::max(40.f * gUi, std::min(want * gUi, ImGui::GetContentRegionAvail().x - 6 * gUi)); }

std::string fpvDir() {
    const std::string dir = plat::dataDir() + "/fpv";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(dir.c_str())), ec);
    return dir;
}

std::string stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char b[40];
    std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tmv);
    return b;
}

// ---------------------------------------------------------------- the controls pushed to the receiver

void pushControls(App& a) {
    dect2::FpvReceiver& rx = a.engine.fpv();
    if (S.pushedDeint != S.deint) { rx.setDeinterlace(S.deint); S.pushedDeint = S.deint; }
    if (S.pushedSys != S.sysHint) { rx.setStandard(S.sysHint < 0 ? -1 : S.sysHint == 0 ? (int)kAtvG : (int)kAtvM, S.sysHint < 0 ? -1 : S.sysHint == 0 ? (int)kAtvPal : (int)kAtvNtsc); S.pushedSys = S.sysHint; }
    if (S.pushedColour != S.colourOn) { rx.setColour(S.colourOn); S.pushedColour = S.colourOn; }
    if (S.pushedSat != S.saturation) { rx.setSaturation(S.saturation); S.pushedSat = S.saturation; }
    if (S.pushedSub != S.sub) { rx.setAudioSubcarrier(S.sub == 1 ? 6.0e6 : S.sub == 2 ? 6.5e6 : 0.0); S.pushedSub = S.sub; }
    if (!S.pushedAudio) { rx.setAudio(true); S.pushedAudio = true; }
    if (a.volume != S.pushedVol || a.muted != S.pushedMute) { rx.setVolume(a.volume); rx.setMuted(a.muted); S.pushedVol = a.volume; S.pushedMute = a.muted; }
}

// ---------------------------------------------------------------- the scan

// the centres to visit: every channel of the plan ends up within 4.5 MHz of one of them
std::vector<double> scanCentres(int plan) {
    std::vector<double> f;
    for (const auto& c : dect2::fpvAllChannels()) if ((c.band == 6 ? 1 : 0) == plan) f.push_back(c.freqHz / 1e6);
    std::sort(f.begin(), f.end());
    f.erase(std::unique(f.begin(), f.end()), f.end());
    std::vector<double> out;
    size_t i = 0;
    while (i < f.size()) {
        // the centre is the first uncovered channel plus 4 MHz, but never above the last channel that it covers
        const double lo = f[i];
        size_t j = i;
        while (j + 1 < f.size() && f[j + 1] - lo <= 8.0) j++;
        out.push_back(std::round((f[i] + f[j]) / 2 * 10) / 10);
        i = j + 1;
    }
    return out;
}

void startScan(App& a, int plan) {
    if (!a.engine.running() || !live(a)) { say(tr("Start the receiver first.")); return; }
    S.scanPlan = plan;
    S.scanList = scanCentres(plan);
    S.scanIdx = 0; S.scanT0 = 0;
    S.scanFrom = a.freqMhz;
    S.scanning = true;
    a.engine.fpv().clearChannels();
}

void stopScan() { S.scanning = false; }

const FpvChannelInfo* strongest(const FpvTelemetry& t, int plan) {
    const FpvChannelInfo* best = nullptr;
    for (const auto& c : t.channels) {
        if ((c.freqHz > 3e9 ? 0 : 1) != plan) continue;
        if (c.present && (!best || c.overDb > best->overDb)) best = &c;
    }
    return best;
}

void scanStep(App& a) {
    if (!S.scanning) return;
    if (!a.engine.running() || !live(a)) { S.scanning = false; return; }
    const double now = ImGui::GetTime();
    if (S.scanT0 > 0 && now - S.scanT0 < 0.7) return;   // settle, then the receiver has measured the channels in the capture
    if (S.scanIdx >= S.scanList.size()) {
        S.scanning = false;
        const FpvChannelInfo* b = strongest(a.rx.fpv, S.scanPlan);
        if (b && S.scanBest) { tuneFreq(a, b->freqHz / 1e6); sayf(tr("Scan done. Strongest: %s"), b->name.c_str()); }
        else { tuneFreq(a, S.scanFrom); if (b) sayf(tr("Scan done. Strongest: %s"), b->name.c_str()); else say(tr("Scan done. No transmitter found.")); }
        return;
    }
    tuneFreq(a, S.scanList[S.scanIdx++]);
    S.scanT0 = now;
}

// ---------------------------------------------------------------- the picture

void pullFrame(App& a) {
    if (!live(a)) return;
    auto f = a.engine.fpv().frame(S.frameSeq);
    if (!f || f->width <= 0 || f->height <= 0 || f->rgba.size() < (size_t)f->width * f->height * 4) return;
    S.frame = f; S.frameDirty = true;
    if (S.rec && ImGui::GetTime() - S.recLast >= 0.2) {   // up to 5 pictures a second as numbered PNG files
        char n[40]; snprintf(n, sizeof n, "/%05d.png", S.recN++);
        const std::string p = S.recDir + n;
        gfx::writePng(p.c_str(), f->rgba.data(), f->width, f->height);
        S.recLast = ImGui::GetTime();
    }
}

void upload() {
    if (!S.frame || !S.frameDirty || !gGfx) return;
    S.frameDirty = false;
    const auto& f = *S.frame;
    if (!S.img || S.imgW != f.width || S.imgH != f.height) {
        delete S.img;
        S.img = gGfx->createImage(f.width, f.height, 0xFF000000u);
        S.imgW = f.width; S.imgH = f.height;
    }
    std::vector<uint32_t> px((size_t)f.width * f.height);
    memcpy(px.data(), f.rgba.data(), px.size() * 4);
    S.img->update(0, 0, f.width, f.height, px.data());
}

void snapshot(App& a) {
    if (!S.frame) { say(tr("No picture yet.")); return; }
    const std::string p = fpvDir() + "/fpv-" + stamp() + ".png";
    if (gfx::writePng(p.c_str(), S.frame->rgba.data(), S.frame->width, S.frame->height)) sayf(tr("Saved %s"), p.c_str());
    else say(tr("The picture could not be saved."));
    (void)a;
}

void toggleRecord(App& a) {
    if (S.rec) { S.rec = false; char b[200]; snprintf(b, sizeof b, tr("Recording stopped: %d pictures"), S.recN); say(b); return; }
    const FpvTelemetry& t = a.rx.fpv;
    S.recDir = fpvDir() + "/" + stamp() + (t.tunedName.empty() ? "" : "-" + t.tunedName);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(S.recDir.c_str())), ec);
    S.recN = 0; S.recLast = 0; S.rec = true;
    sayf(tr("Recording into %s"), S.recDir.c_str());
}

void picture(App& a, ImVec2 box) {
    const bool on = live(a);
    const FpvTelemetry& t = a.rx.fpv;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##fpvpic", box);
    dl->AddRectFilled(p0, ImVec2(p0.x + box.x, p0.y + box.y), IM_COL32(0, 0, 0, 255));
    const float ar = S.img && S.imgH > 0 ? (float)S.imgW / S.imgH : 4.f / 3.f;
    ImVec2 sz = box;
    if (box.x / box.y > ar) sz.x = box.y * ar; else sz.y = box.x / ar;
    const ImVec2 q0(p0.x + (box.x - sz.x) * 0.5f, p0.y + (box.y - sz.y) * 0.5f), q1(q0.x + sz.x, q0.y + sz.y);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && S.img && S.frame) { a.videoOnly = true; if (!glfwGetWindowMonitor(gWindow)) toggleFullscreen(); }   // double-click: full screen (#326)
    if (ImGui::IsItemHovered() && S.img && S.frame) ImGui::SetTooltip("%s", tr("Double-click (or press F) for full screen, Esc to leave"));
    if (S.img && S.frame) {
        dl->AddImage(S.img->texture(), q0, q1);
        if (!on || t.state != 2) dl->AddRectFilled(q0, q1, IM_COL32(0, 0, 0, 150));   // an old picture
    }
    if (S.rec) dl->AddCircleFilled(ImVec2(p0.x + 14 * gUi, p0.y + 14 * gUi), 5 * gUi, IM_COL32(230, 60, 50, 255));
    const char* msg = nullptr;
    if (!a.engine.running()) msg = tr("stopped");
    else if (!on) msg = tr("starting");
    else if (t.state == 0) msg = tr("no transmitter on this channel");
    else if (t.state == 1) msg = tr("transmitter found, waiting for sync");
    else if (!S.frame) msg = tr("waiting for a picture");
    if (msg) {
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(p0.x + (box.x - ts.x) * 0.5f, p0.y + (box.y - ts.y) * 0.5f), IM_COL32(170, 174, 178, 255), msg);
    }
}

std::string caption(const App& a) {
    const FpvTelemetry& t = a.rx.fpv;
    if (!live(a)) return a.engine.running() ? tr("starting") : tr("stopped");
    char b[220];
    const char* ch = t.tunedName.empty() ? "" : t.tunedName.c_str();
    if (t.video.system.empty()) { snprintf(b, sizeof b, "%s%s%.0f MHz", ch, ch[0] ? "  " : "", t.tunedHz / 1e6); return b; }
    snprintf(b, sizeof b, "%s%s%.0f MHz   %s   %s", ch, ch[0] ? "  " : "", t.tunedHz / 1e6, t.video.system.c_str(),
             t.video.colour ? (t.video.colourSystem.empty() ? tr("colour") : t.video.colourSystem.c_str()) : tr("monochrome"));
    return b;
}

void videoView(App& a) {
    pullFrame(a);
    upload();
    const bool on = live(a);
    const FpvTelemetry& t = a.rx.fpv;
    const float W = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    {
        const float pillsX = W - 250 * gUi, x0 = ImGui::GetCursorPosX();
        ImGui::PushFont(a.mono, 0);
        const std::string cap = caption(a), shown = ellipsize(cap, std::max(40 * gUi, pillsX - x0 - 12 * gUi));
        ImGui::TextUnformatted(shown.c_str());
        ImGui::PopFont();
        if (shown != cap && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", cap.c_str());
    }
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20 * gUi, W - 250 * gUi));
    if (pillButton(tr("Snapshot"), false)) snapshot(a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Saves the picture as a PNG file into <data folder>/fpv/."));
    ImGui::SameLine(0, 4 * gUi);
    if (pillButton(S.rec ? tr("Stop") : tr("Record"), S.rec)) toggleRecord(a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Records up to 5 pictures a second as numbered PNG files into a new folder in <data folder>/fpv/."));
    const float rest = ImGui::GetContentRegionAvail().y - (S.note.empty() ? 0 : ImGui::GetTextLineHeightWithSpacing());
    const bool side = W - rest * 4.f / 3.f > 260 * gUi;
    if (side) {
        const float sw = std::min(W * 0.35f, std::max(240.f * gUi, W - rest * 4.f / 3.f - 8 * gUi));
        picture(a, ImVec2(W - sw - 8 * gUi, rest));
        ImGui::SameLine(0, 8 * gUi);
        ImGui::BeginGroup();
        kvRow(a, tr("channel"), "%s", on && !t.tunedName.empty() ? (t.tunedName + "  " + std::to_string((int)std::lround(t.tunedHz / 1e6)) + " MHz").c_str() : "-");
        kvRow(a, tr("level"), "%s", on ? (std::to_string((int)std::lround(t.overDb)) + " dB").c_str() : "-");
        kvRow(a, tr("video SNR"), "%s", on && t.state == 2 ? (std::to_string((int)std::lround(t.snrDb)) + " dB").c_str() : "-");
        kvRow(a, tr("sync"), "%s", on && t.state >= 1 ? (std::to_string((int)std::lround(t.video.syncQuality * 100)) + " %").c_str() : "-");
        char b[64];
        if (on && t.audioPresent) snprintf(b, sizeof b, "%.1f MHz", t.audioSubHz / 1e6); else snprintf(b, sizeof b, "-");
        kvRow(a, tr("sound"), "%s", b);
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("The on-screen display (OSD) of the flight controller is part of the picture; it is shown, not decoded."));
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
    } else {
        picture(a, ImVec2(W, rest));
    }
    if (!S.note.empty()) {
        if (ImGui::GetTime() - S.noteT > 12) S.note.clear();
        else { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.note.c_str()); ImGui::PopTextWrapPos(); }
    }
}

// ---------------------------------------------------------------- the channel grid

ImU32 levelCol(const FpvChannelInfo& c) {
    if (c.ageSec < 0) return IM_COL32(60, 64, 68, 255);
    if (!c.present) return IM_COL32(70, 90, 110, 255);
    return c.overDb > 25 ? IM_COL32(80, 200, 110, 255) : c.overDb > 15 ? IM_COL32(150, 200, 90, 255) : IM_COL32(220, 190, 80, 255);
}

void channelsView(App& a) {
    const bool on = live(a);
    const FpvTelemetry& t = a.rx.fpv;
    if (!S.scanning) {
        if (ImGui::Button(tr("Scan 5.8 GHz"))) startScan(a, 0);
        ImGui::SameLine();
        if (ImGui::Button(tr("Scan 1.2 GHz"))) startScan(a, 1);
    } else {
        if (ImGui::Button(tr("Stop scan"))) stopScan();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu / %zu", S.scanIdx, S.scanList.size());
    }
    ImGui::SameLine();
    ImGui::Checkbox(tr("Tune to the strongest"), &S.scanBest);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("When the scan is done, tune to the channel with the most signal. Otherwise go back to the frequency before the scan."));
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear"))) a.engine.fpv().clearChannels();
    if (!on) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the channels")); }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("A scan tunes the radio over the channels one after the other and measures each one inside the capture. Click a channel to tune to it."));
    ImGui::PopTextWrapPos();
    d40FpvPickLine(t);   // 4.0: free channels for N pilots (app/d40)
    ImGui::Spacing();
    const float W = ImGui::GetContentRegionAvail().x;
    const float labW = 54 * gUi, cellW = std::max(48.f * gUi, (W - labW) / 8.f), cellH = 46 * gUi;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int b = 0; b < dect2::kFpvBandCount; b++) {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(W, cellH + 4 * gUi));
        char lab[8]; snprintf(lab, sizeof lab, "%c", dect2::kFpvBands[b].letter == 'G' ? '1' : dect2::kFpvBands[b].letter);
        dl->AddText(ImVec2(p0.x + 4 * gUi, p0.y + cellH * 0.5f - ImGui::GetTextLineHeight() * 0.5f), IM_COL32(190, 194, 198, 255), dect2::kFpvBands[b].letter == 'G' ? "1.2G" : lab);
        for (int c = 0; c < 8; c++) {
            const size_t idx = (size_t)b * 8 + (size_t)c;
            const FpvChannelInfo* ci = on && idx < t.channels.size() ? &t.channels[idx] : nullptr;
            const ImVec2 q0(p0.x + labW + c * cellW, p0.y), q1(q0.x + cellW - 3 * gUi, q0.y + cellH);
            const double mhz = dect2::kFpvBands[b].mhz[c];
            const bool tuned = on && std::fabs(t.tunedHz / 1e6 - mhz) < 0.5 && !t.tunedName.empty() && t.tunedName == dect2::fpvChannelName(b, c);
            dl->AddRectFilled(q0, q1, IM_COL32(26, 28, 31, 255), 3 * gUi);
            if (ci && ci->ageSec >= 0) {   // a bar for the level over the noise, 0 .. 40 dB
                const float h = std::max(0.f, std::min(1.f, ci->overDb / 40.f)) * (cellH - 4 * gUi);
                dl->AddRectFilled(ImVec2(q0.x + 2 * gUi, q1.y - 2 * gUi - h), ImVec2(q1.x - 2 * gUi, q1.y - 2 * gUi), levelCol(*ci), 2 * gUi);
            }
            if (tuned) dl->AddRect(q0, q1, IM_COL32(230, 200, 90, 255), 3 * gUi, 0, 2.f);
            char n[16]; snprintf(n, sizeof n, "%c%d", dect2::kFpvBands[b].letter, c + 1);
            char f[16]; snprintf(f, sizeof f, "%.0f", mhz);
            dl->AddText(ImVec2(q0.x + 4 * gUi, q0.y + 2 * gUi), IM_COL32(235, 238, 240, 255), n);
            dl->AddText(ImVec2(q0.x + 4 * gUi, q0.y + 2 * gUi + ImGui::GetTextLineHeight()), IM_COL32(170, 176, 182, 255), f);
            ImGui::SetCursorScreenPos(q0);
            char id[24]; snprintf(id, sizeof id, "##fc%d_%d", b, c);
            if (ImGui::InvisibleButton(id, ImVec2(q1.x - q0.x, q1.y - q0.y)) && !S.scanning) tuneFreq(a, mhz);
            if (ImGui::IsItemHovered()) {
                if (ci && ci->ageSec >= 0) ImGui::SetTooltip("%s  %.0f MHz\n%.1f dBFS, %.1f dB %s", ci->name.c_str(), mhz, ci->levelDb, ci->overDb, tr("over the noise"));
                else ImGui::SetTooltip("%s  %.0f MHz\n%s", dect2::fpvChannelName(b, c).c_str(), mhz, tr("not measured yet"));
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + cellH + 4 * gUi));
    }
    if (on && t.rssiHist.size() > 1) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("tuned channel level"));
        float lo = 1e30f, hi = -1e30f;
        for (float v : t.rssiHist) { lo = std::min(lo, v); hi = std::max(hi, v); }
        ImGui::PlotLines("##fpvrssi", t.rssiHist.data(), (int)t.rssiHist.size(), 0, nullptr, lo - 3, hi + 3, ImVec2(ImGui::GetContentRegionAvail().x, 46 * gUi));
    }
}

// ---------------------------------------------------------------- the spectrum

void spectrumView(App& a) {
    const bool on = live(a);
    const FpvTelemetry& t = a.rx.fpv;
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(120.f * gUi, std::min(260.f * gUi, ImGui::GetContentRegionAvail().y * 0.5f)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##fpvspec", size);
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    dl->AddRectFilled(p0, p1, IM_COL32(8, 9, 10, 255));
    const float labW = 34 * gUi, labH = ImGui::GetTextLineHeight() + 2;
    const ImVec2 a0(p0.x + labW, p0.y + 4 * gUi), a1(p1.x - 6 * gUi, p1.y - labH);
    if (a1.x - a0.x >= 20 && a1.y - a0.y >= 20) {
        const ImU32 grid = IM_COL32(48, 52, 54, 255), txt = IM_COL32(120, 124, 128, 255);
        const double span = on && t.specSpanMhz > 0 ? t.specSpanMhz : 20.0;
        auto X = [&](double mhz) { return a0.x + (float)((mhz + span / 2) / span) * (a1.x - a0.x); };
        float top = -20, bot = -90;
        if (on && !t.spec.empty()) { float mx = -200; for (float v : t.spec) mx = std::max(mx, v); top = std::ceil(mx / 10.f) * 10.f + 10.f; bot = top - 70.f; }
        auto Y = [&](float v) { return a1.y - (v - bot) / (top - bot) * (a1.y - a0.y); };
        for (float d = bot; d <= top + 0.1f; d += 10.f) {
            dl->AddLine(ImVec2(a0.x, Y(d)), ImVec2(a1.x, Y(d)), grid);
            char b[16]; snprintf(b, sizeof b, "%.0f", d);
            dl->AddText(ImVec2(p0.x + 3 * gUi, Y(d) - ImGui::GetTextLineHeight() * 0.5f), txt, b);
        }
        float lastX = -FLT_MAX;
        for (int m = (int)std::ceil(-span / 2); m <= (int)std::floor(span / 2); m += 5) {
            const float x = X(m);
            dl->AddLine(ImVec2(x, a0.y), ImVec2(x, a1.y), grid);
            char b[16]; snprintf(b, sizeof b, "%+d", m);
            const float tw = ImGui::CalcTextSize(b).x;
            if (x - tw * 0.5f < lastX + 4 * gUi) continue;
            dl->AddText(ImVec2(x - tw * 0.5f, a1.y + 1), txt, b);
            lastX = x + tw * 0.5f;
        }
        if (on && t.spec.size() > 1) {
            dl->PushClipRect(a0, a1, true);
            for (const auto& c : t.channels) {   // the channels inside the capture
                if (!c.inCapture) continue;
                const float x = X((c.freqHz - t.centreHz) / 1e6);
                dl->AddLine(ImVec2(x, a0.y), ImVec2(x, a1.y), c.present ? IM_COL32(120, 200, 130, 120) : IM_COL32(200, 160, 70, 70));
                dl->AddText(ImVec2(x + 2, a0.y), IM_COL32(200, 204, 208, 200), c.name.c_str());
            }
            std::vector<ImVec2> pts(t.spec.size());
            for (size_t i = 0; i < pts.size(); i++) pts[i] = ImVec2(a0.x + (float)i / (float)(pts.size() - 1) * (a1.x - a0.x), Y(std::max(bot, std::min(top, t.spec[i]))));
            dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(140, 230, 160, 255), 0, 1.4f);
            dl->PopClipRect();
        }
    }
    ImGui::TextDisabled("%s", tr("the capture, MHz from the centre; the lines are the channels of the plans inside it"));
    if (!on) ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the spectrum"));
}

void tab(App& a) {
    loadState();
    subNav("fpv", S.view, {tr("Video"), tr("Channels"), tr("Spectrum")});
    if (S.view == 0) videoView(a);
    else { pullFrame(a); if (S.view == 1) channelsView(a); else spectrumView(a); }
}

// ---------------------------------------------------------------- the other hooks

void tick(App& a) {
    loadState();
    const bool run = a.engine.running();
    if (run) {
        if (!S.wasRunning) { S.pushedDeint = -1; S.pushedSys = -2; S.pushedSub = -1; S.pushedColour = !S.colourOn; S.pushedSat = -1.f; S.pushedVol = -1.f; S.pushedAudio = false; }
        pushControls(a);
        scanStep(a);
        if (S.view != 0) pullFrame(a);
    } else {
        S.scanning = false;
        S.rec = false;
    }
    S.wasRunning = run;
}

void tuner(App& a, bool&) {
    loadState();
    // the band and the channel of the frequency now
    int band = -1, ch = -1;
    const auto all = dect2::fpvAllChannels();
    const int nearIdx = dect2::fpvNearest(all, a.freqMhz * 1e6, 1.0e6);
    if (nearIdx >= 0) { band = all[(size_t)nearIdx].band; ch = all[(size_t)nearIdx].ch; }
    ImGui::TextDisabled("%s", tr("Band")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(fit(120));
    if (ImGui::BeginCombo("##fpvband", band >= 0 ? dect2::kFpvBands[band].name : tr("(none)"))) {
        for (int b = 0; b < dect2::kFpvBandCount; b++)
            if (ImGui::Selectable(dect2::kFpvBands[b].name, b == band)) { tuneFreq(a, dect2::kFpvBands[b].mhz[ch >= 0 ? ch : 4]); }
        ImGui::EndCombo();
    }
    nextIf(150);
    ImGui::TextDisabled("%s", tr("Channel")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(fit(110));
    char cur[40] = "-";
    if (band >= 0 && ch >= 0) snprintf(cur, sizeof cur, "%d  %.0f MHz", ch + 1, dect2::kFpvBands[band].mhz[ch]);
    if (ImGui::BeginCombo("##fpvch", cur)) {
        if (band >= 0)
            for (int c = 0; c < 8; c++) {
                char n[40]; snprintf(n, sizeof n, "%d  %.0f MHz", c + 1, dect2::kFpvBands[band].mhz[c]);
                if (ImGui::Selectable(n, c == ch)) tuneFreq(a, dect2::kFpvBands[band].mhz[c]);
            }
        ImGui::EndCombo();
    }
}

void decoder(App& a, bool&) {
    loadState();
    ImGui::TextDisabled("%s", tr("System")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(fit(120));
    const char* sys[] = {tr("automatic"), "PAL (625)", "NTSC (525)"};
    if (ImGui::BeginCombo("##fpvsys", sys[S.sysHint + 1])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(sys[i], S.sysHint + 1 == i)) S.sysHint = i - 1;
        ImGui::EndCombo();
    }
    nextIf(240);
    ImGui::TextDisabled("%s", tr("Sound")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(fit(120));
    const char* sub[] = {tr("automatic"), "6.0 MHz", "6.5 MHz"};
    if (ImGui::BeginCombo("##fpvsub", sub[S.sub])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(sub[i], S.sub == i)) { S.sub = i; plat::prefs().setI("fpvSub", i); savePrefs(a); }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The sound subcarrier of the video transmitter, an FM carrier above the picture. Automatic takes the stronger of 6.0 and 6.5 MHz."));
    ImGui::Checkbox(tr("Colour"), &S.colourOn);
    nextIf(220);
    ImGui::TextDisabled("%s", tr("Saturation")); ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(fit(90));
    ImGui::SliderFloat("##fpvsat", &S.saturation, 0.f, 2.f, "%.2f");
    ImGui::TextDisabled("%s", tr("Fields")); ImGui::SameLine(0, 5 * gUi);
    if (pillButton(tr("Weave"), S.deint == 0)) { S.deint = 0; plat::prefs().setI("fpvDeint", 0); savePrefs(a); }
    ImGui::SameLine(0, 4 * gUi);
    if (pillButton(tr("Bob"), S.deint == 1)) { S.deint = 1; plat::prefs().setI("fpvDeint", 1); savePrefs(a); }
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const FpvTelemetry& t = a.rx.fpv;
    kvRow(a, tr("state"), "%s", dect2::fpvSummary(t).c_str());
    kvRow(a, tr("input"), "%.3f Msps, %.3f MHz", t.inputRate / 1e6, t.centreHz / 1e6);
    kvRow(a, tr("level"), "%.1f dBFS, %.1f dB %s (%.1f dBFS)", t.levelDb, t.overDb, tr("over the noise"), t.noiseFloorDb);
    kvRow(a, tr("offset"), "%+.0f kHz", t.cfoHz / 1e3);
    if (!t.video.system.empty()) {
        kvRow(a, tr("picture"), "%s, %d %s, %.2f %s", t.video.system.c_str(), t.video.lines, tr("lines"), t.video.fieldHz, tr("fields/s"));
        kvRow(a, tr("sync"), "%.0f %%, %s %.0f %%", t.video.syncQuality * 100, tr("depth"), t.video.syncDepthPct);
        kvRow(a, tr("fields"), "%llu %s, %llu %s", (unsigned long long)t.blocksOk, tr("ok"), (unsigned long long)t.blocksBad, tr("damaged"));
        kvRow(a, tr("polarity"), "%s", t.video.fmSyncLow ? tr("sync tip is the lowest frequency") : tr("sync tip is the highest frequency"));
    }
    kvRow(a, tr("sound"), "6.0 MHz %.0f dB, 6.5 MHz %.0f dB %s%s", t.audioSubDb[0], t.audioSubDb[1], tr("over the noise beside them"), t.audioPresent ? "" : (std::string(", ") + tr("none found")).c_str());
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("5.8 GHz bands A, B, E, F, R (Raceband) and L, and the 1.2 GHz plan. Needs a wide radio: 20 Msps (a HackRF). The picture is the frequency of the carrier; PAL or NTSC is found by itself."));
    ImGui::PopTextWrapPos();
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const FpvTelemetry& t = a.rx.fpv;
    int n = 0;
    std::vector<const FpvChannelInfo*> v;
    for (const auto& c : t.channels) if (c.present) v.push_back(&c);
    std::sort(v.begin(), v.end(), [](const FpvChannelInfo* x, const FpvChannelInfo* y) { return x->overDb > y->overDb; });
    for (const FpvChannelInfo* c : v) {
        if (n++ >= 10) break;
        char b[80]; snprintf(b, sizeof b, "%s  %.0f MHz  %.0f dB", c->name.c_str(), c->freqHz / 1e6, c->overDb);
        ImGui::PushFont(a.mono, 0);
        if (ImGui::Selectable(b, std::fabs(c->freqHz - t.tunedHz) < 1e3) && !S.scanning) tuneFreq(a, c->freqHz / 1e6);
        ImGui::PopFont();
    }
    if (v.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("no transmitter heard: scan the channels, or tune to one")); ImGui::PopTextWrapPos(); }
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const FpvTelemetry& t = a.rx.fpv;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Transmitter"), !on ? 0 : t.state >= 1 ? 1 : 0); flowNext(12 * gUi);
    lamp(tr("Video sync"), !on ? 0 : t.state == 2 ? 1 : t.state == 1 ? 2 : 0); flowNext(12 * gUi);
    lamp(tr("Sound"), !on ? 0 : t.audioPresent ? 1 : t.state >= 1 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::fpvSummary(t).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = tr("FPV video");
    if (live(a)) l2 = dect2::fpvSummary(a.rx.fpv);
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const bool on = live(a);
    const FpvTelemetry& t = a.rx.fpv;
    out.push_back({"OVER NOISE  dB", "%.1f", on ? t.overDb : 0, 0, 50, !on ? 0 : t.overDb >= 20 ? 1 : t.overDb >= 8 ? 2 : 3});
    out.push_back({"VIDEO SNR  dB", "%.1f", on ? t.snrDb : 0, 0, 50, !on ? 0 : t.snrDb >= 30 ? 1 : t.snrDb >= 20 ? 2 : 3});
    out.push_back({"SYNC FOUND  %", "%.0f", on ? t.video.syncQuality * 100 : 0, 0, 100, !on ? 0 : t.video.syncQuality > 0.9f ? 1 : t.video.syncQuality > 0.5f ? 2 : 3});
}

void synth(App& a, bool& changed) {
    dect2::SynthConfig& sc = a.tune.synth;
    auto combo = [&](const char* id, int& v, std::initializer_list<const char*> names, float w) {
        ImGui::SetNextItemWidth(w * gUi);
        int i = 0; const char* cur = "";
        for (const char* n : names) { if (i == v) cur = n; i++; }
        if (ImGui::BeginCombo(id, cur)) {
            i = 0;
            for (const char* n : names) { if (ImGui::Selectable(n, v == i)) { v = i; changed = true; } i++; }
            ImGui::EndCombo();
        }
    };
    ImGui::TextDisabled("%s", tr("system")); ImGui::SameLine(0, 5 * gUi);
    combo("##fsys", sc.modeOpt[0], {"PAL (625)", "NTSC (525)"}, 100);
    nextIf(200);
    ImGui::TextDisabled("%s", tr("picture")); ImGui::SameLine(0, 5 * gUi);
    combo("##fpic", sc.modeOpt[1], {tr("test card"), tr("colour bars")}, 100);
    nextIf(220);
    ImGui::TextDisabled("%s", tr("Sound")); ImGui::SameLine(0, 5 * gUi);
    combo("##fsnd", sc.modeOpt[2], {"6.5 MHz", "6.0 MHz", tr("none")}, 90);
    bool off = sc.modeOpt[3] == 1;
    nextIf(200);
    if (ImGui::Checkbox(tr("no transmitter (noise only)"), &off)) { sc.modeOpt[3] = off ? 1 : 0; changed = true; }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("The test signal is a transmitter on the channel the radio is tuned to: FM video with a 1 kHz tone on the sound subcarrier."));
    ImGui::PopTextWrapPos();
}

} // namespace

bool fpvFullPicture(App& a, ImVec2 disp) {   // full screen draws instead of the tab: take the new frames here too (#239)
    loadState();
    pullFrame(a);
    upload();
    if (!S.img || !S.frame) return false;
    const float ar = S.imgH > 0 ? (float)S.imgW / S.imgH : 4.f / 3.f;
    ImVec2 sz = disp;
    if (disp.x / disp.y > ar) sz.x = disp.y * ar; else sz.y = disp.x / ar;
    ImGui::SetCursorPos(ImVec2((disp.x - sz.x) * 0.5f, (disp.y - sz.y) * 0.5f));
    ImGui::Image(S.img->texture(), sz);
    return true;
}
bool fpvHasPicture() { return S.img && S.frame; }

extern const ModeUi kFpvUi;
const ModeUi kFpvUi = {
    .sideTitle = "CHANNELS",
    .tabName = "FPV",
    .tabIcon = Ic::Tv,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .decoder = decoder,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
