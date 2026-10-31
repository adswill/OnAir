#include "qr.h"
#include <algorithm>
#include <cstdint>

namespace webshare {

namespace {

// ECC level M: check bytes per block and number of blocks, versions 1..10
const int kEcPerBlock[11] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
const int kBlocks[11] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};

int rawModules(int ver) {
    int r = (16 * ver + 128) * ver + 64;
    if (ver >= 2) { const int a = ver / 7 + 2; r -= (25 * a - 10) * a - 55; if (ver >= 7) r -= 36; }
    return r;
}
int dataCodewords(int ver) { return rawModules(ver) / 8 - kEcPerBlock[ver] * kBlocks[ver]; }

uint8_t gfMul(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; i--) { z = (z << 1) ^ ((z >> 7) * 0x11D); z ^= ((y >> i) & 1) * x; }
    return (uint8_t)z;
}

std::vector<uint8_t> rsDivisor(int degree) {
    std::vector<uint8_t> r((size_t)degree, 0);
    r[(size_t)degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (size_t j = 0; j < r.size(); j++) { r[j] = gfMul(r[j], root); if (j + 1 < r.size()) r[j] ^= r[j + 1]; }
        root = gfMul(root, 2);
    }
    return r;
}

struct Grid {
    int n;
    std::vector<uint8_t> dark, fn;   // dark modules; function-pattern modules (not data)
    explicit Grid(int size) : n(size), dark((size_t)size * (size_t)size, 0), fn((size_t)size * (size_t)size, 0) {}
    void set(int x, int y, bool d) { dark[(size_t)y * (size_t)n + (size_t)x] = d; fn[(size_t)y * (size_t)n + (size_t)x] = 1; }
    bool get(int x, int y) const { return dark[(size_t)y * (size_t)n + (size_t)x]; }
    bool isFn(int x, int y) const { return fn[(size_t)y * (size_t)n + (size_t)x]; }
};

void drawFinder(Grid& g, int cx, int cy) {
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            const int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= g.n || y >= g.n) continue;
            const int d = std::max(std::abs(dx), std::abs(dy));
            g.set(x, y, d != 2 && d != 4);
        }
}
void drawAlign(Grid& g, int cx, int cy) {
    for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) g.set(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
}
std::vector<int> alignPositions(int ver, int n) {
    if (ver == 1) return {};
    const int cnt = ver / 7 + 2;
    const int step = ver == 32 ? 26 : (ver * 4 + cnt * 2 + 1) / (cnt * 2 - 2) * 2;
    std::vector<int> r(1, 6);
    for (int pos = n - 7; (int)r.size() < cnt; pos -= step) r.insert(r.begin() + 1, pos);
    return r;
}

unsigned bch(unsigned data, int dataBits, unsigned poly, int polyDeg) {
    unsigned rem = data;
    for (int i = 0; i < dataBits; i++) rem = (rem << 1) ^ ((rem >> (polyDeg - 1)) * poly);
    return rem;
}

void drawFormat(Grid& g, int mask) {
    const unsigned bits = qrFormatBitsM(mask);
    auto b = [&](int i) { return ((bits >> i) & 1) != 0; };
    for (int i = 0; i <= 5; i++) g.set(8, i, b(i));
    g.set(8, 7, b(6)); g.set(8, 8, b(7)); g.set(7, 8, b(8));
    for (int i = 9; i < 15; i++) g.set(14 - i, 8, b(i));
    for (int i = 0; i < 8; i++) g.set(g.n - 1 - i, 8, b(i));
    for (int i = 8; i < 15; i++) g.set(8, g.n - 15 + i, b(i));
    g.set(8, g.n - 8, true);
}

void drawVersion(Grid& g, int ver) {
    if (ver < 7) return;
    unsigned rem = (unsigned)ver;
    for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    const unsigned bits = ((unsigned)ver << 12) | rem;
    for (int i = 0; i < 18; i++) {
        const bool bit = ((bits >> i) & 1) != 0;
        const int a = g.n - 11 + i % 3, b2 = i / 3;
        g.set(a, b2, bit);
        g.set(b2, a, bit);
    }
}

