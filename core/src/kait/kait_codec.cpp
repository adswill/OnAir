// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT image codec: see kait_codec.h. Layout: colour planes -> CDF 9/7 lifting wavelet -> bands -> embedded bitplane passes (significance, refinement)
// with a binary range coder. The same pass code runs in the encoder and the decoder (template on the coder), so they cannot drift apart.
#include "dect2/kait/kait_codec.h"
#include "dect2/kris/kris_common.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2::kait {
namespace {

// ---------------------------------------------------------------- range coder
struct Prob {
    uint16_t a = 16384, b = 16384;   // probability of a 0 bit, 15 bits, fast and slow estimate
    int p() const {
        const int v = (a + b) >> 1;
        return v < 48 ? 48 : (v > 32720 ? 32720 : v);
    }
    void upd(int bit) {
        if (bit) { a -= a >> 4; b -= b >> 7; }
        else { a += (32768 - a) >> 4; b += (32768 - b) >> 7; }
    }
};

struct Enc {
    static constexpr bool kDec = false;
    uint64_t low = 0;
    uint32_t range = 0xFFFFFFFFu;
    uint8_t cache = 0;
    uint64_t cacheSize = 1;
    std::vector<uint8_t> out;
    size_t maxBytes;
    bool stopped = false;
    uint32_t decisions = 0;
    explicit Enc(size_t mb) : maxBytes(mb) {}
    void shiftLow() {
        if ((uint32_t)low < 0xFF000000u || (low >> 32) != 0) {
            const uint8_t carry = (uint8_t)(low >> 32);
            uint8_t temp = cache;
            do { out.push_back((uint8_t)(temp + carry)); temp = 0xFF; } while (--cacheSize);
            cache = (uint8_t)((low >> 24) & 0xFF);
        }
        cacheSize++;
        low = (low & 0x00FFFFFFu) << 8;
    }
    int code(Prob& pr, int bit) {
        if (stopped) return bit;
        if (out.size() + cacheSize + 8 > maxBytes + 1) { stopped = true; return bit; }   // the flush needs 4 more bytes; one leading zero byte is dropped
        const uint32_t bound = (range >> 15) * (uint32_t)pr.p();
        if (!bit) range = bound;
        else { low += bound; range -= bound; }
        pr.upd(bit);
        while (range < (1u << 24)) { range <<= 8; shiftLow(); }
        decisions++;
        return bit;
    }
    std::vector<uint8_t> finish() {
        for (int i = 0; i < 5; i++) shiftLow();
        std::vector<uint8_t> r(out.begin() + 1, out.end());   // the first byte is always 0
        while (!r.empty() && r.back() == 0) r.pop_back();     // the decoder reads zeros past the end
        if (r.size() > maxBytes) r.resize(maxBytes);
        return r;
    }
};

struct Dec {
    static constexpr bool kDec = true;
    const uint8_t* d;
    size_t n;
    size_t pos = 0;
    uint32_t range = 0xFFFFFFFFu, cd = 0;
    bool stopped = false;
    uint32_t decisions = 0, limit;
    Dec(const uint8_t* data, size_t len, uint32_t lim) : d(data), n(len), limit(lim) {
        for (int i = 0; i < 4; i++) cd = (cd << 8) | next();
    }
    uint32_t next() { pos++; return pos <= n ? d[pos - 1] : 0u; }
    int code(Prob& pr, int) {
        if (stopped) return 0;
        if (decisions >= limit) { stopped = true; return 0; }
        while (range < (1u << 24)) { range <<= 8; cd = (cd << 8) | next(); }
        if (pos > n) { stopped = true; return 0; }   // the decision would depend on bytes we do not have
        const uint32_t bound = (range >> 15) * (uint32_t)pr.p();
        int bit;
        if (cd < bound) { range = bound; bit = 0; }
        else { cd -= bound; range -= bound; bit = 1; }
        pr.upd(bit);
        decisions++;
        return bit;
    }
};

// ---------------------------------------------------------------- wavelet
constexpr float kAlpha = -1.586134342059924f, kBeta = -0.052980118572961f, kGamma = 0.882911075530934f, kDelta = 0.443506852043971f;
constexpr float kK = 1.230174104914001f;
constexpr float kLowScale = 1.41421356237f / kK;    // low-pass: DC gain sqrt(2) (orthonormal-like)
constexpr float kHighScale = kK / 1.41421356237f;

inline int refl(int i, int n) { return i < 0 ? -i : (i >= n ? 2 * (n - 1) - i : i); }

// x: n interleaved samples (even = low). In place lifting.
void liftFwd(float* x, int n) {
    for (int i = 1; i < n; i += 2) x[i] += kAlpha * (x[refl(i - 1, n)] + x[refl(i + 1, n)]);
    for (int i = 0; i < n; i += 2) x[i] += kBeta * (x[refl(i - 1, n)] + x[refl(i + 1, n)]);
    for (int i = 1; i < n; i += 2) x[i] += kGamma * (x[refl(i - 1, n)] + x[refl(i + 1, n)]);
    for (int i = 0; i < n; i += 2) x[i] += kDelta * (x[refl(i - 1, n)] + x[refl(i + 1, n)]);
    for (int i = 0; i < n; i += 2) x[i] *= kLowScale;
    for (int i = 1; i < n; i += 2) x[i] *= kHighScale;
}
void liftInv(float* x, int n) {
    for (int i = 0; i < n; i += 2) x[i] /= kLowScale;
    for (int i = 1; i < n; i += 2) x[i] /= kHighScale;
    for (int i = 0; i < n; i += 2) x[i] -= kDelta * (x[refl(i - 1, n)] + x[refl(i + 1, n)]);
    for (int i = 1; i < n; i += 2) x[i] -= kGamma * (x[refl(i - 1, n)] + x[refl(i + 1, n)]);
    for (int i = 0; i < n; i += 2) x[i] -= kBeta * (x[refl(i - 1, n)] + x[refl(i + 1, n)]);
    for (int i = 1; i < n; i += 2) x[i] -= kAlpha * (x[refl(i - 1, n)] + x[refl(i + 1, n)]);
}

// one 1-D line of n samples at p with the given stride: transform and put the low half first, the high half after
void lineFwd(float* p, int n, ptrdiff_t stride, std::vector<float>& tmp) {
    if (n < 2) return;
    tmp.resize((size_t)n);
    for (int i = 0; i < n; i++) tmp[(size_t)i] = p[i * stride];
    liftFwd(tmp.data(), n);
    const int nl = (n + 1) / 2;
    for (int i = 0; i < n; i++) p[(i >> 1) * stride + ((i & 1) ? (ptrdiff_t)nl * stride : 0)] = tmp[(size_t)i];
}
void lineInv(float* p, int n, ptrdiff_t stride, std::vector<float>& tmp) {
    if (n < 2) return;
    tmp.resize((size_t)n);
    const int nl = (n + 1) / 2;
    for (int i = 0; i < n; i++) tmp[(size_t)i] = p[(i >> 1) * stride + ((i & 1) ? (ptrdiff_t)nl * stride : 0)];
    liftInv(tmp.data(), n);
    for (int i = 0; i < n; i++) p[i * stride] = tmp[(size_t)i];
}

void dwtFwd(std::vector<float>& pl, int w, int h, int levels) {
    std::vector<float> tmp;
    int cw = w, ch = h;
    for (int l = 0; l < levels; l++) {
        for (int y = 0; y < ch; y++) lineFwd(&pl[(size_t)y * w], cw, 1, tmp);
        for (int x = 0; x < cw; x++) lineFwd(&pl[(size_t)x], ch, w, tmp);
        cw = (cw + 1) / 2;
        ch = (ch + 1) / 2;
    }
}
void dwtInv(std::vector<float>& pl, int w, int h, int levels) {
    std::vector<float> tmp;
    std::vector<int> cws{w}, chs{h};
    for (int l = 0; l < levels; l++) { cws.push_back((cws.back() + 1) / 2); chs.push_back((chs.back() + 1) / 2); }
    for (int l = levels - 1; l >= 0; l--) {
        const int cw = cws[(size_t)l], ch = chs[(size_t)l];
        for (int x = 0; x < cw; x++) lineInv(&pl[(size_t)x], ch, w, tmp);
        for (int y = 0; y < ch; y++) lineInv(&pl[(size_t)y * w], cw, 1, tmp);
    }
}

// ---------------------------------------------------------------- bands
struct Band {
    int comp = 0, orient = 0, level = 0, rank = 0;   // orient 0 LL, 1 HL, 2 LH, 3 HH; rank = luma-equivalent level (LL: top)
    int ox = 0, oy = 0, bw = 0, bh = 0;
    int parent = -1;
    std::vector<int32_t> q;      // magnitudes (true in the encoder, decoded so far in the decoder)
    std::vector<uint8_t> neg;    // encoder: sign
    std::vector<uint8_t> sv;     // padded (bw + 2) x (bh + 2): 0 insignificant, 1 positive, 2 negative
    std::vector<int8_t> sp;      // plane at which it became significant
    std::vector<int8_t> lp;      // lowest plane known
};

struct Layout {
    int w[3], h[3], lv[3];
    std::vector<Band> bands;
};

Layout makeLayout(int w, int h, int L) {
    Layout ly;
    const int cwFull = (w + 1) / 2, chFull = (h + 1) / 2;
    ly.w[0] = w; ly.h[0] = h; ly.lv[0] = L;
    for (int c = 1; c < 3; c++) { ly.w[c] = cwFull; ly.h[c] = chFull; ly.lv[c] = std::max(0, L - 1); }
    // sizes per component and level
    std::vector<std::vector<int>> cw(3), ch(3);
    for (int c = 0; c < 3; c++) {
        cw[(size_t)c].push_back(ly.w[c]);
        ch[(size_t)c].push_back(ly.h[c]);
        for (int l = 0; l < ly.lv[c]; l++) {
            cw[(size_t)c].push_back((cw[(size_t)c].back() + 1) / 2);
            ch[(size_t)c].push_back((ch[(size_t)c].back() + 1) / 2);
        }
    }
    auto add = [&](int c, int orient, int level, int rank, int ox, int oy, int bw, int bh) {
        Band b;
        b.comp = c; b.orient = orient; b.level = level; b.rank = rank;
        b.ox = ox; b.oy = oy; b.bw = bw; b.bh = bh;
        ly.bands.push_back(std::move(b));
        return (int)ly.bands.size() - 1;
    };
    // coding order: the LL bands, then for rank L .. 1 the luma level and the chroma level rank - 1 (HL, LH, HH)
    for (int c = 0; c < 3; c++) add(c, 0, ly.lv[c], L, 0, 0, cw[(size_t)c][(size_t)ly.lv[c]], ch[(size_t)c][(size_t)ly.lv[c]]);
    std::vector<std::vector<int>> idx(3, std::vector<int>((size_t)L + 2, -1));   // idx[c][level] of the HL band; LH = +1, HH = +2 (consecutive)
    for (int r = L; r >= 1; r--) {
        for (int c = 0; c < 3; c++) {
            const int lvl = c == 0 ? r : r - 1;
            if (lvl < 1 || lvl > ly.lv[c]) continue;
            const int pw = cw[(size_t)c][(size_t)lvl - 1], ph = ch[(size_t)c][(size_t)lvl - 1];
            const int lw = cw[(size_t)c][(size_t)lvl], lh = ch[(size_t)c][(size_t)lvl];
            const int hw = pw - lw, hh = ph - lh;
            idx[(size_t)c][(size_t)lvl] = add(c, 1, lvl, r, lw, 0, hw, lh);
            add(c, 2, lvl, r, 0, lh, lw, hh);
            add(c, 3, lvl, r, lw, lh, hw, hh);
        }
    }
    for (Band& b : ly.bands) {
        if (b.orient == 0 || b.level >= ly.lv[b.comp]) continue;
        const int base = idx[(size_t)b.comp][(size_t)b.level + 1];
        if (base >= 0) b.parent = base + (b.orient - 1);
    }
    for (Band& b : ly.bands) {
        const size_t n = (size_t)b.bw * (size_t)b.bh;
        b.q.assign(n, 0);
        b.sv.assign((size_t)(b.bw + 2) * (size_t)(b.bh + 2), 0);
        b.sp.assign(n, -1);
        b.lp.assign(n, -1);
    }
    return ly;
}

// ---------------------------------------------------------------- the passes
template <class RC>
struct Passes {
    RC& rc;
    Layout& ly;
    Prob sigP[4][2][6][2];
    Prob grpP[4][2][4];
    Prob refP[3];
    Prob sgnP[9];
    Passes(RC& r, Layout& l) : rc(r), ly(l) {}

