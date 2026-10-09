// HF digital, the SSTV view of the main tab (called by hfdig_ui.cpp): the picture being received (scaled to fit, aspect kept) with a
// progress bar and the mode and slant under it; beside it (under it in a narrow pane) the last finished pictures as thumbnails to click,
// Save (PNG) and Clear, and "Save every picture" (PNG files in <data folder>/sstv/, named by the time and the mode).
#include "app.h"
#include "gfx.h"
#include "platform.h"
#include "dect2/hfdig_tel.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

using dect2::HfdigSstvImage;

struct Item {
    std::shared_ptr<const HfdigSstvImage> im;
    gfx::Image* tex = nullptr;
};

struct State {
    bool loaded = false;
    bool autoSave = false;
    std::vector<Item> items;             // finished pictures, newest first
    int sel = -1;                        // -1: the live picture, else an index into items
    gfx::Image* live = nullptr;
    int liveW = 0, liveH = 0;
    uint64_t liveSeq = ~0ull;
    uint64_t lastId = 0;                 // newest finished picture taken into the list
    uint64_t hideId = 0;                 // the live picture the user cleared
    std::string msg;
};
State S;

void toRgba(const HfdigSstvImage& im, std::vector<uint32_t>& px) {
    px.resize((size_t)im.width * im.height);
    for (size_t i = 0; i < px.size() && i * 3 + 2 < im.rgb.size(); i++)
        px[i] = 0xFF000000u | ((uint32_t)im.rgb[i * 3 + 2] << 16) | ((uint32_t)im.rgb[i * 3 + 1] << 8) | im.rgb[i * 3];
}

gfx::Image* makeTexture(const HfdigSstvImage& im) {
    if (!gGfx || im.width <= 0 || im.height <= 0) return nullptr;
    std::vector<uint32_t> px;
    toRgba(im, px);
    gfx::Image* t = gGfx->createImage(im.width, im.height, 0xFF000000u);
    if (t) t->update(0, 0, im.width, im.height, px.data());
    return t;
}

std::string fileFor(const HfdigSstvImage& im) {
    char ts[32];
    const time_t tt = (time_t)im.unixTime;
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &lt);
    std::string m = im.mode;
    for (char& c : m) if (c == ' ') c = '-';
    return std::string(ts) + "-" + m + ".png";
}

void savePng(const HfdigSstvImage& im) {
    if (im.width <= 0 || im.rgb.size() < (size_t)im.width * im.height * 3) return;
    const std::string dir = plat::dataDir() + "/sstv";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(dir.c_str())), ec);
    const std::string path = dir + "/" + fileFor(im);
    std::vector<uint8_t> rgba((size_t)im.width * im.height * 4);
    for (size_t i = 0; i < (size_t)im.width * im.height; i++) {
        rgba[i * 4] = im.rgb[i * 3]; rgba[i * 4 + 1] = im.rgb[i * 3 + 1]; rgba[i * 4 + 2] = im.rgb[i * 3 + 2]; rgba[i * 4 + 3] = 255;
    }
    S.msg = gfx::writePng(path.c_str(), rgba.data(), im.width, im.height) ? "saved " + path : "could not write " + path;
}

void dropItem(Item& it) { delete it.tex; it.tex = nullptr; }

// takes the pictures that finished since the last frame into the list, updates the live texture
void update(const dect2::HfdigSstvTelemetry& t) {
    if (!S.loaded) { S.loaded = true; S.autoSave = plat::prefs().getB("hfdigSstvAutoSave", false); }
    for (size_t k = t.history.size(); k-- > 0;) {
        const auto& h = t.history[k];
        if (!h || h->id <= S.lastId) continue;
        S.lastId = h->id;
        Item it;
        it.im = h;
        it.tex = makeTexture(*h);
        S.items.insert(S.items.begin(), it);
        if (S.sel >= 0) S.sel++;
        if (S.autoSave) savePng(*h);
        while (S.items.size() > (size_t)dect2::kSstvHistory) { dropItem(S.items.back()); S.items.pop_back(); if (S.sel >= (int)S.items.size()) S.sel = -1; }
    }
    if (t.image && t.imageSeq != S.liveSeq && gGfx) {
        S.liveSeq = t.imageSeq;
        const HfdigSstvImage& im = *t.image;
        if (im.width > 0 && im.height > 0) {
            if (!S.live || S.liveW != im.width || S.liveH != im.height) {
                delete S.live;
                S.live = gGfx->createImage(im.width, im.height, 0xFF000000u);
                S.liveW = im.width; S.liveH = im.height;
            }
            std::vector<uint32_t> px;
            toRgba(im, px);
            if (S.live) S.live->update(0, 0, im.width, im.height, px.data());
        }
    }
}

