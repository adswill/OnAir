#include "adsb_map.h"
#include "app.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace adsbmap {
namespace {

namespace fs = std::filesystem;

struct Tile {
    int state = 0;   // 0 wanted, 1 downloading, 2 file ready (to be decoded), 3 on the screen, 4 failed
    gfx::Image* img = nullptr;
    double failedAt = 0;
    int lastUsed = 0;   // the frame it was last drawn in
};

std::mutex mu;
std::condition_variable cv;
std::map<uint64_t, Tile> tiles;
std::deque<uint64_t> queue;
std::thread worker;
std::atomic<bool> stopping{false};
bool started = false, inited = false;
// Two folders of tiles: those kept until the user deletes them, and this session's, deleted when OnAir closes. New tiles go to the first
// while keepNew is set. Both are read.
std::string kept, temp;
std::atomic<bool> keepNew{true};
bool onlineMap = true;   // the "online map" switch, shared by every map

uint64_t key(int z, int x, int y) { return ((uint64_t)z << 56) | ((uint64_t)x << 28) | (uint64_t)y; }

fs::path fsPath(const std::string& p) { return fs::path(reinterpret_cast<const char8_t*>(p.c_str())); }   // UTF-8 (Windows user names)
bool exists(const std::string& p) { std::error_code ec; const auto n = fs::file_size(fsPath(p), ec); return !ec && n > 100; }

std::string pathIn(const std::string& root, int z, int x, int y) { char b[64]; snprintf(b, sizeof b, "/%d/%d/%d.png", z, x, y); return root + b; }
std::string found(int z, int x, int y) {   // the file of a tile already downloaded, or ""
    std::string p = pathIn(kept, z, x, y);
    if (exists(p)) return p;
    p = pathIn(temp, z, x, y);
    return exists(p) ? p : std::string();
}

// move the tiles under one folder into another (a tile already there wins), then remove the first
void moveTree(const std::string& from, const std::string& to) {
    std::error_code ec;
    const fs::path f = fsPath(from), t = fsPath(to);
    if (!fs::exists(f, ec)) return;
    if (!fs::exists(t, ec)) {   // the usual case: a single rename
        fs::create_directories(t.parent_path(), ec);
        fs::rename(f, t, ec);
        if (!ec) return;
    }
    std::vector<fs::path> files;
    ec.clear();
    for (auto it = fs::recursive_directory_iterator(f, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->path().extension() == ".png") files.push_back(it->path());
    for (const auto& p : files) {
        const fs::path d = t / p.lexically_relative(f);
        std::error_code e;
        if (fs::exists(d, e)) continue;
        fs::create_directories(d.parent_path(), e);
        fs::rename(p, d, e);
        if (e) { e.clear(); fs::copy_file(p, d, e); }   // on another disk
    }
    fs::remove_all(f, ec);
}

// the folders, once: this session's starts empty (a crash leaves one behind), the tiles of versions that kept them in the cache are moved over
void init() {
    if (inited) return;
    inited = true;
    const std::string cache = plat::cacheDir();
    kept = plat::dataDir() + "/maps/osm";
    temp = cache + "/osm-temp";
    keepNew = plat::prefs().getB("mapKeepTiles", true);
    onlineMap = plat::prefs().getB("adsbMap", true);   // the key from when only the ADS-B screen had a map
    std::error_code ec;
    fs::remove_all(fsPath(temp), ec);
    moveTree(cache + "/osm", kept);
}

void setKeep(bool k) {
    keepNew = k;
    plat::prefs().setB("mapKeepTiles", k);
    plat::prefs().flush();
}

void run() {
    for (;;) {
        uint64_t k;
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [] { return stopping || !queue.empty(); });
            if (stopping) return;
            k = queue.front(); queue.pop_front();
            tiles[k].state = 1;
        }
        const int z = (int)(k >> 56), x = (int)((k >> 28) & 0xFFFFFFF), y = (int)(k & 0xFFFFFFF);
        bool ok = !found(z, x, y).empty();
        if (!ok) {
            // no shell commands here: the same code has to work on Windows
            const std::string path = pathIn(keepNew ? kept : temp, z, x, y);
            std::error_code ec;
            fs::create_directories(fsPath(path).parent_path(), ec);
            char url[128];
            snprintf(url, sizeof url, "https://tile.openstreetmap.org/%d/%d/%d.png", z, x, y);
            const std::string part = path + ".part";
            if (!ec && plat::fetchUrl(url, part, std::string("OnAir/") + ONAIR_VERSION + " (ADS-B map)")) {
                fs::rename(fsPath(part), fsPath(path), ec);
                ok = !ec && exists(path);
            }
            if (!ok) fs::remove(fsPath(part), ec);
        }
        std::lock_guard<std::mutex> lk(mu);
        tiles[k].state = ok ? 2 : 4;
        tiles[k].failedAt = glfwGetTime();
    }
}

