// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Image enhancement: conversions, shared filters, the pipeline, presets, parameter text, before/after split and the quality measures.
#include "enhance_util.h"
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace dect2::image {
using namespace detail;

namespace detail {

void convolvePlane(const float* in, float* out, int w, int h, const std::vector<float>& k) {
    const int r = (int)k.size() / 2;
    std::vector<float> tmp((size_t)w * h);
    forRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const float* row = in + (size_t)y * w;
            float* t = tmp.data() + (size_t)y * w;
            for (int x = 0; x < w; x++) {
                float s = 0;
                if (x >= r && x + r < w) for (int i = -r; i <= r; i++) s += k[(size_t)(i + r)] * row[x + i];
                else for (int i = -r; i <= r; i++) s += k[(size_t)(i + r)] * row[clampi(x + i, 0, w - 1)];
                t[x] = s;
            }
        }
    });
    forRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            float* o = out + (size_t)y * w;
            for (int x = 0; x < w; x++) o[x] = 0;
            for (int i = -r; i <= r; i++) {
                const float kv = k[(size_t)(i + r)];
                const float* t = tmp.data() + (size_t)clampi(y + i, 0, h - 1) * w;
                for (int x = 0; x < w; x++) o[x] += kv * t[x];
            }
        }
    });
}

void boxPlane(const float* in, float* out, int w, int h, int r) {
    std::vector<float> k((size_t)(2 * r + 1), 1.f / (2 * r + 1));
    convolvePlane(in, out, w, h, k);
}

std::vector<float> lumaPlane(const ImageF& im) {
    const size_t n = (size_t)im.w * im.h;
    if (im.ch == 1) return std::vector<float>(im.d.begin(), im.d.begin() + (ptrdiff_t)n);
    std::vector<float> y(n);
    const float *r = im.plane(0), *g = im.plane(1), *b = im.plane(2);
    for (size_t i = 0; i < n; i++) y[i] = 0.299f * r[i] + 0.587f * g[i] + 0.114f * b[i];
    return y;
}

void applyLuma(ImageF& im, const std::vector<float>& oldY, const std::vector<float>& newY) {
    const size_t n = (size_t)im.w * im.h;
    if (im.ch == 1) { for (size_t i = 0; i < n; i++) im.d[i] = clamp01(newY[i]); return; }
    for (int c = 0; c < 3; c++) {
        float* p = im.plane(c);
        for (size_t i = 0; i < n; i++) {
            const float o = oldY[i], nv = newY[i];
            p[i] = clamp01(o > 0.02f ? p[i] * (nv / o) : p[i] + (nv - o));
        }
    }
}

}  // namespace detail

// ---------------------------------------------------------------- conversions

ImageF fromRgba(const uint8_t* rgba, int w, int h, bool forceGrey) {
    const size_t n = (size_t)w * h;
    bool grey = forceGrey;
    if (!grey) {
        grey = true;
        for (size_t i = 0; i < n && grey; i++) if (rgba[i * 4] != rgba[i * 4 + 1] || rgba[i * 4] != rgba[i * 4 + 2]) grey = false;
    }
    ImageF im(w, h, grey ? 1 : 3);
    if (grey) {
        for (size_t i = 0; i < n; i++) im.d[i] = (0.299f * rgba[i * 4] + 0.587f * rgba[i * 4 + 1] + 0.114f * rgba[i * 4 + 2]) / 255.f;
    } else {
        for (int c = 0; c < 3; c++) { float* p = im.plane(c); for (size_t i = 0; i < n; i++) p[i] = rgba[i * 4 + (size_t)c] / 255.f; }
    }
    return im;
}

ImageF fromGrey(const uint8_t* g, int w, int h) {
    ImageF im(w, h, 1);
    for (size_t i = 0; i < im.d.size(); i++) im.d[i] = g[i] / 255.f;
    return im;
}

std::vector<uint8_t> toRgba(const ImageF& im) {
    const size_t n = (size_t)im.w * im.h;
    std::vector<uint8_t> o(n * 4);
    for (size_t i = 0; i < n; i++) {
        for (int c = 0; c < 3; c++) o[i * 4 + (size_t)c] = (uint8_t)std::lround(clamp01(im.plane(im.ch == 3 ? c : 0)[i]) * 255.f);
        o[i * 4 + 3] = 255;
    }
    return o;
}

ImageF luma(const ImageF& rgb) {
    ImageF y(rgb.w, rgb.h, 1);
    y.d = lumaPlane(rgb);
    return y;
}