    static int nbCount(const std::vector<uint8_t>& sv, size_t pi, int W) {
        return (sv[pi - 1] != 0) + (sv[pi + 1] != 0) + (sv[pi - W] != 0) + (sv[pi + W] != 0) + (sv[pi - W - 1] != 0) + (sv[pi - W + 1] != 0) +
               (sv[pi + W - 1] != 0) + (sv[pi + W + 1] != 0);
    }
    bool sigPass(int p) {
        const int32_t T = 1 << p;
        for (Band& b : ly.bands) {
            if (b.bw == 0 || b.bh == 0) continue;
            const int W = b.bw + 2;
            const int lc = b.rank >= 3 ? 1 : 0;
            const Band* par = b.parent >= 0 ? &ly.bands[(size_t)b.parent] : nullptr;
            for (int y = 0; y < b.bh; y++) {
                for (int x0 = 0; x0 < b.bw; x0 += 8) {
                    const int len = std::min(8, b.bw - x0);
                    bool quiet = true;
                    for (int dy = -1; dy <= 1 && quiet; dy++) {
                        const size_t row = (size_t)(y + 1 + dy) * (size_t)W;
                        for (int xx = x0 - 1; xx <= x0 + len; xx++)
                            if (b.sv[row + (size_t)(xx + 1)]) { quiet = false; break; }
                    }
                    if (quiet) {
                        int pc = 0;
                        if (par && par->bw > 0 && par->bh > 0) {
                            const int pw = par->bw + 2;
                            const int py = std::min(y >> 1, par->bh - 1);
                            for (int px = x0 >> 1; px <= (x0 + len - 1) >> 1; px++) {
                                const int pxc = std::min(px, par->bw - 1);
                                if (par->sv[(size_t)(py + 1) * (size_t)pw + (size_t)(pxc + 1)]) pc++;
                            }
                        }
                        int flag = 0;
                        if (!RC::kDec)
                            for (int xx = 0; xx < len; xx++)
                                if (b.q[(size_t)y * (size_t)b.bw + (size_t)(x0 + xx)] >= T) { flag = 1; break; }
                        flag = rc.code(grpP[b.orient][lc][std::min(pc, 3)], flag);
                        if (rc.stopped) return false;
                        if (!flag) continue;
                    }
                    for (int xx = x0; xx < x0 + len; xx++) {
                        const size_t pi = (size_t)(y + 1) * (size_t)W + (size_t)(xx + 1);
                        if (b.sv[pi]) continue;
                        const size_t qi = (size_t)y * (size_t)b.bw + (size_t)xx;
                        const int nb = nbCount(b.sv, pi, W);
                        int ps = 0;
                        if (par && par->bw > 0 && par->bh > 0) {
                            const int px = std::min(xx >> 1, par->bw - 1), py = std::min(y >> 1, par->bh - 1);
                            ps = par->sv[(size_t)(py + 1) * (size_t)(par->bw + 2) + (size_t)(px + 1)] ? 1 : 0;
                        }
                        int bit = RC::kDec ? 0 : (b.q[qi] >= T ? 1 : 0);
                        bit = rc.code(sigP[b.orient][lc][std::min(nb, 5)][ps], bit);
                        if (rc.stopped) return false;
                        if (bit) {
                            const int sctx = 3 * b.sv[pi - 1] + b.sv[pi - W];
                            int s = RC::kDec ? 0 : b.neg[qi];
                            s = rc.code(sgnP[sctx], s);
                            if (rc.stopped) return false;
                            b.sv[pi] = (uint8_t)(1 + s);
                            b.sp[qi] = (int8_t)p;
                            b.lp[qi] = (int8_t)p;
                            if (RC::kDec) { b.q[qi] = T; b.neg[qi] = (uint8_t)s; }
                        }
                    }
                }
            }
        }
        return true;
    }
    bool refPass(int p) {
        for (Band& b : ly.bands) {
            if (b.bw == 0 || b.bh == 0) continue;
            const int W = b.bw + 2;
            for (int y = 0; y < b.bh; y++) {
                for (int x = 0; x < b.bw; x++) {
                    const size_t qi = (size_t)y * (size_t)b.bw + (size_t)x;
                    if (b.sp[qi] <= p) continue;   // insignificant (-1) or became significant on this plane
                    const bool first = b.sp[qi] == p + 1;
                    int ctx = 2;
                    if (first) ctx = nbCount(b.sv, (size_t)(y + 1) * (size_t)W + (size_t)(x + 1), W) > 0 ? 1 : 0;
                    int bit = RC::kDec ? 0 : ((b.q[qi] >> p) & 1);
                    bit = rc.code(refP[ctx], bit);
                    if (rc.stopped) return false;
                    if (RC::kDec && bit) b.q[qi] |= (1 << p);
                    b.lp[qi] = (int8_t)p;
                }
            }
        }
        return true;
    }
};

// ---------------------------------------------------------------- colour
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void toPlanes(const Image& img, std::vector<float> pl[3]) {
    const int w = img.w, h = img.h, cw = (w + 1) / 2, ch = (h + 1) / 2;
    pl[0].assign((size_t)w * h, 0.f);
    pl[1].assign((size_t)cw * ch, 0.f);
    pl[2].assign((size_t)cw * ch, 0.f);
    std::vector<float> cb((size_t)w * h), cr((size_t)w * h);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const float r = img.rgb[3 * i], g = img.rgb[3 * i + 1], b = img.rgb[3 * i + 2];
        pl[0][i] = 0.299f * r + 0.587f * g + 0.114f * b - 128.f;
        cb[i] = -0.168736f * r - 0.331264f * g + 0.5f * b;
        cr[i] = 0.5f * r - 0.418688f * g - 0.081312f * b;
    }
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) {
            float sb = 0, sr = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    const size_t i = (size_t)std::min(2 * y + dy, h - 1) * w + (size_t)std::min(2 * x + dx, w - 1);
                    sb += cb[i];
                    sr += cr[i];
                }
            pl[1][(size_t)y * cw + x] = sb * 0.25f;
            pl[2][(size_t)y * cw + x] = sr * 0.25f;
        }
}

