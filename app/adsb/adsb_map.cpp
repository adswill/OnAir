// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "adsb_map.h"
#include "app.h"
#include "dect2/geo/mbtiles.h"
#include "dect2/geo/tilecache.h"
#include "dect2/geo/worldlow.h"
#include "map/map_colours.h"
#include "map/my_location.h"
#include "tile_url.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
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
    std::vector<uint32_t> px;   // state 2: the picture already decoded by the worker (only the upload is left for the frame)
    double failedAt = 0;
    int lastUsed = 0;   // the frame it was last drawn in
};

std::mutex mu;
std::condition_variable cv;
std::map<uint64_t, Tile> tiles;
std::deque<uint64_t> queue;
std::atomic<int> blockedCode{0};   // 403 or 429 from the tile server: no more requests this session (OSM tile usage policy), the map says so
const std::string kUserAgent = std::string("OnAir/") + ONAIR_VERSION + " (+https://github.com/adswill/OnAir)";   // the app, its version and a contact URL
// one tile into `part`; false on any failure. A 403/429 is remembered in blockedCode and never retried.
bool fetchTile(const std::string& url, const std::string& part) {
    if (blockedCode) return false;
    const plat::HttpResult r = plat::httpCall("GET", url, "", "", {}, kUserAgent, 12, 4u << 20);
    if (r.status == 403 || r.status == 429) { blockedCode = r.status; return false; }
    if (r.status != 200 || r.body.size() < 100) return false;
    FILE* f = fopen(part.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(r.body.data(), 1, r.body.size(), f) == r.body.size();
    fclose(f);
    return ok;
}
int failStreak = 0;   // tiles failed in a row (mu): a server that says no is asked again only every 10 minutes
std::thread worker;
std::atomic<bool> stopping{false};
bool started = false, inited = false;
// Two folders of tiles: those kept until the user deletes them, and this session's, deleted when OnAir closes. New tiles go to the first
// while keepNew is set. Both are read.
std::string kept, temp;
std::string tileTpl = tileurl::kDefault;   // the tile server address template (set in init(), before the downloads start)
std::atomic<bool> keepNew{true};
bool onlineMap = true;   // the "online map" switch, shared by every map

uint64_t key(int z, int x, int y) { return ((uint64_t)z << 56) | ((uint64_t)x << 28) | (uint64_t)y; }

fs::path fsPath(const std::string& p) { return fs::path(reinterpret_cast<const char8_t*>(p.c_str())); }   // UTF-8 (Windows user names)
bool exists(const std::string& p) { std::error_code ec; const auto n = fs::file_size(fsPath(p), ec); return !ec && n > 100; }

std::string pathIn(const std::string& root, int z, int x, int y) { char b[64]; snprintf(b, sizeof b, "/%d/%d/%d.png", z, x, y); return root + b; }
// ---- an MBTiles file the user chose (settings: tiles menu). Mode 1: used after the tiles on disk and before the network; 2: used first.
// Its raster tiles are copied out to files (mbx/z/x/y.png) on first use, so the one decoder path serves all the sources.
dect2::MbTiles mb;
std::string mbx, mbPath, mbError;
std::atomic<int> mbMode{0};   // 0 off, 1 after the cache, 2 first

void mbClose() { mbMode = 0; mb.close(); }
void mbOpen(const std::string& path, int mode) {
    mbClose();
    mbPath = path; mbError.clear();
    std::error_code ec;
    fs::remove_all(fsPath(mbx), ec);   // the copies of another file
    if (path.empty() || mode == 0) return;
    if (mb.open(path, &mbError)) mbMode = mode;
}
std::string fromMb(int z, int x, int y) {   // the file of a tile of the MBTiles file, or ""
    if (!mbMode) return std::string();
    const std::string p = pathIn(mbx, z, x, y);
    if (exists(p)) return p;
    std::vector<uint8_t> d;
    if (!mb.tile(z, x, y, d) || !dect2::MbTiles::isRaster(d)) return std::string();
    std::error_code ec;
    fs::create_directories(fsPath(p).parent_path(), ec);
    const std::string part = p + ".part";
    FILE* f = fopen(part.c_str(), "wb");
    if (!f) return std::string();
    const bool w = fwrite(d.data(), 1, d.size(), f) == d.size();
    fclose(f);
    if (!w) { fs::remove(fsPath(part), ec); return std::string(); }
    fs::rename(fsPath(part), fsPath(p), ec);
    return !ec && exists(p) ? p : std::string();
}

std::string found(int z, int x, int y) {   // the file of a tile already downloaded (or in the MBTiles file), or ""
    std::string p;
    if (mbMode == 2 && !(p = fromMb(z, x, y)).empty()) return p;
    p = pathIn(kept, z, x, y);
    if (exists(p)) return p;
    p = pathIn(temp, z, x, y);
    if (exists(p)) return p;
    return mbMode == 1 ? fromMb(z, x, y) : std::string();
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
    tileTpl = tileurl::effective(plat::prefs().getS("mapTileUrl", ""));   // Settings > Network and updates > Map tile server (read once: restart)
    const std::string cname = tileurl::cacheName(tileTpl);   // "osm" for OpenStreetMap, so the tiles of earlier versions stay in use
    kept = plat::dataDir() + "/maps/" + cname;
    temp = cache + "/" + cname + "-temp";
    mbx = cache + "/mbtiles-x";
    keepNew = plat::prefs().getB("mapKeepTiles", true);
    onlineMap = plat::prefs().getB("adsbMap", true);   // the key from when only the ADS-B screen had a map
    std::error_code ec;
    fs::remove_all(fsPath(temp), ec);
    moveTree(cache + "/osm", kept);
    mbOpen(plat::prefs().getS("mapMbPath", ""), (int)plat::prefs().getI("mapMbMode", 1));
}

void setKeep(bool k) {
    keepNew = k;
    plat::prefs().setB("mapKeepTiles", k);
    plat::prefs().flush();
}

uint64_t capBytes() {
    long mb = (long)plat::prefs().getI("mapCapMb", (int)dect2::kDefaultTileCapMb);
    if (mb < 20) mb = 20;
    return (uint64_t)mb * 1024 * 1024;
}
void enforceCaps() { dect2::enforceCap(kept, capBytes()); dect2::enforceCap(temp, capBytes()); }

void run() {
    enforceCaps();   // at start (the cache may have grown under an older or larger setting)
    int fetches = 0;
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
            const std::string url = tileurl::expand(tileTpl, z, x, y);
            const std::string part = path + ".part";
            if (!ec && fetchTile(url, part)) {
                fs::rename(fsPath(part), fsPath(path), ec);
                ok = !ec && exists(path);
            }
            if (!ok) fs::remove(fsPath(part), ec);
            else if (++fetches % 50 == 0) enforceCaps();   // and every 50 downloads
        } else dect2::touchTile(found(z, x, y));
        // decoded here, not in the frame: OnAir's own PNG decoder (Linux, Windows) takes milliseconds a tile, and a pan or zoom asks
        // for dozens (docs/PERF-LINUX.md)
        std::vector<uint32_t> px;
        if (ok) {
            int w = 0, h = 0;
            const std::string path = found(z, x, y);
            if (path.empty() || !plat::decodeImage(path, w, h, px) || w != 256 || h != 256) px.clear();
        }
        std::lock_guard<std::mutex> lk(mu);
        tiles[k].state = ok ? 2 : 4;
        tiles[k].px = std::move(px);
        tiles[k].failedAt = glfwGetTime();
        failStreak = ok ? 0 : failStreak + 1;   // under mu
    }
}

