// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF digital, the HamDRM view of the main tab (called by hfdig_ui.cpp): the file being received with its name, callsign and a progress bar
// of the segments (and the ones still missing), the last finished picture (scaled to fit), the finished files as a list to click, Save, and
// "Save every picture" (the files as sent, in <data folder>/hamdrm/, named by the time and the sender's file name).
#include "app.h"
#include "panels/gallery.h"
#include "gfx/gfx.h"
#include "platform/platform.h"
#include "dect2/hfdig/hfdig_tel.h"
#include "dect2/common/png.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using dect2::HamdrmFile;

struct Item {
    std::shared_ptr<const HamdrmFile> f;
    gfx::Image* tex = nullptr;
    int w = 0, h = 0;
};

struct State {
    bool loaded = false;
    bool autoSave = true;
    std::vector<Item> items;             // finished files, newest first
    int sel = 0;
    uint64_t lastId = 0;
    std::string msg;
};
State S;

std::string safeName(const HamdrmFile& f) {
    std::string n = f.name.empty() ? std::string("hamdrm") + (f.mime == "image/png" ? ".png" : ".jpg") : f.name;
    for (char& c : n) if (c == '/' || c == '\\' || c == ':' || c == ' ') c = '_';
    char ts[32];
    const time_t tt = (time_t)f.unixTime;
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &lt);
    return std::string(ts) + "-" + n;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& d) {
    std::ofstream o(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
    if (!o) return false;
    o.write((const char*)d.data(), (std::streamsize)d.size());
    return (bool)o;
}

void save(const HamdrmFile& f) {
    const std::string dir = plat::dataDir() + "/hamdrm";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(dir.c_str())), ec);
    const std::string path = dir + "/" + safeName(f);
    S.msg = writeFile(path, f.data) ? "saved " + path : "could not write " + path;
}

