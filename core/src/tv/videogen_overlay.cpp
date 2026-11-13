// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Digital video generator: overlay layout, snapping and renderer (videogen_overlay.h). Text is drawn with stb_truetype (the copy that
// comes with Dear ImGui, compiled here as static functions). The logo is a picture file: PNG through OnAir's own decoder (every build,
// transparency kept), other formats through FFmpeg when the build has a decoder for them; scaled with swscale.
#include "dect2/tv/videogen_overlay.h"
#include "dect2/common/png.h"
#include "dect2/common/u8file.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace dect2::vgen {

namespace {

std::string escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}

std::string unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) { o += s[i + 1] == 'n' ? '\n' : s[i + 1]; i++; }
        else o += s[i];
    }
    return o;
}

// one code point from UTF-8; a broken byte counts as itself
uint32_t nextCp(const std::string& s, size_t& i) {
    const uint8_t c = (uint8_t)s[i++];
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = n == 3 ? (c & 0x07u) : n == 2 ? (c & 0x0Fu) : n == 1 ? (c & 0x1Fu) : c;
    while (n-- > 0 && i < s.size() && ((uint8_t)s[i] & 0xC0) == 0x80) cp = (cp << 6) | ((uint8_t)s[i++] & 0x3Fu);
    return cp;
}

size_t cpCount(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) { nextCp(s, i); n++; }
    return n;
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> v(1);
    for (char c : s) {
        if (c == '\n') v.emplace_back();
        else v.back() += c;
    }
    return v;
}

// straight-alpha "over" of one colour with coverage a (0..255) onto an RGBA pixel
inline void over(uint8_t* d, uint32_t abgr, int cover) {
    const int sa = (int)((abgr >> 24) & 0xFF) * cover / 255;
    if (sa <= 0) return;
    const int sr = abgr & 0xFF, sg = (abgr >> 8) & 0xFF, sb = (abgr >> 16) & 0xFF;
    const int da = d[3];
    const int oa = sa + da * (255 - sa) / 255;
    if (oa <= 0) return;
    d[0] = (uint8_t)((sr * sa + d[0] * da * (255 - sa) / 255) / oa);
    d[1] = (uint8_t)((sg * sa + d[1] * da * (255 - sa) / 255) / oa);
    d[2] = (uint8_t)((sb * sa + d[2] * da * (255 - sa) / 255) / oa);
    d[3] = (uint8_t)oa;
}

void fillRect(std::vector<uint8_t>& rgba, int W, int H, Rect r, uint32_t abgr) {
    const int x0 = std::max(0, (int)std::floor(r.x)), x1 = std::min(W, (int)std::ceil(r.x + r.w));
    const int y0 = std::max(0, (int)std::floor(r.y)), y1 = std::min(H, (int)std::ceil(r.y + r.h));
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) over(&rgba[((size_t)y * W + x) * 4], abgr, 255);
}

// A picture file through FFmpeg (JPEG, BMP, WebP, SVG with librsvg ... whatever this build decodes) as RGBA premultiplied by alpha
bool decodeWithFFmpeg(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba) {
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) < 0) return false;
    bool ok = false;
    const AVCodec* c = nullptr;
    AVCodecContext* dec = nullptr;
    AVPacket* pk = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    const int si = avformat_find_stream_info(fc, nullptr) >= 0 ? av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, &c, 0) : -1;
    if (si >= 0 && c && (dec = avcodec_alloc_context3(c)) && avcodec_parameters_to_context(dec, fc->streams[si]->codecpar) >= 0 &&
        avcodec_open2(dec, c, nullptr) >= 0) {
        bool got = false;
        while (!got && av_read_frame(fc, pk) >= 0) {
            if (pk->stream_index == si && avcodec_send_packet(dec, pk) >= 0) got = avcodec_receive_frame(dec, fr) >= 0;
            av_packet_unref(pk);
        }
        if (!got && avcodec_send_packet(dec, nullptr) >= 0) got = avcodec_receive_frame(dec, fr) >= 0;
        if (got && fr->width > 0 && fr->height > 0 && fr->width <= 8192 && fr->height <= 8192) {
            w = fr->width; h = fr->height;
            rgba.assign((size_t)w * h * 4, 0);
            SwsContext* sws = sws_getContext(w, h, (AVPixelFormat)fr->format, w, h, AV_PIX_FMT_RGBA, SWS_POINT, nullptr, nullptr, nullptr);
            if (sws) {
                uint8_t* d[1] = {rgba.data()};
                const int ds[1] = {w * 4};
                sws_scale(sws, fr->data, fr->linesize, 0, h, d, ds);
                sws_freeContext(sws);
                for (size_t i = 0; i < rgba.size(); i += 4)
                    for (int k = 0; k < 3; k++) rgba[i + k] = (uint8_t)((rgba[i + k] * rgba[i + 3] + 127) / 255);
                ok = true;
            }
        }
    }
    av_frame_free(&fr);
    av_packet_free(&pk);
    avcodec_free_context(&dec);
    avformat_close_input(&fc);
    return ok;
}

