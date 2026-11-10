// The picture gallery window (View > Images): the pictures every image receiver finished, kept across restarts in <data folder>/images/
// (index.tsv, one PNG per picture, a small thumbnail, the separate channels of a multi-channel picture, the ground track of a pass), capped by
// size. A viewer with zoom and pan, contrast stretch, gamma, histogram equalise, invert, false-colour presets, and for passes with a ground
// track an equirectangular map projection (graticule, coastlines, borders; saved as PNG with a world file, or as GeoTIFF), tone curves,
// colour palettes, a histogram, crop to the visible part, and a mosaic that stitches several projected passes into one map.
#include "../app.h"
#include "gallery.h"
#include "enhance_ui.h"
#include "../gfx/gfx.h"
#include "../ui/icons.h"
#include "../platform/platform.h"
#include "dect2/common/png.h"
#include "dect2/dsp/wxstudio.h"
#include "dect2/engine/updater.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include "picsave_ui.h"
#include "dect2/automation/engine.h"
#include <map>
#include "../state/picsave.h"
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace dect2::img;

namespace {

picsave::PendingDelete gPend;   // pictures deleted from the list: hidden for a few seconds (Undo), then really removed
constexpr double kUndoSec = 8.0;

struct Item {
    std::string id, key, source, sat, detail;
    double time = 0;
    int w = 0, h = 0, planeMask = 0, lpt = 0;
    double swathKm = 0;
    uint64_t bytes = 0;
    gfx::Image* thumb = nullptr;
    int thumbW = 0, thumbH = 0;
    bool thumbTried = false;
};

struct View {
    std::string id;
    int w = 0, h = 0;
    std::vector<uint8_t> src;                       // the picture as stored
    std::vector<std::vector<uint8_t>> planes;       // the stored channels (grey), 6 slots
    std::vector<uint8_t> disp;                      // src with the enhancements
    gfx::Image* tex = nullptr; int texW = 0, texH = 0;
    bool dirty = true;
    int preset = 0;
    bool stretch = false; float loPct = 1.f, hiPct = 99.f;
    float gamma = 1.f;
    bool equalise = false, invert = false;
    // geometry
    bool geo = false;
    SwathGeom sg; int lpt = 8;
    bool mapOn = false, mapDirty = true, grat = true; int gratStep = 2;   // index into kSteps
    MapImage map;
    gfx::Image* mapTex = nullptr; int mapTexW = 0, mapTexH = 0;
    float zoom = 1.f; ImVec2 pan = ImVec2(0, 0); bool fit = true;
    // studio
    int curve = 0, palette = 0;
    bool coast = true, border = false, hist = false, ovDirty = false;
    std::array<uint32_t, 256> hbins{};
    MapImage mapBase;                               // the projection without the overlays (mosaic parts, GeoTIFF)
    bool cropOn = false; int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;   // pixels of the image on show (the map, or the picture)
    float vis[4] = {0, 0, 1, 1};                    // the part of the image on screen, in its pixels
    std::string msg;
    // enhancement (enhance_ui.h): fed with the picture after the false colour, the studio works on its view
    enhui::Enhancer enh;
    int fedPreset = -1;
    int dw = 0, dh = 0;                             // size of disp (the enhanced picture can be larger: upscale, fax width)
};

struct State {
    bool loaded = false;
    std::string dir;
    std::vector<Item> items;     // newest first
    std::string sel;
    View v;
    long capMb = 500;
    std::string msg;
    std::vector<MapImage> mosaic;                   // projected passes added for stitching
    std::vector<std::string> mosaicNames;
    bool showMosaic = false;
    View mos;                                       // the stitched map on show
};
State S;

// the names of the curves and palettes live in the core; marked here for the translation catalogs (tr() is applied where they are drawn)
[[maybe_unused]] const char* const kStudioNames[] = {N_("S-curve (contrast)"), N_("Lift shadows"), N_("Cloud detail (flat bright end)"), N_("Night infrared"),
                                                     N_("No palette"), N_("Thermal"), N_("Rainbow"), N_("Enhanced infrared"), N_("Sepia")};

const double kSteps[] = {1, 2, 5, 10, 15, 30};
const long kCaps[] = {100, 250, 500, 1000, 2000, 5000};

std::string imgDir() {
    std::string d = plat::dataDir() + "/images";
    std::error_code ec;
    fs::create_directories(fs::path(reinterpret_cast<const char8_t*>(d.c_str())), ec);
    return d;
}
fs::path P(const std::string& s) { return fs::path(reinterpret_cast<const char8_t*>(s.c_str())); }

std::string clean(std::string s) { for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return s; }

void saveIndex() {
    std::ofstream f(P(S.dir + "/index.tsv"), std::ios::trunc);
    for (const Item& it : S.items)
        f << it.id << '\t' << clean(it.key) << '\t' << clean(it.source) << '\t' << clean(it.sat) << '\t' << clean(it.detail) << '\t' << (long long)it.time << '\t'
          << it.w << '\t' << it.h << '\t' << it.planeMask << '\t' << it.lpt << '\t' << it.swathKm << '\t' << it.bytes << '\n';
}

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.capMb = plat::prefs().getI("galleryCapMb", 500);
    S.dir = imgDir();
    std::ifstream f(P(S.dir + "/index.tsv"));
    std::string line;
    while (std::getline(f, line)) {
        std::vector<std::string> c;
        std::stringstream ss(line);
        std::string t;
        while (std::getline(ss, t, '\t')) c.push_back(t);
        if (c.size() < 12) continue;
        Item it;
        it.id = c[0]; it.key = c[1]; it.source = c[2]; it.sat = c[3]; it.detail = c[4];
        it.time = atof(c[5].c_str()); it.w = atoi(c[6].c_str()); it.h = atoi(c[7].c_str()); it.planeMask = atoi(c[8].c_str());
        it.lpt = atoi(c[9].c_str()); it.swathKm = atof(c[10].c_str()); it.bytes = (uint64_t)atoll(c[11].c_str());
        std::error_code ec;
        if (!fs::exists(P(S.dir + "/" + it.id + ".png"), ec)) continue;
        S.items.push_back(it);
    }
    std::stable_sort(S.items.begin(), S.items.end(), [](const Item& a, const Item& b) { return a.time > b.time; });
}