void start() {
    if (started) return;
    started = true;
    worker = dect2::namedThread("onair-admap", run);
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
int decodedThisFrame = 0, uploadedThisFrame = 0;
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
    if (t.state == 2 && !t.px.empty() && gGfx && uploadedThisFrame < 24) {   // decoded by the worker: only the texture upload (256 KB)
        uploadedThisFrame++;
        t.img = gGfx->createImage(256, 256, 0xFF000000u);
        t.img->update(0, 0, 256, 256, t.px.data());
        std::vector<uint32_t>().swap(t.px);
        t.state = 3;
        nImages++;
    } else if (t.state == 2 && t.px.empty() && decodedThisFrame < 6) {   // found on disk without the worker (offline)
        decodedThisFrame++;
        int w = 0, h = 0;
        std::vector<uint32_t> px;
        const std::string path = found(z, x, y);
        if (!path.empty() && plat::decodeImage(path, w, h, px) && w == 256 && h == 256 && gGfx) {
            dect2::touchTile(path);   // used: the size cap evicts the least recently used first
            t.img = gGfx->createImage(256, 256, 0xFF000000u);
            t.img->update(0, 0, 256, 256, px.data());
            t.state = 3;
            nImages++;
        } else t.state = 4;
    }
    if (t.state == 4) {
        failed = true;
        if (online && !blockedCode && glfwGetTime() - t.failedAt > (failStreak >= 8 ? 600 : 30)) { t.state = 0; t.failedAt = 1e18; queue.push_back(k); cv.notify_one(); }   // try again later
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

void unproject(ImVec2 p, double& lat, double& lon) {
    lon = lonOf(gCx + (p.x - (gOrigin.x + gSize.x * 0.5f)), gZoom);
    lon = std::fmod(std::fmod(lon + 180.0, 360.0) + 360.0, 360.0) - 180.0;   // back on the globe when the map was panned round it
    lat = std::max(-85.0, std::min(85.0, latOf(gCy + (p.y - (gOrigin.y + gSize.y * 0.5f)), gZoom)));
}

bool clickedAt(ImVec2& p) { if (gClicked) p = gClickPos; return gClicked; }
bool tilesAvailable() { return gAny; }

const char* const kCredit = "map: OpenStreetMap contributors";

// The map colours (the "tiles" menu > Map colours): a preset or custom, kept in the settings (mapColPreset, mapCol*), one for every map.
mapcol::Scheme gScheme = mapcol::kPresets[mapcol::kDefault].s;
int gSchemeSel = mapcol::kDefault;
bool gSchemeLoaded = false;
ImU32 u32(uint32_t c, int a = 255) { return IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, a); }
void schemeLoad() {
    gSchemeLoaded = true;
    int sel = (int)plat::prefs().getI("mapColPreset", mapcol::kDefault);
    if (sel < 0 || sel > mapcol::kCustom) sel = mapcol::kDefault;
    gSchemeSel = sel;
    if (sel < mapcol::kCustom) { gScheme = mapcol::kPresets[sel].s; return; }
    auto g = [](const char* k, uint32_t d) { return (uint32_t)(plat::prefs().getI(k, (int)d) & 0xFFFFFF); };
    const mapcol::Scheme& d = mapcol::kPresets[mapcol::kDefault].s;
    gScheme = {g("mapColSea", d.sea), g("mapColGrid", d.grid), g("mapColCoast", d.coast), g("mapColLabel", d.label), g("mapColTint", d.tint)};
}
void schemeSave() {
    plat::prefs().setI("mapColPreset", gSchemeSel);
    plat::prefs().setI("mapColSea", (int)gScheme.sea); plat::prefs().setI("mapColGrid", (int)gScheme.grid); plat::prefs().setI("mapColCoast", (int)gScheme.coast);
    plat::prefs().setI("mapColLabel", (int)gScheme.label); plat::prefs().setI("mapColTint", (int)gScheme.tint);
    plat::prefs().flush();
}
const mapcol::Scheme& scheme() { if (!gSchemeLoaded) schemeLoad(); return gScheme; }

unsigned seaColour() { return scheme().sea; }
unsigned overlayText() { return mapcol::textOn(scheme().sea); }
unsigned panelColour() { return mapcol::panelOn(scheme().sea); }
unsigned legible(unsigned c) { return mapcol::legibleOn(c, scheme().sea); }

bool draw(View& v, ImVec2 size) {
    init();
    if (onlineMap) start();
    decodedThisFrame = 0; uploadedThisFrame = 0;
    v.zoom = std::max(kMinZoom, std::min(kMaxZoom, v.zoom));
    v.lat = std::max(-84.0, std::min(84.0, v.lat));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemAllowOverlap();   // the screens put buttons on the map afterwards: they must get the clicks, not the map under them
    ImGui::InvisibleButton("##map", size, ImGuiButtonFlags_MouseButtonLeft);
    const bool hov = ImGui::IsItemHovered();
    gClicked = false;
    static bool panned = false;   // IsMouseDragging is false on the release frame itself: remember that this press was a drag (#235)
    if (ImGui::IsItemActivated()) panned = false;
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 3.f)) {   // pan
        panned = true;
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        v.lon = lonOf(mercX(v.lon, v.zoom) - d.x, v.zoom);
        v.lat = latOf(mercY(v.lat, v.zoom) - d.y, v.zoom);
    } else if (ImGui::IsItemDeactivated() && !panned && hov) { gClicked = true; gClickPos = ImGui::GetIO().MousePos; }
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
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), u32(scheme().sea));
    // graticule every 10 degrees (what remains when there are no tiles)
    {
        for (int lo = -180; lo <= 180; lo += (v.zoom < 4 ? 30 : 10)) { const float x = p0.x + size.x * 0.5f + (float)(mercX(lo, v.zoom) - gCx); if (x > p0.x && x < p0.x + size.x) dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + size.y), u32(gScheme.grid)); }
        for (int la = -80; la <= 80; la += (v.zoom < 4 ? 30 : 10)) { const float y = p0.y + size.y * 0.5f + (float)(mercY(la, v.zoom) - gCy); if (y > p0.y && y < p0.y + size.y) dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x + size.x, y), u32(gScheme.grid)); }
    }
    // the rough world outline: under the tiles, and what is left when there are none (offline, nothing downloaded yet)
    if (v.zoom <= 8) {
        const double ws = worldSize(v.zoom);
        const ImU32 col = u32(gScheme.coast);
        std::vector<ImVec2> pts;
        for (const auto& poly : dect2::worldOutline()) {
            pts.clear();
            for (const auto& q : poly) pts.push_back(ImVec2(p0.x + size.x * 0.5f + (float)(mercX(q.first, v.zoom) - gCx), p0.y + size.y * 0.5f + (float)(mercY(q.second, v.zoom) - gCy)));
            for (int k = -2; k <= 2; k++) {   // the copies one world to the left and right
                const float dx = (float)(k * ws);
                float x0 = 1e30f, x1 = -1e30f;
                for (const ImVec2& q : pts) { x0 = std::min(x0, q.x + dx); x1 = std::max(x1, q.x + dx); }
                if (x1 < p0.x || x0 > p0.x + size.x) continue;
                std::vector<ImVec2> sh(pts);
                for (ImVec2& q : sh) q.x += dx;
                dl->AddPolyline(sh.data(), (int)sh.size(), col, ImDrawFlags_Closed, 1.5f);
            }
        }
    }
    gAny = false;
    const int n = 1 << v.zoom;
    const int tx0 = (int)std::floor((gCx - size.x * 0.5) / 256.0), tx1 = (int)std::floor((gCx + size.x * 0.5) / 256.0);
    const int ty0 = std::max(0, (int)std::floor((gCy - size.y * 0.5) / 256.0)), ty1 = std::min(n - 1, (int)std::floor((gCy + size.y * 0.5) / 256.0));
    const ImU32 dim = u32(gScheme.tint);   // dimmed in the default: the standard map is bright
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
    if (onlineMap && blockedCode) {
        char m[200];
        snprintf(m, sizeof m, "Map server refused (%d): change the map tile server in Settings > Network and updates.", (int)blockedCode);
        dl->AddText(ImVec2(p0.x + 8, p0.y + size.y - 3 * ImGui::GetTextLineHeightWithSpacing() - 6), IM_COL32(255, 200, 90, 255), ellipsize(m, size.x - 16).c_str());
    }
    if (onlineMap || gAny) { const ImVec2 ts = ImGui::CalcTextSize(kCredit); dl->AddText(ImVec2(p0.x + size.x - ts.x - 6, p0.y + size.y - ts.y - 3), u32(gScheme.label, 220), kCredit); }
    else {   // a line above the caption the screens put along the bottom (legend()), cut short in a narrow map
        const char* m = !onlineMap ? "map switched off: rough outline only" : missing ? "loading the map..." : "no map tiles (no network, or no picture decoder on this system): rough outline only";
        dl->AddText(ImVec2(p0.x + 8, p0.y + size.y - 2 * ImGui::GetTextLineHeightWithSpacing() - 6), u32(gScheme.label), ellipsize(m, size.x - 16).c_str());
    }
    mylocmap::overlay(dl, p0, size, hov);
    return hov;
}

