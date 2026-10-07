// Analog TV test card, see atv_card.h.
#include "dect2/atv_card.h"
#include "atv_dsp.h"
#include "dect2/dsp_compat.h"
#include "atv_font.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace dect2 {

namespace {
using atvdsp::kPi;

struct Yuv { float y, u, v; };
Yuv fromRgb(float r, float g, float b) { Yuv c; atvRgbToYuv(r, g, b, c.y, c.u, c.v); return c; }

float triangle(double x) { const double m = std::fmod(std::fabs(x), 2.0); return (float)(m < 1 ? m : 2 - m); }

// Row-wise convolution of x0 .. x1-1 with a symmetric filter, the row edge repeated
void filterSpan(float* row, int w, int x0, int x1, const std::vector<float>& h) {
    x0 = std::max(0, x0); x1 = std::min(w, x1);
    if (x1 <= x0) return;
    const int nt = (int)h.size(), m = nt / 2, n = x1 - x0;
    thread_local std::vector<float> pad, out;
    pad.resize((size_t)(n + nt - 1)); out.resize((size_t)n);
    for (int i = 0; i < n + nt - 1; i++) pad[(size_t)i] = row[std::min(w - 1, std::max(0, x0 + i - m))];
    desamp(pad.data(), 1, h.data(), out.data(), n, nt);
    for (int i = 0; i < n; i++) row[x0 + i] = out[(size_t)i];
}
} // namespace

// A drawing surface over planar rows. parity < 0: every frame row is stored at its own index; parity 0 or 1: only the rows of that field,
// stored at row >> 1. Drawing calls use frame coordinates and skip the rows that are not stored.
struct AtvCard::Canvas {
    int w = 0, h = 0, parity = -1;
    float *y = nullptr, *u = nullptr, *v = nullptr;
    bool has(int r) const { return r >= 0 && r < h && (parity < 0 || (r & 1) == parity); }
    size_t idx(int r) const { return (size_t)(parity < 0 ? r : r >> 1) * (size_t)w; }
    void rect(int x0, int y0, int x1, int y1, Yuv c) {
        x0 = std::max(0, x0); x1 = std::min(w, x1);
        for (int r = std::max(0, y0); r < std::min(h, y1); r++) {
            if (!has(r)) continue;
            const size_t o = idx(r);
            for (int x = x0; x < x1; x++) { y[o + (size_t)x] = c.y; u[o + (size_t)x] = c.u; v[o + (size_t)x] = c.v; }
        }
    }
    void blend(int x, int r, float a, Yuv c) {
        if (x < 0 || x >= w || !has(r) || a <= 0) return;
        const size_t o = idx(r) + (size_t)x;
        y[o] += a * (c.y - y[o]); u[o] += a * (c.u - u[o]); v[o] += a * (c.v - v[o]);
    }
    void disk(double cx, double cy, double rad, Yuv c) {      // anti-aliased filled circle
        for (int r = (int)std::floor(cy - rad - 1); r <= (int)std::ceil(cy + rad + 1); r++) {
            if (!has(r)) continue;
            for (int x = (int)std::floor(cx - rad - 1); x <= (int)std::ceil(cx + rad + 1); x++) {
                const double d = std::hypot(x + 0.5 - cx, r + 0.5 - cy);
                blend(x, r, (float)std::min(1.0, std::max(0.0, rad - d + 0.5)), c);
            }
        }
    }
    void text(int x0, int y0, int s, const std::string& str, Yuv c) {
        for (size_t i = 0; i < str.size(); i++) {
            const uint8_t* g = atvGlyph(str[i]);
            for (int ry = 0; ry < 7; ry++)
                for (int rx = 0; rx < 5; rx++)
                    if (g[ry] & (1 << (4 - rx))) rect(x0 + ((int)i * 6 + rx) * s, y0 + ry * s, x0 + ((int)i * 6 + rx + 1) * s, y0 + (ry + 1) * s, c);
        }
    }
};

static int textWidth(int n, int s) { return n > 0 ? (n * 6 - 1) * s : 0; }