void rmFiles(const Item& it) {
    std::error_code ec;
    fs::remove(P(S.dir + "/" + it.id + ".png"), ec);
    fs::remove(P(S.dir + "/" + it.id + "_t.png"), ec);
    fs::remove(P(S.dir + "/" + it.id + "_e.png"), ec);
    fs::remove(P(S.dir + "/" + it.id + ".trk"), ec);
    for (int c = 0; c < 6; c++) fs::remove(P(S.dir + "/" + it.id + "_c" + std::to_string(c) + ".png"), ec);
}

void dropItemAt(size_t i) {
    Item& it = S.items[i];
    rmFiles(it);
    delete it.thumb;
    if (S.v.id == it.id) { delete S.v.tex; delete S.v.mapTex; S.v = View(); S.sel.clear(); }
    S.items.erase(S.items.begin() + (long)i);
}

void enforceCap() {
    uint64_t tot = 0;
    for (const Item& it : S.items) tot += it.bytes;
    const uint64_t cap = (uint64_t)S.capMb * 1024 * 1024;
    while (tot > cap && S.items.size() > 1) { tot -= S.items.back().bytes; dropItemAt(S.items.size() - 1); }
    while (S.items.size() > 2000) dropItemAt(S.items.size() - 1);
}

uint64_t fileSize(const std::string& p) { std::error_code ec; const auto s = fs::file_size(P(p), ec); return ec ? 0 : (uint64_t)s; }

// the grey plane as RGBA for the PNG writer
bool writeGrey(const std::string& path, const std::vector<uint8_t>& g, int w, int h) {
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (size_t i = 0; i < g.size() && i < (size_t)w * h; i++) { rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = g[i]; rgba[i * 4 + 3] = 255; }
    return gfx::writePng(path.c_str(), rgba.data(), w, h);
}

bool readPng(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return dect2::decodePng(b.data(), b.size(), w, h, rgba) && w > 0 && h > 0;
}

void makeThumb(const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out, int& tw, int& th) {
    const double sc = std::min(1.0, 160.0 / std::max(w, h));
    tw = std::max(1, (int)std::lround(w * sc)); th = std::max(1, (int)std::lround(h * sc));
    out.assign((size_t)tw * th * 4, 255);
    for (int y = 0; y < th; y++) {
        const int y0 = (int)((int64_t)y * h / th), y1 = std::max(y0 + 1, (int)((int64_t)(y + 1) * h / th));
        for (int x = 0; x < tw; x++) {
            const int x0 = (int)((int64_t)x * w / tw), x1 = std::max(x0 + 1, (int)((int64_t)(x + 1) * w / tw));
            uint32_t s[3] = {0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1 && yy < h; yy += std::max(1, (y1 - y0) / 4))
                for (int xx = x0; xx < x1 && xx < w; xx += std::max(1, (x1 - x0) / 4)) { const uint8_t* p = rgba + ((size_t)yy * w + xx) * 4; s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; n++; }
            if (n) for (int c = 0; c < 3; c++) out[((size_t)y * tw + x) * 4 + c] = (uint8_t)(s[c] / n);
        }
    }
}

std::string stamp(double t) {
    char b[32]; struct tm tmv; const time_t tt = (time_t)t;
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tmv);
    return b;
}
std::string fileStamp(double t) {
    char b[32]; struct tm tmv; const time_t tt = (time_t)t;
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tmv);
    return b;
}
std::string fsafe(std::string s) { for (char& c : s) if (!isalnum((unsigned char)c) && c != '-') c = '-'; return s; }

}  // namespace