// PNG with OnAir's own decoder, JPEG (and the rest) through the system's picture decoder
void makeTexture(Item& it) {
    if (!gGfx || !it.f || it.f->data.empty()) return;
    const auto& b = it.f->data;
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    std::vector<uint8_t> rgba;
    if (b.size() > 8 && b[0] == 0x89 && b[1] == 'P' && dect2::decodePng(b.data(), b.size(), w, h, rgba) && w > 0 && h > 0 && w <= 4096 && h <= 4096) {
        px.resize((size_t)w * h);
        memcpy(px.data(), rgba.data(), px.size() * 4);
    } else {
        char name[64];
        snprintf(name, sizeof name, "/hamdrm_%llu.img", (unsigned long long)it.f->id);
        const std::string path = plat::cacheDir() + name;
        if (!writeFile(path, b) || !plat::decodeImage(path, w, h, px) || w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    }
    it.tex = gGfx->createImage(w, h, 0xFF000000u);
    if (it.tex) it.tex->update(0, 0, w, h, px.data());
    it.w = w; it.h = h;
}

void update(const dect2::HfdigHamdrmTelemetry& t) {
    if (!S.loaded) { S.loaded = true; S.autoSave = plat::prefs().getB("hfdigHamdrmAutoSave", true); }
    for (size_t k = t.history.size(); k-- > 0;) {
        const auto& h = t.history[k];
        if (!h || h->id <= S.lastId) continue;
        S.lastId = h->id;
        Item it;
        it.f = h;
        makeTexture(it);
        S.items.insert(S.items.begin(), it);
        S.sel = 0;
        if (S.autoSave) save(*h);
        if (h->mime.rfind("image/", 0) == 0) {
            gallery::Meta gm; gm.detail = h->callsign.empty() ? h->name : h->callsign + " " + h->name;
            gallery::publishFile("hamdrm-" + std::to_string((long long)h->unixTime) + "-" + std::to_string(h->id), "HamDRM", (double)h->unixTime, h->data, gm);
        }
        while (S.items.size() > (size_t)dect2::kHamdrmHistory) { delete S.items.back().tex; S.items.pop_back(); }
    }
}

void progress(const dect2::HfdigHamdrmTelemetry& t, float width) {
    ImGui::PushTextWrapPos(0);
    if (!t.haveFile) {
        ImGui::TextDisabled("no file yet");
        ImGui::ProgressBar(0, ImVec2(width, 0), "");
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::Text("%s", t.name.empty() ? "(file name not received yet)" : t.name.c_str());
    if (!t.callsign.empty()) { ImGui::SameLine(0, 8 * gUi); ImGui::TextColored(pal::heading(), "%s", t.callsign.c_str()); }
    const float frac = t.segTotal > 0 ? (float)t.segHave / (float)t.segTotal : 0.f;
    char ov[64];
    if (t.segTotal > 0) snprintf(ov, sizeof ov, "%d / %d segments%s", t.segHave, t.segTotal, t.fileDone ? ", complete" : "");
    else snprintf(ov, sizeof ov, "%d segments", t.segHave);
    ImGui::ProgressBar(t.fileDone ? 1.f : frac, ImVec2(width, 0), ov);
    std::string info;
    char b[96];
    if (t.bodySize > 0) { snprintf(b, sizeof b, "%lld bytes", (long long)t.bodySize); info = b; }
    snprintf(b, sizeof b, "%s%d transmission%s heard", info.empty() ? "" : ", ", t.transmissions, t.transmissions == 1 ? "" : "s");
    info += b;
    ImGui::TextDisabled("%s", info.c_str());
    if (!t.fileDone && !t.missing.empty() && t.segTotal > 0) {
        std::string m;
        for (size_t i = 0; i < t.missing.size() && i < 20; i++) m += (i ? ", " : "") + std::to_string(t.missing[i]);
        if ((int)t.missing.size() < t.segTotal - t.segHave || t.missing.size() > 20) m += ", ...";
        ImGui::TextDisabled("missing: %s%s", m.c_str(), t.state == 2 ? " (waiting for a repeat)" : "");
    }
    ImGui::PopTextWrapPos();
}

void pictureBox(ImVec2 size, bool running) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(18, 19, 21, 255));
    ImGui::Dummy(size);
    const Item* it = S.sel >= 0 && S.sel < (int)S.items.size() ? &S.items[(size_t)S.sel] : nullptr;
    if (it && it->tex && it->w > 0) {
        const float z = std::min(size.x / (float)it->w, size.y / (float)it->h);
        const float iw = it->w * z, ih = it->h * z;
        const ImVec2 q0(p0.x + (size.x - iw) * 0.5f, p0.y + (size.y - ih) * 0.5f);
        dl->AddImage(it->tex->texture(), q0, ImVec2(q0.x + iw, q0.y + ih));
        return;
    }
    const std::string msg = ellipsize(!running ? "start the receiver" : it ? "received (not a picture this program can show)" : "waiting for a picture", size.x - 12 * gUi);
    const ImVec2 ts = ImGui::CalcTextSize(msg.c_str());
    dl->AddText(ImVec2(p0.x + std::max(4.f, (size.x - ts.x) * 0.5f), p0.y + size.y * 0.5f), IM_COL32(150, 154, 158, 255), msg.c_str());
}

