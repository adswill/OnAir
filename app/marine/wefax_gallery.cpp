// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "app.h"
#include "wefax_gallery.h"
#include "platform/platform.h"
#include "dect2/marine/wefax_archive.h"
#include "dect2/marine/wefax_rotate.h"
#include "dect2/marine/wefax_phase.h"
#include "dect2/marine/marine_wefax_stations.h"
#include "dect2/engine/updater.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

namespace fs = std::filesystem;
using namespace dect2;

namespace wefaxgal {
namespace {

fs::path P(const std::string& utf8) { return fs::path(reinterpret_cast<const char8_t*>(utf8.c_str())); }
std::string U(const fs::path& p) { const auto s = p.u8string(); return std::string(s.begin(), s.end()); }

struct Thumb { std::unique_ptr<gfx::Image> img; int w = 0, h = 0; bool failed = false; };

struct G {
    bool loaded = false;
    bool dirty = true;
    std::vector<WefaxArchiveEntry> entries;
    std::map<std::string, WefaxArchiveMeta> meta;
    std::map<std::string, Thumb> thumbs;
    int sort = 0;
    char query[64] = "";
    bool starredOnly = false;
    std::string sel;                          // base of the chart in the large view, "" = grid
    std::unique_ptr<gfx::Image> big; int bigW = 0, bigH = 0; std::string bigBase;
    float zoom = 0; ImVec2 pan{0, 0};         // zoom 0 = fit
    std::vector<std::string> trashed;         // bases moved to .trash by the last delete (the undo)
    std::string msg;
};
G g;

bool enabled() { return plat::prefs().getB("wefaxAutosave", true); }
long budgetMb() { return plat::prefs().getI("wefaxBudgetMb", 500); }
std::string folder() {
    const std::string d = plat::prefs().getS("wefaxDir", "");
    return d.empty() ? plat::dataDir() + "/wefax" : d;
}
void ensureFolder() { std::error_code ec; fs::create_directories(P(folder()), ec); }

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}
bool writeFile(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << s;
    return (bool)f;
}

void scan() {
    g.entries.clear(); g.meta.clear(); g.dirty = false;
    std::error_code ec;
    const fs::path dir = P(folder());
    if (!fs::is_directory(dir, ec)) return;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (!de.is_regular_file(ec) || de.path().extension() != ".json") continue;
        const std::string base = U(de.path().stem());
        if (base.rfind("wefax_", 0) != 0) continue;
        std::string js;
        if (!readFile(de.path(), js)) continue;
        WefaxArchiveMeta m;
        if (!wefaxMetaFromJson(js, m)) continue;
        const fs::path png = dir / P(base + ".png");
        if (!fs::exists(png, ec)) continue;
        WefaxArchiveEntry e;
        e.base = base; e.startUtc = m.startUtc; e.dialKhz = m.dialKhz; e.station = m.station; e.starred = m.starred;
        e.bytes = (long long)fs::file_size(png, ec) + (long long)js.size();
        g.entries.push_back(e);
        g.meta[base] = m;
    }
}

void writeMeta(const std::string& base) {
    auto it = g.meta.find(base);
    if (it != g.meta.end()) writeFile(P(folder()) / P(base + ".json"), wefaxMetaToJson(it->second));
}

void removeBase(const std::string& base) {
    std::error_code ec;
    const fs::path d = P(folder());
    fs::remove(d / P(base + ".png"), ec);
    fs::remove(d / P(base + ".json"), ec);
}

void cleanup() {
    scan();
    for (size_t i : wefaxCleanupPlan(g.entries, budgetMb() * 1024LL * 1024LL)) removeBase(g.entries[i].base);
    g.dirty = true;
}

bool writeGreyPng(const fs::path& path, const FaxImage& im) {
    std::vector<uint8_t> rgba((size_t)im.width * im.height * 4);
    for (size_t i = 0; i < (size_t)im.width * im.height && i < im.pix.size(); i++) {
        rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = im.pix[i]; rgba[i * 4 + 3] = 255;
    }
    return gfx::writePng(U(path).c_str(), rgba.data(), im.width, im.height);
}

void emptyTrash() {
    std::error_code ec;
    fs::remove_all(P(folder()) / ".trash", ec);
    g.trashed.clear();
}

bool moveBase(const std::string& base, const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    bool ok = true;
    for (const char* ext : {".png", ".json"}) {
        fs::rename(from / P(base + ext), to / P(base + ext), ec);
        if (ec) ok = false;
    }
    return ok;
}