namespace gallery {

namespace {
struct PassCtx { std::string sat; double aos = 0, maxEl = 0, until = 0; } gPassCtx;
}

void setPassContext(const std::string& sat, double aos, double maxElDeg, double validUntil) {
    gPassCtx = PassCtx{sat, aos, maxElDeg, validUntil};
}

// image.done: a picture whose key has not changed for kDoneSec is finished (a receiver republishes a growing picture under one key)
struct Pending { double lastWall = 0; std::string path, source, sat, detail; int w = 0, h = 0; double time = 0; };
std::map<std::string, Pending> gPending;
constexpr double kDoneSec = 20;

void emitDone() {
    if (gPending.empty()) return;
    const double now = (double)std::time(nullptr);
    for (auto it = gPending.begin(); it != gPending.end();) {
        if (now - it->second.lastWall < kDoneSec) { ++it; continue; }
        const Pending& p = it->second;
        if (dect2::automation::wants(dect2::automation::FImage)) {
            dect2::automation::Event ev("image.done");
            ev.at(now, p.source, 0);
            ev.str("path", p.path).str("source", p.source).str("satellite", p.sat).str("detail", p.detail).str("key", it->first).num("width", p.w).num("height", p.h).num("picture_time", p.time);
            dect2::automation::emit(std::move(ev));
        }
        it = gPending.erase(it);
    }
}

void publish(const std::string& key, const std::string& source, double unixTime, int w, int h, const uint8_t* rgba, const Meta& meta) {
    if (w <= 0 || h <= 0 || !rgba) return;
    load();
    // the same key replaces the picture
    std::string id;
    for (size_t i = 0; i < S.items.size(); i++)
        if (S.items[i].key == key) { id = S.items[i].id; rmFiles(S.items[i]); dropItemAt(i); break; }
    if (id.empty()) { static int n = 0; id = fileStamp(unixTime) + "-" + fsafe(source) + "-" + std::to_string(n++ % 1000); }
    Item it;
    it.id = id; it.key = key; it.source = source; it.sat = meta.satellite; it.detail = meta.detail; it.time = unixTime; it.w = w; it.h = h;
    if (!gPassCtx.sat.empty() && unixTime >= gPassCtx.aos - 60 && unixTime <= gPassCtx.until) {   // taken during a scheduled pass
        if (it.sat.empty()) it.sat = gPassCtx.sat;
        const std::time_t tt = (std::time_t)gPassCtx.aos;
        std::tm lt = *std::localtime(&tt);
        char b[64];
        std::strftime(b, sizeof b, "pass %H:%M", &lt);
        it.detail += (it.detail.empty() ? "" : ", ") + std::string(b) + ", max " + std::to_string((int)std::lround(gPassCtx.maxEl)) + " deg";
    }
    const std::string base = S.dir + "/" + id;
    if (!gfx::writePng((base + ".png").c_str(), rgba, w, h)) { S.msg = "could not write " + base + ".png"; return; }
    it.bytes = fileSize(base + ".png");
    enhui::autoEnhanceFile(source, rgba, w, h, base + "_e.png");   // "Apply to all new images" of this mode: an enhanced copy beside the raw one
    std::vector<uint8_t> th; int tw, thh;
    makeThumb(rgba, w, h, th, tw, thh);
    gfx::writePng((base + "_t.png").c_str(), th.data(), tw, thh);
    it.bytes += fileSize(base + "_t.png");
    for (size_t c = 0; c < meta.planes.size() && c < 6; c++)
        if (meta.planes[c].size() >= (size_t)w * h && writeGrey(base + "_c" + std::to_string(c) + ".png", meta.planes[c], w, h)) {
            it.planeMask |= 1 << c; it.bytes += fileSize(base + "_c" + std::to_string(c) + ".png");
        }
    if (meta.track.size() >= 2) {
        std::ofstream f(P(base + ".trk"));
        f << meta.linesPerTrack << ' ' << meta.swathKm << '\n';
        for (const auto& p : meta.track) f << p.lat << ' ' << p.lon << '\n';
        it.lpt = meta.linesPerTrack; it.swathKm = meta.swathKm;
        it.bytes += fileSize(base + ".trk");
    }
    S.items.insert(S.items.begin(), it);
    std::stable_sort(S.items.begin(), S.items.end(), [](const Item& a, const Item& b) { return a.time > b.time; });
    enforceCap();
    saveIndex();
    Pending& pd = gPending[key];
    pd.lastWall = (double)std::time(nullptr); pd.path = base + ".png"; pd.source = source; pd.sat = it.sat; pd.detail = it.detail; pd.w = w; pd.h = h; pd.time = unixTime;
    if (gPending.size() > 200) gPending.erase(gPending.begin());
}

void publishGray(const std::string& key, const std::string& source, double t, int w, int h, const uint8_t* g, const Meta& meta) {
    if (w <= 0 || h <= 0 || !g) return;
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) { rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = g[i]; rgba[i * 4 + 3] = 255; }
    publish(key, source, t, w, h, rgba.data(), meta);
}

void publishRgb(const std::string& key, const std::string& source, double t, int w, int h, const uint8_t* rgb, const Meta& meta) {
    if (w <= 0 || h <= 0 || !rgb) return;
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) { rgba[i * 4] = rgb[i * 3]; rgba[i * 4 + 1] = rgb[i * 3 + 1]; rgba[i * 4 + 2] = rgb[i * 3 + 2]; rgba[i * 4 + 3] = 255; }
    publish(key, source, t, w, h, rgba.data(), meta);
}

void publishFile(const std::string& key, const std::string& source, double t, const std::vector<uint8_t>& bytes, const Meta& meta) {
    if (bytes.size() < 16) return;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (bytes[0] == 0x89 && bytes[1] == 'P' && dect2::decodePng(bytes.data(), bytes.size(), w, h, rgba) && w > 0 && h > 0) {
        publish(key, source, t, w, h, rgba.data(), meta);
        return;
    }
    const std::string tmp = plat::cacheDir() + "/gallery_in.img";
    {
        std::ofstream f(P(tmp), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    }
    std::vector<uint32_t> px;
    if (plat::decodeImage(tmp, w, h, px) && w > 0 && h > 0 && w <= 16384 && h <= 16384) {
        std::vector<uint8_t> b(px.size() * 4);
        memcpy(b.data(), px.data(), b.size());
        publish(key, source, t, w, h, b.data(), meta);
    }
}

}  // namespace gallery

namespace {

// ---------------------------------------------------------------- the viewer

void selectItem(const Item& it) {
    delete S.v.tex; delete S.v.mapTex;
    S.v = View();
    S.sel = it.id;
    View& v = S.v;
    v.id = it.id; v.w = it.w; v.h = it.h;
    v.enh.setSource(it.source);
    v.enh.setSaveName(fileStamp(it.time) + "-" + fsafe(it.sat.empty() ? it.source : it.sat) + "-enhanced.png");
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!readPng(S.dir + "/" + it.id + ".png", w, h, rgba) || w != it.w || h != it.h) { v.msg = "could not read the picture"; v.w = v.h = 0; return; }
    v.src = std::move(rgba);
    v.planes.assign(6, {});
    for (int c = 0; c < 6; c++)
        if (it.planeMask >> c & 1) {
            std::vector<uint8_t> p; int pw, ph;
            if (readPng(S.dir + "/" + it.id + "_c" + std::to_string(c) + ".png", pw, ph, p) && pw == it.w && ph == it.h) {
                v.planes[(size_t)c].resize((size_t)pw * ph);
                for (size_t i = 0; i < v.planes[(size_t)c].size(); i++) v.planes[(size_t)c][i] = p[i * 4];
            }
        }
    std::ifstream f(P(S.dir + "/" + it.id + ".trk"));
    if (f) {
        double swath; int lpt;
        f >> lpt >> swath;
        double la, lo;
        while (f >> la >> lo) v.sg.track.push_back({la, lo});
        v.sg.swathKm = swath; v.sg.cols = it.w; v.lpt = lpt;
        v.geo = v.sg.track.size() >= 2 && lpt > 0;
    }
}

