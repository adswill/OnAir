// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Full-disk loops (round 4 #24): the GIF encoder against its own reader on frames that fill the 12-bit table many times (noise, gradients,
// flat), sizes not a multiple of anything, one frame, the GIF89a header and the endless-loop block; the product key; a loop from PNG files.
#include "dect2/image/gif_loop.h"
#include "dect2/common/png.h"
#include "../common/test_util.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <random>
using namespace dect2::img;

// a plain PNG (RGBA, stored deflate blocks), only for the test's input files
static uint32_t crc(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; }
    return c;
}
static void be32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 3; i >= 0; i--) o.push_back((uint8_t)(v >> (8 * i))); }
static void chunk(std::vector<uint8_t>& o, const char* t, const std::vector<uint8_t>& d) {
    be32(o, (uint32_t)d.size());
    std::vector<uint8_t> td(t, t + 4); td.insert(td.end(), d.begin(), d.end());
    o.insert(o.end(), td.begin(), td.end());
    be32(o, crc(td.data(), td.size()) ^ 0xFFFFFFFFu);
}
static std::vector<uint8_t> testPng(const std::vector<uint8_t>& rgba, int w, int h) {
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; y++) { raw.push_back(0); raw.insert(raw.end(), rgba.begin() + (long)y * w * 4, rgba.begin() + (long)(y + 1) * w * 4); }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t i = 0; i < raw.size(); i += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - i);
        z.push_back(i + n == raw.size() ? 1 : 0); z.push_back((uint8_t)n); z.push_back((uint8_t)(n >> 8)); z.push_back((uint8_t)~n); z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + (long)i, raw.begin() + (long)(i + n));
    }
    be32(z, b << 16 | a);
    std::vector<uint8_t> o = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ih; be32(ih, (uint32_t)w); be32(ih, (uint32_t)h); ih.push_back(8); ih.push_back(6); ih.push_back(0); ih.push_back(0); ih.push_back(0);
    chunk(o, "IHDR", ih); chunk(o, "IDAT", z); chunk(o, "IEND", {});
    return o;
}

int main() {
    std::mt19937 rng(5);
    const int w = 333, h = 211;
    std::vector<std::vector<uint8_t>> f(4, std::vector<uint8_t>((size_t)w * h));
    for (size_t i = 0; i < f[0].size(); i++) { f[0][i] = (uint8_t)rng(); f[1][i] = (uint8_t)((i % w) * 255 / w); f[2][i] = 128; f[3][i] = (uint8_t)((i / w + i % 7) & 0xff); }
    const auto g = gifEncodeGray(f, w, h, 20);
    CHECK(std::string(g.begin(), g.begin() + 6) == "GIF89a" && g.back() == 0x3B && std::string(g.begin() + 13 + 768 + 3, g.begin() + 13 + 768 + 14) == "NETSCAPE2.0", "header, loop block, trailer");
    std::vector<std::vector<uint8_t>> back;
    int bw = 0, bh = 0;
    CHECK(gifDecodeGray(g, back, bw, bh) && bw == w && bh == h && back.size() == 4, "decoded %zu frames %dx%d", back.size(), bw, bh);
    for (size_t k = 0; k < back.size() && k < 4; k++) CHECK(back[k] == f[k], "frame %zu exact", k);
    std::vector<std::vector<uint8_t>> one(1, std::vector<uint8_t>(1, 77));
    CHECK(gifDecodeGray(gifEncodeGray(one, 1, 1, 5), back, bw, bh) && back.size() == 1 && back[0][0] == 77, "1x1 frame");
    std::string err;
    CHECK(!gifWriteGray("/nonexistent/x/y.gif", {}, 0, 0, 5, err), "no frames refused");
    CHECK(loopProductKey("/a/b/IMG_FD_023_IR105_20190907_000000.lrit") == "IMG_FD__IR__.lrit" && loopProductKey("IMG_FD_024_IR105_20190907_001000.lrit") == loopProductKey("IMG_FD_023_IR105_20190907_000000.lrit") &&
          loopProductKey("IMG_FD_023_SW038_20190907_000000.lrit") != loopProductKey("IMG_FD_023_IR105_20190907_000000.lrit"), "product key");
    // a loop from PNG files (a plain PNG written by the test)
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "onair_test_goes_loop";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::vector<std::string> pngs;
    for (int k = 0; k < 3; k++) {
        std::vector<uint8_t> rgba((size_t)400 * 300 * 4);
        for (size_t i = 0; i < rgba.size(); i += 4) rgba[i] = rgba[i + 1] = rgba[i + 2] = (uint8_t)(k * 80), rgba[i + 3] = 255;
        const std::vector<uint8_t> png = testPng(rgba, 400, 300);
        const std::string p = (dir / ("f" + std::to_string(k) + ".png")).string();
        std::ofstream(p, std::ios::binary).write((const char*)png.data(), (std::streamsize)png.size());
        pngs.push_back(p);
    }
    const std::string out = (dir / "loop.gif").string();
    CHECK(gifLoopFromPngs(pngs, out, 200, 25, err), "loop: %s", err.c_str());
    std::ifstream in(out, std::ios::binary);
    std::vector<uint8_t> gb((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();   // Windows cannot delete a file that is still open
    CHECK(gifDecodeGray(gb, back, bw, bh) && bw == 200 && bh == 150 && back.size() == 3 && back[0][0] == 0 && back[2][5000] == 160, "loop frames scaled, oldest first");
    CHECK(!gifLoopFromPngs({pngs[0]}, out, 200, 25, err), "one picture is no loop");
    std::error_code cleanup;
    std::filesystem::remove_all(dir, cleanup);
    return testSummary("goes_loop");
}
