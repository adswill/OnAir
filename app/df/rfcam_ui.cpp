// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RF camera: the spatial spectrum of the direction finder laid over a picture as a heat map (rfcam.h does the maths). The picture is a built-in
// demo skyline, a picture or panorama file (reloaded when it changes, so a grabber that rewrites a still gives a live view) or a camera's
// snapshot URL (JPEG, fetched with curl about once a second). Nothing here talks to a camera or radio: azimuth only, see docs/rf-camera.md.
#include "dect2/common/thread_name.h"
#include "rfcam_ui.h"
#include "../app.h"
#include "../gfx/gfx.h"
#include "../ui/scale.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

namespace {

using namespace dect2::df;

enum Pic { PicDemo, PicFile, PicUrl, NumPic };

struct State {
    bool loaded = false;
    bool on = false;
    int pic = PicDemo;
    char path[512] = "";
    char url[512] = "";
    View view;
    int landmark = 0;
    float lmX[2] = {100, 500}, lmAz[2] = {0, 60};
    float floorDb = 12, alpha = 0.65f;
    int bands = 16;
    float lo = 0, hi = 0;                           // band filter, kHz relative to the tuned frequency (both 0 = everything)
    bool markers = true;
    float minStrength = 0.15f;
    // picture
    int picW = 0, picH = 0;
    std::vector<uint32_t> base, comp;
    unsigned gen = 1;                               // bumped when the picture or the result changes: the composition is redone only then
    struct CompKey { unsigned gen = 0; float floorDb = 0, alpha = 0; View view; bool haveRes = false; } compKey;
    std::filesystem::file_time_type fileTime{};
    double picAt = 0;
    std::string picKey;
    gfx::Image* tex = nullptr;
    int texW = 0, texH = 0;
    std::atomic<int> fetch{0};                      // 0 idle, 1 running, 2 done ok, 3 failed
    std::thread th;
    // result
    CamResult res;
    bool haveRes = false;
    double resAt = 0;
    std::string msg;
    std::vector<Marker> shown;
};
State S;

void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& p = plat::prefs();
    S.on = p.getB("rcOn", false); S.pic = std::clamp((int)p.getI("rcPic", 0), 0, NumPic - 1);
    snprintf(S.path, sizeof S.path, "%s", p.getS("rcPath", "").c_str());
    snprintf(S.url, sizeof S.url, "%s", p.getS("rcUrl", "").c_str());
    S.view.proj = p.getI("rcProj", 0) ? Projection::Equirect : Projection::Rectilinear;
    S.view.hfovDeg = p.getD("rcFov", 60); S.view.centerAzDeg = p.getD("rcCenter", 0);
    S.floorDb = (float)p.getD("rcFloor", 12); S.alpha = (float)p.getD("rcAlpha", 0.65);
    S.bands = (int)p.getI("rcBands", 16); S.lo = (float)p.getD("rcLo", 0); S.hi = (float)p.getD("rcHi", 0);
    S.markers = p.getB("rcMarkers", true); S.minStrength = (float)p.getD("rcMin", 0.15);
}
void save() {
    plat::Prefs& p = plat::prefs();
    p.setB("rcOn", S.on); p.setI("rcPic", S.pic); p.setS("rcPath", S.path); p.setS("rcUrl", S.url);
    p.setI("rcProj", S.view.proj == Projection::Equirect); p.setD("rcFov", S.view.hfovDeg); p.setD("rcCenter", S.view.centerAzDeg);
    p.setD("rcFloor", S.floorDb); p.setD("rcAlpha", S.alpha); p.setI("rcBands", S.bands); p.setD("rcLo", S.lo); p.setD("rcHi", S.hi);
    p.setB("rcMarkers", S.markers); p.setD("rcMin", S.minStrength);
}