void start() {
    if (started) return;
    started = true;
    worker = std::thread(run);
}

// a long session at street level would fill the graphics memory: past this many, the pictures not on the screen go, the oldest first
// (their files stay, so they come back without a download)
constexpr size_t kMaxImages = 512;
size_t nImages = 0;

void evict(int frame) {   // with mu held
    if (nImages <= kMaxImages) return;
    std::vector<std::pair<int, uint64_t>> old;
    for (const auto& [k, t] : tiles) if (t.state == 3 && t.lastUsed != frame) old.push_back({t.lastUsed, k});
    std::sort(old.begin(), old.end());
    for (size_t i = 0; i < old.size() && nImages > kMaxImages * 3 / 4; i++) {
        auto it = tiles.find(old[i].second);
        delete it->second.img;
        tiles.erase(it);
        nImages--;
    }
}

// Tiles queued for a view the user has since zoomed or panned away from are not fetched: at zoom 19 a quick turn of the wheel would
// otherwise queue hundreds, all downloaded from the tile server for nothing. A queued tile drawn neither in this frame nor the one before
// (two maps may share a frame) leaves the queue, and is queued again if it comes back on the screen.
void dropStale(int frame) {   // with mu held
    std::deque<uint64_t> keep;
    for (uint64_t k : queue) {
        auto it = tiles.find(k);
        if (it != tiles.end() && it->second.state == 0 && it->second.lastUsed < frame - 1) tiles.erase(it);
        else keep.push_back(k);
    }
    queue.swap(keep);
}

// the picture of a tile if it is ready (decoding at most a few per frame); queue it otherwise
int decodedThisFrame = 0;
ImTextureID tileTexture(int z, int x, int y, bool online, bool& failed) {
    const uint64_t k = key(z, x, y);
    failed = false;
    std::lock_guard<std::mutex> lk(mu);
    auto it = tiles.find(k);
    if (it == tiles.end()) {
        if (!online) {   // only what is on disk, without the downloader
            if (found(z, x, y).empty()) { failed = true; return 0; }
            tiles[k].state = 2;
            return 0;
        }
        tiles[k] = Tile();
        tiles[k].lastUsed = ImGui::GetFrameCount();
        queue.push_back(k);
        cv.notify_one();
        return 0;
    }
    Tile& t = it->second;
    t.lastUsed = ImGui::GetFrameCount();
    if (t.state == 2 && decodedThisFrame < 6) {
        decodedThisFrame++;
        int w = 0, h = 0;
        std::vector<uint32_t> px;
        const std::string path = found(z, x, y);
        if (!path.empty() && plat::decodeImage(path, w, h, px) && w == 256 && h == 256 && gGfx) {
            t.img = gGfx->createImage(256, 256, 0xFF000000u);
            t.img->update(0, 0, 256, 256, px.data());
            t.state = 3;
            nImages++;
        } else t.state = 4;
    }
    if (t.state == 4) {
        failed = true;
        if (online && glfwGetTime() - t.failedAt > 30) { t.state = 0; t.failedAt = 1e18; queue.push_back(k); cv.notify_one(); }   // try again later
        return 0;
    }
    return t.state == 3 && t.img ? t.img->texture() : (ImTextureID)0;
}