void loadBig(const std::string& base) {
    g.big.reset(); g.bigW = g.bigH = 0; g.bigBase = base;
    int w = 0, h = 0; std::vector<uint32_t> px;
    if (!plat::decodeImage(U(P(folder()) / P(base + ".png")), w, h, px) || w <= 0 || h <= 0 || !gGfx) return;
    g.big.reset(gGfx->createImage(w, h, 0xFF000000u));
    g.big->update(0, 0, w, h, px.data());
    g.bigW = w; g.bigH = h;
}

void makeThumb(const std::string& base) {
    Thumb& t = g.thumbs[base];
    int w = 0, h = 0; std::vector<uint32_t> px;
    if (!gGfx || !plat::decodeImage(U(P(folder()) / P(base + ".png")), w, h, px) || w <= 0 || h <= 0) { t.failed = true; return; }
    std::vector<uint8_t> grey(px.size());
    for (size_t i = 0; i < px.size(); i++) grey[i] = (uint8_t)(px[i] & 0xFF);
    int tw, th; wefaxThumbSize(w, h, 200, tw, th);
    std::vector<uint8_t> out; wefaxThumbnail(grey.data(), w, h, tw, th, out);
    std::vector<uint32_t> rgba(out.size());
    for (size_t i = 0; i < out.size(); i++) rgba[i] = 0xFF000000u | (out[i] << 16) | (out[i] << 8) | out[i];
    t.img.reset(gGfx->createImage(tw, th, 0xFF000000u));
    t.img->update(0, 0, tw, th, rgba.data());
    t.w = tw; t.h = th;
}

// Turns a saved chart on disk by `turns` quarter turns clockwise: the PNG is rewritten, the side-car gets the new size, the thumbnail goes
void rotateSaved(const std::string& base, int turns) {
    int w = 0, h = 0; std::vector<uint32_t> px;
    const fs::path png = P(folder()) / P(base + ".png");
    if (!plat::decodeImage(U(png), w, h, px) || w <= 0 || h <= 0) { g.msg = "could not read the picture"; return; }
    FaxImage grey;
    grey.pix.resize(px.size());
    for (size_t i = 0; i < px.size(); i++) grey.pix[i] = (uint8_t)(px[i] & 0xFF);
    FaxImage o;
    wefaxRotate(grey.pix.data(), w, h, turns, o.pix, o.width, o.height);
    const fs::path tmp = P(folder()) / P(base + ".rot.png");
    if (!writeGreyPng(tmp, o)) { g.msg = "could not write the rotated picture"; return; }
    std::error_code ec;
    fs::rename(tmp, png, ec);
    if (ec) { fs::remove(tmp, ec); g.msg = "could not replace the picture"; return; }
    auto mi = g.meta.find(base);
    if (mi != g.meta.end()) { mi->second.width = o.width; mi->second.height = o.height; writeMeta(base); }
    g.thumbs.erase(base);
    g.bigBase.clear();                  // reloaded by the big view
    g.msg = "rotated";
    g.dirty = true;
}

// Phases a saved chart: every line moves `shift` pixels to the right (circular); `automatic` searches the black margin bar instead.
// The PNG is rewritten, so a wrong step is undone by the opposite shift.
void phaseSaved(const std::string& base, int shift, bool automatic) {
    int w = 0, h = 0; std::vector<uint32_t> px;
    const fs::path png = P(folder()) / P(base + ".png");
    if (!plat::decodeImage(U(png), w, h, px) || w <= 0 || h <= 0) { g.msg = "could not read the picture"; return; }
    std::vector<uint8_t> grey(px.size());
    for (size_t i = 0; i < px.size(); i++) grey[i] = (uint8_t)(px[i] & 0xFF);
    if (automatic) shift = wefaxPhaseAuto(grey.data(), w, h);
    if (wefaxPhaseWrap(shift, w) == 0) { g.msg = automatic ? "no black bar found" : ""; return; }
    FaxImage o; o.width = w; o.height = h;
    wefaxPhaseShift(grey.data(), w, h, shift, o.pix);
    const fs::path tmp = P(folder()) / P(base + ".rot.png");
    if (!writeGreyPng(tmp, o)) { g.msg = "could not write the phased picture"; return; }
    std::error_code ec;
    fs::rename(tmp, png, ec);
    if (ec) { fs::remove(tmp, ec); g.msg = "could not replace the picture"; return; }
    g.thumbs.erase(base);
    g.bigBase.clear();                  // reloaded by the big view
    g.msg = "phased";
    g.dirty = true;
}