void drawFit(ImDrawList* dl, gfx::Image* tex, int w, int h, ImVec2 p0, ImVec2 box) {
    const float z = std::min(box.x / (float)w, box.y / (float)h);
    const float iw = w * z, ih = h * z;
    const ImVec2 q0(p0.x + (box.x - iw) * 0.5f, p0.y + (box.y - ih) * 0.5f);
    dl->AddImage(tex->texture(), q0, ImVec2(q0.x + iw, q0.y + ih));
}

void picture(dect2::HfdigSstvTelemetry const& t, bool running, ImVec2 size) {
    const bool showLive = S.sel < 0;
    const HfdigSstvImage* im = showLive ? t.image.get() : (S.sel < (int)S.items.size() ? S.items[(size_t)S.sel].im.get() : nullptr);
    gfx::Image* tex = showLive ? S.live : (S.sel < (int)S.items.size() ? S.items[(size_t)S.sel].tex : nullptr);
    if (showLive && im && im->id == S.hideId) { im = nullptr; tex = nullptr; }
    const float textH = ImGui::GetFrameHeightWithSpacing() * 2;
    const ImVec2 box(size.x, std::max(40.f, size.y - textH));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + box.x, p0.y + box.y), IM_COL32(18, 19, 21, 255));
    ImGui::Dummy(box);
    if (im && tex) {
        drawFit(dl, tex, im->width, im->height, p0, box);
    } else {
        const std::string msg = ellipsize(!running ? "start the receiver" : "waiting for a picture (VIS header, or the line sync pulses)", box.x - 12 * gUi);
        const ImVec2 ts = ImGui::CalcTextSize(msg.c_str());
        dl->AddText(ImVec2(p0.x + std::max(4.f, (box.x - ts.x) * 0.5f), p0.y + box.y * 0.5f), IM_COL32(150, 154, 158, 255), msg.c_str());
    }
    // progress and the mode
    const int lines = showLive ? t.lines : (im ? im->lines : 0);
    const int total = im ? im->height : t.height;
    const float frac = total > 0 ? std::min(1.f, (float)lines / (float)total) : 0.f;
    char ov[48];
    snprintf(ov, sizeof ov, "%d / %d lines", lines, total);
    ImGui::ProgressBar(frac, ImVec2(box.x, 0), ov);
    ImGui::PushTextWrapPos(0);
    if (im) {
        const double slant = showLive ? t.slantPpm : im->slantPpm;
        if (showLive && t.visCode >= 0) ImGui::TextDisabled("%s, VIS %d, slant %+.0f ppm, tuning %+.0f Hz", im->mode.c_str(), t.visCode, slant, t.offsetHz);
        else if (showLive) ImGui::TextDisabled("%s (no VIS header: from the line period), slant %+.0f ppm", im->mode.c_str(), slant);
        else ImGui::TextDisabled("%s, %dx%d, slant %+.0f ppm%s", im->mode.c_str(), im->width, im->height, slant, im->complete ? "" : ", cut off");
    }
    ImGui::PopTextWrapPos();
}