// the picture of a tile only if it is already decoded (nothing is queued)
ImTextureID shownTexture(int z, int x, int y) {
    std::lock_guard<std::mutex> lk(mu);
    auto it = tiles.find(key(z, x, y));
    if (it == tiles.end() || it->second.state != 3 || !it->second.img) return 0;
    it->second.lastUsed = ImGui::GetFrameCount();
    return it->second.img->texture();
}

// the transform of the map as last drawn
ImVec2 gOrigin, gSize;
double gCx = 0, gCy = 0;   // the centre in world pixels
int gZoom = 7;
bool gClicked = false, gAny = false;
ImVec2 gClickPos;

double worldSize(int z) { return 256.0 * (double)(1 << z); }
double mercX(double lon, int z) { return (lon + 180.0) / 360.0 * worldSize(z); }
double mercY(double lat, int z) {
    const double s = std::sin(std::max(-85.05, std::min(85.05, lat)) * M_PI / 180.0);
    return (0.5 - std::log((1 + s) / (1 - s)) / (4 * M_PI)) * worldSize(z);
}
double lonOf(double x, int z) { return x / worldSize(z) * 360.0 - 180.0; }
double latOf(double y, int z) { const double n = M_PI - 2 * M_PI * y / worldSize(z); return 180.0 / M_PI * std::atan(0.5 * (std::exp(n) - std::exp(-n))); }

// ---------------------------------------------------------------- the tiles menu

struct Usage {
    long n[kMaxZoom + 1] = {};
    uint64_t bytes[kMaxZoom + 1] = {};
    long files() const { long s = 0; for (long v : n) s += v; return s; }
    uint64_t size() const { uint64_t s = 0; for (uint64_t v : bytes) s += v; return s; }
};

Usage scan(const std::string& root) {   // the tiles under a folder (root/zoom/x/y.png) by zoom level
    Usage u;
    std::error_code ec;
    const fs::path r = fsPath(root);
    for (auto it = fs::recursive_directory_iterator(r, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->path().extension() != ".png") continue;
        std::error_code e;
        const auto sz = it->file_size(e);
        if (e) continue;
        const int z = atoi(it->path().lexically_relative(r).begin()->string().c_str());
        if (z < 0 || z > kMaxZoom) continue;
        u.n[z]++; u.bytes[z] += sz;
    }
    return u;
}

std::string sizeText(uint64_t b) {
    char s[32];
    if (b < (1u << 20)) snprintf(s, sizeof s, "%.0f KB", b / 1024.0);
    else if (b < (1ull << 30)) snprintf(s, sizeof s, "%.1f MB", b / 1048576.0);
    else snprintf(s, sizeof s, "%.2f GB", b / 1073741824.0);
    return s;
}

const char* zoomName(int z) { return z <= 4 ? "continents" : z <= 7 ? "countries" : z <= 10 ? "regions" : z <= 13 ? "cities" : z <= 16 ? "streets" : "buildings"; }

void removeZoom(const std::string& root, int z) { std::error_code ec; fs::remove_all(fsPath(root + "/" + std::to_string(z)), ec); }

} // namespace

ImVec2 project(double lat, double lon) {
    double dx = mercX(lon, gZoom) - gCx;
    const double ws = worldSize(gZoom);
    if (dx > ws / 2) dx -= ws; else if (dx < -ws / 2) dx += ws;   // the short way round the globe
    return ImVec2(gOrigin.x + gSize.x * 0.5f + (float)dx, gOrigin.y + gSize.y * 0.5f + (float)(mercY(lat, gZoom) - gCy));
}

bool clickedAt(ImVec2& p) { if (gClicked) p = gClickPos; return gClicked; }
bool tilesAvailable() { return gAny; }

const char* const kCredit = "map: OpenStreetMap contributors";

