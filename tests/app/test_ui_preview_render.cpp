// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The hover preview's miniature rasterised on the CPU (app/ui/ui_preview_draw.cpp): ImGui records the draw lists of every picture of
// uiprev::kGallery, a small triangle rasteriser (vertex colours, the font atlas, clip rectangles, alpha blending) turns them into
// pixels, and the pixels are checked: the picture fills its rectangle, has many colours, the title bar has the look's panel colour,
// the waterfall shows only colours of its colour map, the strip has the map's middle colour in the middle, a dark look previewed
// while the app is Light comes out dark after the Light pass, High contrast has a white outline.
// ONAIR_UIPREVIEW_PNG=1 prints the gallery as a base64 PNG between "UIPREVIEW-PNG-BEGIN" and "UIPREVIEW-PNG-END" (to look at it).
#include "ui/ui_preview.h"
#include "imgui.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "stb_image_write.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

using namespace uiprev;

static const unsigned char* gTex = nullptr;
static int gTw = 0, gTh = 0;

struct Img {
    int w, h;
    std::vector<float> px;   // rgb 0..1
    Img(int w_, int h_, float r, float g, float b) : w(w_), h(h_), px((size_t)w_ * h_ * 3) { for (size_t i = 0; i < px.size(); i += 3) { px[i] = r; px[i + 1] = g; px[i + 2] = b; } }
    uint32_t at(int x, int y) const {
        const float* p = &px[((size_t)y * w + x) * 3];
        auto q = [](float v) { return (uint32_t)std::lround(std::min(1.f, std::max(0.f, v)) * 255.f); };
        return q(p[0]) | (q(p[1]) << 8) | (q(p[2]) << 16) | 0xFF000000u;
    }
};

// the triangles of one draw list into the image (pixel centres, vertex colours and texture interpolated, source-over blending)
static void raster(const ImDrawList* dl, Img& img) {
    for (const ImDrawCmd& cmd : dl->CmdBuffer) {
        if (cmd.UserCallback) continue;
        const ImVec4 cr = cmd.ClipRect;
        for (unsigned e = 0; e + 2 < cmd.ElemCount; e += 3) {
            const ImDrawVert* v[3];
            for (int k = 0; k < 3; k++) v[k] = &dl->VtxBuffer[(int)(cmd.VtxOffset + dl->IdxBuffer[(int)(cmd.IdxOffset + e + k)])];
            const float x0 = v[0]->pos.x, y0 = v[0]->pos.y, x1 = v[1]->pos.x, y1 = v[1]->pos.y, x2 = v[2]->pos.x, y2 = v[2]->pos.y;
            const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            if (std::fabs(area) < 1e-9f) continue;
            const int bx0 = std::max({0, (int)std::floor(std::min({x0, x1, x2})), (int)std::floor(cr.x)});
            const int by0 = std::max({0, (int)std::floor(std::min({y0, y1, y2})), (int)std::floor(cr.y)});
            const int bx1 = std::min({img.w - 1, (int)std::ceil(std::max({x0, x1, x2})), (int)std::ceil(cr.z) - 1});
            const int by1 = std::min({img.h - 1, (int)std::ceil(std::max({y0, y1, y2})), (int)std::ceil(cr.w) - 1});
            for (int y = by0; y <= by1; y++)
                for (int x = bx0; x <= bx1; x++) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    if (px < cr.x || px >= cr.z || py < cr.y || py >= cr.w) continue;
                    const float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area;
                    const float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area;
                    const float w2 = 1.f - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    float c[4] = {0, 0, 0, 0}, u = 0, t = 0;
                    for (int k = 0; k < 3; k++) {
                        const float wk = k == 0 ? w0 : k == 1 ? w1 : w2;
                        const ImU32 col = v[k]->col;
                        for (int ch = 0; ch < 4; ch++) c[ch] += wk * (float)((col >> (8 * ch)) & 255) / 255.f;
                        u += wk * v[k]->uv.x; t += wk * v[k]->uv.y;
                    }
                    const int tx = std::max(0, std::min(gTw - 1, (int)(u * gTw))), ty = std::max(0, std::min(gTh - 1, (int)(t * gTh)));
                    const unsigned char* tp = gTex + ((size_t)ty * gTw + tx) * 4;
                    for (int ch = 0; ch < 4; ch++) c[ch] *= tp[ch] / 255.f;
                    float* d = &img.px[((size_t)y * img.w + x) * 3];
                    for (int ch = 0; ch < 3; ch++) d[ch] = c[ch] * c[3] + d[ch] * (1 - c[3]);
                }
        }
    }
}