void toYCbCr(ImageF& im) {
    if (im.ch != 3) return;
    const size_t n = (size_t)im.w * im.h;
    float *r = im.plane(0), *g = im.plane(1), *b = im.plane(2);
    for (size_t i = 0; i < n; i++) {
        const float y = 0.299f * r[i] + 0.587f * g[i] + 0.114f * b[i];
        const float cb = 0.5f + (b[i] - y) * 0.564f, cr = 0.5f + (r[i] - y) * 0.713f;
        r[i] = y; g[i] = cb; b[i] = cr;
    }
}

void fromYCbCr(ImageF& im) {
    if (im.ch != 3) return;
    const size_t n = (size_t)im.w * im.h;
    float *p0 = im.plane(0), *p1 = im.plane(1), *p2 = im.plane(2);
    for (size_t i = 0; i < n; i++) {
        const float y = p0[i], cb = p1[i] - 0.5f, cr = p2[i] - 0.5f;
        const float r = y + 1.403f * cr, b = y + 1.773f * cb, g = (y - 0.299f * r - 0.114f * b) / 0.587f;
        p0[i] = clamp01(r); p1[i] = clamp01(g); p2[i] = clamp01(b);
    }
}

void gaussian(ImageF& im, float sigma) {
    if (sigma <= 0.05f) return;
    const auto k = gaussKernel(sigma);
    std::vector<float> o((size_t)im.w * im.h);
    for (int c = 0; c < im.ch; c++) { convolvePlane(im.plane(c), o.data(), im.w, im.h, k); std::copy(o.begin(), o.end(), im.plane(c)); }
}

// ---------------------------------------------------------------- kinds and presets

Kind kindForSource(const std::string& s0) {
    std::string s;
    for (char c : s0) s += (char)std::tolower((unsigned char)c);
    auto has = [&](const char* t) { return s.find(t) != std::string::npos; };
    if (has("fax")) return Kind::Fax;
    if (has("sstv")) return Kind::Sstv;
    if (has("apt")) return Kind::Apt;
    if (has("meteor") || has("lrpt") || has("goes") || has("hrit") || has("lrit") || has("elektro") || has("meteosat") || has("hrpt") ||
        has("himawari") || has("fengyun"))
        return Kind::Satellite;
    if (has("tv") || has("camera") || has("fpv") || has("frame")) return Kind::Tv;
    if (has("drm") || has("dab") || has("slide") || has("kait") || has("ssdv") || has("wenet") || has("picture")) return Kind::Slide;
    return Kind::Generic;
}

const char* presetName(Preset p) {
    switch (p) {
    case Preset::Auto: return "Auto";
    case Preset::Chart: return "Chart";
    case Preset::Photo: return "Photo";
    case Preset::Satellite: return "Satellite";
    case Preset::ColourSstv: return "Colour SSTV";
    case Preset::Custom: return "Custom";
    }
    return "?";
}

// how bimodal the luma is: Otsu between-class variance over the total variance (1 = two spikes)
static double bimodality(const ImageF& im) {
    const auto y = lumaPlane(im);
    double hist[256] = {};
    for (float v : y) hist[clampi((int)(v * 255.f + 0.5f), 0, 255)]++;
    const double n = (double)y.size();
    double mu = 0;
    for (int i = 0; i < 256; i++) mu += i * hist[i];
    mu /= n;
    double var = 0;
    for (int i = 0; i < 256; i++) var += hist[i] * (i - mu) * (i - mu);
    var /= n;
    if (var < 1e-9) return 0;
    double w0 = 0, s0 = 0, best = 0;
    for (int t = 0; t < 255; t++) {
        w0 += hist[t]; s0 += t * hist[t];
        const double w1 = n - w0;
        if (w0 < 1 || w1 < 1) continue;
        const double m0 = s0 / w0, m1 = (mu * n - s0) / w1;
        best = std::max(best, w0 / n * w1 / n * (m0 - m1) * (m0 - m1));
    }
    return best / var;
}

Preset autoPreset(const ImageF& im, Kind k) {
    switch (k) {
    case Kind::Fax: return im.ch == 1 && bimodality(im) > 0.8 ? Preset::Chart : Preset::Photo;
    case Kind::Sstv: return im.ch == 3 ? Preset::ColourSstv : Preset::Photo;
    case Kind::Apt: case Kind::Satellite: return Preset::Satellite;
    default: return im.ch == 1 && bimodality(im) > 0.9 ? Preset::Chart : Preset::Photo;
    }
}