void side(float width, bool wrap) {
    const dect2::HfdigSstvImage* cur = nullptr;
    if (S.sel >= 0 && S.sel < (int)S.items.size()) cur = S.items[(size_t)S.sel].im.get();
    const bool canSave = cur != nullptr;
    ImGui::BeginDisabled(!canSave);
    if (ImGui::Button("Save PNG") && cur) savePng(*cur);
    ImGui::EndDisabled();
    if (!canSave && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Click a picture below, then save it. Finished pictures are saved\nby themselves when \"Save every picture\" is on.");
    if (wrap) flowNext(8 * gUi); else ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        for (Item& it : S.items) dropItem(it);
        S.items.clear(); S.sel = -1;
        S.hideId = ~0ull;                               // set below from the telemetry shown
        S.msg.clear();
    }
    if (wrap) flowNext(12 * gUi);
    if (ImGui::Checkbox("Save every picture", &S.autoSave)) plat::prefs().setB("hfdigSstvAutoSave", S.autoSave);
    if (wrap) flowEnd();
    if (!S.msg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.msg.c_str()); ImGui::PopTextWrapPos(); }
    ImGui::Spacing();
    ImGui::TextDisabled("Last pictures");
    if (S.sel >= 0) { if (wrap) flowNext(10 * gUi); else ImGui::SameLine(); if (ImGui::SmallButton("Live")) S.sel = -1; }
    if (S.items.empty()) { ImGui::TextDisabled("none yet"); return; }
    const float th = 84 * gUi, gap = 6 * gUi;
    const int perRow = std::max(1, (int)((width + gap) / (th + gap)));
    int col = 0;
    for (size_t i = 0; i < S.items.size(); i++) {
        const Item& it = S.items[i];
        if (!it.tex || !it.im) continue;
        if (col > 0) { if (wrap) flowNext(gap); else if (col < perRow) ImGui::SameLine(0, gap); else col = 0; }
        const float z = std::min(th / (float)it.im->width, th / (float)it.im->height);
        const ImVec2 sz(it.im->width * z, it.im->height * z);
        ImGui::PushID((int)i);
        ImGui::BeginGroup();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##th", ImVec2(th, th))) S.sel = (int)i;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + th, p.y + th), IM_COL32(18, 19, 21, 255));
        dl->AddImage(it.tex->texture(), ImVec2(p.x + (th - sz.x) * 0.5f, p.y + (th - sz.y) * 0.5f), ImVec2(p.x + (th + sz.x) * 0.5f, p.y + (th + sz.y) * 0.5f));
        if (S.sel == (int)i) dl->AddRect(p, ImVec2(p.x + th, p.y + th), IM_COL32(80, 160, 255, 255), 0, 0, 2.f);
        else if (ImGui::IsItemHovered()) dl->AddRect(p, ImVec2(p.x + th, p.y + th), IM_COL32(150, 154, 158, 255));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s, %d lines", it.im->mode.c_str(), it.im->lines);
        ImGui::EndGroup();
        ImGui::PopID();
        col++;
    }
    if (wrap) flowEnd();
}

} // namespace

void hfdigSstvTab(App& a, const dect2::HfdigTelemetry& t) {
    const dect2::HfdigSstvTelemetry& s = t.sstv;
    update(s);
    const bool run = a.engine.running();
    if (S.hideId == ~0ull) S.hideId = s.image ? s.image->id : 0;      // Clear: the live picture stays hidden until the next one starts
    lamp("SSTV", !run ? 0 : (s.state == 1 ? 1 : 2));
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", !run ? "stopped" : s.state == 1 ? "receiving" : s.state == 2 ? "picture done, waiting for the next" : "waiting for a picture");
    flowEnd();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 60 * gUi || avail.y < 60 * gUi) return;
    const float sideW = 200 * gUi;
    const bool wide = avail.x > 560 * gUi;
    if (wide) {
        const float picW = avail.x - sideW - 10 * gUi;
        ImGui::BeginChild("##sstvpic", ImVec2(picW, avail.y), false, ImGuiWindowFlags_NoScrollbar);
        picture(s, run, ImVec2(picW, avail.y));
        ImGui::EndChild();
        ImGui::SameLine(0, 10 * gUi);
        ImGui::BeginChild("##sstvside", ImVec2(sideW, avail.y), false);
        side(sideW, false);
        ImGui::EndChild();
    } else {
        ImGui::BeginChild("##sstvpic", ImVec2(avail.x, std::max(160 * gUi, avail.y * 0.62f)), false, ImGuiWindowFlags_NoScrollbar);
        picture(s, run, ImGui::GetContentRegionAvail());
        ImGui::EndChild();
        ImGui::BeginChild("##sstvside", ImVec2(avail.x, 0), false);
        side(avail.x, true);
        ImGui::EndChild();
    }
}
