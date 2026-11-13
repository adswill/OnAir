// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT picture helpers (see kait_image.h).
#include "dect2/kait/kait_image.h"
#include "dect2/common/png.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dect2::kait {

namespace {
bool readAll(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t b[65536];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) {
        out.insert(out.end(), b, b + n);
        if (out.size() > (size_t)256 << 20) { fclose(f); return false; }
    }
    fclose(f);
    return true;
}
bool ppmInt(const std::vector<uint8_t>& d, size_t& pos, int& v) {
    for (;;) {
        while (pos < d.size() && (d[pos] == ' ' || d[pos] == '\t' || d[pos] == '\r' || d[pos] == '\n')) pos++;
        if (pos < d.size() && d[pos] == '#') { while (pos < d.size() && d[pos] != '\n') pos++; continue; }
        break;
    }
    if (pos >= d.size() || d[pos] < '0' || d[pos] > '9') return false;
    long x = 0;
    while (pos < d.size() && d[pos] >= '0' && d[pos] <= '9') { x = x * 10 + (d[pos] - '0'); pos++; if (x > 1000000) return false; }
    v = (int)x;
    return true;
}
}  // namespace

bool loadPicture(const std::string& path, Image& out, std::string& err) {
    std::vector<uint8_t> d;
    if (!readAll(path, d) || d.size() < 16) { err = "cannot read the picture file"; return false; }
    if (d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        if (!decodePng(d.data(), d.size(), w, h, rgba) || w < 1 || h < 1 || w > 8191 || h > 8191) { err = "the PNG file cannot be decoded"; return false; }
        out.w = w; out.h = h;
        out.rgb.resize((size_t)w * h * 3);
        for (size_t i = 0; i < (size_t)w * h; i++) std::memcpy(&out.rgb[3 * i], &rgba[4 * i], 3);
        return true;
    }
    if (d[0] == 'P' && d[1] == '6') {
        size_t pos = 2;
        int w = 0, h = 0, mx = 0;
        if (!ppmInt(d, pos, w) || !ppmInt(d, pos, h) || !ppmInt(d, pos, mx) || mx != 255 || w < 1 || h < 1 || w > 8191 || h > 8191) { err = "the PPM file is not a 255-level P6 picture"; return false; }
        pos++;   // one whitespace byte after the header
        if (d.size() < pos + (size_t)w * h * 3) { err = "the PPM file is cut short"; return false; }
        out.w = w; out.h = h;
        out.rgb.assign(d.begin() + (long)pos, d.begin() + (long)(pos + (size_t)w * h * 3));
        return true;
    }
    err = "KAIT reads PNG and PPM files here (the app converts other formats)";
    return false;
}

Image fitPicture(const Image& in, int maxW, int maxH) {
    if (in.w <= maxW && in.h <= maxH) return in;
    const double sc = std::min((double)maxW / in.w, (double)maxH / in.h);
    Image o;
    o.w = std::max(1, (int)(in.w * sc));
    o.h = std::max(1, (int)(in.h * sc));
    o.rgb.resize((size_t)o.w * o.h * 3);
    for (int y = 0; y < o.h; y++) {
        const int y0 = (int)((long long)y * in.h / o.h), y1 = std::max(y0 + 1, (int)((long long)(y + 1) * in.h / o.h));
        for (int x = 0; x < o.w; x++) {
            const int x0 = (int)((long long)x * in.w / o.w), x1 = std::max(x0 + 1, (int)((long long)(x + 1) * in.w / o.w));
            long s[3] = {0, 0, 0};
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++)
                    for (int c = 0; c < 3; c++) s[c] += in.rgb[3 * ((size_t)yy * in.w + (size_t)xx) + (size_t)c];
            const long cnt = (long)(y1 - y0) * (x1 - x0);
            for (int c = 0; c < 3; c++) o.rgb[3 * ((size_t)y * o.w + (size_t)x) + (size_t)c] = (uint8_t)((s[c] + cnt / 2) / cnt);
        }
    }
    return o;
}

bool writePpm(const std::string& path, const Image& img) {
    if (img.w < 1 || img.h < 1 || img.rgb.size() < (size_t)img.w * img.h * 3) return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", img.w, img.h);
    const bool ok = fwrite(img.rgb.data(), 1, (size_t)img.w * img.h * 3, f) == (size_t)img.w * img.h * 3;
    fclose(f);
    return ok;
}

} // namespace dect2::kait