void legend(float mapWidth, const char* fmt, ...) {
    char b[256];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    const float room = mapWidth - 16 * gUi - ((onlineMap || gAny) ? ImGui::CalcTextSize(kCredit).x + 12 * gUi : 0.f);
    ImGui::TextDisabled("%s", ellipsize(b, std::max(0.f, room)).c_str());
}

bool online() { init(); return onlineMap; }

namespace {

// ---------------------------------------------------------------- "download this area for offline use"
// One thread, one tile every 0.4 s, at most dect2::kMaxDownloadTiles tiles per download, tiles already on disk skipped, stoppable.
// The OpenStreetMap tile usage policy forbids bulk downloading: this is for the area you look at, never the world.
std::thread dlThread;
std::atomic<bool> dlStop{false}, dlRunning{false};
std::atomic<int> dlDone{0}, dlTotal{0}, dlFailed{0};

void dlJoin() { if (dlThread.joinable()) dlThread.join(); }

void dlStart(std::vector<dect2::TileKey> list) {
    dlJoin();
    dlStop = false; dlDone = 0; dlFailed = 0; dlTotal = (int)list.size(); dlRunning = true;
    const std::string root = kept;
    dlThread = dect2::namedThread("onair-adsbdl", [list = std::move(list), root] {
        int consecutive = 0, fetched = 0;
        for (const dect2::TileKey& t : list) {
            if (dlStop) break;
            const std::string path = dect2::tilePath(root, t.z, t.x, t.y);
            if (exists(path) || !found(t.z, t.x, t.y).empty()) { dlDone++; continue; }
            std::error_code ec;
            fs::create_directories(fsPath(path).parent_path(), ec);
            const std::string url = tileurl::expand(tileTpl, t.z, t.x, t.y);
            const std::string part = path + ".part";
            bool ok = false;
            if (!ec && fetchTile(url, part)) {
                fs::rename(fsPath(part), fsPath(path), ec);
                ok = !ec && exists(path);
            }
            if (!ok) { fs::remove(fsPath(part), ec); dlFailed++; }
            consecutive = ok ? 0 : consecutive + 1;
            dlDone++;
            if (ok && ++fetched % 50 == 0) enforceCaps();
            if (consecutive >= 5) break;   // the server says no (or there is no network): do not hammer it
            for (int i = 0; i < 8 && !dlStop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));   // 0.4 s per tile
        }
        enforceCaps();
        dlRunning = false;
    });
}

