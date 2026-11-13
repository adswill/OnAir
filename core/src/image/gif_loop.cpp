// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Animated grey GIF and full-disk loops: see gif_loop.h. The LZW coder follows the GIF89a specification (variable code size 9..12 bits,
// clear and end codes, a clear when the table is full).
#include "dect2/image/gif_loop.h"
#include "dect2/common/png.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace dect2::img {

namespace {
struct Bits {
    std::vector<uint8_t>& out; uint32_t acc = 0; int n = 0;
    std::vector<uint8_t> block;
    explicit Bits(std::vector<uint8_t>& o) : out(o) {}
    void put(uint32_t code, int size) {
        acc |= code << n; n += size;
        while (n >= 8) { byte((uint8_t)(acc & 0xff)); acc >>= 8; n -= 8; }
    }
    void byte(uint8_t b) { block.push_back(b); if (block.size() == 255) flushBlock(); }
    void flushBlock() { if (block.empty()) return; out.push_back((uint8_t)block.size()); out.insert(out.end(), block.begin(), block.end()); block.clear(); }
    void finish() { if (n > 0) byte((uint8_t)(acc & 0xff)); acc = 0; n = 0; flushBlock(); out.push_back(0); }
};
void lzw(std::vector<uint8_t>& out, const uint8_t* px, size_t n) {
    const int minSize = 8;
    out.push_back(minSize);
    Bits b(out);
    const uint32_t clear = 256, end = 257;
    std::unordered_map<uint32_t, uint32_t> dict;   // (prefix code << 8 | byte) -> code
    dict.reserve(8192);
    uint32_t next = 258;
    int size = 9;
    b.put(clear, size);
    if (n == 0) { b.put(end, size); b.finish(); return; }
    uint32_t cur = px[0];
    for (size_t i = 1; i < n; i++) {
        const uint32_t k = cur << 8 | px[i];
        auto it = dict.find(k);
        if (it != dict.end()) { cur = it->second; continue; }
        b.put(cur, size);
        if (next < 4096) {
            dict[k] = next++;
            if (next > (1u << size) && size < 12) size++;
        } else { b.put(clear, size); dict.clear(); next = 258; size = 9; }
        cur = px[i];
    }
    b.put(cur, size);
    b.put(end, size);
    b.finish();
}
void le16(std::vector<uint8_t>& o, int v) { o.push_back((uint8_t)(v & 0xff)); o.push_back((uint8_t)(v >> 8)); }
}  // namespace

std::vector<uint8_t> gifEncodeGray(const std::vector<std::vector<uint8_t>>& frames, int w, int h, int delayCs) {
    std::vector<uint8_t> o;
    const char* hdr = "GIF89a";
    o.insert(o.end(), hdr, hdr + 6);
    le16(o, w); le16(o, h);
    o.push_back(0xF7); o.push_back(0); o.push_back(0);   // global table of 256 colours, 8 bits
    for (int i = 0; i < 256; i++) { o.push_back((uint8_t)i); o.push_back((uint8_t)i); o.push_back((uint8_t)i); }
    const uint8_t loop[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01, 0x00, 0x00, 0x00};
    o.insert(o.end(), loop, loop + sizeof loop);
    for (const auto& f : frames) {
        o.push_back(0x21); o.push_back(0xF9); o.push_back(4); o.push_back(0x04); le16(o, delayCs); o.push_back(0); o.push_back(0);   // keep the frame
        o.push_back(0x2C); le16(o, 0); le16(o, 0); le16(o, w); le16(o, h); o.push_back(0);
        lzw(o, f.data(), std::min(f.size(), (size_t)w * h));
    }
    o.push_back(0x3B);
    return o;
}

bool gifWriteGray(const std::string& path, const std::vector<std::vector<uint8_t>>& frames, int w, int h, int delayCs, std::string& err) {
    if (w <= 0 || h <= 0 || w > 65535 || h > 65535 || frames.empty()) { err = "no frames or a bad size"; return false; }
    for (const auto& f : frames) if (f.size() < (size_t)w * h) { err = "a frame is too small"; return false; }
    const std::vector<uint8_t> g = gifEncodeGray(frames, w, h, delayCs);
    const std::filesystem::path p(reinterpret_cast<const char8_t*>(path.c_str()));
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    const std::filesystem::path tmp = p.string() + ".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f.write((const char*)g.data(), (std::streamsize)g.size()); if (!f) { err = "cannot write " + path; return false; } }
    std::filesystem::rename(tmp, p, ec);
    if (ec) { err = ec.message(); return false; }
    return true;
}