// A deterministic stand-in panorama: sky, a skyline of blocks, ground. Equirectangular, 360 degrees, north in the middle.
void demoPicture() {
    const int W = 1080, H = 270;
    S.picW = W; S.picH = H;
    S.base.assign((size_t)W * H, 0);
    S.gen++;
    Rng r(11);
    std::vector<int> top(W, H * 6 / 10);
    for (int x = 0; x < W;) {
        const int bw = 20 + (int)(r.uni() * 60), bh = H * 6 / 10 - (int)(r.uni() * H * 0.28);
        for (int i = 0; i < bw && x + i < W; i++) top[x + i] = bh;
        x += bw;
    }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t c;
            if (y >= H * 6 / 10 + 8) { const int g = 60 + (y - H * 6 / 10) / 3; c = 0xFF000000u | ((uint32_t)(g * 7 / 10) << 16) | ((uint32_t)g << 8) | (uint32_t)(g * 7 / 10); }
            else if (y >= top[x]) { const int g = 70 + ((x / 7 + y / 9) & 1) * 14; c = 0xFF000000u | ((uint32_t)g << 16) | ((uint32_t)g << 8) | (uint32_t)g; }
            else { const int s = 90 + y * 120 / H; c = 0xFF000000u | ((uint32_t)(255 - y / 3) << 16) | ((uint32_t)(s + 60) << 8) | (uint32_t)(s - 40 < 0 ? 0 : s - 40); }
            S.base[(size_t)y * W + x] = c;
        }
}

bool loadFile(const std::string& path) {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    if (!plat::decodeImage(path, w, h, px) || w <= 0 || h <= 0) return false;
    while (w > 1280) {       // halve until it is cheap enough to blend at 8 Hz
        std::vector<uint32_t> q((size_t)(w / 2) * (h / 2));
        for (int y = 0; y < h / 2; y++) for (int x = 0; x < w / 2; x++) q[(size_t)y * (w / 2) + x] = px[(size_t)(2 * y) * w + 2 * x];
        px.swap(q); w /= 2; h /= 2;
    }
    S.base.swap(px); S.picW = w; S.picH = h; S.gen++;
    return true;
}

void updatePicture(double now) {
    const std::string key = std::to_string(S.pic) + "|" + (S.pic == PicFile ? S.path : S.pic == PicUrl ? S.url : "");
    const bool changed = key != S.picKey;
    if (S.pic == PicDemo) {
        if (changed) { demoPicture(); S.picKey = key; S.view.w = S.picW; S.view.h = S.picH; }
        return;
    }
    if (S.pic == PicFile) {
        if (!changed && now - S.picAt < 1.0) return;
        S.picAt = now;
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(S.path, ec);
        if (ec) { if (changed) { S.picKey = key; S.base.clear(); S.gen++; S.msg = tr("Cannot load the picture."); } return; }
        if (!changed && t == S.fileTime) return;
        S.picKey = key; S.fileTime = t;
        if (loadFile(S.path)) { S.view.w = S.picW; S.view.h = S.picH; S.msg.clear(); } else { S.base.clear(); S.gen++; S.msg = tr("Cannot load the picture."); }
        return;
    }
    // URL: one fetch at a time, about once a second
    const std::string file = plat::cacheDir() + "/rfcam_snapshot.jpg";
    const int st = S.fetch.load();
    if (st == 2 || st == 3) {
        if (S.th.joinable()) S.th.join();
        if (st == 2 && loadFile(file)) { S.view.w = S.picW; S.view.h = S.picH; S.msg.clear(); } else S.msg = tr("Cannot load the picture.");
        S.fetch = 0; S.picAt = now;
    }
    if (changed) { S.picKey = key; S.picAt = 0; }
    if (S.fetch.load() == 0 && S.url[0] && now - S.picAt > 1.0) {
        if (S.th.joinable()) S.th.join();
        S.fetch = 1;
        const std::string u = S.url;
        S.th = dect2::namedThread("onair-rfcam", [u, file] { S.fetch = plat::fetchUrl(u, file, "OnAir RF camera") ? 2 : 3; });
    }
}

}   // namespace