void areaDownload() {
    static int toZoom = 0, lastZoom = -1;
    ImGui::TextUnformatted("Download this area for offline use");
    if (dlRunning) {
        ImGui::ProgressBar(dlTotal > 0 ? (float)dlDone / (float)dlTotal : 0.f, ImVec2(220 * gUi, 0));
        ImGui::SameLine();
        ImGui::Text("%d / %d", dlDone.load(), dlTotal.load());
        if (dlFailed) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1), "%d failed", dlFailed.load()); }
        if (ImGui::Button("Stop")) dlStop = true;
        return;
    }
    dlJoin();
    ImVec2 a = gOrigin, b(gOrigin.x + gSize.x, gOrigin.y + gSize.y);
    double n1, w1, s1, e1;
    unproject(a, n1, w1); unproject(b, s1, e1);
    const bool wide = gSize.x >= worldSize(gZoom) * 0.95;   // the whole width of the globe on screen
    const int z0 = gZoom;
    if (lastZoom != z0 || toZoom < z0) { toZoom = std::min(kMaxZoom - 2, std::max(z0, dect2::deepestZoomFitting(s1, w1, n1, e1, z0, std::min(z0 + 3, 16)))); lastZoom = z0; }
    ImGui::SetNextItemWidth(90 * gUi);
    ImGui::InputInt("deepest zoom", &toZoom, 1, 1);
    toZoom = std::max(z0, std::min(16, toZoom));
    const dect2::AreaPlan plan = wide ? dect2::planArea(-85, -180, 85, 180, z0, toZoom) : dect2::planArea(s1, w1, n1, e1, z0, toZoom);
    if (plan.tooMany) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1), "Too many tiles (over %d): zoom in or lower the deepest zoom.", dect2::kMaxDownloadTiles);
    } else {
        ImGui::Text("Zoom %d to %d: %ld tiles, about %s. Tiles you already have are skipped.", z0, toZoom, plan.count, sizeText(plan.estBytes).c_str());
        const bool osm = tileTpl == tileurl::kDefault;   // the OpenStreetMap tile usage policy forbids bulk downloading
        const bool can = onlineMap && plan.count > 0 && !osm;
        if (!can) ImGui::BeginDisabled();
        if (ImGui::Button("Download")) dlStart(plan.tiles);
        if (!can) ImGui::EndDisabled();
        if (osm && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("OpenStreetMap's tile servers do not allow bulk downloads. Choose another tile server in Settings, or use an MBTiles file.");
        else if (!onlineMap && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Switch on \"online map\" first.");
        ImGui::SameLine();
        ImGui::TextDisabled("one tile every 0.4 s, at most %d per download", dect2::kMaxDownloadTiles);
    }
    if (dlTotal > 0 && dlDone > 0) ImGui::TextDisabled("Last download: %d of %d tiles done, %d failed.", dlDone.load(), dlTotal.load(), dlFailed.load());
}