// the Light pass of app/ui/light_theme.cpp on the shapes (the miniature draws no glyphs)
static void lightPass(ImDrawList* dl) {
    for (ImDrawVert& v : dl->VtxBuffer) {
        const int r = (int)(v.col & 255), g = (int)((v.col >> 8) & 255), b = (int)((v.col >> 16) & 255);
        const int d = 255 - std::max(r, std::max(g, b)) - std::min(r, std::min(g, b));
        auto cl = [](int x) { return (ImU32)std::max(0, std::min(255, x)); };
        v.col = (v.col & 0xFF000000u) | cl(r + d) | (cl(g + d) << 8) | (cl(b + d) << 16);
    }
}

// one frame: `fn` draws into the foreground list; its triangles go into `img`
template <class F> static void frame(Img& img, bool pass, F fn) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)img.w, (float)img.h);
    io.DeltaTime = 1.f / 60.f;
    ImGui::NewFrame();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    fn(dl);
    ImGui::Render();
    unsigned char* tp; int tw, th;
    io.Fonts->GetTexDataAsRGBA32(&tp, &tw, &th);   // the atlas as it is now (a glyph may have been added)
    gTex = tp; gTw = tw; gTh = th;
    if (pass) lightPass(dl);
    raster(dl, img);
}

static int dist(uint32_t a, uint32_t b) {
    int m = 0;
    for (int s = 0; s < 24; s += 8) m = std::max(m, std::abs((int)((a >> s) & 255) - (int)((b >> s) & 255)));
    return m;
}
static float lum(uint32_t c) { return contrast::luminance(rgbOf(c)); }

static void pictures() {
    const Look cur;
    const Desc now = describe(cur);
    for (int i = 0; i < kGalleryCount; i++) {
        const Sample& s = kGallery[i];
        const Desc d = previewFor(cur, s.o, s.v);
        const int W = (int)previewW(d), H = (int)previewH(d);
        Img img(W, H, 1.f, 0.f, 1.f);   // magenta: what the picture does not cover
        frame(img, false, [&](ImDrawList* dl) { draw(dl, 0, 0, (float)W, (float)H, d, unitFor(now, d, 1.f), false); });
        int bare = 0;
        std::set<uint32_t> colours;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { const uint32_t c = img.at(x, y); if (c == 0xFFFF00FFu) bare++; colours.insert(c); }
        CHECK(bare < W * H / 100, "%s: %d pixels not covered", s.name, bare);
        CHECK(colours.size() >= 12, "%s: only %zu colours", s.name, colours.size());
        if (!d.plotOnly) {
            CHECK(dist(img.at(W * 4 / 5, 3), d.rail) <= 3, "%s: title bar %08X, the panel colour is %08X", s.name, img.at(W * 4 / 5, 3), d.rail);
        } else {
            // the waterfall: every pixel near the bottom-left is a colour of the map
            int worst = 0;
            for (int y = H - 10; y < H - 3; y++)
                for (int x = 3; x < 40; x++) {
                    int best = 255;
                    for (int k = 0; k <= 255; k++) best = std::min(best, dist(img.at(x, y), cmap::color(d.wfMap, k / 255.f)));
                    worst = std::max(worst, best);
                }
            CHECK(worst <= 4, "%s: a waterfall pixel is %d away from the map's colours", s.name, worst);
            if (d.strip) {   // the middle of the strip is the middle of the map (within one segment of the gradient)
                int best = 255;
                for (int k = 47; k <= 53; k++) best = std::min(best, dist(img.at(W / 2, 6), cmap::color(d.wfMap, k / 100.f)));
                CHECK(best <= 4, "%s: the strip's middle is %08X", s.name, img.at(W / 2, 6));
            }
        }
    }
}