bool rfCamEnabled() { load(); return S.on; }

void rfCamFeed(const Frame& x, const Array& arr, double hz, double fs, const Options& df) {
    load();
    if (!S.on) return;
    CamOptions o;
    o.df = df; o.bands = S.bands;
    o.filterLoHz = S.lo * 1e3; o.filterHiHz = S.hi * 1e3;
    S.res = analyse(x, arr, hz, fs, o);
    S.haveRes = !S.res.spectrum.empty();
    S.gen++;
    S.resAt = ImGui::GetTime();
}

static void saveSnapshot() {
    char name[64];
    const time_t t = time(nullptr);
    strftime(name, sizeof name, "/rfcamera_%Y%m%d_%H%M%S", localtime(&t));
    const std::string base = plat::dataDir() + name;
    std::vector<uint8_t> rgba(S.comp.size() * 4);
    for (size_t i = 0; i < S.comp.size(); i++) { rgba[4 * i] = S.comp[i] & 0xFF; rgba[4 * i + 1] = (S.comp[i] >> 8) & 0xFF; rgba[4 * i + 2] = (S.comp[i] >> 16) & 0xFF; rgba[4 * i + 3] = 0xFF; }
    // burn the marker ticks into the picture (the labels go to the text file next to it)
    for (const Marker& m : S.shown) {
        const int x = (int)std::lround(m.x), y0 = (int)m.y;
        for (int dy = 0; dy < 12 && y0 + dy < S.picH; dy++)
            for (int dx = -(12 - dy) / 3; dx <= (12 - dy) / 3; dx++)
                if (x + dx >= 0 && x + dx < S.picW) { uint8_t* p = &rgba[((size_t)(y0 + dy) * S.picW + x + dx) * 4]; p[0] = 255; p[1] = 255; p[2] = 255; }
    }
    const bool ok = gfx::writePng((base + ".png").c_str(), rgba.data(), S.picW, S.picH);
    if (ok) {
        std::ofstream f(base + ".txt");
        f << "azimuth_deg,frequency_hz,strength\n";
        for (const CamPeak& p : S.res.peaks) { char b[96]; snprintf(b, sizeof b, "%.1f,%.0f,%.2f\n", p.bearingDeg, p.freqHz, p.strength); f << b; }
        { char b[640]; snprintf(b, sizeof b, tr("Snapshot saved: %s"), (base + ".png").c_str()); S.msg = b; }
    } else S.msg = tr("Cannot save the snapshot.");
}