std::string when(long long t) {
    char b[40]; const time_t tt = (time_t)t; struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &tt);
#else
    gmtime_r(&tt, &tmv);
#endif
    strftime(b, sizeof b, "%Y-%m-%d %H:%M UTC", &tmv);
    return b;
}

void bigView(App&) {
    const auto mi = g.meta.find(g.sel);
    if (mi == g.meta.end()) { g.sel.clear(); return; }
    WefaxArchiveMeta& m = mi->second;
    if (g.bigBase != g.sel) { loadBig(g.sel); g.zoom = 0; g.pan = ImVec2(0, 0); }
    if (ImGui::SmallButton("< Back")) { g.sel.clear(); g.big.reset(); g.bigBase.clear(); return; }
    ImGui::SameLine();
    if (ImGui::SmallButton(m.starred ? "Unstar" : "Star")) { m.starred = !m.starred; writeMeta(g.sel); g.dirty = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete")) {
        emptyTrash();
        if (moveBase(g.sel, P(folder()), P(folder()) / ".trash")) { g.trashed = {g.sel}; g.msg = "deleted, Undo brings it back"; }
        g.sel.clear(); g.big.reset(); g.bigBase.clear(); g.dirty = true;
        return;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Export PNG")) {
        const std::string out = plat::saveFileDialog((g.sel + ".png").c_str());
        if (!out.empty()) {
            std::error_code ec;
            fs::copy_file(P(folder()) / P(g.sel + ".png"), P(out), fs::copy_options::overwrite_existing, ec);
            g.msg = ec ? "could not write " + out : "saved " + out;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Rotate right")) { rotateSaved(g.sel, 1); return; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turn this saved chart 90 degrees clockwise. The PNG on disk is rewritten.");
    ImGui::SameLine();
    if (ImGui::SmallButton("left")) { rotateSaved(g.sel, 3); return; }
    ImGui::SameLine();
    if (ImGui::SmallButton("180")) { rotateSaved(g.sel, 2); return; }
    ImGui::SameLine();
    {   // hand phasing of the saved picture: the slider shows the step, it is applied when you let go (the PNG is rewritten)
        static int step = 0; static std::string stepFor;
        if (stepFor != g.sel) { step = 0; stepFor = g.sel; }
        const int pw = g.bigW > 1 ? g.bigW : 2;
        ImGui::TextDisabled("Phase");
        ImGui::SameLine();
        if (ImGui::SmallButton("-##gph-")) { phaseSaved(g.sel, -1, false); return; }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160 * gUi);
        ImGui::SliderInt("##gphase", &step, -(pw / 2), pw / 2, "%d px");
        if (ImGui::IsItemDeactivatedAfterEdit()) { const int s = step; step = 0; phaseSaved(g.sel, s, false); return; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Slide every line of this saved chart sideways (wrapping round), for a chart whose black margin bar is in the middle. Applied when you let go; the PNG on disk is rewritten, the opposite shift undoes it.");
        ImGui::SameLine();
        if (ImGui::SmallButton("+##gph+")) { phaseSaved(g.sel, 1, false); return; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Auto##gpha")) { phaseSaved(g.sel, 0, true); return; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Finds the darkest vertical bar and moves it to the left edge.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy path")) ImGui::SetClipboardText(U(P(folder()) / P(g.sel + ".png")).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("fit")) { g.zoom = 0; g.pan = ImVec2(0, 0); }
    ImGui::SameLine();
    if (ImGui::SmallButton("1:1")) g.zoom = 1;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s  %.1f kHz USB  %s  IOC %d  %d lpm  %d x %d  sync %s, SNR %.0f dB  ended: %s",
                        when(m.startUtc).c_str(), m.dialKhz, m.station.empty() ? "station unknown" : m.station.c_str(), m.ioc, m.lpm, m.width, m.height,
                        m.phasingLines > 0 ? "phased" : "no phasing", m.snrDb, m.endReason.c_str());
    ImGui::PopTextWrapPos();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 box = ImGui::GetContentRegionAvail();
    if (box.x < 20 || box.y < 20) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + box.x, p0.y + box.y), IM_COL32(18, 19, 21, 255));
    ImGui::InvisibleButton("##wfgbig", box);
    if (!g.big) { dl->AddText(ImVec2(p0.x + 8, p0.y + 8), IM_COL32(150, 154, 158, 255), "could not read the picture"); return; }
    const bool hov = ImGui::IsItemHovered();
    const float fit = std::min(box.x / g.bigW, box.y / g.bigH);
    float z = g.zoom > 0 ? g.zoom : fit;
    if (hov && ImGui::GetIO().MouseWheel != 0) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        const float nz = std::max(fit * 0.25f, std::min(8.f, z * std::pow(1.15f, ImGui::GetIO().MouseWheel)));
        if (g.zoom <= 0) { g.pan.x = (box.x - g.bigW * z) * 0.5f; g.pan.y = (box.y - g.bigH * z) * 0.5f; }
        g.pan.x = (mp.x - p0.x) - ((mp.x - p0.x) - g.pan.x) * nz / z;
        g.pan.y = (mp.y - p0.y) - ((mp.y - p0.y) - g.pan.y) * nz / z;
        g.zoom = nz; z = nz;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (g.zoom <= 0) { g.zoom = z; g.pan = ImVec2((box.x - g.bigW * z) * 0.5f, (box.y - g.bigH * z) * 0.5f); }
        g.pan.x += ImGui::GetIO().MouseDelta.x; g.pan.y += ImGui::GetIO().MouseDelta.y;
    }
    const float w = g.bigW * z, h = g.bigH * z;
    if (g.zoom <= 0) g.pan = ImVec2((box.x - w) * 0.5f, (box.y - h) * 0.5f);
    else {
        g.pan.x = w <= box.x ? (box.x - w) * 0.5f : std::min(0.f, std::max(g.pan.x, box.x - w));
        g.pan.y = h <= box.y ? (box.y - h) * 0.5f : std::min(0.f, std::max(g.pan.y, box.y - h));
    }
    dl->PushClipRect(p0, ImVec2(p0.x + box.x, p0.y + box.y), true);
    dl->AddImage(g.big->texture(), ImVec2(p0.x + g.pan.x, p0.y + g.pan.y), ImVec2(p0.x + g.pan.x + w, p0.y + g.pan.y + h));
    dl->PopClipRect();
}

} // namespace