// The logo file as RGBA premultiplied by alpha: PNG with OnAir's own decoder first (it is in every build), then FFmpeg
bool loadPicture(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> bytes;
    if (FILE* f = u8fopen(path, "rb")) {
        uint8_t b[65536];
        size_t n;
        while ((n = fread(b, 1, sizeof b, f)) > 0 && bytes.size() < (64u << 20)) bytes.insert(bytes.end(), b, b + n);
        fclose(f);
    }
    if (bytes.size() > 8 && bytes[0] == 0x89 && bytes[1] == 'P' && decodePng(bytes.data(), bytes.size(), w, h, rgba) && w > 0 && h > 0) return true;
    return !bytes.empty() && decodeWithFFmpeg(path, w, h, rgba);
}

} // namespace

std::string saveLayout(const Layout& l) {
    std::ostringstream o;
    for (const Block& b : l.blocks) {
        o << "[block]\n" << "kind=" << (int)b.kind << "\n" << "param=" << (int)b.param << "\n" << "text=" << escape(b.text) << "\n"
          << "x=" << b.x << "\n" << "y=" << b.y << "\n" << "w=" << b.w << "\n" << "h=" << b.h << "\n" << "font=" << b.fontPx << "\n"
          << "color=" << b.color << "\n" << "bg=" << (b.background ? 1 : 0) << "\n" << "dir=" << b.direction << "\n" << "cps=" << b.charsPerSec << "\n";
    }
    return o.str();
}

Layout loadLayout(const std::string& text) {
    Layout l;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line == "[block]") { l.blocks.emplace_back(); continue; }
        if (l.blocks.empty()) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        Block& b = l.blocks.back();
        const double d = atof(v.c_str());
        if (k == "kind") b.kind = (BlockKind)std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "param") b.param = (Param)std::clamp(atoi(v.c_str()), 0, (int)Param::Count - 1);
        else if (k == "text") b.text = unescape(v);
        else if (k == "x") b.x = (float)std::clamp(d, -1.0, 2.0);
        else if (k == "y") b.y = (float)std::clamp(d, -1.0, 2.0);
        else if (k == "w") b.w = (float)std::clamp(d, 0.01, 1.0);
        else if (k == "h") b.h = (float)std::clamp(d, 0.01, 1.0);
        else if (k == "font") b.fontPx = (float)std::clamp(d, 6.0, 200.0);
        else if (k == "color") b.color = (uint32_t)strtoul(v.c_str(), nullptr, 10);
        else if (k == "bg") b.background = atoi(v.c_str()) != 0;
        else if (k == "dir") b.direction = std::clamp(atoi(v.c_str()), 0, 1);
        else if (k == "cps") b.charsPerSec = (float)std::clamp(d, 0.1, 100.0);
    }
    return l;
}