AtvCard::AtvCard(const AtvFormat& f, int pattern) : f_(f), pattern_(pattern), w_(f.picW), h_(f.picH) {
    const double fpix = w_ / (f.activeUs * 1e-6);
    const double bw = std::min(f.videoBwMhz, 5.0) * 1e6;
    lpY_ = atvdsp::lowpass(0.8 * bw, std::min(bw + 0.6e6, 0.48 * fpix), fpix, 55, 61);
    lpC_ = atvdsp::lowpass(0.9e6, 2.2e6, fpix, 45, 61);
    textScale_ = std::max(2, (int)std::lround(h_ / 144.0));
    label_ = f.name;
    buildStatic();
}

void AtvCard::buildStatic() {
    const int W = w_, H = h_;
    sy_.assign((size_t)W * H, 0.f); su_.assign(sy_.size(), 0.f); sv_.assign(sy_.size(), 0.f);
    Canvas c; c.w = W; c.h = H; c.parity = -1; c.y = sy_.data(); c.u = su_.data(); c.v = sv_.data();
    const Yuv white = fromRgb(1, 1, 1), bg = fromRgb(0.2f, 0.2f, 0.2f), black = fromRgb(0.05f, 0.05f, 0.05f);
    c.rect(0, 0, W, H, bg);
    auto X = [&](double fx) { return (int)std::lround(fx * W); };
    auto Y = [&](double fy) { return (int)std::lround(fy * H); };
    auto bars = [&](int x0, int x1, int y0, int y1) {
        for (int i = 0; i < 8; i++) {
            float rgb[3]; atvEbuBar(i, rgb);
            c.rect(x0 + (x1 - x0) * i / 8, y0, x0 + (x1 - x0) * (i + 1) / 8, y1, fromRgb(rgb[0], rgb[1], rgb[2]));
        }
    };
    auto ramp = [&](int x0, int x1, int y0, int y1) {
        for (int x = x0; x < x1; x++) { const float g = (x - x0 + 0.5f) / (x1 - x0); c.rect(x, y0, x + 1, y1, fromRgb(g, g, g)); }
    };
    if (pattern_ == 1) {
        barX0_ = 0; barX1_ = W; barY0_ = 0; barY1_ = H;
        bars(0, W, 0, H);
    } else if (pattern_ == 2) {
        rampX0_ = 0; rampX1_ = W; rampY0_ = 0; rampY1_ = H;
        ramp(0, W, 0, H);
    } else {
        const int x0 = X(0.06), x1 = X(0.94);
        barX0_ = x0; barX1_ = x1; barY0_ = Y(0.07); barY1_ = Y(0.30);
        bars(x0, x1, barY0_, barY1_);
        // grey ramp and a staircase of eleven steps
        rampX0_ = x0; rampX1_ = X(0.50); rampY0_ = Y(0.33); rampY1_ = Y(0.40);
        ramp(rampX0_, rampX1_, rampY0_, rampY1_);
        for (int i = 0; i < 11; i++) {
            const float g = i / 10.f;
            c.rect(rampX0_ + (rampX1_ - rampX0_) * i / 11, Y(0.40), rampX0_ + (rampX1_ - rampX0_) * (i + 1) / 11, Y(0.47), fromRgb(g, g, g));
        }
        // multiburst: flat grey here, the generator draws the sine packets itself
        const int mx0 = X(0.53), mx1 = X(0.94);
        mbRow0_ = Y(0.33); mbRow1_ = Y(0.47);
        c.rect(mx0, mbRow0_, mx1, mbRow1_, fromRgb(0.5f, 0.5f, 0.5f));
        const double mhz[6] = {0.5, 1.0, 2.0, 3.0, f_.ntsc || f_.sys == kAtvM || f_.sys == kAtvN ? 3.58 : 4.0, f_.sys == kAtvM || f_.sys == kAtvN ? 4.2 : 4.8};
        const double uPerPx = f_.activeUs / W;
        for (int i = 0; i < 6; i++) {
            const int a = mx0 + (mx1 - mx0) * i / 6, b = mx0 + (mx1 - mx0) * (i + 1) / 6;
            mb_.push_back({f_.blankEndUs + (a + 3) * uPerPx, f_.blankEndUs + (b - 3) * uPerPx, mhz[i]});
        }
        // the name of the test
        const int ts = textScale_;
        const std::string title = "ONAIR TEST";
        c.text((W - textWidth((int)title.size(), ts)) / 2, Y(0.50), ts, title, white);
        // resolution wedge: a sine sweep from 12 pixels to 2.2 pixels a period
        const int wx0 = x0, wx1 = X(0.30), wy0 = Y(0.60), wy1 = Y(0.75);
        {
            const double p0 = 12, p1 = 2.2, L = wx1 - wx0, a = std::log(p1 / p0) / L;
            for (int x = wx0; x < wx1; x++) {
                const double s = x + 0.5 - wx0, ph = 2 * kPi / p0 * (1 - std::exp(-a * s)) / a;
                const float g = (float)(0.5 + 0.4 * std::sin(ph));
                c.rect(x, wy0, x + 1, wy1, fromRgb(g, g, g));
            }
        }
        // checkerboard of 4 pixel squares
        const int cy0 = Y(0.77), cy1 = Y(0.92);
        for (int r = cy0; r < cy1; r++)
            for (int x = wx0; x < wx1; x++) { const float g = (((x - wx0) / 4 + (r - cy0) / 4) & 1) ? 0.9f : 0.1f; c.rect(x, r, x + 1, r + 1, fromRgb(g, g, g)); }
        // the circle with a cross, the name of the system inside
        const double cx = 0.5 * W, cyc = 0.76 * H, R = 0.16 * H;
        c.disk(cx, cyc, R, white);
        c.disk(cx, cyc, R - 4, fromRgb(0.3f, 0.3f, 0.3f));
        c.rect((int)(cx - R - 8), (int)cyc - 1, (int)(cx + R + 8), (int)cyc + 1, white);
        c.rect((int)cx - 1, (int)(cyc - R - 8), (int)cx + 1, (int)(cyc + R + 8), white);
        std::string l1 = f_.colourName + " " + f_.sysName, l2 = std::to_string(f_.lines) + "/" + (f_.lines == 625 ? "50" : "60");
        if (f_.colour == kAtvMono) l1 = f_.sysName;
        c.rect((int)(cx - R + 8), (int)(cyc - 18), (int)(cx + R - 8), (int)(cyc + 18), fromRgb(0.3f, 0.3f, 0.3f));
        c.text((int)(cx - textWidth((int)l1.size(), 2) / 2.0), (int)cyc - 15, 2, l1, white);
        c.text((int)(cx - textWidth((int)l2.size(), 2) / 2.0), (int)cyc + 2, 2, l2, white);
        // the panel the box moves in
        trackX0_ = X(0.70); trackX1_ = X(0.94); trackY0_ = Y(0.60); trackY1_ = Y(0.92);
        boxSize_ = (int)std::lround(0.11 * H);
        c.rect(trackX0_, trackY0_, trackX1_, trackY1_, white);
        c.rect(trackX0_ + 2, trackY0_ + 2, trackX1_ - 2, trackY1_ - 2, black);
        // the clock strip
        clockY0_ = Y(0.935); clockY1_ = Y(0.985);
        // a white line round the picture, to see where the picture edge is
        c.rect(0, 0, W, 3, white); c.rect(0, H - 3, W, H, white); c.rect(0, 0, 3, H, white); c.rect(W - 3, 0, W, H, white);
    }
    // the picture goes through the video band: Y to about 5 MHz, the colour differences to about 1 MHz
    for (int r = 0; r < H; r++) {
        const size_t o = (size_t)r * W;
        filterSpan(&sy_[o], W, 0, W, lpY_);
        filterSpan(&su_[o], W, 0, W, lpC_);
        filterSpan(&sv_[o], W, 0, W, lpC_);
    }
}