void render(View& v) {
    const size_t n = (size_t)v.w * v.h;
    v.disp = v.src;
    const auto& pre = falseColourPresets();
    if (v.preset > 0 && v.preset < (int)pre.size()) {
        const Preset& p = pre[(size_t)v.preset];
        auto pl = [&](int c) -> const uint8_t* { return c >= 0 && c < (int)v.planes.size() && v.planes[(size_t)c].size() >= n ? v.planes[(size_t)c].data() : nullptr; };
        const uint8_t *a = pl(p.r), *b = pl(p.g), *c = pl(p.b);
        if (a || b || c) composeRgba(a, b, c, n, p.thermal, v.disp.data());
    }
    // the enhancement works on the picture with its false colour; the tone controls below work on what it shows
    if (v.fedPreset != v.preset) { v.fedPreset = v.preset; v.enh.feed(v.disp.data(), v.w, v.h, true); }
    v.dw = v.w; v.dh = v.h;
    if (v.enh.on()) {
        int ew, eh;
        const std::vector<uint8_t>& ev = v.enh.view(ew, eh);
        if (!ev.empty() && ew > 0 && eh > 0) { v.disp = ev; v.dw = ew; v.dh = eh; }
    }
    const size_t nd = (size_t)v.dw * v.dh;
    std::vector<uint8_t> y = lumaOf(v.disp.data(), nd);
    std::array<uint8_t, 256> lut;
    for (int i = 0; i < 256; i++) lut[(size_t)i] = (uint8_t)i;
    if (v.stretch) {
        int lo, hi;
        percentileRange(y.data(), y.size(), v.loPct, v.hiPct, true, lo, hi);
        lut = composeLut(lut, stretchLut(lo, hi));
    }
    if (v.gamma != 1.f) lut = composeLut(lut, gammaLut(v.gamma));
    if (v.curve > 0 && v.curve < (int)curvePresets().size()) lut = composeLut(lut, curveLut(curvePresets()[(size_t)v.curve].pts));
    if (v.equalise) {
        std::vector<uint8_t> y2(y.size());
        for (size_t i = 0; i < y.size(); i++) y2[i] = lut[y[i]];
        lut = composeLut(lut, equaliseLut(y2.data(), y2.size(), true));
    }
    if (v.invert) lut = composeLut(lut, invertLut());
    applyLutRgba(v.disp.data(), nd, lut);
    if (v.palette > 0) applyPaletteRgba(v.disp.data(), nd, v.palette);
    if (v.hist) { const std::vector<uint8_t> yy = lumaOf(v.disp.data(), nd); histogramOf(yy.data(), yy.size(), true, v.hbins); }
    v.dirty = false;
    v.mapDirty = true;
    if (gGfx) {
        if (!v.tex || v.texW != v.dw || v.texH != v.dh) { delete v.tex; v.tex = gGfx->createImage(v.dw, v.dh, 0xFF000000u); v.texW = v.dw; v.texH = v.dh; }
        if (v.tex) v.tex->update(0, 0, v.dw, v.dh, reinterpret_cast<const uint32_t*>(v.disp.data()));
    }
}

// the map texture from mapBase with the graticule and vectors on top
void applyOverlays(View& v) {
    v.map = v.mapBase;
    if (v.grat) drawGraticule(v.map, kSteps[v.gratStep], 0xFF40E0FFu);
    if (v.border || v.coast) drawVectors(v.map, (v.coast ? kVecCoast : 0u) | (v.border ? kVecBorder : 0u), 0xFF60FF60u);
    if (gGfx) {
        if (!v.mapTex || v.mapTexW != v.map.w || v.mapTexH != v.map.h) { delete v.mapTex; v.mapTex = gGfx->createImage(v.map.w, v.map.h, 0xFF000000u); v.mapTexW = v.map.w; v.mapTexH = v.map.h; }
        if (v.mapTex) {
            // transparent where the pass did not reach: dark grey
            std::vector<uint8_t> px = v.map.rgba;
            for (size_t i = 0; i < px.size(); i += 4) if (!px[i + 3]) { px[i] = px[i + 1] = px[i + 2] = 24; px[i + 3] = 255; }
            v.mapTex->update(0, 0, v.map.w, v.map.h, reinterpret_cast<const uint32_t*>(px.data()));
        }
    }
    v.ovDirty = false;
}

void project(View& v) {
    v.mapDirty = false;
    if (v.dw != v.w || v.dh != v.h) { v.msg = "the map needs the picture at its own size (turn off Upscale and Fax width in the enhancement)"; v.mapOn = false; return; }
    SwathGeom g = v.sg;
    g.cols = v.w;
    if (!projectSwath(v.disp.data(), v.w, v.h, v.lpt, g, 2400, v.mapBase)) { v.msg = "could not project"; v.mapOn = false; return; }
    v.cropOn = false;
    applyOverlays(v);
    v.fit = true;
}

// the part of the map or picture to export: the crop when one is set (the map keeps its geography)
bool exportMap(const View& v, const MapImage& src, MapImage& out) {
    if (src.rgba.empty()) return false;
    if (!v.cropOn) { out = src; return true; }
    const double d = src.degPerPx;
    return cropMap(src, src.west + v.cx0 * d, src.north - v.cy0 * d, src.west + v.cx1 * d, src.north - v.cy1 * d, out);
}