Rect snapRect(Rect r, const std::vector<Rect>& others, float W, float H, float dist) {
    // candidate left edges / top edges that put one of r's edges (or its centre) on a line of another block or of the picture
    std::vector<float> xs = {0, W - r.w, W / 2 - r.w / 2}, ys = {0, H - r.h, H / 2 - r.h / 2};
    for (const Rect& o : others) {
        xs.insert(xs.end(), {o.x, o.x + o.w, o.x - r.w, o.x + o.w - r.w, o.x + o.w / 2 - r.w / 2});
        ys.insert(ys.end(), {o.y, o.y + o.h, o.y - r.h, o.y + o.h - r.h, o.y + o.h / 2 - r.h / 2});
    }
    auto best = [dist](float v, const std::vector<float>& c) {
        float b = v, bd = dist;
        for (float x : c) if (std::fabs(x - v) <= bd) { bd = std::fabs(x - v); b = x; }
        return b;
    };
    r.x = best(r.x, xs);
    r.y = best(r.y, ys);
    return r;
}

struct OverlayRenderer::Impl {
    std::shared_ptr<const std::vector<uint8_t>> ttf;
    stbtt_fontinfo font{};
    bool ok = false;
    struct Glyph { int w = 0, h = 0, xoff = 0, yoff = 0; float adv = 0; std::vector<uint8_t> bits; };
    std::map<std::pair<uint32_t, int>, Glyph> glyphs;   // (code point, size in 1/4 px)
    struct Logo { std::string path; int w = 0, h = 0; std::vector<uint8_t> rgba; bool bad = false; };   // scaled to its box, straight alpha
    std::vector<Logo> logos;
    std::string srcPath;                  // the last logo file decoded, kept so a resize does not read it again
    int srcW = 0, srcH = 0;
    std::vector<uint8_t> src;             // premultiplied
    bool srcOk = false;

    float scale(float px) const { return stbtt_ScaleForMappingEmToPixels(&font, px); }
    void metrics(float px, float& ascent, float& lineH) const {
        int a = 0, d = 0, g = 0;
        stbtt_GetFontVMetrics(&font, &a, &d, &g);
        const float s = scale(px);
        ascent = a * s;
        lineH = (a - d + g) * s;
    }
    const Glyph& glyph(uint32_t cp, float px) {
        const int key = (int)std::lround(px * 4);
        auto it = glyphs.find({cp, key});
        if (it != glyphs.end()) return it->second;
        if (glyphs.size() > 4000) glyphs.clear();
        Glyph g;
        const float s = scale(px);
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&font, (int)cp, &adv, &lsb);
        g.adv = adv * s;
        unsigned char* b = stbtt_GetCodepointBitmap(&font, s, s, (int)cp, &g.w, &g.h, &g.xoff, &g.yoff);
        if (b) { g.bits.assign(b, b + (size_t)g.w * g.h); stbtt_FreeBitmap(b, nullptr); }
        return glyphs.emplace(std::make_pair(cp, key), std::move(g)).first->second;
    }
    float width(const std::string& s, float px) {
        if (!ok) return 0.55f * px * (float)cpCount(s);
        float w = 0;
        for (size_t i = 0; i < s.size();) w += glyph(nextCp(s, i), px).adv;
        return w;
    }
    // one line with its top-left at (x, y); only the part inside clip is drawn
    void drawLine(std::vector<uint8_t>& rgba, int W, int H, const std::string& s, float px, float x, float y, uint32_t col, Rect clip) {
        if (!ok) return;
        float asc = 0, lh = 0;
        metrics(px, asc, lh);
        const int cx0 = std::max(0, (int)clip.x), cx1 = std::min(W, (int)(clip.x + clip.w));
        const int cy0 = std::max(0, (int)clip.y), cy1 = std::min(H, (int)(clip.y + clip.h));
        float pen = x;
        for (size_t i = 0; i < s.size();) {
            const Glyph& g = glyph(nextCp(s, i), px);
            const int gx = (int)std::lround(pen) + g.xoff, gy = (int)std::lround(y + asc) + g.yoff;
            pen += g.adv;
            if (gx >= cx1 || gx + g.w <= cx0) continue;
            for (int r = 0; r < g.h; r++) {
                const int yy = gy + r;
                if (yy < cy0 || yy >= cy1) continue;
                for (int c = 0; c < g.w; c++) {
                    const int xx = gx + c;
                    if (xx < cx0 || xx >= cx1) continue;
                    const int a = g.bits[(size_t)r * g.w + c];
                    if (a) over(&rgba[((size_t)yy * W + xx) * 4], col, a);
                }
            }
        }
    }
    const Logo* logo(const std::string& path, int w, int h) {
        for (Logo& l : logos) if (l.path == path && l.w == w && l.h == h) return l.bad ? nullptr : &l;
        if (logos.size() > 8) logos.erase(logos.begin());
        if (path != srcPath) { srcPath = path; srcOk = !path.empty() && loadPicture(path, srcW, srcH, src); }
        Logo l;
        l.path = path; l.w = w; l.h = h;
        l.bad = !srcOk || w <= 0 || h <= 0;
        if (!l.bad) {
            // fit inside the box, centred, keeping the logo's own proportions; scaled premultiplied (no dark fringe at the edges)
            const double s = std::min((double)w / srcW, (double)h / srcH);
            const int dw = std::max(1, (int)std::lround(srcW * s)), dh = std::max(1, (int)std::lround(srcH * s));
            const int ox = (w - dw) / 2, oy = (h - dh) / 2;
            l.rgba.assign((size_t)w * h * 4, 0);
            SwsContext* sws = sws_getContext(srcW, srcH, AV_PIX_FMT_RGBA, dw, dh, AV_PIX_FMT_RGBA, SWS_AREA, nullptr, nullptr, nullptr);
            if (!sws) l.bad = true;
            else {
                const uint8_t* in[1] = {src.data()};
                const int is[1] = {srcW * 4};
                uint8_t* out[1] = {l.rgba.data() + ((size_t)oy * w + ox) * 4};
                const int os[1] = {w * 4};
                sws_scale(sws, in, is, 0, srcH, out, os);
                sws_freeContext(sws);
                for (size_t i = 0; i < l.rgba.size(); i += 4) {
                    const int al = l.rgba[i + 3];
                    if (al == 0 || al == 255) continue;
                    for (int k = 0; k < 3; k++) l.rgba[i + k] = (uint8_t)std::min(255, (l.rgba[i + k] * 255 + al / 2) / al);
                }
            }
        }
        logos.push_back(std::move(l));
        return logos.back().bad ? nullptr : &logos.back();
    }
};