bool draw(View& v, ImVec2 size) {
    init();
    if (onlineMap) start();
    decodedThisFrame = 0;
    v.zoom = std::max(kMinZoom, std::min(kMaxZoom, v.zoom));
    v.lat = std::max(-84.0, std::min(84.0, v.lat));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemAllowOverlap();   // the screens put buttons on the map afterwards: they must get the clicks, not the map under them
    ImGui::InvisibleButton("##map", size, ImGuiButtonFlags_MouseButtonLeft);
    const bool hov = ImGui::IsItemHovered();
    gClicked = false;
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 3.f)) {   // pan
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        v.lon = lonOf(mercX(v.lon, v.zoom) - d.x, v.zoom);
        v.lat = latOf(mercY(v.lat, v.zoom) - d.y, v.zoom);
    } else if (ImGui::IsItemDeactivated() && !ImGui::IsMouseDragging(0, 3.f) && hov) { gClicked = true; gClickPos = ImGui::GetIO().MousePos; }
    if (hov && ImGui::GetIO().MouseWheel != 0) {   // zoom around the pointer
        const ImVec2 m = ImGui::GetIO().MousePos;
        const double mx = mercX(v.lon, v.zoom) + (m.x - (p0.x + size.x * 0.5f)), my = mercY(v.lat, v.zoom) + (m.y - (p0.y + size.y * 0.5f));
        const double pl = lonOf(mx, v.zoom), pa = latOf(my, v.zoom);
        v.zoom = std::max(kMinZoom, std::min(kMaxZoom, v.zoom + (ImGui::GetIO().MouseWheel > 0 ? 1 : -1)));
        // keep the point under the pointer: the centre sits the pointer's offset from the centre away from it
        v.lon = lonOf(mercX(pl, v.zoom) - (m.x - (p0.x + size.x * 0.5f)), v.zoom);
        v.lat = latOf(mercY(pa, v.zoom) - (m.y - (p0.y + size.y * 0.5f)), v.zoom);
        ImGui::GetIO().MouseWheel = 0;
    }
    gOrigin = p0; gSize = size; gZoom = v.zoom;
    gCx = mercX(v.lon, v.zoom); gCy = mercY(v.lat, v.zoom);
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(14, 16, 18, 255));
    // graticule every 10 degrees (what remains when there are no tiles)
    {
        for (int lo = -180; lo <= 180; lo += (v.zoom < 4 ? 30 : 10)) { const float x = p0.x + size.x * 0.5f + (float)(mercX(lo, v.zoom) - gCx); if (x > p0.x && x < p0.x + size.x) dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + size.y), IM_COL32(30, 34, 38, 255)); }
        for (int la = -80; la <= 80; la += (v.zoom < 4 ? 30 : 10)) { const float y = p0.y + size.y * 0.5f + (float)(mercY(la, v.zoom) - gCy); if (y > p0.y && y < p0.y + size.y) dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x + size.x, y), IM_COL32(30, 34, 38, 255)); }
    }
    gAny = false;
    const int n = 1 << v.zoom;
    const int tx0 = (int)std::floor((gCx - size.x * 0.5) / 256.0), tx1 = (int)std::floor((gCx + size.x * 0.5) / 256.0);
    const int ty0 = std::max(0, (int)std::floor((gCy - size.y * 0.5) / 256.0)), ty1 = std::min(n - 1, (int)std::floor((gCy + size.y * 0.5) / 256.0));
    const ImU32 dim = IM_COL32(120, 128, 140, 255);   // dimmed: the standard map is bright
    int missing = 0;
    for (int ty = ty0; ty <= ty1; ty++)
        for (int tx = tx0; tx <= tx1; tx++) {
            bool failed = false;
            const int wx = ((tx % n) + n) % n;
            const ImTextureID tex = tileTexture(v.zoom, wx, ty, onlineMap, failed);
            const ImVec2 a(p0.x + size.x * 0.5f + (float)(tx * 256.0 - gCx), p0.y + size.y * 0.5f + (float)(ty * 256.0 - gCy));
            if (tex) { dl->AddImage(tex, a, ImVec2(a.x + 256, a.y + 256), ImVec2(0, 0), ImVec2(1, 1), dim); gAny = true; continue; }
            if (!failed) missing++;
            // until it comes (or when it cannot): the part of a coarser tile already decoded, magnified, so zooming in never goes blank
            for (int up = 1; up <= 6 && v.zoom - up >= kMinZoom; up++) {
                const ImTextureID ptex = shownTexture(v.zoom - up, wx >> up, ty >> up);
                if (!ptex) continue;
                const float f = 1.f / (float)(1 << up), u0 = (float)(wx & ((1 << up) - 1)) * f, v0 = (float)(ty & ((1 << up) - 1)) * f;
                dl->AddImage(ptex, a, ImVec2(a.x + 256, a.y + 256), ImVec2(u0, v0), ImVec2(u0 + f, v0 + f), dim);
                gAny = true;
                break;
            }
        }
    dl->PopClipRect();
    { std::lock_guard<std::mutex> lk(mu); dropStale(ImGui::GetFrameCount()); evict(ImGui::GetFrameCount()); }
    if (gAny) { const ImVec2 ts = ImGui::CalcTextSize(kCredit); dl->AddText(ImVec2(p0.x + size.x - ts.x - 6, p0.y + size.y - ts.y - 3), IM_COL32(140, 144, 148, 220), kCredit); }
    else {   // a line above the caption the screens put along the bottom (legend()), cut short in a narrow map
        const char* m = !onlineMap ? "map switched off: showing positions only" : missing ? "loading the map..." : "no map tiles (no network, or no picture decoder on this system): showing positions only";
        dl->AddText(ImVec2(p0.x + 8, p0.y + size.y - 2 * ImGui::GetTextLineHeightWithSpacing() - 6), IM_COL32(150, 154, 158, 255), ellipsize(m, size.x - 16).c_str());
    }
    return hov;
}