void setCropToView(View& v, int iw, int ih) {
    v.cx0 = std::clamp((int)std::floor(v.vis[0]), 0, iw); v.cy0 = std::clamp((int)std::floor(v.vis[1]), 0, ih);
    v.cx1 = std::clamp((int)std::ceil(v.vis[2]), 0, iw); v.cy1 = std::clamp((int)std::ceil(v.vis[3]), 0, ih);
    v.cropOn = v.cx1 - v.cx0 >= 8 && v.cy1 - v.cy0 >= 8 && (v.cx1 - v.cx0 < iw || v.cy1 - v.cy0 < ih);
}

std::string stemOf(std::string path) {
    const size_t dot = path.rfind('.');
    if (dot != std::string::npos && dot > path.find_last_of("/\\") + 0) path.resize(dot);
    return path;
}

void saveMapPng(View& v, const std::string& name) {
    MapImage m;
    if (!exportMap(v, v.map, m)) { v.msg = "nothing to save"; return; }
    const std::string path = saveFileDialog(name.c_str());
    if (path.empty()) return;
    bool ok = gfx::writePng(path.c_str(), m.rgba.data(), m.w, m.h);
    if (ok) { std::ofstream f(P(stemOf(path) + ".pgw"), std::ios::trunc); f << worldFile(m); }
    v.msg = ok ? "saved " + path + " and its .pgw world file" : "could not write " + path;
}

void saveGeoTiffFile(View& v, const std::string& name) {
    MapImage m;
    if (!exportMap(v, v.mapBase, m)) { v.msg = "nothing to save"; return; }
    const std::vector<uint8_t> t = geoTiff(m);
    if (t.empty()) { v.msg = "could not make the GeoTIFF"; return; }
    const std::string path = saveFileDialog(name.c_str());
    if (path.empty()) return;
    std::ofstream f(P(path), std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(t.data()), (std::streamsize)t.size());
    v.msg = f ? "saved " + path + " (map without the overlays, transparent where the pass did not reach)" : "could not write " + path;
}

void saveView(const Item& it, View& v) {
    const bool map = v.mapOn && v.geo && !v.map.rgba.empty();
    const std::string name = fileStamp(it.time) + "-" + fsafe(it.sat.empty() ? it.source : it.sat) + (map ? "-map" : "") + ".png";
    if (map) { saveMapPng(v, name); return; }
    const std::string path = saveFileDialog(name.c_str());
    if (path.empty()) return;
    bool ok;
    if (v.cropOn && v.cx1 > v.cx0 && v.cy1 > v.cy0) {
        const int cw = v.cx1 - v.cx0, ch = v.cy1 - v.cy0;
        std::vector<uint8_t> px((size_t)cw * ch * 4);
        for (int y = 0; y < ch; y++) std::memcpy(&px[(size_t)y * cw * 4], &v.disp[((size_t)(y + v.cy0) * v.dw + v.cx0) * 4], (size_t)cw * 4);
        ok = gfx::writePng(path.c_str(), px.data(), cw, ch);
    } else ok = gfx::writePng(path.c_str(), v.disp.data(), v.dw, v.dh);
    v.msg = ok ? "saved " + path : "could not write " + path;
}

void saveOriginal(const Item& it) {
    const std::string path = saveFileDialog((fileStamp(it.time) + "-" + fsafe(it.source) + ".png").c_str());
    if (path.empty()) return;
    std::error_code ec;
    fs::copy_file(P(S.dir + "/" + it.id + ".png"), P(path), fs::copy_options::overwrite_existing, ec);
    S.msg = ec ? "could not write " + path : "saved " + path;
}

// the zoom and pan canvas for an image of iw x ih pixels; map: the pointer shows latitude and longitude of the shown map
void canvas(View& v, gfx::Image* tex, int iw, int ih, const MapImage* map) {
    ImGui::BeginChild("##imgview", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), sz = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##canvas", sz);
    const bool hov = ImGui::IsItemHovered();
    if (v.fit && sz.x > 8 && sz.y > 8) {
        v.zoom = std::min(sz.x / iw, sz.y / ih);
        v.pan = ImVec2((sz.x - iw * v.zoom) / 2, (sz.y - ih * v.zoom) / 2);
        v.fit = false;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (hov && io.MouseWheel != 0) {
        const float old = v.zoom;
        v.zoom = std::clamp(v.zoom * std::pow(1.15f, io.MouseWheel), 0.02f, 40.f);
        const ImVec2 m(io.MousePos.x - p0.x, io.MousePos.y - p0.y);
        v.pan = ImVec2(m.x - (m.x - v.pan.x) * v.zoom / old, m.y - (m.y - v.pan.y) * v.zoom / old);
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f)) { v.pan.x += io.MouseDelta.x; v.pan.y += io.MouseDelta.y; }
    const ImVec2 a(p0.x + v.pan.x, p0.y + v.pan.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddImage(tex->texture(), a, ImVec2(a.x + iw * v.zoom, a.y + ih * v.zoom));
    v.vis[0] = -v.pan.x / v.zoom; v.vis[1] = -v.pan.y / v.zoom; v.vis[2] = (sz.x - v.pan.x) / v.zoom; v.vis[3] = (sz.y - v.pan.y) / v.zoom;
    if (v.cropOn)
        dl->AddRect(ImVec2(a.x + v.cx0 * v.zoom, a.y + v.cy0 * v.zoom), ImVec2(a.x + v.cx1 * v.zoom, a.y + v.cy1 * v.zoom), IM_COL32(255, 220, 60, 255), 0.f, 0, 2.f);
    if (hov && map) {
        const double lon = map->west + ((io.MousePos.x - a.x) / v.zoom) * map->degPerPx, lat = map->north - ((io.MousePos.y - a.y) / v.zoom) * map->degPerPx;
        ImGui::SetTooltip("%.3f, %.3f", lat, lon);
    }
    ImGui::EndChild();
}

// graticule, coastline and border switches of a map; true when something changed
bool overlayControls(View& v) {
    bool ch = false;
    if (ImGui::Checkbox(tr("Coastlines"), &v.coast)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Simplified coastlines drawn into the map (good to about 50 - 100 km, not a survey)"));
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Borders"), &v.border)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A few of the longest country borders, simplified"));
    ImGui::SameLine();
    if (ImGui::Checkbox("Graticule", &v.grat)) ch = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60 * gUi);
    char cur[16]; snprintf(cur, sizeof cur, "%g deg", kSteps[v.gratStep]);
    if (ImGui::BeginCombo("##gstep", cur)) {
        for (int i = 0; i < 6; i++) { snprintf(cur, sizeof cur, "%g deg", kSteps[i]); if (ImGui::Selectable(cur, i == v.gratStep)) { v.gratStep = i; ch = true; } }
        ImGui::EndCombo();
    }
    return ch;
}