OverlayRenderer::OverlayRenderer() : p_(new Impl) {}
OverlayRenderer::~OverlayRenderer() = default;

bool OverlayRenderer::setFont(std::shared_ptr<const std::vector<uint8_t>> ttf) {
    p_->ok = false;
    p_->glyphs.clear();
    p_->ttf = std::move(ttf);
    if (!p_->ttf || p_->ttf->empty()) return false;
    const unsigned char* d = p_->ttf->data();
    const int off = stbtt_GetFontOffsetForIndex(d, 0);
    p_->ok = off >= 0 && stbtt_InitFont(&p_->font, d, off) != 0;
    return p_->ok;
}

bool OverlayRenderer::hasFont() const { return p_->ok; }

std::string OverlayRenderer::blockText(const Block& b, const Settings& s) {
    if (b.kind == BlockKind::Param) return (b.text.empty() ? std::string() : b.text + ": ") + paramValue(b.param, s);
    if (b.kind == BlockKind::Logo) return std::string();
    return b.text;
}

float OverlayRenderer::textWidth(const std::string& utf8, float px) { return p_->width(utf8, px); }

Rect OverlayRenderer::blockRect(const Block& b, const Settings& s, int W, int H) {
    Rect r;
    r.x = b.x * (float)W;
    r.y = b.y * (float)H;
    const float px = b.fontPx, pad = std::round(px * 0.25f);
    float asc = px, lh = px * 1.2f;
    if (p_->ok) p_->metrics(px, asc, lh);
    if (b.kind == BlockKind::Logo) { r.w = b.w * (float)W; r.h = b.h * (float)H; return r; }
    if (b.kind == BlockKind::Rolling) { r.w = b.w * (float)W; r.h = lh + 2 * pad; return r; }
    const std::vector<std::string> lines = splitLines(blockText(b, s));
    float w = 0;
    for (const std::string& l : lines) w = std::max(w, p_->width(l, px));
    r.w = std::max(px, w) + 2 * pad;
    r.h = lh * (float)lines.size() + 2 * pad;
    return r;
}