Params presetParams(Preset p, Kind k) {
    Params q;
    const bool lineScan = k == Kind::Fax || k == Kind::Sstv;   // pictures sent line by line against a free running clock
    switch (p) {
    case Preset::Auto: case Preset::Custom: break;
    case Preset::Chart:   // geometry + impulses + threshold-aware contrast + text sharpening; never blurs
        q.autoSlant = q.autoPhase = lineScan; q.repair = true; q.impulse = true; q.impulseThr = 0.3f;
        q.chart = true; q.chartStrength = 0.6f; q.sharpen = 0.5f; q.sharpenRadius = 0.7f;
        break;
    case Preset::Photo:
        q.autoSlant = lineScan; q.autoPhase = k == Kind::Fax; q.repair = lineScan; q.impulse = true;
        q.denoise = 25; q.levels = true; q.sharpen = 0.5f; q.deinterlace = k == Kind::Tv; q.greyWorld = false;
        break;
    case Preset::Satellite:
        q.repair = true; q.impulse = true; q.destripe = true; q.denoise = 15; q.levels = true; q.loPct = 0.3f; q.hiPct = 99.7f;
        q.clahe = 2.0f; q.sharpen = 0.3f;
        break;
    case Preset::ColourSstv:
        q.autoSlant = true; q.repair = true; q.impulse = true; q.chroma = true; q.greyWorld = true; q.denoise = 20;
        q.levels = true; q.sharpen = 0.4f;
        break;
    }
    return q;
}

// ---------------------------------------------------------------- the pipeline

ImageF enhance(const ImageF& in, const Params& p, Report* rep, bool preview) {
    const auto t0 = std::chrono::steady_clock::now();
    Report r;
    ImageF im = in;
    if (im.empty()) { if (rep) *rep = r; return im; }
    // geometry
    double slant = p.slant;
    if (p.autoSlant && im.h >= 16) {
        const ImageF y = im.ch == 1 ? im : luma(im);
        const SlantFit f = estimateSlant(y);
        r.slantPx = f.pxPerLine;
        slant += f.pxPerLine;
    }
    if (std::abs(slant) > 1e-6) im = deslant(im, slant, 0, true);
    int phase = p.phase;
    if (p.autoPhase) { r.phase = findPhase(im); phase += r.phase; }
    if (wrapi(phase, im.w)) im = rotateColumns(im, wrapi(phase, im.w));
    if (p.repair) r.repaired = repairLines(im, p.repairCorr);
    if (p.registerRows && !preview) registerLines(im);
    if (p.ioc > 0) im = iocWidth(im, p.ioc);
    // noise
    if (p.impulse) r.impulses = switchingMedian(im, p.impulseThr);
    if (p.deinterlace) r.deint = deinterlace(im);
    if (p.destripe) destripe(im);
    if (p.deband) deband(im);
    if (p.denoise > 0) im = denoise(im, p.denoise, p.nlm && !preview);
    // colour
    if (im.ch == 3 && p.chroma) r.chromaShift = chromaFix(im);
    if (im.ch == 3 && p.greyWorld) greyWorld(im);
    // tone
    if (p.chart) chartLevels(im, p.chartStrength);
    else if (p.levels) autoLevels(im, p.loPct, p.hiPct);
    if (std::abs(p.gamma - 1.f) > 1e-3f) gamma(im, p.gamma);
    if (p.clahe >= 1.f) clahe(im, p.clahe);
    // detail
    if (p.upscale && !preview && (size_t)im.w * im.h <= 4096u * 4096u) im = lanczos2x(im, 0.f);
    if (p.sharpen > 0) unsharp(im, p.sharpenRadius * (p.upscale && !preview ? 2.f : 1.f), p.sharpen);
    if (p.palette != Palette::None) im = applyPalette(im, p.palette);
    r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (rep) *rep = r;
    return im;
}

std::vector<uint8_t> enhanceRgba(const uint8_t* rgba, int w, int h, const Params& p, int& ow, int& oh, Report* rep, bool preview) {
    ImageF out = enhance(fromRgba(rgba, w, h), p, rep, preview);
    ow = out.w; oh = out.h;
    return toRgba(out);
}

void splitView(const uint8_t* before, const uint8_t* after, int w, int h, float splitX, uint8_t* out) {
    const int sx = clampi((int)std::lround(clamp01(splitX) * w), 0, w);
    for (int y = 0; y < h; y++) {
        const size_t o = (size_t)y * w * 4;
        if (sx) std::memcpy(out + o, before + o, (size_t)sx * 4);
        if (sx < w) std::memcpy(out + o + (size_t)sx * 4, after + o + (size_t)sx * 4, (size_t)(w - sx) * 4);
        if (sx > 0 && sx < w) { uint8_t* d = out + o + (size_t)sx * 4; d[0] = 255; d[1] = 220; d[2] = 60; }   // the split line
    }
}