float sampleBilinear(const std::vector<float>& p, int pw, int ph, float fx, float fy) {
    fx = clampf(fx, 0.f, (float)(pw - 1));
    fy = clampf(fy, 0.f, (float)(ph - 1));
    const int x0 = (int)fx, y0 = (int)fy;
    const int x1 = std::min(x0 + 1, pw - 1), y1 = std::min(y0 + 1, ph - 1);
    const float ax = fx - (float)x0, ay = fy - (float)y0;
    const float a = p[(size_t)y0 * pw + x0] * (1 - ax) + p[(size_t)y0 * pw + x1] * ax;
    const float b = p[(size_t)y1 * pw + x0] * (1 - ax) + p[(size_t)y1 * pw + x1] * ax;
    return a * (1 - ay) + b * ay;
}

Image fromPlanes(const std::vector<float> pl[3], int w, int h) {
    Image im;
    im.w = w; im.h = h;
    im.rgb.resize((size_t)w * h * 3);
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const float Y = pl[0][(size_t)y * w + x] + 128.f;
            const float fx = ((float)x - 0.5f) * 0.5f, fy = ((float)y - 0.5f) * 0.5f;
            const float cb = sampleBilinear(pl[1], cw, ch, fx, fy);
            const float cr = sampleBilinear(pl[2], cw, ch, fx, fy);
            const float r = Y + 1.402f * cr, g = Y - 0.344136f * cb - 0.714136f * cr, b = Y + 1.772f * cb;
            uint8_t* o = &im.rgb[3 * ((size_t)y * w + x)];
            o[0] = (uint8_t)(clampf(r, 0.f, 255.f) + 0.5f);
            o[1] = (uint8_t)(clampf(g, 0.f, 255.f) + 0.5f);
            o[2] = (uint8_t)(clampf(b, 0.f, 255.f) + 0.5f);
        }
    return im;
}