// forget every tile picture and queued request (the source changed): they are decoded again from the new source
void resetTiles() {
    std::lock_guard<std::mutex> lk(mu);
    for (auto& [k, t] : tiles) if (t.img) { delete t.img; nImages--; }
    tiles.clear(); queue.clear();
}

// the MBTiles file: chosen here, kept in the settings (mapMbPath, mapMbMode)
void mbSettings() {
    ImGui::TextUnformatted("Offline map file (MBTiles)");
    if (mbPath.empty()) ImGui::TextDisabled("none chosen");
    else {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380 * gUi);
        ImGui::TextDisabled("%s", mbPath.c_str());
        ImGui::PopTextWrapPos();
    }
    if (ImGui::SmallButton("Choose file...")) {
        const std::string p = plat::openFileDialog();
        if (!p.empty()) {
            int m = (int)plat::prefs().getI("mapMbMode", 1);
            if (m == 0) m = 1;
            mbOpen(p, m);
            plat::prefs().setS("mapMbPath", mbError.empty() ? p : std::string());
            plat::prefs().setI("mapMbMode", m); plat::prefs().flush();
            resetTiles();
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) { mbOpen(std::string(), 0); mbPath.clear(); plat::prefs().setS("mapMbPath", ""); plat::prefs().flush(); resetTiles(); }
    if (!mbError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1), "%s", mbError.c_str());
    else if (mb.isOpen()) {
        const std::string nm = mb.meta("name"), at = mb.meta("attribution");
        ImGui::TextDisabled("%s%s%s", nm.empty() ? "opened" : nm.c_str(), mb.hasIndex() ? "" : " (no index: slow lookups)", at.empty() ? "" : ("  -  " + at).c_str());
        int m = mbMode;
        bool changed = false;
        changed |= ImGui::RadioButton("after the downloaded tiles, before the network", &m, 1);
        changed |= ImGui::RadioButton("first (instead of downloaded tiles)", &m, 2);
        if (changed) { mbMode = m; plat::prefs().setI("mapMbMode", m); plat::prefs().flush(); }
    }
    ImGui::TextDisabled("Raster 256 px PNG/JPEG tiles only (vector MBTiles cannot be drawn).");
}