std::string autosave(const Capture& c) {
    if (!enabled() || c.im.width <= 0 || c.im.height < 50 || c.im.pix.size() < (size_t)c.im.width * c.im.height) return "";
    ensureFolder();
    if (g.dirty || !g.loaded) { scan(); g.loaded = true; }
    std::vector<std::string> taken;
    for (const auto& e : g.entries) taken.push_back(e.base);
    const std::string base = wefaxArchiveUniqueBase(wefaxArchiveBase(c.startUtc, c.dialKhz), taken);
    WefaxArchiveMeta m;
    m.dialKhz = c.dialKhz; m.ioc = c.ioc; m.lpm = c.lpm; m.startUtc = c.startUtc; m.stopUtc = c.stopUtc;
    m.width = c.im.width; m.height = c.im.height; m.snrDb = c.snrDb; m.phasingLines = c.phasingLines; m.endReason = c.reason;
    if (const WefaxStation* s = wefaxStationForDial(c.dialKhz)) { m.station = s->name; m.stationId = s->id; }
    const fs::path d = P(folder());
    if (!writeGreyPng(d / P(base + ".png"), c.im)) return "autosave: could not write " + U(d / P(base + ".png"));
    if (!writeFile(d / P(base + ".json"), wefaxMetaToJson(m))) return "autosave: could not write the side-car";
    cleanup();
    g.loaded = true;
    return "autosaved " + base + ".png";
}