int levelsFor(int w, int h, int want) {
    int l = want;
    while (l > 1 && (std::min(w, h) >> l) < 1) l--;
    return std::max(1, l);
}

}  // namespace

Encoded encodeImage(const Image& img, size_t maxBytes, int levels) {
    Encoded out;
    out.info.w = img.w;
    out.info.h = img.h;
    if (img.w <= 0 || img.h <= 0 || img.rgb.size() < (size_t)img.w * img.h * 3) return out;
    const int L = levelsFor(img.w, img.h, levels);
    out.info.levels = L;
    std::vector<float> pl[3];
    toPlanes(img, pl);
    Layout ly = makeLayout(img.w, img.h, L);
    int maxq = 0;
    for (int c = 0; c < 3; c++) dwtFwd(pl[c], ly.w[c], ly.h[c], ly.lv[c]);
    for (Band& b : ly.bands) {
        b.neg.assign(b.q.size(), 0);
        const std::vector<float>& P = pl[b.comp];
        for (int y = 0; y < b.bh; y++)
            for (int x = 0; x < b.bw; x++) {
                const float v = P[(size_t)(b.oy + y) * (size_t)ly.w[b.comp] + (size_t)(b.ox + x)];
                const int32_t q = (int32_t)std::min(std::fabs(v), 30000.f);
                b.q[(size_t)y * (size_t)b.bw + (size_t)x] = q;
                b.neg[(size_t)y * (size_t)b.bw + (size_t)x] = v < 0 ? 1 : 0;
                maxq = std::max(maxq, (int)q);
            }
    }
    int top = -1;
    for (int p = 0; p < 15; p++) if ((1 << p) <= maxq) top = p;
    out.info.topPlane = top;
    Enc enc(maxBytes);
    Passes<Enc> ps(enc, ly);
    for (int p = out.info.topPlane; p >= 0; p--) {
        if (!ps.sigPass(p)) break;
        if (!ps.refPass(p)) break;
    }
    out.info.decisions = enc.decisions;
    out.bytes = enc.finish();
    return out;
}