void OverlayRenderer::render(const Layout& l, const Settings& s, double t, int W, int H, std::vector<uint8_t>& rgba) {
    rgba.assign((size_t)W * H * 4, 0);
    const uint32_t bg = 0x99000000u;
    for (const Block& b : l.blocks) {
        const Rect r = blockRect(b, s, W, H);
        if (b.kind == BlockKind::Logo) {
            const Impl::Logo* lg = p_->logo(b.text, (int)std::lround(r.w), (int)std::lround(r.h));
            if (!lg) continue;
            const int x0 = (int)std::lround(r.x), y0 = (int)std::lround(r.y);
            for (int y = 0; y < lg->h; y++) {
                const int yy = y0 + y;
                if (yy < 0 || yy >= H) continue;
                for (int x = 0; x < lg->w; x++) {
                    const int xx = x0 + x;
                    if (xx < 0 || xx >= W) continue;
                    const uint8_t* sp = &lg->rgba[((size_t)y * lg->w + x) * 4];
                    if (!sp[3]) continue;
                    over(&rgba[((size_t)yy * W + xx) * 4], (uint32_t)sp[0] | ((uint32_t)sp[1] << 8) | ((uint32_t)sp[2] << 16) | 0xFF000000u, sp[3]);
                }
            }
            continue;
        }
        if (b.background) fillRect(rgba, W, H, r, bg);
        const float px = b.fontPx, pad = std::round(px * 0.25f);
        float asc = px, lh = px * 1.2f;
        if (p_->ok) p_->metrics(px, asc, lh);
        if (b.kind == BlockKind::Rolling) {
            // The text runs through the band and comes back when it has left it; the speed is in characters per second
            const float tw = p_->width(b.text, px);
            const size_t n = std::max<size_t>(1, cpCount(b.text));
            const float speed = std::max(0.1f, b.charsPerSec) * tw / (float)n;
            const float span = r.w + tw;
            const float off = span > 0 ? (float)std::fmod(t * speed, (double)span) : 0;
            const float tx = b.direction == 0 ? r.x - tw + off : r.x + r.w - off;
            p_->drawLine(rgba, W, H, b.text, px, tx, r.y + pad, b.color, r);
            continue;
        }
        const std::vector<std::string> lines = splitLines(blockText(b, s));
        for (size_t i = 0; i < lines.size(); i++) p_->drawLine(rgba, W, H, lines[i], px, r.x + pad, r.y + pad + lh * (float)i, b.color, r);
    }
}

void OverlayRenderer::blendYuv420(const uint8_t* rgba, int W, int H, uint8_t* y, int ys, uint8_t* u, int us, uint8_t* v, int vs) {
    for (int j = 0; j < H; j += 2) {
        for (int i = 0; i < W; i += 2) {
            int asum = 0, uacc = 0, vacc = 0;
            for (int dj = 0; dj < 2 && j + dj < H; dj++) {
                for (int di = 0; di < 2 && i + di < W; di++) {
                    const uint8_t* p = rgba + ((size_t)(j + dj) * W + i + di) * 4;
                    const int a = p[3];
                    if (!a) continue;
                    const int R = p[0], G = p[1], B = p[2];
                    const int Y = ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16;
                    uint8_t& py = y[(size_t)(j + dj) * ys + i + di];
                    py = (uint8_t)((py * (255 - a) + Y * a + 127) / 255);
                    asum += a;
                    uacc += a * (((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128);
                    vacc += a * (((112 * R - 94 * G - 18 * B + 128) >> 8) + 128);
                }
            }
            if (!asum) continue;
            const int a = asum / 4;   // mean coverage of the 2x2 block
            uint8_t& pu = u[(size_t)(j / 2) * us + i / 2];
            uint8_t& pv = v[(size_t)(j / 2) * vs + i / 2];
            pu = (uint8_t)((pu * (255 - a) + (uacc / asum) * a + 127) / 255);
            pv = (uint8_t)((pv * (255 - a) + (vacc / asum) * a + 127) / 255);
        }
    }
}

} // namespace dect2::vgen
