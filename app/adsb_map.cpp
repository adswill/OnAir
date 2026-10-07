#include "adsb_map.h"
#include "app.h"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <thread>

namespace adsbmap {
namespace {

struct Tile {
    int state = 0;   // 0 wanted, 1 downloading, 2 file ready (to be decoded), 3 on the screen, 4 failed
    gfx::Image* img = nullptr;
    double failedAt = 0;
};

std::mutex mu;
std::condition_variable cv;
std::map<uint64_t, Tile> tiles;
std::deque<uint64_t> queue;
std::thread worker;
std::atomic<bool> stopping{false};
bool started = false;
std::string cache;

uint64_t key(int z, int x, int y) { return ((uint64_t)z << 56) | ((uint64_t)x << 28) | (uint64_t)y; }

bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && st.st_size > 100; }

std::string pathOf(int z, int x, int y) { char b[64]; snprintf(b, sizeof b, "/osm/%d/%d/%d.png", z, x, y); return cache + b; }

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
        const std::string path = pathOf(z, x, y);
        bool ok = exists(path);
        if (!ok) {
            char dir[512]; snprintf(dir, sizeof dir, "mkdir -p '%s/osm/%d/%d'", cache.c_str(), z, x);
            if (system(dir) == 0) {
                char cmd[1024];
                snprintf(cmd, sizeof cmd, "curl -fsSL --max-time 12 -A 'OnAir/%s (ADS-B map)' -o '%s.part' 'https://tile.openstreetmap.org/%d/%d/%d.png' && mv '%s.part' '%s'",
                         ONAIR_VERSION, path.c_str(), z, x, y, path.c_str(), path.c_str());
                ok = system(cmd) == 0 && exists(path);
            }
        }
        std::lock_guard<std::mutex> lk(mu);
        tiles[k].state = ok ? 2 : 4;
        tiles[k].failedAt = glfwGetTime();
    }
}

void start() {
    if (started) return;
    started = true;
    cache = plat::cacheDir();
    worker = std::thread(run);
}

// the picture of a tile if it is ready (decoding at most a few per frame); queue it otherwise
int decodedThisFrame = 0;
ImTextureID tileTexture(int z, int x, int y, bool online, bool& failed) {
    const uint64_t k = key(z, x, y);
    failed = false;
    std::lock_guard<std::mutex> lk(mu);
    auto it = tiles.find(k);
    if (it == tiles.end()) {
        if (!online && !exists(pathOf(z, x, y))) { failed = true; return 0; }
        tiles[k] = Tile();
        queue.push_back(k);
        cv.notify_one();
        return 0;
    }
    Tile& t = it->second;
    if (t.state == 2 && decodedThisFrame < 6) {
        decodedThisFrame++;
        int w = 0, h = 0;
        std::vector<uint32_t> px;
        if (plat::decodeImage(pathOf(z, x, y), w, h, px) && w == 256 && h == 256 && gGfx) {
            t.img = gGfx->createImage(256, 256, 0xFF000000u);
            t.img->update(0, 0, 256, 256, px.data());
            t.state = 3;
        } else t.state = 4;
    }
    if (t.state == 4) {
        failed = true;
        if (online && glfwGetTime() - t.failedAt > 30) { t.state = 0; t.failedAt = 1e18; queue.push_back(k); cv.notify_one(); }   // try again later
        return 0;
    }
    return t.state == 3 && t.img ? t.img->texture() : (ImTextureID)0;
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

} // namespace

ImVec2 project(double lat, double lon) {
    double dx = mercX(lon, gZoom) - gCx;
    const double ws = worldSize(gZoom);
    if (dx > ws / 2) dx -= ws; else if (dx < -ws / 2) dx += ws;   // the short way round the globe
    return ImVec2(gOrigin.x + gSize.x * 0.5f + (float)dx, gOrigin.y + gSize.y * 0.5f + (float)(mercY(lat, gZoom) - gCy));
}

bool clickedAt(ImVec2& p) { if (gClicked) p = gClickPos; return gClicked; }
bool tilesAvailable() { return gAny; }

bool draw(View& v, ImVec2 size) {
    if (v.online) start();
    decodedThisFrame = 0;
    v.zoom = std::max(2, std::min(12, v.zoom));
    v.lat = std::max(-84.0, std::min(84.0, v.lat));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
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
        v.zoom = std::max(2, std::min(12, v.zoom + (ImGui::GetIO().MouseWheel > 0 ? 1 : -1)));
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
        const double ws = worldSize(v.zoom);
        for (int lo = -180; lo <= 180; lo += (v.zoom < 4 ? 30 : 10)) { const float x = p0.x + size.x * 0.5f + (float)(mercX(lo, v.zoom) - gCx); if (x > p0.x && x < p0.x + size.x) dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + size.y), IM_COL32(30, 34, 38, 255)); }
        for (int la = -80; la <= 80; la += (v.zoom < 4 ? 30 : 10)) { const float y = p0.y + size.y * 0.5f + (float)(mercY(la, v.zoom) - gCy); if (y > p0.y && y < p0.y + size.y) dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x + size.x, y), IM_COL32(30, 34, 38, 255)); }
        (void)ws;
    }
    gAny = false;
    const int n = 1 << v.zoom;
    const int tx0 = (int)std::floor((gCx - size.x * 0.5) / 256.0), tx1 = (int)std::floor((gCx + size.x * 0.5) / 256.0);
    const int ty0 = std::max(0, (int)std::floor((gCy - size.y * 0.5) / 256.0)), ty1 = std::min(n - 1, (int)std::floor((gCy + size.y * 0.5) / 256.0));
    int missing = 0;
    for (int ty = ty0; ty <= ty1; ty++)
        for (int tx = tx0; tx <= tx1; tx++) {
            bool failed = false;
            const ImTextureID tex = tileTexture(v.zoom, ((tx % n) + n) % n, ty, v.online, failed);
            const ImVec2 a(p0.x + size.x * 0.5f + (float)(tx * 256.0 - gCx), p0.y + size.y * 0.5f + (float)(ty * 256.0 - gCy));
            if (tex) { dl->AddImage(tex, a, ImVec2(a.x + 256, a.y + 256), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(120, 128, 140, 255)); /* dimmed: the standard map is bright */ gAny = true; }
            else if (!failed) missing++;
        }
    dl->PopClipRect();
    const char* credit = "map: OpenStreetMap contributors";
    if (gAny) { const ImVec2 ts = ImGui::CalcTextSize(credit); dl->AddText(ImVec2(p0.x + size.x - ts.x - 6, p0.y + size.y - ts.y - 3), IM_COL32(140, 144, 148, 220), credit); }
    else {
        const char* m = !v.online ? "map switched off: showing positions only" : missing ? "loading the map..." : "no map tiles (no network, or no picture decoder on this system): showing positions only";
        dl->AddText(ImVec2(p0.x + 8, p0.y + size.y - ImGui::GetTextLineHeight() - 6), IM_COL32(150, 154, 158, 255), m);
    }
    return hov;
}

void shutdown() {
    if (!started) return;
    stopping = true;
    cv.notify_all();
    if (worker.joinable()) worker.join();
}

namespace { struct Closer { ~Closer() { shutdown(); } } closer; }   // the downloader thread must be joined before the program ends

} // namespace adsbmap