// Map colours: presets, custom colours (sea, grid, outline, text, tile tint), and the reset to today's look.
void colourSettings() {
    using namespace mapcol;
    scheme();
    ImGui::TextUnformatted("Map colours");
    ImGui::SetNextItemWidth(170 * gUi);
    const char* cur = gSchemeSel < kPresetCount ? kPresets[gSchemeSel].name : "Custom";
    bool ch = false;
    if (ImGui::BeginCombo("##mapcolpreset", cur)) {
        for (int i = 0; i < kPresetCount; i++) if (ImGui::Selectable(kPresets[i].name, gSchemeSel == i)) { gSchemeSel = i; gScheme = kPresets[i].s; ch = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Colours of the map on every screen, full screen too. Overlay text, markers and panels adapt to the sea colour.");
    auto pick = [&](const char* label, uint32_t& c, const char* tip) {
        float f[3] = {unpack(c).r, unpack(c).g, unpack(c).b};
        if (ImGui::ColorEdit3(label, f, ImGuiColorEditFlags_NoInputs)) { c = pack({f[0], f[1], f[2]}); gSchemeSel = kCustom; ch = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    pick("sea / background", gScheme.sea, "The map background (shows where there is no tile, and the sea).");
    pick("grid", gScheme.grid, "The graticule lines.");
    pick("land outline / borders", gScheme.coast, "The rough world outline drawn under the tiles.");
    pick("labels", gScheme.label, "The map caption and credit text.");
    pick("tile tint", gScheme.tint, "Multiplies the downloaded tile pictures: white leaves them as they are, grey dims them.");
    if (ImGui::SmallButton("Reset to default")) { gSchemeSel = kDefault; gScheme = kPresets[kDefault].s; ch = true; }
    if (gSchemeSel == kCustom && ratio(gScheme.label, gScheme.sea) < kMinLabelRatio) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1), "Labels are hard to read on this sea colour.");
    if (ch) schemeSave();
}

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
    {
        int mb = (int)plat::prefs().getI("mapCapMb", (int)dect2::kDefaultTileCapMb);
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::InputInt("size limit (MB)", &mb, 50, 100, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
            mb = std::max(20, std::min(100000, mb));
            plat::prefs().setI("mapCapMb", mb); plat::prefs().flush();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("When the tiles take more room than this, the ones used longest ago are deleted (checked at start and every 50 downloads). Default 500 MB.");
    }
    ImGui::Separator();
    colourSettings();
    ImGui::Separator();
    mbSettings();
    ImGui::Separator();
    areaDownload();
    ImGui::Separator();
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

namespace {
struct FullCtx { const void* owner = nullptr; std::function<void()> body; };
FullCtx gFullCtx;                       // the tab body being drawn (set by FullScope)
bool gFullOn = false, gFullFamilyKnown = false;
const void* gFullOwner = nullptr;
std::function<void()> gFullBody;
int gFullFamily = 0;
void fullToggle();

void fullToggle() {
    if (gFullOn) gFullOn = false;
    else if (gFullCtx.body) { gFullOn = true; gFullOwner = gFullCtx.owner; gFullBody = gFullCtx.body; gFullFamilyKnown = false; }
}
}  // namespace

bool fullActive() { return gFullOn; }

void fullCorner(ImVec2 p0, ImVec2 size) {
    if (!gFullCtx.body && !gFullOn) return;
    const ImVec2 keep = ImGui::GetCursorScreenPos();
    const float w = ImGui::CalcTextSize(gFullOn ? tr("Exit full screen") : tr("Full screen")).x + 2 * ImGui::GetStyle().FramePadding.x;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + size.x - w - 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    static bool autoFull = std::getenv("ONAIR_MAP_FULL") != nullptr;   // dev: see tileControls()
    const bool autoHit = autoFull && !gFullOn && (autoFull = false, true);
    if (fullButton(gFullOn, "screen") || autoHit) fullToggle();
    ImGui::PopStyleColor();
    restoreCursor(keep);
}

void tileControls(const char* privacy, bool fullBtn) {
    init();
    // a line of their own under the screen's buttons when they would run past the map's right edge (a narrow map, a large display scale)
    const ImGuiStyle& st = ImGui::GetStyle();
    const bool canFull = fullBtn && (gFullCtx.body || gFullOn);
    const float need = ImGui::CalcTextSize("tiles").x + 2 * st.FramePadding.x + 10 * gUi + ImGui::GetFrameHeight() + st.ItemInnerSpacing.x + ImGui::CalcTextSize("online map").x
                       + (canFull ? ImGui::CalcTextSize(tr("Exit full screen")).x + 2 * st.FramePadding.x + 6 * gUi : 0.f);
    if (ImGui::GetCursorScreenPos().x + need > gOrigin.x + gSize.x - 8 * gUi) ImGui::SetCursorScreenPos(ImVec2(gOrigin.x + 8 * gUi, ImGui::GetItemRectMax().y + 4 * gUi));
    const ImVec2 at = ImGui::GetCursorScreenPos();   // where "tiles" goes: the checkbox is placed from this, not from the last item
    tilesMenu(ImVec4(0.06f, 0.07f, 0.08f, 0.85f));   // the colour of the buttons on the maps
    ImGui::SetCursorScreenPos(ImVec2(at.x + ImGui::CalcTextSize("tiles").x + 2 * st.FramePadding.x + 10 * gUi, at.y));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.82f, 0.84f, 1));
    if (ImGui::Checkbox("online map", &onlineMap)) { plat::prefs().setB("adsbMap", onlineMap); plat::prefs().flush(); }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fetch map tiles from the OpenStreetMap tile server (tile.openstreetmap.org) and %s.\nOnly tile numbers are sent%s%s. "
                          "Switch off to work offline: the map then shows the tiles already downloaded, or a plain grid.\nThis switch and the tiles menu are the same on every map.",
                          keepNew ? "keep them on this computer until you delete them (the tiles menu)" : "keep them only until OnAir closes (the tiles menu)",
                          privacy ? ", " : "", privacy ? privacy : "");
    if (canFull) {
        ImGui::SameLine(0, 6 * gUi);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
        static bool autoFull = std::getenv("ONAIR_MAP_FULL") != nullptr;   // dev: screenshots of the full-screen view (--shot) enter it by themselves
        const bool autoHit = autoFull && !gFullOn && (autoFull = false, true);
        if (fullButton(gFullOn, "screen") || autoHit) {
            fullToggle();
        }
        ImGui::PopStyleColor();
    }
}