bool maskBit(int mask, int x, int y) {
    switch (mask) {
        case 0: return (x + y) % 2 == 0;
        case 1: return y % 2 == 0;
        case 2: return x % 3 == 0;
        case 3: return (x + y) % 3 == 0;
        case 4: return (x / 3 + y / 2) % 2 == 0;
        case 5: return x * y % 2 + x * y % 3 == 0;
        case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
        default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

int penalty(const Grid& g) {
    const int n = g.n;
    int p = 0;
    for (int pass = 0; pass < 2; pass++) {
        auto at = [&](int a, int b) { return pass == 0 ? g.get(b, a) : g.get(a, b); };
        for (int a = 0; a < n; a++) {
            int run = 1;
            for (int b = 1; b < n; b++) {
                if (at(a, b) == at(a, b - 1)) { run++; if (run == 5) p += 3; else if (run > 5) p++; } else run = 1;
            }
            // 1:1:3:1:1 with 4 light modules on a side
            for (int b = 0; b + 10 < n + 0 && b + 10 <= n - 1; b++) {
                static const int pat[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
                bool f = true, r = true;
                for (int k = 0; k < 11; k++) { if (at(a, b + k) != (pat[k] != 0)) f = false; if (at(a, b + 10 - k) != (pat[k] != 0)) r = false; }
                if (f) p += 40;
                if (r) p += 40;
            }
        }
    }
    for (int y = 0; y + 1 < n; y++)
        for (int x = 0; x + 1 < n; x++) {
            const bool c = g.get(x, y);
            if (c == g.get(x + 1, y) && c == g.get(x, y + 1) && c == g.get(x + 1, y + 1)) p += 3;
        }
    int dark = 0;
    for (uint8_t d : g.dark) dark += d;
    const int total = n * n;
    const int k = (std::abs(dark * 20 - total * 10) + total - 1) / total - 1;
    p += std::max(0, k) * 10;
    return p;
}

} // namespace

unsigned qrFormatBitsM(int mask) {
    const unsigned data = (0u << 3) | (unsigned)mask;   // error correction level M = 00
    unsigned rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    return ((data << 10) | rem) ^ 0x5412;
}

std::vector<unsigned char> qrReedSolomon(const std::vector<unsigned char>& data, int degree) {
    const std::vector<uint8_t> div = rsDivisor(degree);
    std::vector<uint8_t> r((size_t)degree, 0);
    for (uint8_t b : data) {
        const uint8_t f = b ^ r[0];
        r.erase(r.begin());
        r.push_back(0);
        for (size_t i = 0; i < r.size(); i++) r[i] ^= gfMul(div[i], f);
    }
    return r;
}

bool qrEncode(const std::string& text, QrCode& out) {
    int ver = 0;
    for (int v = 1; v <= 10; v++) {
        const int need = 4 + (v >= 10 ? 16 : 8) + 8 * (int)text.size();
        if (need <= dataCodewords(v) * 8) { ver = v; break; }
    }
    if (!ver) return false;
    std::vector<uint8_t> bits;   // one bit per element
    auto put = [&](unsigned val, int len) { for (int i = len - 1; i >= 0; i--) bits.push_back((val >> i) & 1); };
    put(0x4, 4);
    put((unsigned)text.size(), ver >= 10 ? 16 : 8);
    for (unsigned char c : text) put(c, 8);
    const size_t cap = (size_t)dataCodewords(ver) * 8;
    put(0, (int)std::min<size_t>(4, cap - bits.size()));
    while (bits.size() % 8) bits.push_back(0);
    for (unsigned pad = 0xEC; bits.size() < cap; pad ^= 0xEC ^ 0x11) put(pad, 8);
    std::vector<uint8_t> data(bits.size() / 8, 0);
    for (size_t i = 0; i < bits.size(); i++) data[i >> 3] |= (uint8_t)(bits[i] << (7 - (i & 7)));

    // blocks, check bytes, interleave
    const int nb = kBlocks[ver], ecl = kEcPerBlock[ver], raw = rawModules(ver) / 8;
    const int shortLen = raw / nb - ecl, numShort = nb - raw % nb;
    std::vector<std::vector<uint8_t>> dataBlocks, ecBlocks;
    for (int i = 0, k = 0; i < nb; i++) {
        const int len = shortLen + (i < numShort ? 0 : 1);
        std::vector<uint8_t> d(data.begin() + k, data.begin() + k + len);
        k += len;
        ecBlocks.push_back(qrReedSolomon(d, ecl));
        dataBlocks.push_back(std::move(d));
    }
    std::vector<uint8_t> all;
    for (int i = 0; i <= shortLen; i++) for (int j = 0; j < nb; j++) if (i < (int)dataBlocks[(size_t)j].size()) all.push_back(dataBlocks[(size_t)j][(size_t)i]);
    for (int i = 0; i < ecl; i++) for (int j = 0; j < nb; j++) all.push_back(ecBlocks[(size_t)j][(size_t)i]);

    const int n = ver * 4 + 17;
    Grid g(n);
    for (int i = 0; i < n; i++) { g.set(6, i, i % 2 == 0); g.set(i, 6, i % 2 == 0); }
    drawFinder(g, 3, 3); drawFinder(g, n - 4, 3); drawFinder(g, 3, n - 4);
    const std::vector<int> ap = alignPositions(ver, n);
    for (size_t i = 0; i < ap.size(); i++)
        for (size_t j = 0; j < ap.size(); j++) {
            if ((i == 0 && j == 0) || (i == 0 && j == ap.size() - 1) || (i == ap.size() - 1 && j == 0)) continue;
            drawAlign(g, ap[i], ap[j]);
        }
    drawFormat(g, 0);
    drawVersion(g, ver);

    // data, zigzag
    size_t bi = 0;
    for (int right = n - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < n; vert++)
            for (int j = 0; j < 2; j++) {
                const int x = right - j;
                const bool up = ((right + 1) & 2) == 0;
                const int y = up ? n - 1 - vert : vert;
                if (!g.isFn(x, y) && bi < all.size() * 8) { g.dark[(size_t)y * (size_t)n + (size_t)x] = (all[bi >> 3] >> (7 - (bi & 7))) & 1; bi++; }
            }
    }
    // best mask
    int best = 0, bestP = 1 << 30;
    for (int mk = 0; mk < 8; mk++) {
        Grid t = g;
        for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) if (!t.isFn(x, y) && maskBit(mk, x, y)) t.dark[(size_t)y * (size_t)n + (size_t)x] ^= 1;
        drawFormat(t, mk);
        const int p = penalty(t);
        if (p < bestP) { bestP = p; best = mk; }
    }
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) if (!g.isFn(x, y) && maskBit(best, x, y)) g.dark[(size_t)y * (size_t)n + (size_t)x] ^= 1;
    drawFormat(g, best);
    out.size = n;
    out.m.assign(g.dark.begin(), g.dark.end());
    return true;
}

} // namespace webshare
