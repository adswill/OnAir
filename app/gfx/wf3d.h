// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The 3D waterfall (as in SDRangel and SDRoxide): the spectrum history as a height field seen through a perspective camera that orbits,
// pans and zooms. Pure (no ImGui, no GPU): the camera and projection, the decimation of the history to the pixel budget, the mesh in
// screen space in painter's order (far quads first, so no depth buffer is needed), the peak-hold ridge and the ray pick that turns a
// click on the surface into a frequency. Drawn by wf3d_draw.h (ImDrawList) from app/gfx/wf3d_plot.cpp; tested in tests/app/test_wf3d.cpp.
//
// World: x in [-0.5, 0.5] is the frequency (low to high), z in [-depth/2, depth/2] the time (the newest row at the front, +z), y is up:
// the level (0..1) times the height scale.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace w3d {

struct V3 { float x = 0, y = 0, z = 0; };
inline V3 v3(float x, float y, float z) { V3 v; v.x = x; v.y = y; v.z = z; return v; }
inline V3 operator+(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline V3 operator-(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline V3 operator*(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline V3 norm(V3 a) { const float l = std::sqrt(dot(a, a)); return l > 0 ? a * (1.f / l) : a; }

// ------------------------------------------------------------------------------------------------ camera
struct Camera {
    float yaw = 0.45f;     // radians about y: 0 looks from the front (the newest rows) towards the oldest
    float pitch = 0.62f;   // radians above the horizon
    float dist = 2.1f;     // from the target
    float fov = 0.75f;     // vertical field of view, radians
    float tx = 0, ty = 0.08f, tz = 0;   // the target (pan)
    void clamp() {
        const float pi = 3.14159265f;
        if (!(yaw == yaw)) yaw = 0;
        while (yaw > pi) yaw -= 2 * pi;
        while (yaw < -pi) yaw += 2 * pi;
        pitch = std::min(1.553f, std::max(0.03f, pitch));
        dist = std::min(8.f, std::max(0.35f, dist));
        tx = std::min(1.f, std::max(-1.f, tx)); ty = std::min(1.f, std::max(-0.5f, ty)); tz = std::min(1.f, std::max(-1.f, tz));
    }
    V3 eye() const {
        const float cp = std::cos(pitch);
        return v3(tx + dist * cp * std::sin(yaw), ty + dist * std::sin(pitch), tz + dist * cp * std::cos(yaw));
    }
    // mouse: dx, dy in pixels
    void orbit(float dx, float dy) { yaw -= dx * 0.008f; pitch += dy * 0.008f; clamp(); }
    void zoom(float steps) { dist *= std::pow(0.88f, steps); clamp(); }   // steps > 0: closer
    void pan(float dx, float dy, float viewH) {   // the target moves with the picture: right/left on the ground, up/down in depth
        const float k = 2.f * dist * std::tan(fov / 2) / std::max(1.f, viewH);
        const float sy = std::sin(yaw), cy = std::cos(yaw);
        tx -= dx * k * cy; tz += dx * k * sy;
        tx -= dy * k * sy; tz -= dy * k * cy;
        clamp();
    }
};

enum Preset { Preset_Iso, Preset_Front, Preset_Top, Preset_Side, Preset_Low, NumPresets };
inline const char* presetName(int p) {
    static const char* const n[NumPresets] = {"Angled", "Front", "Top", "Side", "Low"};
    return n[p < 0 || p >= NumPresets ? 0 : p];
}
inline Camera preset(int p) {
    Camera c;
    switch (p) {
    case Preset_Front: c.yaw = 0; c.pitch = 0.42f; c.dist = 1.9f; break;
    case Preset_Top: c.yaw = 0; c.pitch = 1.55f; c.dist = 1.75f; c.ty = 0; break;   // looks like the flat waterfall
    case Preset_Side: c.yaw = 1.2f; c.pitch = 0.35f; c.dist = 2.0f; break;
    case Preset_Low: c.yaw = 0.25f; c.pitch = 0.12f; c.dist = 1.6f; c.ty = 0.05f; break;
    default: break;   // Preset_Iso: the defaults
    }
    return c;
}

// ------------------------------------------------------------------------------------------------ projection
struct Proj {
    V3 eye, right, up, fwd;
    float f = 1, aspect = 1, vx = 0, vy = 0, vw = 1, vh = 1, nearZ = 0.02f;
    float ox = 0, oy = 0;   // the framing's shift (normalised screen units, Fit)
};
// The framing (fitScale): a lens scale and a shift that put the surface's box in the middle of the viewport
struct Fit { float lens = 1, ox = 0, oy = 0; };
// the viewport (vx, vy, vw, vh) in pixels
inline Proj makeProj(const Camera& c, float vx, float vy, float vw, float vh, Fit fit = Fit()) {
    Proj p;
    p.eye = c.eye();
    p.fwd = norm(v3(c.tx, c.ty, c.tz) - p.eye);
    p.right = norm(cross(p.fwd, v3(0, 1, 0)));
    if (dot(p.right, p.right) < 1e-6f) p.right = v3(std::cos(c.yaw), 0, -std::sin(c.yaw));   // looking straight down
    p.up = cross(p.right, p.fwd);
    p.f = fit.lens / std::tan(c.fov / 2);
    p.ox = fit.ox; p.oy = fit.oy;
    p.vx = vx; p.vy = vy; p.vw = std::max(1.f, vw); p.vh = std::max(1.f, vh);
    p.aspect = p.vw / p.vh;
    return p;
}
// Framing: the lens scale and shift that make the surface's box (1 wide, depth deep, hs high) fill `fill` of the viewport in its
// tighter direction, centred, as seen from the camera's angle at the preset distance with the target in the middle. Applied on top of
// the camera, so a wide, short plot is not mostly empty, while the camera's own distance still zooms and a pan still moves the picture.
inline Fit fitScale(const Camera& c, float vw, float vh, float hs, float depth, float fill = 0.9f) {
    Camera k = c; k.dist = Camera().dist; k.tx = 0; k.tz = 0; k.ty = Camera().ty;
    const V3 eye = k.eye(), fwd = norm(v3(k.tx, k.ty, k.tz) - eye);
    V3 right = norm(cross(fwd, v3(0, 1, 0)));
    if (dot(right, right) < 1e-6f) right = v3(std::cos(k.yaw), 0, -std::sin(k.yaw));
    const V3 up = cross(right, fwd);
    const float f = 1.f / std::tan(k.fov / 2), aspect = std::max(1.f, vw) / std::max(1.f, vh);
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int i = 0; i < 8; i++) {
        const V3 w = v3(i & 1 ? 0.5f : -0.5f, i & 2 ? hs : 0.f, i & 4 ? depth / 2 : -depth / 2) - eye;
        const float z = std::max(0.05f, dot(w, fwd));
        const float nx = dot(w, right) * f / (aspect * z), ny = dot(w, up) * f / z;
        x0 = std::min(x0, nx); x1 = std::max(x1, nx); y0 = std::min(y0, ny); y1 = std::max(y1, ny);
    }
    Fit r;
    const float h = std::max(x1 - x0, y1 - y0) / 2;
    if (!(h > 0)) return r;
    r.lens = std::min(4.f, std::max(0.25f, fill / h));
    r.ox = r.lens * (x0 + x1) / 2; r.oy = r.lens * (y0 + y1) / 2;
    return r;
}

// false when the point is behind (or too close to) the camera
inline bool project(const Proj& p, V3 w, float& sx, float& sy, float& depth) {
    const V3 d = w - p.eye;
    const float z = dot(d, p.fwd);
    if (z < p.nearZ) return false;
    const float nx = dot(d, p.right) * p.f / (p.aspect * z) - p.ox, ny = dot(d, p.up) * p.f / z - p.oy;
    sx = p.vx + (nx * 0.5f + 0.5f) * p.vw;
    sy = p.vy + (0.5f - ny * 0.5f) * p.vh;
    depth = z;
    return true;
}
// the ray through a screen point (the inverse of project)
inline void screenRay(const Proj& p, float sx, float sy, V3& o, V3& dir) {
    const float nx = ((sx - p.vx) / p.vw - 0.5f) * 2 + p.ox, ny = (0.5f - (sy - p.vy) / p.vh) * 2 + p.oy;
    o = p.eye;
    dir = norm(p.fwd + p.right * (nx * p.aspect / p.f) + p.up * (ny / p.f));
}

// ------------------------------------------------------------------------------------------------ decimation
// n output columns from span source columns, each the highest of the ones it covers (a narrow carrier is never skipped); n <= span
inline void decimateMax(const uint8_t* src, int span, int n, uint8_t* out) {
    if (n <= 0 || span <= 0) return;
    if (n >= span) { for (int x = 0; x < n; x++) out[x] = src[(long long)x * span / n]; return; }
    for (int x = 0; x < n; x++) {
        const int a = (int)((long long)span * x / n), b = std::max(a + 1, (int)((long long)span * (x + 1) / n));
        uint8_t m = src[a];
        for (int c = a + 1; c < b; c++) m = std::max(m, src[c]);
        out[x] = m;
    }
}

struct Budget { int cols = 0, rows = 0, group = 1; };   // rows shown, each the highest of group history rows
// The mesh size for a plot of plotWpx device pixels: a column at least pxPerCol pixels wide, no more columns than the span has, and the
// rows (rowsWanted, 64..1024) grouped by powers of two until cols x rows stays under the vertex budget. Low power: coarser and smaller.
inline Budget budget(int span, float plotWpx, int rowsWanted, bool lowPower) {
    Budget b;
    const float pxPerCol = lowPower ? 5.f : 3.f;
    const int maxCols = lowPower ? 320 : 768, maxVerts = lowPower ? 40000 : 160000;
    b.cols = std::max(2, std::min(std::min(span, maxCols), (int)(plotWpx / pxPerCol)));
    rowsWanted = std::max(2, rowsWanted);
    while (b.group < rowsWanted && (long long)b.cols * ((rowsWanted + b.group - 1) / b.group) > maxVerts) b.group *= 2;
    b.rows = std::max(2, (rowsWanted + b.group - 1) / b.group);
    return b;
}

// The 3D view's own history: up to 1024 rows of `cols` levels, already decimated across (the main history keeps 512 rows of the full width)
struct History {
    int cols = 0, cap = 0;
    long long count = 0;   // rows pushed since the reset
    std::vector<uint8_t> d;
    void reset(int c, int n) { cols = std::max(1, c); cap = std::max(1, n); count = 0; d.assign((size_t)cols * cap, 0); }
    void push(const uint8_t* row) { std::memcpy(d.data() + (size_t)(count % cap) * cols, row, (size_t)cols); count++; }
    int have() const { return (int)std::min<long long>(count, cap); }
    const uint8_t* row(int age) const {   // 0: the newest
        if (age < 0 || age >= have()) return nullptr;
        return d.data() + (size_t)((count - 1 - age) % cap) * cols;
    }
};

// The heights shown: rows x cols levels 0..1 (row 0 the newest), the level for the colour (0..255, the index into the palette), and the
// peak hold per column (the highest over all the rows shown).
struct Grid {
    int cols = 0, rows = 0;
    std::vector<float> h;
    std::vector<uint8_t> lv;
    std::vector<float> peak;
    float at(int r, int c) const { return h[(size_t)r * cols + c]; }
    // bilinear height at u (0..1 across) and v (0 newest .. 1 oldest)
    float sample(float u, float v) const {
        if (cols < 2 || rows < 2) return 0;
        const float x = std::min(1.f, std::max(0.f, u)) * (cols - 1), y = std::min(1.f, std::max(0.f, v)) * (rows - 1);
        const int c = std::min(cols - 2, (int)x), r = std::min(rows - 2, (int)y);
        const float fx = x - c, fy = y - r;
        const float a = at(r, c) + (at(r, c + 1) - at(r, c)) * fx, b = at(r + 1, c) + (at(r + 1, c + 1) - at(r + 1, c)) * fx;
        return a + (b - a) * fy;
    }
};

// [1 2 1] / 4 across the columns and down the rows (the optional smoothing): softens the FFT noise into a surface
inline void smooth121(std::vector<float>& h, int rows, int cols, std::vector<float>& tmp) {
    if (rows < 1 || cols < 3) return;
    tmp.resize(h.size());
    for (int r = 0; r < rows; r++) {
        const float* s = h.data() + (size_t)r * cols; float* o = tmp.data() + (size_t)r * cols;
        o[0] = (3 * s[0] + s[1]) * 0.25f; o[cols - 1] = (3 * s[cols - 1] + s[cols - 2]) * 0.25f;
        for (int c = 1; c < cols - 1; c++) o[c] = (s[c - 1] + 2 * s[c] + s[c + 1]) * 0.25f;
    }
    if (rows < 3) { h.swap(tmp); return; }
    for (int r = 0; r < rows; r++) {
        const int ra = std::max(0, r - 1), rb = std::min(rows - 1, r + 1);
        const float* a = tmp.data() + (size_t)ra * cols; const float* m = tmp.data() + (size_t)r * cols; const float* b = tmp.data() + (size_t)rb * cols;
        float* o = h.data() + (size_t)r * cols;
        for (int c = 0; c < cols; c++) o[c] = (a[c] + 2 * m[c] + b[c]) * 0.25f;
    }
}

// rows shown x hist.cols from the newest rows of hist, each the highest of `group` rows; missing rows (a young history) are 0
inline void buildGrid(const History& hist, int rows, int group, bool smooth, Grid& g, std::vector<float>& tmp) {
    g.cols = hist.cols; g.rows = std::max(2, rows);
    group = std::max(1, group);
    g.h.assign((size_t)g.rows * g.cols, 0.f);
    g.lv.assign((size_t)g.rows * g.cols, 0);
    g.peak.assign((size_t)g.cols, 0.f);
    for (int r = 0; r < g.rows; r++) {
        uint8_t* o = g.lv.data() + (size_t)r * g.cols;
        for (int k = 0; k < group; k++) {
            const uint8_t* s = hist.row(r * group + k);
            if (!s) break;
            for (int c = 0; c < g.cols; c++) o[c] = std::max(o[c], s[c]);
        }
        float* h = g.h.data() + (size_t)r * g.cols;
        for (int c = 0; c < g.cols; c++) h[c] = o[c] * (1.f / 255.f);
    }
    if (smooth) {
        smooth121(g.h, g.rows, g.cols, tmp);
        for (size_t i = 0; i < g.h.size(); i++) g.lv[i] = (uint8_t)std::min(255.f, g.h[i] * 255.f + 0.5f);
    }
    for (int r = 0; r < g.rows; r++)
        for (int c = 0; c < g.cols; c++) g.peak[c] = std::max(g.peak[c], g.at(r, c));
}

// ------------------------------------------------------------------------------------------------ mesh
struct Look {
    float heightScale = 0.35f;   // the top of the scale this high (the surface is 1 wide)
    float depth = 1.0f;          // the time axis this long
    bool shade = true;           // light the slopes a little (reads the shape better than the colour alone)
};
inline V3 gridPoint(const Grid& g, const Look& lk, int r, int c, float h) {
    return v3(-0.5f + (float)c / (float)(g.cols - 1), h * lk.heightScale, lk.depth * 0.5f - lk.depth * (float)r / (float)(g.rows - 1));
}

struct Vtx { float x, y; uint32_t col; };   // screen position (pixels) and colour (0xAABBGGRR, as ImGui's IM_COL32)
struct Chunk { int v0, nv, i0, ni; };       // vertices [v0, v0 + nv), indices [i0, i0 + ni) counted from v0 (each chunk < 65536 vertices)
struct Mesh {
    std::vector<Vtx> v;
    std::vector<uint16_t> idx;
    std::vector<Chunk> chunks;
    std::vector<uint8_t> ok;      // scratch: the vertex is in front of the camera
    std::vector<int> colOrder;    // scratch
    int quads = 0;
    void clear() { v.clear(); idx.clear(); chunks.clear(); quads = 0; }
};

inline uint32_t shadeColour(uint32_t c, float k) {
    const uint32_t r = (uint32_t)std::min(255.f, (c & 255) * k), gg = (uint32_t)std::min(255.f, ((c >> 8) & 255) * k), b = (uint32_t)std::min(255.f, ((c >> 16) & 255) * k);
    return (c & 0xFF000000u) | (b << 16) | (gg << 8) | r;
}

// Painter's order for a height field: the strips (quads between two neighbouring grid lines) from both ends towards the one under the eye.
// e: where the eye is, in grid lines (may be outside 0..n-1); returns the n - 1 strips in drawing order.
inline void farToNear(int n, float e, std::vector<int>& out) {
    out.clear();
    const int s = n - 1;
    if (s <= 0) return;
    int k = (int)std::floor(e);   // the strip the eye is over
    k = std::max(-1, std::min(s, k));
    for (int i = 0; i < k && i < s; i++) out.push_back(i);              // left of the eye: far to near is ascending
    for (int i = s - 1; i > k; i--) out.push_back(i);                   // right of it: descending
    if (k >= 0 && k < s) out.push_back(k);                              // the one underneath last
}

// The grid as screen-space triangles, far quads first. lut: 256 colours (the waterfall's palette).
inline void buildMesh(const Grid& g, const Proj& p, const Look& lk, const uint32_t* lut, Mesh& m) {
    m.clear();
    if (g.cols < 2 || g.rows < 2) return;
    const int C = g.cols, R = g.rows;
    // the eye in grid coordinates
    const float ec = (p.eye.x + 0.5f) * (C - 1), er = (lk.depth * 0.5f - p.eye.z) / lk.depth * (R - 1);
    std::vector<int> rowOrder;
    farToNear(R, er, rowOrder);
    farToNear(C, ec, m.colOrder);
    const V3 L = norm(v3(-0.35f, 0.85f, 0.4f));
    const float dx = 1.f / (C - 1), dz = lk.depth / (R - 1);
    // chunks: runs of neighbouring strips in drawing order, each holding the grid rows it needs (< 65536 vertices)
    const int maxStrips = std::max(1, 65535 / C - 1);
    size_t i = 0;
    while (i < rowOrder.size()) {
        size_t j = i + 1;
        const int dir = (i + 1 < rowOrder.size()) ? rowOrder[i + 1] - rowOrder[i] : 1;
        while (j < rowOrder.size() && rowOrder[j] - rowOrder[j - 1] == dir && (dir == 1 || dir == -1) && (int)(j - i) < maxStrips) j++;
        const int sA = std::min(rowOrder[i], rowOrder[j - 1]), sB = std::max(rowOrder[i], rowOrder[j - 1]);
        const int r0 = sA, r1 = sB + 1;   // grid rows in this chunk
        Chunk ch; ch.v0 = (int)m.v.size(); ch.nv = (r1 - r0 + 1) * C; ch.i0 = (int)m.idx.size(); ch.ni = 0;
        m.v.resize((size_t)ch.v0 + ch.nv);
        m.ok.resize(m.v.size());
        for (int r = r0; r <= r1; r++)
            for (int c = 0; c < C; c++) {
                const float h = g.at(r, c);
                Vtx& o = m.v[(size_t)ch.v0 + (size_t)(r - r0) * C + c];
                float depth = 0;
                m.ok[(size_t)ch.v0 + (size_t)(r - r0) * C + c] = project(p, gridPoint(g, lk, r, c, h), o.x, o.y, depth) ? 1 : 0;
                uint32_t col = lut[g.lv[(size_t)r * C + c]];
                if (lk.shade) {
                    const float hl = g.at(r, std::max(0, c - 1)), hr = g.at(r, std::min(C - 1, c + 1));
                    const float hn = g.at(std::max(0, r - 1), c), hf = g.at(std::min(R - 1, r + 1), c);
                    const V3 n = norm(v3(-(hr - hl) * lk.heightScale / (2 * dx), 1.f, (hn - hf) * lk.heightScale / (2 * dz)));
                    col = shadeColour(col, 0.62f + 0.48f * std::max(0.f, dot(n, L)));
                }
                o.col = col;
            }
        for (size_t q = i; q < j; q++) {
            const int s = rowOrder[q];
            const int a = (s - r0) * C, b = (s + 1 - r0) * C;   // the strip's two rows, local
            for (int c : m.colOrder) {
                const int v00 = a + c, v01 = a + c + 1, v10 = b + c, v11 = b + c + 1;
                const uint8_t* ok = m.ok.data() + ch.v0;
                if (!ok[v00] || !ok[v01] || !ok[v10] || !ok[v11]) continue;
                const uint16_t t[6] = {(uint16_t)v00, (uint16_t)v01, (uint16_t)v11, (uint16_t)v00, (uint16_t)v11, (uint16_t)v10};
                m.idx.insert(m.idx.end(), t, t + 6);
                m.quads++;
            }
        }
        ch.ni = (int)m.idx.size() - ch.i0;
        m.chunks.push_back(ch);
        i = j;
    }
}

// The peak hold as a line along the back edge (the oldest row) or the front one: pairs of screen points, NaN where a point is behind the eye
inline void peakLine(const Grid& g, const Proj& p, const Look& lk, bool front, std::vector<float>& xy) {
    xy.clear();
    if (g.cols < 2 || g.rows < 2) return;
    const int r = front ? 0 : g.rows - 1;
    for (int c = 0; c < g.cols; c++) {
        float x = NAN, y = NAN, d = 0;
        if (!project(p, gridPoint(g, lk, r, c, g.peak[c]), x, y, d)) x = y = NAN;
        xy.push_back(x); xy.push_back(y);
    }
}
// true when the back edge (the oldest row) is the far side from the eye: the ridge there is drawn before the surface, else after it
inline bool backIsFar(const Proj& p) { return p.eye.z > 0; }

// ------------------------------------------------------------------------------------------------ picking
struct Hit { bool ok = false; float u = 0, v = 0; V3 p; };   // u: 0..1 across the frequency span, v: 0 newest .. 1 oldest
// Where the ray through a screen point first meets the surface (or the floor under it): marches through the box of the surface in
// steps finer than a grid cell, then halves the last step. No hit when the ray passes beside or above the surface.
// Steps of a quarter cell can step over a ridge one cell wide seen exactly edge-on; the hit is then the cell behind it.
inline Hit pick(const Grid& g, const Proj& p, const Look& lk, float sx, float sy) {
    Hit hit;
    if (g.cols < 2 || g.rows < 2) return hit;
    V3 o, d;
    screenRay(p, sx, sy, o, d);
    float top = 0;
    for (float v : g.peak) top = std::max(top, v);
    const float lo[3] = {-0.5f, 0.f, -lk.depth * 0.5f}, hi[3] = {0.5f, top * lk.heightScale + 1e-4f, lk.depth * 0.5f};
    const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
    float t0 = 0, t1 = 1e9f;
    for (int k = 0; k < 3; k++) {   // the slabs
        if (std::fabs(dd[k]) < 1e-9f) { if (oo[k] < lo[k] || oo[k] > hi[k]) return hit; continue; }
        float a = (lo[k] - oo[k]) / dd[k], b = (hi[k] - oo[k]) / dd[k];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a); t1 = std::min(t1, b);
        if (t0 > t1) return hit;
    }
    auto uvAt = [&](const V3& q, float& u, float& v) { u = q.x + 0.5f; v = (lk.depth * 0.5f - q.z) / lk.depth; };
    auto above = [&](float t) {
        const V3 q = o + d * t; float u, v; uvAt(q, u, v);
        return q.y > g.sample(u, v) * lk.heightScale;
    };
    const int n = std::min(8192, 4 * (g.cols + g.rows));
    const float step = (t1 - t0) / n;
    float prev = t0;
    // the surface is a sheet drawn from both sides (no walls): the first crossing is what the eye sees, from above or from below (a
    // ray that came in through a side under the surface)
    const bool s0 = above(t0);
    for (int i = 1; i <= n; i++) {
        const float t = t0 + step * i;
        if (above(t) != s0) {
            float a = prev, b = t;
            for (int k = 0; k < 14; k++) { const float m2 = (a + b) * 0.5f; if (above(m2) == s0) a = m2; else b = m2; }
            hit.ok = true; hit.p = o + d * b; uvAt(hit.p, hit.u, hit.v);
            hit.u = std::min(1.f, std::max(0.f, hit.u)); hit.v = std::min(1.f, std::max(0.f, hit.v));
            return hit;
        }
        prev = t;
    }
    // the ray left the box: through the floor counts (the surface's lowest level is the floor), else nothing
    const V3 q = o + d * t1;
    if (s0 && q.y <= 1e-3f) { hit.ok = true; hit.p = q; uvAt(q, hit.u, hit.v); hit.u = std::min(1.f, std::max(0.f, hit.u)); hit.v = std::min(1.f, std::max(0.f, hit.v)); }
    return hit;
}

// ------------------------------------------------------------------------------------------------ frequency mapping and input
// u (0..1 across the grid, vertex 0 at 0 and vertex cols-1 at 1) to MHz: the grid's column k is the highest of the history columns
// [c0 + span k / cols, c0 + span (k + 1) / cols), and history column j is centred on imgLo + fs (j + 0.5) / W (spec_axis.h imageLo).
inline double uToMhz(double u, int cols, int c0, int c1, int W, double imgLo, double fs) {
    if (cols < 2 || W < 1) return imgLo + fs / 2;
    const double span = c1 - c0, k = std::min(1.0, std::max(0.0, u)) * (cols - 1);
    return imgLo + fs / W * (c0 + span * (k + 0.5) / cols);
}
inline double mhzToU(double mhz, int cols, int c0, int c1, int W, double imgLo, double fs) {
    if (cols < 2 || W < 1 || !(fs > 0) || c1 <= c0) return 0.5;
    const double span = c1 - c0, j = (mhz - imgLo) * W / fs;
    return ((j - c0) * cols / span - 0.5) / (cols - 1);
}

// Touch: a pinch zooms (scale > 1: fingers apart, closer), two fingers pan, one finger orbits (the hook the touch interface calls with
// touch::gesture(): touch_logic.h GestureFrame)
inline void applyGesture(Camera& c, bool pinching, float scale, float dx, float dy, bool oneFinger, float viewH) {
    if (pinching) {
        if (scale > 0) { c.dist /= scale; c.clamp(); }
        c.pan(dx, dy, viewH);
    } else if (oneFinger) c.orbit(dx, dy);
}

// ------------------------------------------------------------------------------------------------ the view's state
// What decides whether the cached mesh is still good: the data (rows pushed, the palette), the camera, the viewport and the looks.
struct Key {
    long long rows = -1; int lutGen = -1, cols = 0, gridRows = 0, group = 0;
    float vw = 0, vh = 0, yaw = 0, pitch = 0, dist = 0, tx = 0, ty = 0, tz = 0, hs = 0;
    bool smooth = false, shade = false;
    bool operator==(const Key& o) const {
        return rows == o.rows && lutGen == o.lutGen && cols == o.cols && gridRows == o.gridRows && group == o.group && vw == o.vw && vh == o.vh &&
               yaw == o.yaw && pitch == o.pitch && dist == o.dist && tx == o.tx && ty == o.ty && tz == o.tz && hs == o.hs && smooth == o.smooth && shade == o.shade;
    }
    bool operator!=(const Key& o) const { return !(*this == o); }
};

struct State {
    // the settings (saved)
    bool on = false;          // the waterfall panel shows the 3D view
    int rows = 256;           // the history rows shown, 64..1024
    float heightScale = 0.35f;
    bool smooth = false, peak = true, marker = true, shade = true;
    int preset = Preset_Iso;  // the angle a double click (or Home) goes back to
    Camera cam;
    // live
    bool frozen = false;      // no new rows into the 3D view (the main waterfall goes on)
    History hist;
    long long srcRows = -1;   // the main history's row count when it was last copied in
    int srcC0 = -1, srcC1 = -1, srcW = 0;
    struct Band { int c0 = 0, c1 = 0, W = 1; double imgLo = 0, fs = 0; } band;   // the columns and band the 3D history came from (kept while frozen)
    Grid grid;
    Mesh mesh;
    Key key;
    std::vector<float> tmp, ridge;
    std::vector<uint8_t> rowBuf;
    double lastBuildMs = 0, lastDrawMs = 0;   // CPU time of the last mesh build and of the last copy into the draw list
};

// Brings the 3D history up to date from the main one (lv: H rows of W levels, the newest number R at row wr, older at wr+1 ... wrapping;
// have: how many rows exist). The columns [c0, c1) become cols columns; when they or cap change, the history is made again from the
// main one (which keeps H rows), otherwise only the new rows are decimated in. Returns true when a row came in.
inline bool ingest(State& s, const uint8_t* lv, int W, int H, int wr, long long R, int have, int c0, int c1, int cols, int cap) {
    c0 = std::max(0, std::min(W, c0)); c1 = std::max(c0, std::min(W, c1));
    if (c1 - c0 < 2 || R < 0 || have <= 0) return false;
    cols = std::max(2, std::min(cols, c1 - c0));
    s.rowBuf.resize((size_t)cols);
    auto src = [&](long long r) { return lv + (size_t)((wr + (R - r)) % H) * W + c0; };   // row number r (R - have < r <= R)
    if (c0 != s.srcC0 || c1 != s.srcC1 || W != s.srcW || cols != s.hist.cols || cap != s.hist.cap || s.srcRows < 0 || R + 1 < s.srcRows) {
        s.hist.reset(cols, cap);
        const long long first = std::max(R - have + 1, R - (long long)std::min(cap, H) + 1);
        for (long long r = first; r <= R; r++) { decimateMax(src(r), c1 - c0, cols, s.rowBuf.data()); s.hist.push(s.rowBuf.data()); }
        s.srcC0 = c0; s.srcC1 = c1; s.srcW = W; s.srcRows = R + 1;
        return true;
    }
    if (R + 1 == s.srcRows) return false;
    const long long first = std::max(s.srcRows, R - (long long)std::min(have, H) + 1);
    for (long long r = first; r <= R; r++) { decimateMax(src(r), c1 - c0, cols, s.rowBuf.data()); s.hist.push(s.rowBuf.data()); }
    s.srcRows = R + 1;
    return true;
}

} // namespace w3d