void AtvCard::drawDynamic(Canvas& c, double t, uint64_t frameNo) const {
    if (pattern_ != 0) return;
    const int W = w_;
    const Yuv white = fromRgb(1, 1, 1), black = fromRgb(0.05f, 0.05f, 0.05f), red = fromRgb(0.9f, 0.1f, 0.1f);
    // the box: bounces inside the panel, 150 and 110 pixels a second
    const int inX0 = trackX0_ + 4, inX1 = trackX1_ - 4, inY0 = trackY0_ + 4, inY1 = trackY1_ - 4;
    const double rx = std::max(1, inX1 - inX0 - boxSize_), ry = std::max(1, inY1 - inY0 - boxSize_);
    const int bx = inX0 + (int)std::lround(triangle(t * 150.0 / rx) * rx), by = inY0 + (int)std::lround(triangle(t * 110.0 / ry + 0.3) * ry);
    c.rect(inX0, inY0, inX1, inY1, black);
    c.rect(bx, by, bx + boxSize_, by + boxSize_, white);
    c.rect(bx + 3, by + 3, bx + boxSize_ - 3, by + boxSize_ - 3, red);
    // the clock and the frame counter
    const int s = std::max(2, textScale_ - 1);
    const int ty = clockY0_ + std::max(0, (clockY1_ - clockY0_ - 7 * s) / 2);
    c.rect(0, clockY0_, W, clockY1_, fromRgb(0.2f, 0.2f, 0.2f));
    const long secs = (long)t;
    char buf[40];
    snprintf(buf, sizeof buf, "%02ld:%02ld:%02ld", (secs / 3600) % 100, (secs / 60) % 60, secs % 60);
    const int lx = (int)std::lround(0.06 * W);
    c.text(lx, ty, s, buf, white);
    snprintf(buf, sizeof buf, "FRAME %06llu", (unsigned long long)(frameNo % 1000000));
    c.text((int)std::lround(0.94 * W) - textWidth((int)strlen(buf), s), ty, s, buf, white);
    // band-limit what was drawn
    auto band = [&](int x0, int y0, int x1, int y1) {
        for (int r = y0; r < y1; r++) {
            if (!c.has(r)) continue;
            const size_t o = c.idx(r);
            filterSpan(c.y + o, W, x0, x1, lpY_);
            filterSpan(c.u + o, W, x0, x1, lpC_);
            filterSpan(c.v + o, W, x0, x1, lpC_);
        }
    };
    band(inX0 - 8, inY0, inX1 + 8, inY1);
    band(0, clockY0_, W, clockY1_);
}