Image decodeImage(const uint8_t* bytes, size_t n, const StreamInfo& info) {
    Image im;
    if (info.w <= 0 || info.h <= 0 || info.levels < 1 || info.levels > 8) return im;
    Layout ly = makeLayout(info.w, info.h, info.levels);
    for (Band& b : ly.bands) b.neg.assign(b.q.size(), 0);
    Dec dec(bytes, n, info.decisions);
    Passes<Dec> ps(dec, ly);
    for (int p = info.topPlane; p >= 0; p--) {
        if (!ps.sigPass(p)) break;
        if (!ps.refPass(p)) break;
    }
    std::vector<float> pl[3];
    for (int c = 0; c < 3; c++) pl[c].assign((size_t)ly.w[c] * ly.h[c], 0.f);
    for (const Band& b : ly.bands) {
        std::vector<float>& P = pl[b.comp];
        for (int y = 0; y < b.bh; y++)
            for (int x = 0; x < b.bw; x++) {
                const size_t qi = (size_t)y * (size_t)b.bw + (size_t)x;
                if (b.lp[qi] < 0) continue;
                const float v = (float)b.q[qi] + 0.42f * (float)(1 << b.lp[qi]);
                P[(size_t)(b.oy + y) * (size_t)ly.w[b.comp] + (size_t)(b.ox + x)] = b.neg[qi] ? -v : v;
            }
    }
    for (int c = 0; c < 3; c++) dwtInv(pl[c], ly.w[c], ly.h[c], ly.lv[c]);
    return fromPlanes(pl, info.w, info.h);
}