void legend(float mapWidth, const char* fmt, ...) {
    char b[256];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    const float room = mapWidth - 16 * gUi - (gAny ? ImGui::CalcTextSize(kCredit).x + 12 * gUi : 0.f);
    ImGui::TextDisabled("%s", ellipsize(b, std::max(0.f, room)).c_str());
}

bool online() { init(); return onlineMap; }

namespace {

void tilesMenu(ImVec4 buttonColour) {
    static Usage uk, ut;   // kept, this session's
    static double countedAt = -1e9;   // counted again every 2 s while the menu is open: tiles keep arriving
    static bool confirm = false;
    ImGui::PushStyleColor(ImGuiCol_Button, buttonColour);
    if (ImGui::Button("tiles")) { ImGui::OpenPopup("##maptiles"); countedAt = -1e9; confirm = false; }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The map tiles downloaded so far: keep them or not, how much room they take, delete them.");
    if (!ImGui::BeginPopup("##maptiles")) return;
    if (ImGui::GetTime() - countedAt > 2) { uk = scan(kept); ut = scan(temp); countedAt = ImGui::GetTime(); }
    const float wrap = 380 * gUi;
    ImGui::TextUnformatted("Downloaded map tiles");
    ImGui::Separator();
    const bool keep = keepNew;
    if (ImGui::RadioButton("Keep them on this computer", keep) && !keep) { moveTree(temp, kept); setKeep(true); countedAt = -1e9; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tiles stay until you delete them here, so the map also works offline where you have looked before.\nSwitching to this keeps the tiles of this session too.");
    if (ImGui::RadioButton("Delete them when OnAir closes", !keep) && keep) setKeep(false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("New tiles go to a temporary folder that is emptied when OnAir closes.\nTiles kept before stay (and are still used) until you delete them below.");
    ImGui::Spacing();
    const long nk = uk.files(), nt = ut.files();
    if (nk + nt == 0) ImGui::TextDisabled("No tiles downloaded yet.");
    else {
        const bool session = nt > 0 || !keep;   // the column of this session's tiles, only when there can be some
        if (ImGui::BeginTable("##tiles", session ? 5 : 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("zoom");
            ImGui::TableSetupColumn("shows");
            ImGui::TableSetupColumn("kept");
            if (session) ImGui::TableSetupColumn("this session");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            auto cell = [](long cnt, uint64_t b) { if (cnt) ImGui::Text("%ld  (%s)", cnt, sizeText(b).c_str()); else ImGui::TextDisabled("-"); };
            for (int z = 0; z <= kMaxZoom; z++) {
                if (!uk.n[z] && !ut.n[z]) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d", z);
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", zoomName(z));
                ImGui::TableNextColumn(); cell(uk.n[z], uk.bytes[z]);
                if (session) { ImGui::TableNextColumn(); cell(ut.n[z], ut.bytes[z]); }
                ImGui::TableNextColumn();
                ImGui::PushID(z);
                if (ImGui::SmallButton("delete")) { removeZoom(kept, z); removeZoom(temp, z); countedAt = -1e9; }
                ImGui::PopID();
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("all");
            ImGui::TableNextColumn();
            ImGui::TableNextColumn(); cell(nk, uk.size());
            if (session) { ImGui::TableNextColumn(); cell(nt, ut.size()); }
            ImGui::TableNextColumn();
            ImGui::EndTable();
        }
        if (!confirm) { if (ImGui::Button("Delete all...")) confirm = true; }
        else {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1), "Delete all %ld tiles (%s)?", nk + nt, sizeText(uk.size() + ut.size()).c_str());
            if (ImGui::Button("Delete")) {
                std::error_code ec;
                fs::remove_all(fsPath(kept), ec); fs::remove_all(fsPath(temp), ec);
                countedAt = -1e9; confirm = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) confirm = false;
        }
    }
    ImGui::Spacing();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::TextDisabled("Kept in %s", kept.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::SmallButton("Open the folder")) { std::error_code ec; fs::create_directories(fsPath(kept), ec); openUrl(kept); }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::TextDisabled("Deleting frees the disk: tiles already on the screen stay there for now. A tile takes about 10-40 KB.");
    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
}

} // namespace

void tileControls(const char* privacy) {
    init();
    // a line of their own under the screen's buttons when they would run past the map's right edge (a narrow map, a large display scale)
    const ImGuiStyle& st = ImGui::GetStyle();
    const float need = ImGui::CalcTextSize("tiles").x + 2 * st.FramePadding.x + 10 * gUi + ImGui::GetFrameHeight() + st.ItemInnerSpacing.x + ImGui::CalcTextSize("online map").x;
    if (ImGui::GetCursorScreenPos().x + need > gOrigin.x + gSize.x - 8 * gUi) ImGui::SetCursorScreenPos(ImVec2(gOrigin.x + 8 * gUi, ImGui::GetItemRectMax().y + 4 * gUi));
    tilesMenu(ImVec4(0.06f, 0.07f, 0.08f, 0.85f));   // the colour of the buttons on the maps
    ImGui::SameLine(0, 10 * gUi);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.82f, 0.84f, 1));
    if (ImGui::Checkbox("online map", &onlineMap)) { plat::prefs().setB("adsbMap", onlineMap); plat::prefs().flush(); }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fetch map tiles from the OpenStreetMap tile server (tile.openstreetmap.org) and %s.\nOnly tile numbers are sent%s%s. "
                          "Switch off to work offline: the map then shows the tiles already downloaded, or a plain grid.\nThis switch and the tiles menu are the same on every map.",
                          keepNew ? "keep them on this computer until you delete them (the tiles menu)" : "keep them only until OnAir closes (the tiles menu)",
                          privacy ? ", " : "", privacy ? privacy : "");
}

void shutdown() {
    if (started) {
        stopping = true;
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }
    if (inited && !temp.empty()) { std::error_code ec; fs::remove_all(fsPath(temp), ec); }   // this session's tiles (those kept are elsewhere)
}

namespace { struct Closer { ~Closer() { shutdown(); } } closer; }   // the downloader thread must be joined before the program ends

} // namespace adsbmap