bool fullButton(bool on, const char* what) {
    const bool hit = ImGui::Button(on ? tr("Exit full screen") : tr("Full screen"));
    if (ImGui::IsItemHovered()) {
        if (on) ImGui::SetTooltip("%s", tr("Back to the normal layout (Esc)"));
        else if (!std::strcmp(what, "map")) ImGui::SetTooltip("%s", tr("The map fills the whole window (Esc to leave)"));
        else if (!std::strcmp(what, "radar")) ImGui::SetTooltip("%s", tr("The radar fills the whole window (Esc to leave)"));
        else ImGui::SetTooltip("%s", tr("This screen fills the whole window (Esc to leave)"));
    }
    return hit;
}

FullScope::FullScope(const void* owner, std::function<void()> body) { if (!gFullOn) { gFullCtx.owner = owner; gFullCtx.body = std::move(body); } }
FullScope::~FullScope() { if (!gFullOn) gFullCtx = {}; }

bool fullDraw(const void* owner, int family, ImVec2 disp) {
    if (!gFullOn || gFullOwner != owner) return false;
    if (!gFullFamilyKnown) { gFullFamily = family; gFullFamilyKnown = true; if (std::getenv("ONAIR_MAP_FULL")) std::fprintf(stderr, "[mapfull] entered, family %d\n", family); }
    if (family != gFullFamily || !gFullBody) { gFullOn = false; return false; }   // another mode was picked: the screen is gone
    if (!ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { gFullOn = false; return false; }
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(disp);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * gUi, 4 * gUi));
    ImGui::Begin("##mapfull", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    gFullBody();
    ImGui::End();
    return true;
}

void shutdown() {
    dlStop = true;
    dlJoin();
    if (started) {
        stopping = true;
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }
    if (inited && !temp.empty()) { std::error_code ec; fs::remove_all(fsPath(temp), ec); }   // this session's tiles (those kept are elsewhere)
}

namespace { struct Closer { ~Closer() { shutdown(); } } closer; }   // the downloader thread must be joined before the program ends

} // namespace adsbmap