// ---------------------------------------------------------------- parameter text

namespace {
struct Field { const char* key; int type; size_t off; };   // type 0 bool, 1 int, 2 float
#define F_(t, m) Field{#m, t, offsetof(Params, m)}
const Field kFields[] = {
    F_(0, autoSlant), F_(2, slant), F_(0, autoPhase), F_(1, phase), F_(0, repair), F_(2, repairCorr), F_(0, registerRows), F_(1, ioc),
    F_(0, impulse), F_(2, impulseThr), F_(0, destripe), F_(0, deband), F_(1, denoise), F_(0, nlm), F_(0, chart), F_(2, chartStrength),
    F_(0, levels), F_(2, loPct), F_(2, hiPct), F_(2, gamma), F_(2, clahe), F_(0, chroma), F_(0, greyWorld), F_(0, deinterlace),
    F_(2, sharpen), F_(2, sharpenRadius), F_(0, upscale)};
#undef F_
}

std::string paramsToString(const Params& p) {
    std::ostringstream o;
    const char* b = reinterpret_cast<const char*>(&p);
    for (const Field& f : kFields) {
        o << f.key << '=';
        if (f.type == 0) o << (*reinterpret_cast<const bool*>(b + f.off) ? 1 : 0);
        else if (f.type == 1) o << *reinterpret_cast<const int*>(b + f.off);
        else o << *reinterpret_cast<const float*>(b + f.off);
        o << ';';
    }
    o << "palette=" << (int)p.palette << ';';
    return o.str();
}

Params paramsFromString(const std::string& s, const Params& base) {
    Params p = base;
    char* b = reinterpret_cast<char*>(&p);
    std::stringstream ss(s);
    std::string kv;
    while (std::getline(ss, kv, ';')) {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        if (k == "palette") { p.palette = (Palette)clampi(atoi(v.c_str()), 0, 3); continue; }
        for (const Field& f : kFields)
            if (k == f.key) {
                if (f.type == 0) *reinterpret_cast<bool*>(b + f.off) = atoi(v.c_str()) != 0;
                else if (f.type == 1) *reinterpret_cast<int*>(b + f.off) = atoi(v.c_str());
                else *reinterpret_cast<float*>(b + f.off) = (float)atof(v.c_str());
            }
    }
    return p;
}

// ---------------------------------------------------------------- measures

double psnr(const ImageF& a, const ImageF& b) {
    if (a.w != b.w || a.h != b.h || a.ch != b.ch || a.d.empty()) return 0;
    double s = 0;
    for (size_t i = 0; i < a.d.size(); i++) { const double e = (double)a.d[i] - b.d[i]; s += e * e; }
    s /= (double)a.d.size();
    return s <= 1e-12 ? 120.0 : 10.0 * std::log10(1.0 / s);
}

double ssim(const ImageF& a, const ImageF& b) {
    if (a.w != b.w || a.h != b.h || a.d.empty()) return 0;
    const int w = a.w, h = a.h;
    const size_t n = (size_t)w * h;
    std::vector<float> x = lumaPlane(a), y = lumaPlane(b), xx(n), yy(n), xy(n);
    for (size_t i = 0; i < n; i++) { xx[i] = x[i] * x[i]; yy[i] = y[i] * y[i]; xy[i] = x[i] * y[i]; }
    const auto k = gaussKernel(1.5f);
    std::vector<float> mx(n), my(n), sxx(n), syy(n), sxy(n);
    convolvePlane(x.data(), mx.data(), w, h, k); convolvePlane(y.data(), my.data(), w, h, k);
    convolvePlane(xx.data(), sxx.data(), w, h, k); convolvePlane(yy.data(), syy.data(), w, h, k); convolvePlane(xy.data(), sxy.data(), w, h, k);
    const double C1 = 0.01 * 0.01, C2 = 0.03 * 0.03;
    double s = 0;
    for (size_t i = 0; i < n; i++) {
        const double ux = mx[i], uy = my[i];
        const double vx = sxx[i] - ux * ux, vy = syy[i] - uy * uy, cxy = sxy[i] - ux * uy;
        s += ((2 * ux * uy + C1) * (2 * cxy + C2)) / ((ux * ux + uy * uy + C1) * (vx + vy + C2));
    }
    return s / (double)n;
}

}  // namespace dect2::image
