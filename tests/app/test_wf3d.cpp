// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The 3D waterfall (app/gfx/wf3d.h, wf3d_draw.h): projection and its inverse, the camera, the decimation and the budget, the 3D history,
// the painter's order, the mesh chunks, the ray pick, and the picture itself: the draw list that the app hands to the GPU is rasterised
// here in software (in the order the GPU draws it) and checked by pixel. With an argument the picture is also written there as a PNG.
// Last, the CPU time per frame on a synthetic history of 16384 bins x 512 rows.
#include "../../app/gfx/wf3d_draw.h"
#include "../../app/gfx/colormap.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

static int fails = 0;
static void check(bool ok, const char* what) { if (!ok) { printf("FAIL: %s\n", what); fails++; } }

using namespace w3d;

// ------------------------------------------------------------------------------------------------ a software rasteriser and PNG writer
struct Canvas {
    int w, h;
    std::vector<uint32_t> px;
    Canvas(int W, int H, uint32_t bg) : w(W), h(H), px((size_t)W * H, bg) {}
    uint32_t at(int x, int y) const { return px[(size_t)std::min(h - 1, std::max(0, y)) * w + std::min(w - 1, std::max(0, x))]; }
    // a filled triangle (pixel centres inside, either winding), each pixel the colour of its nearest corner (the GPU blends them; the
    // checks compare exact palette colours)
    void tri(const ImDrawVert& va, const ImDrawVert& vb, const ImDrawVert& vc) {
        const ImVec2 a = va.pos, b = vb.pos, c = vc.pos;
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-9f) return;
        const int x0 = std::max(0, (int)std::floor(std::min({a.x, b.x, c.x}))), x1 = std::min(w - 1, (int)std::ceil(std::max({a.x, b.x, c.x})));
        const int y0 = std::max(0, (int)std::floor(std::min({a.y, b.y, c.y}))), y1 = std::min(h - 1, (int)std::ceil(std::max({a.y, b.y, c.y})));
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                const float px_ = x + 0.5f, py = y + 0.5f;
                const float e0 = (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px_ - a.x);
                const float e1 = (c.x - b.x) * (py - b.y) - (c.y - b.y) * (px_ - b.x);
                const float e2 = (a.x - c.x) * (py - c.y) - (a.y - c.y) * (px_ - c.x);
                if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)) {
                    const float wa = std::fabs(e1), wb = std::fabs(e2), wc = std::fabs(e0);   // e1 weighs a, e2 b, e0 c
                    px[(size_t)y * w + x] = wa >= wb && wa >= wc ? va.col : wb >= wc ? vb.col : vc.col;
                }
            }
    }
    void draw(const ImDrawList& d) {
        for (const ImDrawCmd& c : d.CmdBuffer)
            for (unsigned i = 0; i + 2 < c.ElemCount; i += 3) {
                const ImDrawVert& a = d.VtxBuffer[(int)c.VtxOffset + d.IdxBuffer[(int)(c.IdxOffset + i)]];
                const ImDrawVert& b = d.VtxBuffer[(int)c.VtxOffset + d.IdxBuffer[(int)(c.IdxOffset + i + 1)]];
                const ImDrawVert& e = d.VtxBuffer[(int)c.VtxOffset + d.IdxBuffer[(int)(c.IdxOffset + i + 2)]];
                tri(a, b, e);
            }
    }
};
static uint32_t crcT[256];
static uint32_t crc(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) { for (size_t i = 0; i < n; i++) c = crcT[(c ^ p[i]) & 255] ^ (c >> 8); return c; }
static void be32(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x); }
static void writePng(const char* path, const Canvas& cv) {   // stored deflate blocks: no zlib needed
    for (uint32_t n = 0; n < 256; n++) { uint32_t c = n; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; crcT[n] = c; }
    FILE* f = fopen(path, "wb");
    if (!f) return;
    auto chunk = [&](const char* t, const std::vector<uint8_t>& d) {
        std::vector<uint8_t> b; be32(b, (uint32_t)d.size()); b.insert(b.end(), t, t + 4); b.insert(b.end(), d.begin(), d.end());
        be32(b, crc(b.data() + 4, b.size() - 4) ^ 0xFFFFFFFFu); fwrite(b.data(), 1, b.size(), f);
    };
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ih; be32(ih, cv.w); be32(ih, cv.h); ih.push_back(8); ih.push_back(6); ih.push_back(0); ih.push_back(0); ih.push_back(0);
    chunk("IHDR", ih);
    std::vector<uint8_t> raw;
    for (int y = 0; y < cv.h; y++) { raw.push_back(0); for (int x = 0; x < cv.w; x++) { const uint32_t c = cv.px[(size_t)y * cv.w + x]; raw.push_back(c & 255); raw.push_back((c >> 8) & 255); raw.push_back((c >> 16) & 255); raw.push_back(255); } }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    for (size_t p = 0; p < raw.size(); p += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - p);
        z.push_back(p + n >= raw.size() ? 1 : 0); z.push_back(n & 255); z.push_back(n >> 8); z.push_back(~n & 255); z.push_back((~n >> 8) & 255);
        z.insert(z.end(), raw.begin() + (long)p, raw.begin() + (long)(p + n));
    }
    be32(z, (b << 16) | a);
    chunk("IDAT", z); chunk("IEND", {});
    fclose(f);
}