void AtvCard::renderField(int field, double t, uint64_t frameNo, float* y, float* u, float* v) const {
    const int rows = f_.fieldRows;
    for (int k = 0; k < rows; k++) {
        const int r = 2 * k + field;
        const size_t src = (size_t)std::min(r, h_ - 1) * w_;
        memcpy(y + (size_t)k * w_, &sy_[src], sizeof(float) * (size_t)w_);
        memcpy(u + (size_t)k * w_, &su_[src], sizeof(float) * (size_t)w_);
        memcpy(v + (size_t)k * w_, &sv_[src], sizeof(float) * (size_t)w_);
    }
    Canvas c; c.w = w_; c.h = h_; c.parity = field; c.y = y; c.u = u; c.v = v;
    drawDynamic(c, t, frameNo);
}

void AtvCard::referenceFrame(double t, uint64_t frameNo, std::vector<uint8_t>& rgba) const {
    const int rows = f_.fieldRows;
    std::vector<float> y((size_t)rows * w_), u(y.size()), v(y.size());
    rgba.assign((size_t)w_ * h_ * 4, 255);
    for (int field = 0; field < 2; field++) {
        renderField(field, t + field / f_.fieldHz, frameNo, y.data(), u.data(), v.data());
        for (int k = 0; k < rows; k++) {
            const int r = 2 * k + field;
            if (r >= h_) break;
            for (int x = 0; x < w_; x++) {
                float R, G, B;
                const size_t i = (size_t)k * w_ + x;
                atvYuvToRgb(y[i], u[i], v[i], R, G, B);
                uint8_t* p = &rgba[((size_t)r * w_ + x) * 4];
                p[0] = (uint8_t)std::lround(std::min(1.f, std::max(0.f, R)) * 255); p[1] = (uint8_t)std::lround(std::min(1.f, std::max(0.f, G)) * 255);
                p[2] = (uint8_t)std::lround(std::min(1.f, std::max(0.f, B)) * 255);
            }
        }
    }
}

} // namespace dect2