bool gifDecodeGray(const std::vector<uint8_t>& g, std::vector<std::vector<uint8_t>>& frames, int& w, int& h) {
    if (g.size() < 13 + 768 || std::string(g.begin(), g.begin() + 6) != "GIF89a") return false;
    w = g[6] | g[7] << 8; h = g[8] | g[9] << 8;
    size_t i = 13 + 768;
    frames.clear();
    while (i < g.size()) {
        const uint8_t t = g[i++];
        if (t == 0x3B) return true;
        if (t == 0x21) { i++; while (i < g.size() && g[i]) i += g[i] + 1; i++; continue; }
        if (t != 0x2C || i + 9 > g.size()) return false;
        i += 9;
        const int minSize = g[i++];
        std::vector<uint8_t> data;
        while (i < g.size() && g[i]) { data.insert(data.end(), g.begin() + (long)i + 1, g.begin() + (long)i + 1 + g[i]); i += g[i] + 1; }
        i++;
        // LZW decode
        const uint32_t clear = 1u << minSize, end = clear + 1;
        std::vector<std::vector<uint8_t>> tab;
        auto reset = [&] { tab.assign(clear + 2, {}); for (uint32_t c = 0; c < clear; c++) tab[c] = {(uint8_t)c}; };
        reset();
        int size = minSize + 1;
        uint32_t acc = 0; int n = 0; size_t bi = 0;
        std::vector<uint8_t> out, prev;
        bool havePrev = false;
        for (;;) {
            while (n < size && bi < data.size()) { acc |= (uint32_t)data[bi++] << n; n += 8; }
            if (n < size) break;
            const uint32_t code = acc & ((1u << size) - 1); acc >>= size; n -= size;
            if (code == clear) { reset(); size = minSize + 1; havePrev = false; continue; }
            if (code == end) break;
            std::vector<uint8_t> e;
            if (code < tab.size()) e = tab[code];
            else if (havePrev && code == tab.size()) { e = prev; e.push_back(prev[0]); }
            else return false;
            out.insert(out.end(), e.begin(), e.end());
            if (havePrev && tab.size() < 4096) { std::vector<uint8_t> ne = prev; ne.push_back(e[0]); tab.push_back(ne); if (tab.size() == (1u << size) && size < 12) size++; }
            prev = e; havePrev = true;
        }
        if (out.size() < (size_t)w * h) return false;
        out.resize((size_t)w * h);
        frames.push_back(out);
    }
    return false;
}

std::string loopProductKey(const std::string& name) {
    std::string base = name;
    const size_t sl = base.find_last_of("/\\");
    if (sl != std::string::npos) base = base.substr(sl + 1);
    std::string o;
    for (unsigned char c : base) if (!std::isdigit(c)) o += (char)c;
    return o;
}

bool gifLoopFromPngs(const std::vector<std::string>& pngs, const std::string& out, int maxSide, int delayCs, std::string& err) {
    std::vector<std::vector<uint8_t>> frames;
    int W = 0, H = 0, ow = 0, oh = 0;
    // the newest decides the size
    for (auto it = pngs.rbegin(); it != pngs.rend(); ++it) {
        std::ifstream f(std::filesystem::path(reinterpret_cast<const char8_t*>(it->c_str())), std::ios::binary);
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        if (b.empty() || !decodePng(b.data(), b.size(), w, h, rgba)) continue;
        if (!W) {
            W = w; H = h;
            const double s = std::min(1.0, (double)maxSide / std::max(w, h));
            ow = std::max(1, (int)std::lround(w * s)); oh = std::max(1, (int)std::lround(h * s));
        }
        if (w != W || h != H) continue;
        std::vector<uint8_t> g((size_t)ow * oh);
        for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {   // box average of the source pixels under each output pixel
            const int x0 = x * W / ow, x1 = std::max(x0 + 1, (x + 1) * W / ow), y0 = y * H / oh, y1 = std::max(y0 + 1, (y + 1) * H / oh);
            uint32_t sum = 0, cnt = 0;
            for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) {
                const uint8_t* p = &rgba[((size_t)yy * W + xx) * 4];
                sum += (uint32_t)(p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8; cnt++;
            }
            g[(size_t)y * ow + x] = (uint8_t)(sum / std::max(1u, cnt));
        }
        frames.insert(frames.begin(), std::move(g));
    }
    if (frames.size() < 2) { err = "fewer than two pictures of this product"; return false; }
    return gifWriteGray(out, frames, ow, oh, delayCs, err);
}

}  // namespace dect2::img