void rfCamSection(App&) {
    load();
    bool ch = false;
    if (!ImGui::CollapsingHeader(trw("RF camera"))) return;
    ch |= ImGui::Checkbox(tr("Enable RF camera"), &S.on);
    ImGui::SameLine(); ImGui::TextDisabled("%s", tr("azimuth only; the picture is just a backdrop"));
    if (!S.on) { if (ch) save(); return; }
    const char* pics[] = {N_("Demo skyline (no camera)"), N_("Picture or panorama file"), N_("Camera snapshot URL (JPEG)")};
    ImGui::SetNextItemWidth(250 * gUi);
    if (ImGui::BeginCombo(tr("Picture"), tr(pics[S.pic]))) {
        for (int i = 0; i < NumPic; i++) if (ImGui::Selectable(tr(pics[i]), S.pic == i)) { S.pic = i; ch = true; if (i == PicDemo) { S.view.proj = Projection::Equirect; S.view.hfovDeg = 360; S.view.centerAzDeg = 0; } }
        ImGui::EndCombo();
    }
    if (S.pic == PicFile) { ImGui::SetNextItemWidth(380 * gUi); ch |= ImGui::InputText("##rcpath", S.path, sizeof S.path); }
    if (S.pic == PicUrl) { ImGui::SetNextItemWidth(380 * gUi); ch |= ImGui::InputText("##rcurl", S.url, sizeof S.url); }
    updatePicture(ImGui::GetTime());
    if (S.pic != PicDemo) {
        int pr = S.view.proj == Projection::Equirect;
        ImGui::SetNextItemWidth(200 * gUi);
        const char* lens[] = {N_("Normal camera lens"), N_("Panorama (equirectangular)")};
        if (ImGui::BeginCombo(tr("Projection"), tr(lens[pr]))) {
            for (int i = 0; i < 2; i++) if (ImGui::Selectable(tr(lens[i]), pr == i)) { S.view.proj = i ? Projection::Equirect : Projection::Rectilinear; ch = true; }
            ImGui::EndCombo();
        }
        float fov = (float)S.view.hfovDeg, cen = (float)S.view.centerAzDeg;
        ImGui::SetNextItemWidth(160 * gUi); if (ImGui::SliderFloat(tr("Field of view"), &fov, 10, 360, "%.1f\xC2\xB0")) { S.view.hfovDeg = std::min(fov, S.view.proj == Projection::Rectilinear ? 170.0f : 360.0f); ch = true; }
        ImGui::SetNextItemWidth(160 * gUi); if (ImGui::SliderFloat(tr("Centre azimuth"), &cen, 0, 359.9f, "%.1f\xC2\xB0")) { S.view.centerAzDeg = cen; ch = true; }
        if (ImGui::TreeNode(trw("Calibrate from two landmarks"))) {
            for (int i = 0; i < 2; i++) {
                ImGui::PushID(i);
                ImGui::RadioButton("##lm", &S.landmark, i); ImGui::SameLine();
                ImGui::SetNextItemWidth(70 * gUi); ImGui::InputFloat(tr("column"), &S.lmX[i], 0, 0, "%.0f"); ImGui::SameLine();
                ImGui::SetNextItemWidth(70 * gUi); ImGui::InputFloat(tr("azimuth"), &S.lmAz[i], 0, 0, "%.1f");
                ImGui::PopID();
            }
            if (ImGui::Button(tr("Calibrate"))) {
                View v = S.view;
                if (calibrateView(v, S.lmX[0], S.lmAz[0], S.lmX[1], S.lmAz[1])) { S.view = v; S.msg = tr("Calibrated."); ch = true; }
                else S.msg = tr("The two landmarks do not fit a picture.");
            }
            ImGui::SameLine(); ImGui::TextDisabled("%s", tr("Click the picture to set the column of the selected landmark."));
            ImGui::TreePop();
        }
    }
    ImGui::SetNextItemWidth(140 * gUi); ch |= ImGui::SliderFloat(tr("Heat strength"), &S.alpha, 0.1f, 0.9f, "%.2f");
    ImGui::SameLine(); ImGui::SetNextItemWidth(140 * gUi); ch |= ImGui::SliderFloat(tr("Range"), &S.floorDb, 3, 30, "%.0f dB");
    ImGui::SetNextItemWidth(70 * gUi); ch |= ImGui::InputFloat(tr("Band from (kHz)"), &S.lo, 0, 0, "%.1f");
    ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ch |= ImGui::InputFloat(tr("to (kHz)"), &S.hi, 0, 0, "%.1f");
    ImGui::SameLine(); ImGui::SetNextItemWidth(60 * gUi); if (ImGui::InputInt(tr("Sub-bands"), &S.bands, 0, 0)) { S.bands = std::clamp(S.bands, 1, 64); ch = true; }
    ch |= ImGui::Checkbox(tr("Peak markers"), &S.markers);
    if (S.markers) { ImGui::SameLine(); ImGui::SetNextItemWidth(120 * gUi); ch |= ImGui::SliderFloat(tr("Weakest shown"), &S.minStrength, 0.05f, 0.8f, "%.2f"); }
    ImGui::SameLine();
    const bool canShot = S.picW > 0 && !S.comp.empty();
    if (!canShot) ImGui::BeginDisabled();
    if (ImGui::Button(tr("Save snapshot"))) saveSnapshot();
    if (!canShot) ImGui::EndDisabled();
    if (!S.msg.empty()) ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "%s", S.msg.c_str());

    if (S.picW <= 0 || S.base.empty()) { ImGui::TextDisabled("%s", tr("No picture.")); if (ch) save(); return; }
    // compose and show
    S.view.w = S.picW; S.view.h = S.picH;
    // the blend and the upload only when something they depend on changed (they ran every frame: a full-picture copy, blend and upload at 60 fps)
    const State::CompKey key{S.gen, S.floorDb, S.alpha, S.view, S.haveRes};
    const State::CompKey& k0 = S.compKey;
    const bool same = k0.gen == key.gen && k0.floorDb == key.floorDb && k0.alpha == key.alpha && k0.haveRes == key.haveRes && k0.view.w == key.view.w &&
                      k0.view.h == key.view.h && k0.view.hfovDeg == key.view.hfovDeg && k0.view.centerAzDeg == key.view.centerAzDeg && k0.view.proj == key.view.proj;
    bool fresh = false;
    if (!S.tex || S.texW != S.picW || S.texH != S.picH) { delete S.tex; S.tex = gGfx ? gGfx->createImage(S.picW, S.picH, 0xFF000000u) : nullptr; S.texW = S.picW; S.texH = S.picH; fresh = true; }
    if (!S.tex) { if (ch) save(); return; }
    if (!same || fresh) {
        S.comp = S.base;
        if (S.haveRes) {
            HeatOptions ho; ho.floorDb = S.floorDb; ho.maxAlpha = S.alpha;
            blendHeat(S.comp, S.view, S.res.bearings, S.res.spectrum, ho);
        }
        S.tex->update(0, 0, S.picW, S.picH, S.comp.data());
        S.compKey = key;
    }
    const float availW = ImGui::GetContentRegionAvail().x;
    const float sc = availW / (float)S.picW;
    const ImVec2 sz(availW, (float)S.picH * sc);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Image(S.tex->texture(), sz);
    if (ImGui::IsItemClicked() && S.pic != PicDemo) {
        S.lmX[S.landmark] = std::round((ImGui::GetIO().MousePos.x - p0.x) / sc);
        S.landmark = 1 - S.landmark;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    S.shown.clear();
    if (S.markers && S.haveRes) {
        MarkerOptions mo; mo.minStrength = S.minStrength; mo.labelWidthPx = 92 * gUi / sc; mo.labelRows = 3;
        S.shown = layoutMarkers(S.res.peaks, S.view, mo);
        for (const Marker& m : S.shown) {
            const ImVec2 tip(p0.x + (float)m.x * sc, p0.y + (float)m.y * sc);
            dl->AddTriangleFilled(ImVec2(tip.x, tip.y), ImVec2(tip.x - 6 * gUi, tip.y + 12 * gUi), ImVec2(tip.x + 6 * gUi, tip.y + 12 * gUi), IM_COL32(255, 255, 255, 235));
            if (m.labelRow < 0) continue;
            char b[96]; snprintf(b, sizeof b, "%s  %.0f\xC2\xB0", m.label.c_str(), m.bearingDeg);
            const ImVec2 ts = ImGui::CalcTextSize(b);
            const ImVec2 tp(tip.x - ts.x * 0.5f, p0.y + 4 * gUi + (float)m.labelRow * (ts.y + 4 * gUi));
            dl->AddRectFilled(ImVec2(tp.x - 3, tp.y - 1), ImVec2(tp.x + ts.x + 3, tp.y + ts.y + 1), IM_COL32(0, 0, 0, 170), 3);
            dl->AddText(tp, IM_COL32(255, 255, 255, 255), b);
        }
    }
    if (ch) save();
}
