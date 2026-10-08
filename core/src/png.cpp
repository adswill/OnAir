#include "dect2/png.h"
#include "dect2/inflate.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
unsigned be16(const uint8_t* p) { return (unsigned)p[0] << 8 | p[1]; }

int paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

// one image, or one of the seven Adam7 passes: its first pixel, the spacing of its pixels and its size
struct Pass { size_t x0, y0, dx, dy, w, h; };

} // namespace

bool decodePng(const uint8_t* d, size_t n, int& w, int& h, std::vector<uint8_t>& rgba) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!d || n < 8 || memcmp(d, sig, 8) != 0) return false;
    uint32_t W = 0, H = 0;
    int depth = 0, ctype = -1, interlace = 0;
    std::vector<uint8_t> idat, plte, trns;
    for (size_t pos = 8; pos + 12 <= n;) {
        const uint32_t len = be32(d + pos);
        if (len > n - pos - 12) return false;
        const uint8_t* type = d + pos + 4;
        const uint8_t* c = d + pos + 8;
        if (!memcmp(type, "IHDR", 4)) {
            if (len < 13 || c[10] != 0 || c[11] != 0) return false;   // compression and filter method are 0 in every PNG
            W = be32(c); H = be32(c + 4); depth = c[8]; ctype = c[9]; interlace = c[12];
        } else if (!memcmp(type, "PLTE", 4)) plte.assign(c, c + len);
        else if (!memcmp(type, "tRNS", 4)) trns.assign(c, c + len);
        else if (!memcmp(type, "IDAT", 4)) idat.insert(idat.end(), c, c + len);
        else if (!memcmp(type, "IEND", 4)) break;
        pos += 12 + (size_t)len;
    }
    if (W == 0 || H == 0 || W > 16384 || H > 16384 || interlace > 1 || idat.size() < 2) return false;
    int chans;
    bool depthOk;
    switch (ctype) {
    case 0: chans = 1; depthOk = depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16; break;
    case 2: chans = 3; depthOk = depth == 8 || depth == 16; break;
    case 3: chans = 1; depthOk = depth == 1 || depth == 2 || depth == 4 || depth == 8; break;
    case 4: chans = 2; depthOk = depth == 8 || depth == 16; break;
    case 6: chans = 4; depthOk = depth == 8 || depth == 16; break;
    default: return false;
    }
    if (!depthOk || (ctype == 3 && plte.size() < 3)) return false;
    // zlib wrapper: deflate, no preset dictionary, header check (the Adler-32 at the end is not checked: the filters catch garbage anyway)
    if ((idat[0] & 0x0F) != 8 || (idat[1] & 0x20) || ((idat[0] << 8 | idat[1]) % 31) != 0) return false;

    const size_t bits = (size_t)chans * depth;                 // per pixel
    const size_t bpp = std::max<size_t>(1, bits / 8);          // the byte distance the filters look back
    auto rowBytes = [&](size_t pw) { return (pw * bits + 7) / 8; };
    std::vector<Pass> passes;
    if (interlace) {
        static const size_t x0[7] = {0, 4, 0, 2, 0, 1, 0}, y0[7] = {0, 0, 4, 0, 2, 0, 1}, dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
        for (int i = 0; i < 7; i++) {
            const size_t pw = W > x0[i] ? (W - x0[i] + dx[i] - 1) / dx[i] : 0, ph = H > y0[i] ? (H - y0[i] + dy[i] - 1) / dy[i] : 0;
            passes.push_back({x0[i], y0[i], dx[i], dy[i], pw, ph});
        }
    } else {
        passes.push_back({0, 0, 1, 1, W, H});
    }
    size_t total = 0;
    for (const auto& p : passes) if (p.w && p.h) total += p.h * (1 + rowBytes(p.w));
    std::vector<uint8_t> raw;
    if (!inflateRaw(idat.data() + 2, idat.size() - 2, raw, total + 4096) || raw.size() < total) return false;

    auto sample = [&](const uint8_t* row, size_t i) -> unsigned {   // the i-th sample of a row at its own depth
        if (depth == 8) return row[i];
        if (depth == 16) return be16(row + 2 * i);
        const size_t bit = i * depth;
        return (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
    };
    auto to8 = [&](unsigned v) -> unsigned { return depth == 16 ? v >> 8 : depth == 8 ? v : v * 255 / ((1u << depth) - 1); };
    const bool keyGrey = ctype == 0 && trns.size() >= 2, keyRgb = ctype == 2 && trns.size() >= 6;

    rgba.assign((size_t)W * H * 4, 0);
    std::vector<uint8_t> prev, cur;
    size_t off = 0;
    for (const auto& p : passes) {
        if (!p.w || !p.h) continue;
        const size_t rb = rowBytes(p.w);
        prev.assign(rb, 0);
        for (size_t y = 0; y < p.h; y++, off += 1 + rb) {
            const uint8_t filter = raw[off];
            cur.assign(raw.begin() + (ptrdiff_t)off + 1, raw.begin() + (ptrdiff_t)(off + 1 + rb));
            for (size_t i = 0; i < rb; i++) {
                const int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
                switch (filter) {
                case 0: break;
                case 1: cur[i] = uint8_t(cur[i] + a); break;
                case 2: cur[i] = uint8_t(cur[i] + b); break;
                case 3: cur[i] = uint8_t(cur[i] + ((a + b) >> 1)); break;
                case 4: cur[i] = uint8_t(cur[i] + paeth(a, b, c)); break;
                default: return false;
                }
            }
            for (size_t x = 0; x < p.w; x++) {
                const size_t s = x * chans;
                unsigned r, g, b, al = 255;
                switch (ctype) {
                case 0: {
                    const unsigned v = sample(cur.data(), s);
                    r = g = b = to8(v);
                    if (keyGrey && v == be16(trns.data())) al = 0;
                    break;
                }
                case 2: {
                    const unsigned vr = sample(cur.data(), s), vg = sample(cur.data(), s + 1), vb = sample(cur.data(), s + 2);
                    r = to8(vr); g = to8(vg); b = to8(vb);
                    if (keyRgb && vr == be16(trns.data()) && vg == be16(trns.data() + 2) && vb == be16(trns.data() + 4)) al = 0;
                    break;
                }
                case 3: {
                    const unsigned i = sample(cur.data(), s);
                    if (i * 3 + 2 < plte.size()) { r = plte[i * 3]; g = plte[i * 3 + 1]; b = plte[i * 3 + 2]; } else r = g = b = 0;
                    if (i < trns.size()) al = trns[i];
                    break;
                }
                case 4: r = g = b = to8(sample(cur.data(), s)); al = to8(sample(cur.data(), s + 1)); break;
                default: r = to8(sample(cur.data(), s)); g = to8(sample(cur.data(), s + 1)); b = to8(sample(cur.data(), s + 2)); al = to8(sample(cur.data(), s + 3)); break;
                }
                uint8_t* o = &rgba[((p.y0 + y * p.dy) * W + p.x0 + x * p.dx) * 4];
                o[0] = uint8_t((r * al + 127) / 255); o[1] = uint8_t((g * al + 127) / 255); o[2] = uint8_t((b * al + 127) / 255); o[3] = uint8_t(al);
            }
            prev.swap(cur);
        }
    }
    w = (int)W; h = (int)H;
    return true;
}

} // namespace dect2
