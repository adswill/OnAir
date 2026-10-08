// PNG decoder (map tiles on Windows and Linux): every colour type and bit depth, all five filters, Adam7, tRNS, broken files.
// The PNGs are made here with stored deflate blocks, so the test needs no image library. Optional argument: a folder of real PNG
// tiles that must all decode (e.g. the macOS map cache).
#include "dect2/inflate.h"
#include "dect2/png.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s)); }

static void chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
    put32(png, (uint32_t)data.size());
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    png.insert(png.end(), td.begin(), td.end());
    put32(png, crc32Ieee(td.data(), td.size()));
}

static std::vector<uint8_t> zlibStored(const std::vector<uint8_t>& d) {
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(65535, d.size() - pos);
        z.push_back(pos + n == d.size() ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8)); z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), d.begin() + (ptrdiff_t)pos, d.begin() + (ptrdiff_t)(pos + n));
        pos += n;
    } while (pos < d.size());
    uint32_t a = 1, b = 0;
    for (uint8_t c : d) { a = (a + c) % 65521; b = (b + a) % 65521; }
    put32(z, b << 16 | a);
    return z;
}

static int paethE(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

struct Img {
    int ctype, depth, w, h, chans;
    bool interlace;
    std::vector<unsigned> s;           // samples, w*h*chans, at the image's depth
    std::vector<uint8_t> plte, trns;
    unsigned at(int x, int y, int c) const { return s[((size_t)y * w + x) * chans + c]; }
};

static Img makeImg(int ctype, int depth, int w, int h, bool interlace, bool withTrns, int seed) {
    Img m{ctype, depth, w, h, ctype == 2 ? 3 : ctype == 4 ? 2 : ctype == 6 ? 4 : 1, interlace, {}, {}, {}};
    const unsigned max = (1u << depth) - 1;
    const unsigned palN = ctype == 3 ? std::min(256u, max + 1) : 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < m.chans; c++) {
                const unsigned v = (unsigned)(x * 7 + y * 13 + c * 29 + seed) * (depth == 16 ? 769u : 1u);
                m.s.push_back(ctype == 3 ? v % palN : v & max);
            }
    if (ctype == 3) {
        for (unsigned i = 0; i < palN; i++) { m.plte.push_back(uint8_t(i * 37)); m.plte.push_back(uint8_t(i * 91 + 5)); m.plte.push_back(uint8_t(i * 151 + 9)); }
        if (withTrns) for (unsigned i = 0; i < std::min(palN, 3u); i++) m.trns.push_back(uint8_t(i * 100));   // shorter than the palette: the rest opaque
    } else if (withTrns && ctype == 0) {
        const unsigned k = m.at(1 % w, 0, 0);
        m.trns = {uint8_t(k >> 8), uint8_t(k)};
    } else if (withTrns && ctype == 2) {
        for (int c = 0; c < 3; c++) { const unsigned k = m.at(0, 0, c); m.trns.push_back(uint8_t(k >> 8)); m.trns.push_back(uint8_t(k)); }
    }
    return m;
}

static std::vector<uint8_t> encode(const Img& m) {
    const size_t bits = (size_t)m.chans * m.depth, bpp = std::max<size_t>(1, bits / 8);
    std::vector<uint8_t> filtered;
    struct P { int x0, y0, dx, dy; };
    std::vector<P> passes = m.interlace ? std::vector<P>{{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}}
                                        : std::vector<P>{{0, 0, 1, 1}};
    int rowNo = 0;
    for (const auto& p : passes) {
        const int pw = m.w > p.x0 ? (m.w - p.x0 + p.dx - 1) / p.dx : 0, ph = m.h > p.y0 ? (m.h - p.y0 + p.dy - 1) / p.dy : 0;
        if (!pw || !ph) continue;
        const size_t rb = ((size_t)pw * bits + 7) / 8;
        std::vector<uint8_t> prev(rb, 0);
        for (int y = 0; y < ph; y++) {
            std::vector<uint8_t> row(rb, 0);
            for (int x = 0; x < pw; x++)
                for (int c = 0; c < m.chans; c++) {
                    const unsigned v = m.at(p.x0 + x * p.dx, p.y0 + y * p.dy, c);
                    const size_t i = (size_t)x * m.chans + c;
                    if (m.depth == 16) { row[2 * i] = uint8_t(v >> 8); row[2 * i + 1] = uint8_t(v); }
                    else if (m.depth == 8) row[i] = uint8_t(v);
                    else { const size_t bit = i * m.depth; row[bit >> 3] |= uint8_t(v << (8 - m.depth - (bit & 7))); }
                }
            const int f = rowNo++ % 5;
            filtered.push_back(uint8_t(f));
            for (size_t i = 0; i < rb; i++) {
                const int a = i >= bpp ? row[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
                const int pr = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) >> 1 : paethE(a, b, c);
                filtered.push_back(uint8_t(row[i] - pr));
            }
            prev = row;
        }
    }
    std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10}, ihdr;
    put32(ihdr, (uint32_t)m.w); put32(ihdr, (uint32_t)m.h);
    ihdr.insert(ihdr.end(), {uint8_t(m.depth), uint8_t(m.ctype), 0, 0, uint8_t(m.interlace ? 1 : 0)});
    chunk(png, "IHDR", ihdr);
    if (!m.plte.empty()) chunk(png, "PLTE", m.plte);
    if (!m.trns.empty()) chunk(png, "tRNS", m.trns);
    const std::vector<uint8_t> z = zlibStored(filtered);
    const size_t half = z.size() / 2;   // two IDAT chunks: they must be joined
    chunk(png, "IDAT", std::vector<uint8_t>(z.begin(), z.begin() + (ptrdiff_t)half));
    chunk(png, "IDAT", std::vector<uint8_t>(z.begin() + (ptrdiff_t)half, z.end()));
    chunk(png, "IEND", {});
    return png;
}