void side(App& a, bool wrap) {
    const Item* cur = S.sel >= 0 && S.sel < (int)S.items.size() ? &S.items[(size_t)S.sel] : nullptr;
    ImGui::BeginDisabled(!cur);
    if (ImGui::Button("Save") && cur) save(*cur->f);
    ImGui::EndDisabled();
    if (wrap) flowNext(8 * gUi); else ImGui::SameLine();
    if (ImGui::Button("Clear")) { for (Item& it : S.items) delete it.tex; S.items.clear(); S.sel = 0; S.msg.clear(); a.engine.hfdig().clearResults(16); }
    if (wrap) flowNext(12 * gUi);
    if (ImGui::Checkbox("Save every picture", &S.autoSave)) plat::prefs().setB("hfdigHamdrmAutoSave", S.autoSave);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The files as they were sent, in %s/hamdrm", plat::dataDir().c_str());
    if (wrap) flowEnd();
    if (!S.msg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.msg.c_str()); ImGui::PopTextWrapPos(); }
    ImGui::Spacing();
    ImGui::TextDisabled("Received");
    if (S.items.empty()) { ImGui::TextDisabled("none yet"); return; }
    for (size_t i = 0; i < S.items.size(); i++) {
        const HamdrmFile& f = *S.items[i].f;
        char tm[16];
        const time_t tt = (time_t)f.unixTime;
        struct tm lt;
#ifdef _WIN32
        localtime_s(&lt, &tt);
#else
        localtime_r(&tt, &lt);
#endif
        strftime(tm, sizeof tm, "%H:%M:%S", &lt);
        char lab[160];
        snprintf(lab, sizeof lab, "%s  %s%s%s##hd%zu", tm, f.name.empty() ? "file" : f.name.c_str(), f.callsign.empty() ? "" : "  ", f.callsign.c_str(), i);
        if (ImGui::Selectable(lab, S.sel == (int)i)) S.sel = (int)i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%zu bytes, %s, after %d transmission%s", f.data.size(), f.mime.c_str(), f.transmissions, f.transmissions == 1 ? "" : "s");
    }
}

} // namespace

void hfdigHamdrmTab(App& a, const dect2::HfdigTelemetry& t) {
    const dect2::HfdigHamdrmTelemetry& h = t.hamdrm;
    update(h);
    const bool run = a.engine.running();
    lamp("HamDRM", !run ? 0 : (h.state == 1 ? 1 : 2));
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    char st[160];
    if (!run) snprintf(st, sizeof st, "stopped");
    else if (h.state == 1 && h.mode >= 0 && h.qam >= 0)
        snprintf(st, sizeof st, "receiving: mode %s, %.1f kHz, %d-QAM, SNR %.0f dB, tuning %+.0f Hz", dect2::hamdrmModeName(h.mode), dect2::hamdrmOccupancyKhz(h.occ),
                 dect2::hamdrmQam(h.qam), h.snrDb, h.offsetHz);
    else if (h.state == 1) snprintf(st, sizeof st, "locked on mode %s", dect2::hamdrmModeName(h.mode));
    else if (h.state == 2) snprintf(st, sizeof st, "transmission ended, waiting for the next");
    else snprintf(st, sizeof st, "searching (modes A, B, E around 1600 Hz)");
    ImGui::TextDisabled("%s", st);
    if (run && h.framesOk + h.framesBad > 0) { flowNext(10 * gUi); ImGui::TextDisabled("frames %llu ok, %llu bad", (unsigned long long)h.framesOk, (unsigned long long)h.framesBad); }
    flowEnd();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 60 * gUi || avail.y < 60 * gUi) return;
    const float sideW = 220 * gUi;
    const bool wide = avail.x > 560 * gUi;
    const float mainW = wide ? avail.x - sideW - 10 * gUi : avail.x;
    ImGui::BeginChild("##hdmain", ImVec2(mainW, wide ? avail.y : std::max(200 * gUi, avail.y * 0.65f)), false, ImGuiWindowFlags_NoScrollbar);
    progress(h, mainW);
    ImGui::Spacing();
    const ImVec2 rest = ImGui::GetContentRegionAvail();
    pictureBox(ImVec2(rest.x, std::max(40.f, rest.y)), run);
    ImGui::EndChild();
    if (wide) ImGui::SameLine(0, 10 * gUi);
    ImGui::BeginChild("##hdside", ImVec2(wide ? sideW : avail.x, wide ? avail.y : 0), false);
    side(a, !wide);
    ImGui::EndChild();
}