static uint32_t lut[256];
static void makeLut() { for (int i = 0; i < 256; i++) lut[i] = cmap::color(cmap::Viridis, i / 255.f); }

// a history of `rows` x `cols` levels: noise around 40, plus carriers
static void synth(std::vector<uint8_t>& lv, int cols, int rows, unsigned seed) {
    std::mt19937 rng(seed);
    lv.resize((size_t)cols * rows);
    for (size_t i = 0; i < lv.size(); i++) lv[i] = (uint8_t)(30 + rng() % 25);
    for (int r = 0; r < rows; r++) {
        uint8_t* o = lv.data() + (size_t)r * cols;
        for (int c = cols / 4 - 40; c < cols / 4 + 40; c++) o[c] = 200;                  // a wide signal
        o[cols / 2] = 250;                                                              // a one-bin carrier
        if ((r / 32) % 2 == 0) for (int c = 3 * cols / 4; c < 3 * cols / 4 + 8; c++) o[c] = 180;   // a bursty one
    }
}

static ImDrawListSharedData* gSd = nullptr;
static void drawInto(ImDrawList& d, const Mesh& m) { d._ResetForNewFrame(); d.PushClipRectFullScreen(); drawMesh(&d, m, 0, 0); }

static double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int main(int argc, char** argv) {
    ImGui::CreateContext();
    ImDrawListSharedData sd;
    sd.InitialFlags = ImDrawListFlags_AllowVtxOffset;
    sd.TexUvWhitePixel = ImVec2(0.5f, 0.5f);
    gSd = &sd;
    makeLut();

    // ---- projection: the target is the middle of the screen; the ray through a projected point goes through that point
    {
        Camera c; Proj p = makeProj(c, 10, 20, 800, 400);
        float x, y, d;
        check(project(p, v3(c.tx, c.ty, c.tz), x, y, d) && std::fabs(x - 410) < 0.01f && std::fabs(y - 220) < 0.01f, "target at the centre");
        check(std::fabs(d - c.dist) < 1e-4f, "depth of the target is the distance");
        std::mt19937 rng(1);
        float worst = 0;
        for (int i = 0; i < 200; i++) {
            const V3 w = v3((rng() % 1000) / 1000.f - 0.5f, (rng() % 1000) / 2000.f, (rng() % 1000) / 1000.f - 0.5f);
            if (!project(p, w, x, y, d)) continue;
            V3 o, dir; screenRay(p, x, y, o, dir);
            const V3 q = o + dir * dot(w - o, dir);   // the closest point of the ray
            worst = std::max(worst, std::sqrt(dot(q - w, q - w)));
        }
        check(worst < 1e-4f, "screenRay inverts project");
        Camera f = preset(Preset_Front);
        Proj pf = makeProj(f, 0, 0, 800, 400);
        float xl, xr, yn, yo;
        project(pf, v3(-0.5f, 0, 0), xl, y, d); project(pf, v3(0.5f, 0, 0), xr, y, d);
        check(xl < 400 && xr > 400, "front view: low frequencies on the left");
        project(pf, v3(0, 0, 0.5f), x, yn, d); project(pf, v3(0, 0, -0.5f), x, yo, d);
        check(yn > yo, "front view: the newest row is lower on the screen (nearer)");
        project(pf, v3(0, 0.3f, 0), x, y, d);
        check(y < 200, "up is up");
        check(!project(pf, f.eye() + (f.eye() - v3(f.tx, f.ty, f.tz)), x, y, d), "behind the eye is not projected");
        Proj pt = makeProj(preset(Preset_Top), 0, 0, 800, 400);   // straight down: still a basis
        check(project(pt, v3(0.4f, 0, 0), x, y, d) && x > 400, "top view: frequency still left to right");
    }
    // ---- framing: at the preset distance the surface's box fills 90% of the tighter direction and stays inside, for any plot shape
    for (int pr = 0; pr < NumPresets; pr++)
        for (float aspect : {0.6f, 1.f, 3.7f, 6.f}) {
            const float vh = 200, vw = vh * aspect;
            Camera c = preset(pr); c.dist = Camera().dist; c.tx = c.tz = 0; c.ty = Camera().ty;
            const Proj p = makeProj(c, 0, 0, vw, vh, fitScale(c, vw, vh, 0.35f, 1.f));
            float mx = 0, my = 0, ax = 1e9f, bx = -1e9f, ay = 1e9f, by = -1e9f;
            for (int i = 0; i < 8; i++) {
                float x, y, d;
                if (!project(p, v3(i & 1 ? 0.5f : -0.5f, i & 2 ? 0.35f : 0.f, i & 4 ? 0.5f : -0.5f), x, y, d)) continue;
                mx = std::max(mx, std::fabs(x - vw / 2) / (vw / 2)); my = std::max(my, std::fabs(y - vh / 2) / (vh / 2));
                ax = std::min(ax, x); bx = std::max(bx, x); ay = std::min(ay, y); by = std::max(by, y);
            }
            const bool centred = std::fabs((ax + bx) / 2 - vw / 2) < 0.01f * vw && std::fabs((ay + by) / 2 - vh / 2) < 0.01f * vh;
            char msg[80]; snprintf(msg, sizeof msg, "fit %s aspect %.1f: %.2f / %.2f", presetName(pr), aspect, mx, my);
            check(centred && std::max(mx, my) > 0.88f && std::max(mx, my) < 0.92f, msg);
        }
    // ---- camera limits
    {
        Camera c; c.orbit(0, -100000); check(c.pitch >= 0.03f, "pitch floor");
        c.orbit(0, 100000); check(c.pitch <= 1.553f, "pitch ceiling");
        c.zoom(1000); check(c.dist >= 0.35f, "zoom in limit"); c.zoom(-1000); check(c.dist <= 8.f, "zoom out limit");
        c.orbit(1e6f, 0); check(c.yaw >= -3.1416f && c.yaw <= 3.1416f, "yaw wraps");
        Camera q; const float tx0 = q.tx; q.pan(100, 0, 400); check(q.tx != tx0, "pan moves the target");
        for (int i = 0; i < NumPresets; i++) { Camera p = preset(i); Camera k = p; k.clamp(); check(p.pitch == k.pitch && p.dist == k.dist, "presets inside the limits"); }
    }
    // ---- frequency mapping: the inverse of each other, and a grid column sits on the centre of the history columns it covers
    {
        const double lo = 100 - 1.0, fs = 2.0;   // imageLo of a 2 MHz band centred on 100 MHz (half-bin shift ignored here)
        for (int cols : {2, 37, 500})
            for (double u : {0.0, 0.25, 0.5, 1.0}) {
                const double f = uToMhz(u, cols, 1000, 3000, 8192, lo, fs);
                check(std::fabs(mhzToU(f, cols, 1000, 3000, 8192, lo, fs) - u) < 1e-9, "uToMhz / mhzToU round trip");
            }
        // 8192 columns, all of them on 8192 grid columns: column k is at imgLo + fs (k + 0.5) / W
        check(std::fabs(uToMhz(0, 8192, 0, 8192, 8192, lo, fs) - (lo + fs * 0.5 / 8192)) < 1e-12, "column 0 centre");
        check(std::fabs(uToMhz(1, 8192, 0, 8192, 8192, lo, fs) - (lo + fs * 8191.5 / 8192)) < 1e-12, "last column centre");
        Camera c; const float d0 = c.dist;
        applyGesture(c, true, 2.f, 0, 0, false, 400); check(std::fabs(c.dist - d0 / 2) < 1e-5f, "pinch apart: closer");
        const float y0 = c.yaw; applyGesture(c, false, 1, 50, 0, true, 400); check(c.yaw != y0, "one finger orbits");
    }
    // ---- decimation and the budget
    {
        std::vector<uint8_t> s(16384, 10), o(500);
        s[12345] = 240;
        decimateMax(s.data(), 16384, 500, o.data());
        int peaks = 0; for (uint8_t v : o) peaks += v == 240;
        check(peaks == 1 && o[(size_t)12345 * 500 / 16384] == 240, "a one-bin carrier survives 16384 -> 500");
        std::vector<uint8_t> u(8), t2(16);
        for (int i = 0; i < 8; i++) u[i] = (uint8_t)i;
        decimateMax(u.data(), 8, 16, t2.data()); check(t2[15] == 7 && t2[0] == 0, "fewer source columns: nearest");
        Budget b = budget(16384, 2000, 1024, false);
        check(b.cols <= 768 && (long long)b.cols * b.rows <= 160000 && b.rows * b.group >= 1024, "budget: big plot, many rows");
        Budget l = budget(16384, 2000, 1024, true);
        check((long long)l.cols * l.rows <= 40000 && l.cols < b.cols, "budget: low power is smaller");
        Budget n = budget(40, 2000, 64, false);
        check(n.cols == 40 && n.rows == 64 && n.group == 1, "budget: narrow span keeps its bins");
        Budget sm = budget(16384, 120, 256, false);
        check(sm.cols == 40, "budget: a column per 3 pixels");
    }
    // ---- the 3D history: rebuild and incremental give the same rows; the ring keeps cap rows beyond the main history's 512
    {
        const int W = 2048, H = 512;
        std::vector<uint8_t> lv((size_t)W * H);
        std::mt19937 rng(5);
        for (auto& v : lv) v = (uint8_t)(rng() % 256);
        State a, b;
        // the main history: newest at wr, the rows written so far R + 1
        int wr = 0; long long R = 99;
        check(ingest(a, lv.data(), W, H, wr, R, 100, 0, W, 256, 1024), "first ingest");
        check(a.hist.have() == 100, "first ingest takes the rows there are");
        check(!ingest(a, lv.data(), W, H, wr, R, 100, 0, W, 256, 1024), "no new row: nothing to do");
        for (int k = 0; k < 700; k++) { wr = (wr + H - 1) % H; R++; ingest(a, lv.data(), W, H, wr, R, std::min<long long>(H, R + 1), 0, W, 256, 1024); }
        check(a.hist.have() == 800, "keeps more than the main history's rows");
        ingest(b, lv.data(), W, H, wr, R, H, 0, W, 256, 1024);
        bool same = b.hist.have() == H;
        for (int age = 0; age < H && same; age++) same = std::memcmp(a.hist.row(age), b.hist.row(age), 256) == 0;
        check(same, "incremental == rebuilt (newest 512 rows)");
        check(ingest(b, lv.data(), W, H, wr, R, H, 100, 900, 256, 1024) && b.hist.have() == H, "a zoom remakes the history");
    }
    // ---- grid: groups take the highest, smoothing keeps flat areas flat, peak hold
    {
        History h; h.reset(4, 16);
        for (int r = 0; r < 16; r++) { uint8_t row[4] = {(uint8_t)r, 100, 0, 0}; if (r == 5) row[2] = 255; h.push(row); }
        Grid g; std::vector<float> tmp;
        buildGrid(h, 4, 4, false, g, tmp);
        check(g.lv[0] == 15 && g.lv[4] == 11 && g.lv[2 * 4 + 2] == 255, "group max (age 10 = row 5 in the third group)");
        check(std::fabs(g.peak[2] - 1.f) < 1e-6f && std::fabs(g.peak[1] - 100 / 255.f) < 1e-6f, "peak hold");
        buildGrid(h, 4, 4, true, g, tmp);
        check(std::fabs(g.at(1, 1) - 100 / 255.f) < 0.15f, "smoothing keeps a flat column near its level");
        check(g.at(2, 2) < 1.f && g.at(2, 2) > 0.2f, "smoothing softens a spike");
        History y; y.reset(4, 16); uint8_t r0[4] = {9, 9, 9, 9}; y.push(r0);
        buildGrid(y, 8, 1, false, g, tmp);
        check(g.lv[0] == 9 && g.lv[4] == 0, "a young history: missing rows are the floor");
    }
    // ---- painter's order: every strip once, the farthest first
    {
        std::vector<int> o;
        farToNear(10, 4.5f, o);
        check(o.size() == 9 && o.front() == 0 && o.back() == 4, "eye over strip 4: starts at an end, ends under the eye");
        farToNear(10, -3, o); check(o.size() == 9 && o.front() == 8 && o.back() == 0, "eye before the first line");
        farToNear(10, 30, o); check(o.size() == 9 && o.front() == 0 && o.back() == 8, "eye past the last line");
    }
    // ---- mesh: chunks fit 16-bit indices, all quads present when everything is in view
    {
        History h; h.reset(700, 256);
        std::vector<uint8_t> row(700, 50);
        for (int r = 0; r < 256; r++) h.push(row.data());
        Grid g; std::vector<float> tmp; buildGrid(h, 256, 1, false, g, tmp);
        for (int pr = 0; pr < NumPresets; pr++) {
            Mesh m; Look lk; Proj p = makeProj(preset(pr), 0, 0, 1200, 600);
            buildMesh(g, p, lk, lut, m);
            bool small = true; size_t ni = 0;
            for (const Chunk& c : m.chunks) { small = small && c.nv < 65536; ni += (size_t)c.ni; for (int i = 0; i < c.ni; i++) small = small && m.idx[(size_t)c.i0 + i] < c.nv; }
            char msg[80]; snprintf(msg, sizeof msg, "preset %s: chunks < 65536 vertices, indices inside", presetName(pr));
            check(small && ni == m.idx.size(), msg);
            snprintf(msg, sizeof msg, "preset %s: every quad drawn (%d of %d)", presetName(pr), m.quads, 699 * 255);
            check(pr == Preset_Low || m.quads == 699 * 255, msg);
        }
    }
    // ---- the picture: a front wall (newest rows) and a taller back wall; drawn as the GPU would, the front one hides the back one where they overlap
    const char* png = argc > 1 ? argv[1] : nullptr;
    {
        const int C = 200, Rr = 128;
        History h; h.reset(C, Rr);
        for (int r = 0; r < Rr; r++) {
            std::vector<uint8_t> row(C, 20);
            const int age = Rr - 1 - r;   // pushed oldest first
            if (age < 6) for (int c = 0; c < C; c++) row[c] = 128;            // the front wall: middle level
            if (age > Rr - 8) for (int c = 0; c < C; c++) row[c] = 250;       // the back wall: near the top
            if (age > 40 && age < 50) row[C / 2] = 240;                       // a spike in the middle
            h.push(row.data());
        }
        Grid g; std::vector<float> tmp; buildGrid(h, Rr, 1, false, g, tmp);
        Look lk; lk.shade = false; lk.heightScale = 0.5f;
        Camera cam = preset(Preset_Front); cam.pitch = 0.25f;
        const int Wpx = 640, Hpx = 360;
        Proj p = makeProj(cam, 0, 0, (float)Wpx, (float)Hpx);
        Mesh m; buildMesh(g, p, lk, lut, m);
        ImDrawList d(&sd); drawInto(d, m);
        const uint32_t bg = 0xFF201010u;
        Canvas cv(Wpx, Hpx, bg);
        cv.draw(d);
        // ridge drawn after (the back edge is far: it is drawn first in the app; here only its position is checked)
        std::vector<float> xy; peakLine(g, p, lk, false, xy);
        if (png) writePng(png, cv);
        size_t covered = 0; for (uint32_t c : cv.px) covered += c != bg;
        check(covered > (size_t)Wpx * Hpx / 10 && covered < (size_t)Wpx * Hpx, "the surface covers part of the picture");
        float x, y, dd;
        project(p, gridPoint(g, lk, 2, C / 2 + 20, g.at(2, C / 2 + 20)), x, y, dd);   // on top of the front wall
        const uint32_t frontCol = lut[128], backCol = lut[250];
        check(cv.at((int)x, (int)y + 2) == frontCol, "the front wall is in front");
        // the back wall's top shows above the front one
        project(p, gridPoint(g, lk, Rr - 3, C / 2 + 20, g.at(Rr - 3, C / 2 + 20)), x, y, dd);
        bool back = false;
        for (int yy = (int)y - 2; yy <= (int)y + 3; yy++) back = back || cv.at((int)x, yy) == backCol;
        check(back, "the back wall's top is visible above it");
        // the one-column spike: a sliver that tapers to its top (no wider than a column), so it is looked for half way up, where it is a pixel wide
        float bx, by;
        project(p, gridPoint(g, lk, 41, C / 2, g.at(41, C / 2)), x, y, dd);
        project(p, gridPoint(g, lk, 41, C / 2 - 1, g.at(41, C / 2 - 1)), bx, by, dd);
        bool spike = false;
        const int ym = (int)((y + by) / 2);
        for (int yy = ym - 2; yy <= ym + 2 && !spike; yy++) for (int xx = (int)x - 2; xx <= (int)x + 2 && !spike; xx++) spike = cv.at(xx, yy) == lut[240];
        check(spike, "the spike is drawn where it projects");
        check(xy.size() == (size_t)2 * C && xy[1] < Hpx / 2.f, "peak ridge along the back, high on the screen");
        // pick: through the projected middle of the floor between the walls comes back to that point
        project(p, gridPoint(g, lk, 30, 60, g.at(30, 60)), x, y, dd);   // low camera: that floor is behind the front wall
        Hit hit = pick(g, p, lk, x, y);
        check(hit.ok && hit.v < 7.f / (Rr - 1) && std::fabs(hit.u - 60.f / (C - 1)) < 0.05f, "pick behind the front wall hits the wall");
        check(cv.at((int)x, (int)y) == frontCol || cv.at((int)x, (int)y) == lut[20], "and the picture shows the wall's back slope there too");
        const Proj pi = makeProj(preset(Preset_Iso), 0, 0, (float)Wpx, (float)Hpx);
        project(pi, gridPoint(g, lk, 80, 60, g.at(80, 60)), x, y, dd);
        hit = pick(g, pi, lk, x, y);
        check(hit.ok && std::fabs(hit.u - 60.f / (C - 1)) < 0.01f && std::fabs(hit.v - 80.f / (Rr - 1)) < 0.02f, "pick finds the floor point");
        project(p, gridPoint(g, lk, 2, 150, g.at(2, 150)), x, y, dd);
        hit = pick(g, p, lk, x, y + 1);
        check(hit.ok && std::fabs(hit.u - 150.f / (C - 1)) < 0.01f && hit.v < 0.06f, "pick finds the front wall");
        check(!pick(g, p, lk, 2, 2).ok, "pick above the surface: nothing");
        // from the top the pick lands where the flat waterfall would
        Proj pt = makeProj(preset(Preset_Top), 0, 0, (float)Wpx, (float)Hpx);
        project(pt, gridPoint(g, lk, 64, 100, g.at(64, 100)), x, y, dd);
        hit = pick(g, pt, lk, x, y);
        check(hit.ok && std::fabs(hit.u - 100.f / (C - 1)) < 0.01f && std::fabs(hit.v - 64.f / (Rr - 1)) < 0.02f, "pick from the top");
        // a smoothed, shaded grid still picks
        Grid gs; buildGrid(h, Rr, 1, true, gs, tmp);
        Look ls; ls.heightScale = 0.5f;
        project(pi, gridPoint(gs, ls, 80, 60, gs.at(80, 60)), x, y, dd);
        hit = pick(gs, pi, ls, x, y);
        check(hit.ok && std::fabs(hit.u - 60.f / (C - 1)) < 0.015f, "pick on the smoothed surface");
    }
    // ---- performance: a synthetic history of 16384 bins x 512 rows, a 1600 x 700 pixel plot (a MacBook at 2x: 800 x 350 points)
    {
        const int W = 16384, H = 512;
        std::vector<uint8_t> lv; synth(lv, W, H, 9);
        const Budget b = budget(W, 1600, 512, false), bl = budget(W, 1600, 512, true);
        State s;
        double t0 = nowMs();
        ingest(s, lv.data(), W, H, 0, H - 1, H, 0, W, b.cols, 1024);
        const double tRebuild = nowMs() - t0;
        // new rows: each one decimated in; the grid and mesh made again; the copy into the draw list each frame
        const int N = 60;
        double tIn = 0, tGrid = 0, tMesh = 0, tDraw = 0, tPick = 0;
        int wr = 0; long long R = H - 1;
        Look lk; Camera cam;
        ImDrawList d(&sd);
        size_t verts = 0, idx = 0;
        for (int i = 0; i < N; i++) {
            wr = (wr + H - 1) % H; R++;
            double t = nowMs(); ingest(s, lv.data(), W, H, wr, R, H, 0, W, b.cols, 1024); tIn += nowMs() - t;
            t = nowMs(); buildGrid(s.hist, b.rows, b.group, i % 2 == 1, s.grid, s.tmp); tGrid += nowMs() - t;
            cam.yaw += 0.01f;
            Proj p = makeProj(cam, 0, 0, 1600, 700);
            t = nowMs(); buildMesh(s.grid, p, lk, lut, s.mesh); peakLine(s.grid, p, lk, false, s.ridge); tMesh += nowMs() - t;
            t = nowMs(); drawInto(d, s.mesh); drawRidge(&d, s.ridge, 0, 0, IM_COL32(255, 255, 255, 200), 1.5f); tDraw += nowMs() - t;
            t = nowMs(); pick(s.grid, p, lk, 800, 400); tPick += nowMs() - t;
            verts = (size_t)d.VtxBuffer.Size; idx = (size_t)d.IdxBuffer.Size;
        }
        // low power
        State sl; ingest(sl, lv.data(), W, H, 0, H - 1, H, 0, W, bl.cols, 1024);
        buildGrid(sl.hist, bl.rows, bl.group, false, sl.grid, sl.tmp);
        double tl = nowMs();
        for (int i = 0; i < N; i++) { Proj p = makeProj(cam, 0, 0, 1600, 700); buildMesh(sl.grid, p, lk, lut, sl.mesh); drawInto(d, sl.mesh); }
        tl = (nowMs() - tl) / N;
        const double rowFrame = (tIn + tGrid + tMesh + tDraw) / N, idleFrame = tDraw / N;
        printf("perf (16384 x 512 history, 1600 x 700 px plot):\n");
        printf("  mesh %d cols x %d rows (group %d): %zu vertices, %zu indices in the draw list\n", b.cols, b.rows, b.group, verts, idx);
        printf("  3D history rebuilt from the main one (zoom or resize): %.2f ms once\n", tRebuild);
        printf("  per new row: ingest %.3f + grid %.3f + mesh %.3f ms; per frame: draw-list copy %.3f ms; pick %.3f ms\n", tIn / N, tGrid / N, tMesh / N, tDraw / N, tPick / N);
        printf("  frame with a new row (or a camera move): %.2f ms; frame without: %.2f ms\n", rowFrame, idleFrame);
        printf("  at 30 rows/s and 60 fps: %.1f ms of CPU a second (%.1f%% of one core)\n", 30 * rowFrame + 30 * idleFrame, (30 * rowFrame + 30 * idleFrame) / 10.0);
        printf("  low power (%d x %d): mesh + copy %.2f ms a frame\n", bl.cols, bl.rows, tl);
        check(rowFrame < 50, "a frame with a new row costs less than 50 ms even unoptimised");
    }
    ImGui::DestroyContext();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