void cropControls(View& v, int iw, int ih) {
    if (ImGui::SmallButton(tr("Crop to view"))) setCropToView(v, iw, ih);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom to the part you want, then crop: saving and mosaic use only the part inside the yellow frame"));
    if (v.cropOn) { ImGui::SameLine(); if (ImGui::SmallButton(tr("Clear crop"))) v.cropOn = false; }
}

void histogramStrip(const View& v) {
    float h[256];
    uint32_t mx = 1;
    for (uint32_t b : v.hbins) mx = std::max(mx, b);
    for (int i = 0; i < 256; i++) h[i] = std::sqrt((float)v.hbins[(size_t)i] / (float)mx);   // square root: the small bins show too
    ImGui::PlotHistogram("##hist", h, 256, 0, nullptr, 0.f, 1.f, ImVec2(256 * gUi, 44 * gUi));
}

void addToMosaic(const Item& it, View& v) {
    MapImage m;
    if (!exportMap(v, v.mapBase, m)) { v.msg = "nothing to add"; return; }
    S.mosaic.push_back(std::move(m));
    S.mosaicNames.push_back((it.sat.empty() ? it.source : it.sat) + "  " + stamp(it.time));
    v.msg = "added to the mosaic (" + std::to_string(S.mosaic.size()) + " passes)";
}

void rebuildMosaic() {
    View& m = S.mos;
    const bool had = m.mapBase.w > 0;
    if (!mosaicMaps(S.mosaic, 3000, m.mapBase)) { m.mapBase = MapImage(); m.map = MapImage(); return; }
    m.cropOn = false; m.mapOn = true; m.geo = true;
    applyOverlays(m);
    if (!had) m.fit = true;
}

void mosaicViewer() {
    View& v = S.mos;
    if (S.mosaic.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("The mosaic is empty. Open a pass with a map, switch Map projection on and press Add to mosaic. Several passes are stitched into one map."));
        ImGui::PopTextWrapPos();
        return;
    }
    if (v.mapBase.w <= 0) rebuildMosaic();
    int drop = -1;
    for (size_t i = 0; i < S.mosaic.size(); i++) {
        ImGui::PushID((int)i);
        if (ImGui::SmallButton("x")) drop = (int)i;
        ImGui::SameLine();
        ImGui::TextUnformatted(S.mosaicNames[i].c_str());
        ImGui::PopID();
        if (i >= 5 && S.mosaic.size() > 6) { ImGui::TextDisabled("... %zu more", S.mosaic.size() - 6); break; }
    }
    if (drop >= 0) { S.mosaic.erase(S.mosaic.begin() + drop); S.mosaicNames.erase(S.mosaicNames.begin() + drop); rebuildMosaic(); if (S.mosaic.empty()) return; }
    if (overlayControls(v)) applyOverlays(v);
    cropControls(v, v.map.w, v.map.h);
    ImGui::SameLine();
    if (ImGui::SmallButton("Fit")) v.fit = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Save PNG...")) saveMapPng(v, "mosaic.png");
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Save GeoTIFF..."))) saveGeoTiffFile(v, "mosaic.tif");
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Clear mosaic"))) { S.mosaic.clear(); S.mosaicNames.clear(); v.mapBase = MapImage(); v.map = MapImage(); return; }
    if (!v.msg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", v.msg.c_str()); }
    if (v.mapTex && v.map.w > 0) canvas(v, v.mapTex, v.map.w, v.map.h, &v.map);
}