void settingsBlock() {
    bool on = enabled();
    if (ImGui::Checkbox("Save every chart automatically", &on)) plat::prefs().setB("wefaxAutosave", on);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("At the end of each chart (stop tone, signal gone, or a new start tone) a PNG and a small JSON file\n(frequency, IOC, lpm, UTC times, station, sync quality) go into the folder below.");
    ImGui::TextDisabled("Folder"); ImGui::SameLine();
    static char buf[512]; static bool init = false;
    if (!init) { snprintf(buf, sizeof buf, "%s", plat::prefs().getS("wefaxDir", "").c_str()); init = true; }
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::InputTextWithHint("##wfgdir", folder().c_str(), buf, sizeof buf)) { plat::prefs().setS("wefaxDir", buf); g.dirty = true; g.thumbs.clear(); }
    ImGui::SameLine();
    if (ImGui::SmallButton("Choose...")) {
        const std::string d = plat::openFolderDialog();
        if (!d.empty()) { snprintf(buf, sizeof buf, "%s", d.c_str()); plat::prefs().setS("wefaxDir", d); g.dirty = true; g.thumbs.clear(); }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Default")) { buf[0] = 0; plat::prefs().setS("wefaxDir", ""); g.dirty = true; g.thumbs.clear(); }
    ImGui::SameLine();
    if (ImGui::SmallButton("Open folder")) { ensureFolder(); dect2::openUrl(folder()); }
    ImGui::TextDisabled("Disk budget (MB, 0 = no limit)"); ImGui::SameLine();
    int mb = (int)budgetMb();
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::InputInt("##wfgmb", &mb, 0, 0)) plat::prefs().setI("wefaxBudgetMb", std::max(0, mb));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("When the saved charts exceed this, the oldest are deleted first. Starred charts are never deleted.");
}

void galleryView(App& a) {
    if (!g.loaded) { g.loaded = true; emptyTrash(); g.dirty = true; g.sort = (int)plat::prefs().getI("wefaxSort", 0); }
    if (g.dirty) scan();
    if (!g.sel.empty()) { bigView(a); return; }

    if (ImGui::CollapsingHeader("Autosave settings")) settingsBlock();
    static const char* const sortN[] = {"newest", "oldest", "station", "frequency"};
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::Combo("##wfgsort", &g.sort, sortN, 4)) plat::prefs().setI("wefaxSort", g.sort);
    ImGui::SameLine(); ImGui::SetNextItemWidth(160 * gUi);
    ImGui::InputTextWithHint("##wfgq", "search station, frequency, date", g.query, sizeof g.query);
    ImGui::SameLine(); ImGui::Checkbox("starred", &g.starredOnly);
    if (!g.trashed.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Undo delete")) {
            for (const auto& b : g.trashed) moveBase(b, P(folder()) / ".trash", P(folder()));
            g.trashed.clear(); g.dirty = true; g.msg = "restored";
        }
    }
    if (!g.msg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", g.msg.c_str()); }
    long long total = 0; for (const auto& e : g.entries) total += e.bytes;
    ImGui::TextDisabled("%zu charts, %.1f MB in %s", g.entries.size(), total / 1048576.0, folder().c_str());

    const std::vector<size_t> order = wefaxGalleryList(g.entries, (WefaxSort)g.sort, g.query, g.starredOnly);
    if (g.entries.empty()) { ImGui::TextDisabled("No charts yet. Each chart that ends is saved here when autosave is on."); return; }
    ImGui::BeginChild("##wfggrid", ImVec2(0, 0), false);
    const float cell = 190 * gUi;
    const int cols = std::max(1, (int)(ImGui::GetContentRegionAvail().x / cell));
    int built = 0, col = 0;
    for (size_t oi : order) {
        const WefaxArchiveEntry& e = g.entries[oi];
        ImGui::PushID(e.base.c_str());
        ImGui::BeginGroup();
        auto it = g.thumbs.find(e.base);
        if (it == g.thumbs.end() && built < 2) { makeThumb(e.base); built++; it = g.thumbs.find(e.base); }
        const ImVec2 box(cell - 10 * gUi, 130 * gUi);
        bool clicked = false;
        if (it != g.thumbs.end() && it->second.img) {
            const Thumb& t = it->second;
            const float s = std::min(box.x / t.w, box.y / t.h);
            clicked = ImGui::ImageButton("##t", t.img->texture(), ImVec2(t.w * s, t.h * s));
        } else clicked = ImGui::Button(it != g.thumbs.end() && it->second.failed ? "unreadable" : "loading", box);
        if (clicked) { g.sel = e.base; g.msg.clear(); }
        ImGui::TextUnformatted(((e.starred ? "* " : "") + when(e.startUtc)).c_str());
        ImGui::TextDisabled("%.1f kHz %s", e.dialKhz, e.station.empty() ? "" : e.station.c_str());
        ImGui::EndGroup();
        ImGui::PopID();
        if (++col < cols) ImGui::SameLine(); else col = 0;
    }
    ImGui::EndChild();
}

} // namespace wefaxgal