static void expected(const Img& m, int x, int y, uint8_t out[4]) {
    const unsigned max = (1u << m.depth) - 1;
    auto to8 = [&](unsigned v) { return m.depth == 16 ? v >> 8 : v * 255 / max; };
    unsigned r, g, b, a = 255;
    auto key = [&](int c) { return (unsigned)m.trns[2 * c] << 8 | m.trns[2 * c + 1]; };
    switch (m.ctype) {
    case 0: r = g = b = to8(m.at(x, y, 0)); if (!m.trns.empty() && m.at(x, y, 0) == key(0)) a = 0; break;
    case 2:
        r = to8(m.at(x, y, 0)); g = to8(m.at(x, y, 1)); b = to8(m.at(x, y, 2));
        if (!m.trns.empty() && m.at(x, y, 0) == key(0) && m.at(x, y, 1) == key(1) && m.at(x, y, 2) == key(2)) a = 0;
        break;
    case 3: { const unsigned i = m.at(x, y, 0); r = m.plte[i * 3]; g = m.plte[i * 3 + 1]; b = m.plte[i * 3 + 2]; if (i < m.trns.size()) a = m.trns[i]; break; }
    case 4: r = g = b = to8(m.at(x, y, 0)); a = to8(m.at(x, y, 1)); break;
    default: r = to8(m.at(x, y, 0)); g = to8(m.at(x, y, 1)); b = to8(m.at(x, y, 2)); a = to8(m.at(x, y, 3)); break;
    }
    out[0] = uint8_t((r * a + 127) / 255); out[1] = uint8_t((g * a + 127) / 255); out[2] = uint8_t((b * a + 127) / 255); out[3] = uint8_t(a);
}

int main(int argc, char** argv) {
    const int kinds[][2] = {{0, 1}, {0, 2}, {0, 4}, {0, 8}, {0, 16}, {2, 8}, {2, 16}, {3, 1}, {3, 2}, {3, 4}, {3, 8}, {4, 8}, {4, 16}, {6, 8}, {6, 16}};
    const int sizes[][2] = {{1, 1}, {3, 2}, {7, 5}, {33, 17}, {256, 256}};
    int cases = 0;
    for (const auto& k : kinds)
        for (const auto& sz : sizes)
            for (int il = 0; il < 2; il++)
                for (int tr = 0; tr < 2; tr++) {
                    if (tr && k[0] != 0 && k[0] != 2 && k[0] != 3) continue;
                    const Img m = makeImg(k[0], k[1], sz[0], sz[1], il != 0, tr != 0, cases);
                    const std::vector<uint8_t> png = encode(m);
                    int w = 0, h = 0;
                    std::vector<uint8_t> px;
                    cases++;
                    if (!decodePng(png.data(), png.size(), w, h, px)) { CHECK(false, "type %d depth %d %dx%d interlace %d trns %d: not decoded", k[0], k[1], sz[0], sz[1], il, tr); continue; }
                    CHECK(w == m.w && h == m.h, "size %dx%d", w, h);
                    int bad = 0;
                    for (int y = 0; y < m.h && w == m.w && h == m.h; y++)
                        for (int x = 0; x < m.w; x++) {
                            uint8_t e[4];
                            expected(m, x, y, e);
                            if (std::equal(e, e + 4, &px[((size_t)y * w + x) * 4])) continue;
                            if (!bad++) printf("type %d depth %d %dx%d il %d trns %d: pixel %d,%d is %u %u %u %u, want %u %u %u %u\n", k[0], k[1], m.w, m.h, il, tr, x, y,
                                               px[((size_t)y * w + x) * 4], px[((size_t)y * w + x) * 4 + 1], px[((size_t)y * w + x) * 4 + 2], px[((size_t)y * w + x) * 4 + 3], e[0], e[1], e[2], e[3]);
                        }
                    CHECK(bad == 0, "%d wrong pixels", bad);
                }

    // broken files are refused, not crashed on
    const std::vector<uint8_t> good = encode(makeImg(6, 8, 33, 17, false, false, 1));
    int w, h;
    std::vector<uint8_t> px;
    for (size_t cut : {size_t(0), size_t(7), size_t(20), good.size() / 2, good.size() - 13}) CHECK(!decodePng(good.data(), cut, w, h, px), "truncated at %zu accepted", cut);
    std::vector<uint8_t> bad = good;
    bad[1] = 'X';
    CHECK(!decodePng(bad.data(), bad.size(), w, h, px), "bad signature accepted");
    {   // filter byte 7 in the first row
        Img m = makeImg(0, 8, 4, 1, false, false, 0);
        std::vector<uint8_t> p = encode(m);
        // the first IDAT data starts after the signature (8), IHDR (25) and the IDAT length+type (8): zlib header 2, stored block header 5
        p[8 + 25 + 8 + 2 + 5] = 7;
        CHECK(!decodePng(p.data(), p.size(), w, h, px), "unknown filter accepted");
    }
    CHECK(!decodePng(nullptr, 0, w, h, px), "null accepted");

    int real = 0;
    if (argc > 1) {
        for (const auto& e : std::filesystem::recursive_directory_iterator(argv[1])) {
            if (e.path().extension() != ".png") continue;
            std::ifstream f(e.path(), std::ios::binary);
            const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            const bool ok = decodePng(d.data(), d.size(), w, h, px);
            CHECK(ok && w == 256 && h == 256, "real tile %s: ok %d %dx%d", e.path().string().c_str(), ok, w, h);
            real++;
        }
    }
    printf("png: %d synthetic images, %d real tiles, %d failures\n", cases, real, fails);
    return fails ? 1 : 0;
}