void viewer(Item& it) {
    View& v = S.v;
    if (v.id != it.id) selectItem(it);
    if (v.w <= 0) { ImGui::TextDisabled("%s", v.msg.c_str()); return; }
    bool ch = false;
    const bool multi = it.planeMask != 0;
    ImGui::PushItemWidth(150 * gUi);
    if (multi) {
        const auto& pre = falseColourPresets();
        if (ImGui::BeginCombo("##preset", pre[(size_t)v.preset].name)) {
            for (int i = 0; i < (int)pre.size(); i++) {
                const Preset& p = pre[(size_t)i];
                bool have = i == 0;
                if (i) {
                    have = true;
                    for (int c : {p.r, p.g, p.b}) if (c >= 0 && !(it.planeMask >> c & 1)) have = false;
                }
                ImGui::BeginDisabled(!have);
                if (ImGui::Selectable(p.name, v.preset == i)) { v.preset = i; ch = true; }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("False colour: which channels go to red, green and blue (channel combinations, or a thermal palette for infrared)");
        ImGui::SameLine();
    }
    ImGui::PopItemWidth();
    if (ImGui::Checkbox("Stretch", &v.stretch)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Contrast stretch: the darkest and brightest few percent of the picture are cut off and the rest spread over the full range");
    ImGui::SameLine();
    if (ImGui::Checkbox("Equalise", &v.equalise)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Histogram equalisation");
    ImGui::SameLine();
    if (ImGui::Checkbox("Invert", &v.invert)) ch = true;
    if (v.stretch) {
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::SliderFloat("##lo", &v.loPct, 0.f, 20.f, "low %.1f%%")) ch = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::SliderFloat("##hi", &v.hiPct, 80.f, 100.f, "high %.1f%%")) ch = true;
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::SliderFloat("##gamma", &v.gamma, 0.3f, 3.f, "gamma %.2f")) ch = true;
    // second row: tone curve, palette, histogram
    ImGui::SetNextItemWidth(170 * gUi);
    if (ImGui::BeginCombo("##curve", tr(curvePresets()[(size_t)v.curve].name))) {
        for (int i = 0; i < (int)curvePresets().size(); i++) if (ImGui::Selectable(tr(curvePresets()[(size_t)i].name), v.curve == i)) { v.curve = i; ch = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Tone curve applied after the gamma"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150 * gUi);
    if (ImGui::BeginCombo("##palette", tr(paletteNames()[(size_t)v.palette]))) {
        for (int i = 0; i < (int)paletteNames().size(); i++) if (ImGui::Selectable(tr(paletteNames()[(size_t)i]), v.palette == i)) { v.palette = i; ch = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Colour palette on the brightness of the picture, for infrared and other single-channel pictures"));
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Histogram"), &v.hist)) ch = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) { v.preset = 0; v.stretch = false; v.loPct = 1.f; v.hiPct = 99.f; v.gamma = 1.f; v.curve = v.palette = 0; v.equalise = v.invert = false; ch = true; }
    // the enhancement: its own row
    if (v.enh.controls("genh")) v.dirty = true;
    if (v.enh.poll()) v.dirty = true;
    if (ch) v.dirty = true;
    if (v.hist) histogramStrip(v);
    if (v.geo) {
        if (ImGui::Checkbox("Map projection", &v.mapOn)) { v.fit = true; v.mapDirty = true; v.cropOn = false; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Equirectangular reprojection of the pass from its ground track (an approximation: 2800 km swath, nadir track from the TLE at the picture time)");
        if (v.mapOn) {
            ImGui::SameLine();
            if (overlayControls(v)) v.ovDirty = true;
        }
    } else if (it.lpt == 0 && it.source.find("Meteor") != std::string::npos) {
        ImGui::TextDisabled("(no map: needs a TLE, download them in View > Passes)");
    }
    cropControls(v, v.mapOn && v.mapTex ? v.map.w : v.dw, v.mapOn && v.mapTex ? v.map.h : v.dh);
    ImGui::SameLine();
    if (ImGui::SmallButton("Fit")) v.fit = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("1:1")) { v.zoom = 1.f; v.pan = ImVec2(0, 0); }
    ImGui::SameLine();
    if (ImGui::SmallButton("Save PNG...")) saveView(it, v);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(v.mapOn ? "The map as PNG, with a .pgw world file next to it" : "The picture as shown (with the enhancements) as PNG");
    ImGui::SameLine();
    if (ImGui::SmallButton("Save original...")) saveOriginal(it);
    if (v.mapOn && v.geo) {
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Save GeoTIFF..."))) saveGeoTiffFile(v, fileStamp(it.time) + "-" + fsafe(it.sat.empty() ? it.source : it.sat) + ".tif");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The map as a GeoTIFF (WGS 84, no overlays, transparent where the pass did not reach) for GIS programs"));
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Add to mosaic"))) addToMosaic(it, v);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Keep this projected pass to stitch with others in the Mosaic view"));
    }
    if (v.dirty) render(v);
    if (v.mapOn && v.mapDirty) project(v);
    else if (v.mapOn && v.ovDirty) applyOverlays(v);
    if (!v.msg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", v.msg.c_str()); }

    gfx::Image* tex = v.mapOn && v.mapTex ? v.mapTex : v.tex;
    const int iw = v.mapOn && v.mapTex ? v.map.w : v.dw, ih = v.mapOn && v.mapTex ? v.map.h : v.dh;
    if (!tex) return;
    canvas(v, tex, iw, ih, v.mapOn ? &v.map : nullptr);
}

void ensureThumb(Item& it) {
    if (it.thumb || it.thumbTried || !gGfx) return;
    it.thumbTried = true;
    std::vector<uint8_t> rgba; int w, h;
    if (!readPng(S.dir + "/" + it.id + "_t.png", w, h, rgba)) return;
    it.thumb = gGfx->createImage(w, h, 0xFF000000u);
    if (it.thumb) it.thumb->update(0, 0, w, h, reinterpret_cast<const uint32_t*>(rgba.data()));
    it.thumbW = w; it.thumbH = h;
}

std::string sizeText(uint64_t b) {
    char s[32];
    if (b >= 1024ull * 1024 * 1024) snprintf(s, sizeof s, "%.1f GB", b / 1073741824.0); else snprintf(s, sizeof s, "%.1f MB", b / 1048576.0);
    return s;
}

int gDev = 0;   // dev: 1 the first picture with its map, histogram and a curve; 2 the mosaic of all pictures (set by galleryDev, run on the first frame)

void runDev() {
    if (!gDev || S.items.empty()) return;
    const int mode = gDev;
    gDev = 0;
    if (mode == 2) {
        for (Item& it : S.items) {
            selectItem(it);
            View& v = S.v;
            if (v.w <= 0 || !v.geo) continue;
            v.mapOn = true;
            render(v);
            project(v);
            addToMosaic(it, v);
        }
        S.showMosaic = true;
        rebuildMosaic();
        return;
    }
    selectItem(S.items[0]);
    View& v = S.v;
    v.hist = true; v.curve = 1; v.mapOn = true; v.border = true; v.stretch = true;
}

void windowPanel(App& a) {
    load();
    runDev();
    firstWindowSize(960 * gUi, 620 * gUi);
    ImGui::SetNextWindowPos(ImVec2(260 * gUi, 60 * gUi), ImGuiCond_FirstUseEver);
    const auto dropIds = [&](const std::vector<std::string>& ids) {
        for (const std::string& id : ids) for (size_t i = 0; i < S.items.size(); i++) if (S.items[i].id == id) { dropItemAt(i); break; }
        if (!ids.empty()) saveIndex();
    };
    dropIds(gPend.expire(ImGui::GetTime(), kUndoSec));
    if (!ImGui::Begin(trw("Images"), &a.galleryOpen)) { ImGui::End(); dropIds(gPend.flush()); return; }
    if (S.sel.empty()) for (Item& it : S.items) if (!gPend.hidden(it.id)) { selectItem(it); break; }
    uint64_t tot = 0; size_t shown = 0;
    for (const Item& it : S.items) if (!gPend.hidden(it.id)) { tot += it.bytes; shown++; }
    ImGui::Text("%zu pictures, %s", shown, sizeText(tot).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Open folder")) dect2::openUrl(S.dir);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    char cap[32]; snprintf(cap, sizeof cap, "keep %ld MB", S.capMb);
    if (ImGui::BeginCombo("##cap", cap)) {
        for (long c : kCaps) { snprintf(cap, sizeof cap, "%ld MB", c); if (ImGui::Selectable(cap, c == S.capMb)) { S.capMb = c; plat::prefs().setI("galleryCapMb", c); enforceCap(); saveIndex(); } }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The oldest pictures are deleted when the folder gets bigger than this");
    ImGui::SameLine();
    char mt[48]; snprintf(mt, sizeof mt, "%s (%zu)##mosaic", tr("Mosaic"), S.mosaic.size());
    if (ImGui::Selectable(mt, S.showMosaic, 0, ImVec2(80 * gUi, 0))) S.showMosaic = !S.showMosaic;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Stitch several projected passes into one map"));
    if (!S.msg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.msg.c_str()); }
    picui::settingRow();
    picui::statusLine();
    if (gPend.canUndo()) {
        ImGui::TextDisabled("%s", tr("Picture deleted."));
        ImGui::SameLine(0, 6 * gUi);
        if (ImGui::SmallButton(tr("Undo##picdel"))) { const std::string id = gPend.undo(); for (Item& it : S.items) if (it.id == id) { selectItem(it); break; } }
    }
    ImGui::Separator();
    const float lw = 300 * gUi;
    ImGui::BeginChild("##glist", ImVec2(lw, 0), ImGuiChildFlags_Borders);
    if (shown == 0) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("No pictures yet. Weather satellites (Meteor, GOES), weather fax, SSTV and HamDRM pictures appear here as they finish."); ImGui::PopTextWrapPos(); }
    int del = -1;
    int made = 0;
    for (size_t i = 0; i < S.items.size(); i++) {
        Item& it = S.items[i];
        if (gPend.hidden(it.id)) continue;
        ImGui::PushID((int)i);
        if (!it.thumb && !it.thumbTried && made < 2) { ensureThumb(it); made++; }
        const float th = 64 * gUi;
        const ImVec2 c0 = ImGui::GetCursorScreenPos();
        const bool selected = S.sel == it.id;
        ImGui::SetNextItemAllowOverlap();   // the save and delete icons sit on top of the row: without this the row takes their clicks
        if (ImGui::Selectable("##row", selected, 0, ImVec2(0, th + 4))) selectItem(it);
        const ImVec2 c1 = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (it.thumb) {
            const float s = std::min(th / it.thumbW, th / it.thumbH);
            dl->AddImage(it.thumb->texture(), ImVec2(c0.x + 2, c0.y + 2), ImVec2(c0.x + 2 + it.thumbW * s, c0.y + 2 + it.thumbH * s));
        } else dl->AddRectFilled(ImVec2(c0.x + 2, c0.y + 2), ImVec2(c0.x + 2 + th, c0.y + 2 + th), IM_COL32(40, 40, 40, 255));
        const float tx = c0.x + th + 8;
        const ImU32 fg = ImGui::GetColorU32(ImGuiCol_Text), dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        const float lh = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(tx, c0.y + 2), fg, it.source.c_str());
        dl->AddText(ImVec2(tx, c0.y + 2 + lh), dim, (it.sat.empty() ? it.detail : it.sat + (it.detail.empty() ? "" : "  " + it.detail)).c_str());
        char l3[96]; snprintf(l3, sizeof l3, "%s  %d x %d", stamp(it.time).c_str(), it.w, it.h);
        dl->AddText(ImVec2(tx, c0.y + 2 + 2 * lh), dim, l3);
        (void)c1;
        ImGui::SetCursorScreenPos(ImVec2(c0.x + lw - 2 * (ImGui::GetFrameHeight() + 6) - 12, c0.y + th - ImGui::GetFrameHeight() + 2));
        {
            const std::string mode = it.sat.empty() ? it.source : it.sat;
            const std::string tip = picsave::tooltipText(picui::folder(), mode);   // says what and where
            if (iconFlat(Ic::Save, tip.c_str())) picui::saveCopy(mode.c_str(), (time_t)it.time, S.dir + "/" + it.id + ".png");
        }
        ImGui::SameLine(0, 4);
        if (iconFlat(Ic::Cross, tr("Delete this picture (Undo for a few seconds)"))) del = (int)i;
        ImGui::SetCursorScreenPos(ImVec2(c0.x, c0.y + th + 4 + ImGui::GetStyle().ItemSpacing.y));
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::PopID();
    }
    if (del >= 0) {
        gPend.mark(S.items[(size_t)del].id, ImGui::GetTime());
        if (S.sel == S.items[(size_t)del].id) S.sel.clear();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##gview", ImVec2(0, 0));
    Item* cur = nullptr;
    for (Item& it : S.items) if (it.id == S.sel) cur = &it;
    if (S.showMosaic) mosaicViewer();
    else if (cur) viewer(*cur);
    else ImGui::TextDisabled("Choose a picture on the left.");
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace

void galleryDev(int mode) { gDev = mode; }

void galleryWindow(App& a) {
    { static double last = 0; const double t = ImGui::GetTime(); if (t - last > 2) { last = t; gallery::emitDone(); } }   // image.done for finished pictures
    if (a.galleryOpen) windowPanel(a);
}