double psnrRgb(const Image& a, const Image& b) {
    if (a.w != b.w || a.h != b.h || a.rgb.size() != b.rgb.size() || a.rgb.empty()) return 0.0;
    double se = 0;
    for (size_t i = 0; i < a.rgb.size(); i++) {
        const double d = (double)a.rgb[i] - (double)b.rgb[i];
        se += d * d;
    }
    if (se == 0) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 / (se / (double)a.rgb.size()));
}

// Demo / test picture: smooth colour gradients, a landscape-like horizon, bars of thin text-like strokes, soft discs and a band of fine noise texture.
Image syntheticPicture(int w, int h) {
    Image im;
    im.w = w; im.h = h;
    im.rgb.resize((size_t)w * h * 3);
    kris::XorShift64 xs{0x77ull * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull};
    bool have = false;
    double spare = 0.0;
    auto uniform = [&] { return (double)(xs.next() >> 11) * (1.0 / 9007199254740992.0); };
    auto gauss = [&] {
        if (have) { have = false; return spare; }
        double u1 = uniform();
        const double u2 = uniform();
        if (u1 < 1e-300) u1 = 1e-300;
        const double r = std::sqrt(-2.0 * std::log(u1));
        spare = r * std::sin(6.283185307179586 * u2);
        have = true;
        return r * std::cos(6.283185307179586 * u2);
    };
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const double u = (double)x / w, v = (double)y / h;
            double r = 60 + 150 * u, g = 90 + 100 * (1 - v) + 40 * std::sin(6.0 * u), b = 200 - 140 * u * v + 30 * std::cos(9.0 * v);
            if (v > 0.62) { r = 40 + 60 * v; g = 110 - 40 * (v - 0.62) / 0.38 + 15 * std::sin(40.0 * u + 8.0 * v); b = 50; }   // ground
            // discs
            const double cx[3] = {0.25, 0.7, 0.5}, cy[3] = {0.3, 0.35, 0.8}, rr[3] = {0.12, 0.09, 0.07};
            for (int k = 0; k < 3; k++) {
                const double dx = (u - cx[k]) * w, dy = (v - cy[k]) * w, d = std::sqrt(dx * dx + dy * dy) / (rr[k] * w);
                if (d < 1.0) {
                    const double s = 0.5 + 0.5 * std::cos(3.14159 * d);
                    r = r * (1 - s) + (k == 0 ? 250 : (k == 1 ? 240 : 60)) * s;
                    g = g * (1 - s) + (k == 0 ? 230 : (k == 1 ? 60 : 200)) * s;
                    b = b * (1 - s) + (k == 0 ? 60 : (k == 1 ? 70 : 220)) * s;
                }
            }
            // text-like strokes in a panel
            if (y > h * 0.08 && y < h * 0.20 && x > w * 0.05 && x < w * 0.55) {
                const int lx = x - (int)(w * 0.05), ly = y - (int)(h * 0.08);
                const bool glyph = ((lx / 6) % 3 != 2) && ((ly / 3) % 4 != 3) && (((lx / 6) * 7 + (ly / 14) * 13) % 5 < 3);
                r = glyph ? 20 : 235; g = glyph ? 20 : 235; b = glyph ? 30 : 225;
            }
            // texture band
            if (y > h * 0.85 && y < h * 0.95) {
                const double nse = gauss() * 18.0;
                r += nse; g += nse; b += nse;
            }
            uint8_t* o = &im.rgb[3 * ((size_t)y * w + x)];
            o[0] = (uint8_t)std::clamp(r, 0.0, 255.0);
            o[1] = (uint8_t)std::clamp(g, 0.0, 255.0);
            o[2] = (uint8_t)std::clamp(b, 0.0, 255.0);
        }
    }
    return im;
}


}  // namespace dect2::kait