static void lightAndContrast() {
    // the app is Light; hovering Dark shows the dark look although the frame is turned over afterwards
    Look light; light.mode = 1;
    const Desc now = describe(light), dark = previewFor(light, Opt::Mode, 0);
    CHECK(frameTurned(light) && !dark.light, "the frame is turned, the preview is dark");
    Img img(260, 160, 1.f, 0.f, 1.f);
    frame(img, true, [&](ImDrawList* dl) { draw(dl, 0, 0, 260, 160, dark, unitFor(now, dark, 1.f), true); });
    CHECK(dist(img.at(208, 3), pack(uipal::palette(3).rail)) <= 3, "dark title bar after the Light pass: %08X", img.at(208, 3));
    CHECK(lum(img.at(208, 3)) < 0.1f, "the preview of Dark is dark");
    // and hovering Light from Dark
    const Look darkCur;
    const Desc lt = previewFor(darkCur, Opt::Mode, 1);
    Img img2(260, 160, 1.f, 0.f, 1.f);
    frame(img2, false, [&](ImDrawList* dl) { draw(dl, 0, 0, 260, 160, lt, unitFor(describe(darkCur), lt, 1.f), false); });
    CHECK(lum(img2.at(208, 3)) > 0.6f, "the preview of Light is light: %08X", img2.at(208, 3));
    // High contrast: a white outline round a black window
    const Desc hc = previewFor(darkCur, Opt::Mode, 2);
    Img img3(260, 160, 1.f, 0.f, 1.f);
    frame(img3, false, [&](ImDrawList* dl) { draw(dl, 0, 0, 260, 160, hc, unitFor(describe(darkCur), hc, 1.f), false); });
    CHECK(lum(img3.at(0, 80)) > 0.5f && lum(img3.at(208, 3)) < 0.02f, "High contrast outline %08X, title %08X", img3.at(0, 80), img3.at(208, 3));
    // a bigger display scale makes a bigger picture with the same layout: the title bar grows with it
    auto titleRows = [&](float scale) {
        const Desc d = describe(darkCur);
        const int W = (int)(260 * scale), H = (int)(160 * scale);
        Img im(W, H, 1.f, 0.f, 1.f);
        frame(im, false, [&](ImDrawList* dl) { draw(dl, 0, 0, (float)W, (float)H, d, unitFor(d, d, scale), false); });
        int n = 0;
        while (n < H && dist(im.at(W * 4 / 5, n + 2), d.rail) <= 3) n++;
        return n;
    };
    const int t1 = titleRows(1.f), t2 = titleRows(2.f);
    CHECK(t2 >= 2 * t1 - 3 && t2 <= 2 * t1 + 3, "high DPI: title bar %d rows at 1x, %d at 2x", t1, t2);
}

static void dumpGallery() {
    const char* e = std::getenv("ONAIR_UIPREVIEW_PNG");
    if (!e || !*e) return;
    const int cols = 6, cw = 260, ch = 160, lab = 16, gap = 8;
    const int rows = (kGalleryCount + cols - 1) / cols;
    Img img(cols * (cw + gap) + gap, rows * (ch + lab + gap) + gap, 0.25f, 0.25f, 0.27f);
    const Look cur;
    const Desc now = describe(cur);
    frame(img, false, [&](ImDrawList* dl) {
        for (int i = 0; i < kGalleryCount; i++) {
            const Desc d = previewFor(cur, kGallery[i].o, kGallery[i].v);
            const float x = (float)(gap + (i % cols) * (cw + gap)), y = (float)(gap + (i / cols) * (ch + lab + gap));
            dl->AddText(ImVec2(x, y), IM_COL32(255, 255, 255, 255), kGallery[i].name);
            draw(dl, x, y + lab, previewW(d), previewH(d), d, unitFor(now, d, 1.f), false);
        }
    });
    std::vector<unsigned char> rgb((size_t)img.w * img.h * 3);
    for (int y = 0; y < img.h; y++) for (int x = 0; x < img.w; x++) { const uint32_t c = img.at(x, y); for (int k = 0; k < 3; k++) rgb[((size_t)y * img.w + x) * 3 + k] = (unsigned char)((c >> (8 * k)) & 255); }
    std::vector<unsigned char> png;
    stbi_write_png_to_func([](void* ctx, void* data, int size) { auto* v = (std::vector<unsigned char>*)ctx; v->insert(v->end(), (unsigned char*)data, (unsigned char*)data + size); },
                           &png, img.w, img.h, 3, rgb.data(), img.w * 3);
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < png.size(); i += 3) {
        const uint32_t n = (uint32_t)png[i] << 16 | (i + 1 < png.size() ? (uint32_t)png[i + 1] << 8 : 0) | (i + 2 < png.size() ? png[i + 2] : 0);
        out += b64[(n >> 18) & 63]; out += b64[(n >> 12) & 63];
        out += i + 1 < png.size() ? b64[(n >> 6) & 63] : '='; out += i + 2 < png.size() ? b64[n & 63] : '=';
    }
    std::printf("UIPREVIEW-PNG-BEGIN %dx%d %zu bytes\n", img.w, img.h, png.size());
    for (size_t i = 0; i < out.size(); i += 120) std::printf("%s\n", out.substr(i, 120).c_str());
    std::printf("UIPREVIEW-PNG-END\n");
}

int main() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;   // as the OpenGL and Metal back ends: a long draw list gets more commands, not wrapped indices
    unsigned char* px; int tw, th;
    io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);
    gTex = px; gTw = tw; gTh = th;
    pictures();
    lightAndContrast();
    dumpGallery();
    ImGui::DestroyContext();
    if (!failures) std::printf("ui preview render OK\n");
    return failures ? 1 : 0;
}
