#include "dect2/conceal.h"
#include "dect2/fftutil.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <cstdlib>
#include <cstdio>

namespace dect2 {

// =============================================================================================== video

namespace {

struct Plane {
    int w = 0, h = 0;
    std::vector<uint8_t> v;
};

Plane downscale4(const uint8_t* y, int w, int h) {
    Plane p; p.w = w / 4; p.h = h / 4; p.v.resize((size_t)p.w * p.h);
    for (int j = 0; j < p.h; j++)
        for (int i = 0; i < p.w; i++) {
            int s = 0;
            for (int dy = 0; dy < 4; dy++) for (int dx = 0; dx < 4; dx++) s += y[(size_t)(j * 4 + dy) * w + (size_t)(i * 4 + dx)];
            p.v[(size_t)j * p.w + i] = (uint8_t)(s >> 4);
        }
    return p;
}

inline float bilin(const uint8_t* pl, int w, int h, float x, float y) {
    x = std::min(std::max(x, 0.f), (float)(w - 1)); y = std::min(std::max(y, 0.f), (float)(h - 1));
    const int x0 = (int)x, y0 = (int)y, x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
    const float fx = x - x0, fy = y - y0;
    const float a = pl[(size_t)y0 * w + x0] * (1 - fx) + pl[(size_t)y0 * w + x1] * fx;
    const float b = pl[(size_t)y1 * w + x0] * (1 - fx) + pl[(size_t)y1 * w + x1] * fx;
    return a * (1 - fy) + b * fy;
}

} // namespace

static bool interpolateGapSoftware(const VideoFrame& A, const VideoFrame& B, int count, std::vector<std::shared_ptr<VideoFrame>>& out, bool cutCheckOnly) {
    if (count <= 0 || A.w != B.w || A.h != B.h || A.w < 64 || A.h < 64) return false;
    if (!A.rgba.empty() || !B.rgba.empty() || A.y.size() != (size_t)A.w * A.h || B.y.size() != A.y.size() || A.uv.size() != B.uv.size() || A.uv.size() < (size_t)A.w * A.h / 2) return false;
    const int w = A.w, h = A.h;
    // ---- block matching, two levels. Level 1 on an eighth-resolution luma image (4x4 blocks = 32x32 pixels, search +-10 = +-80 pixels:
    // a fast pan over several hundred milliseconds), level 2 refines on the quarter-resolution image (8x8 blocks, +-2).
    const Plane pa = downscale4(A.y.data(), w, h), pb = downscale4(B.y.data(), w, h);
    auto half = [](const Plane& q) {
        Plane r; r.w = q.w / 2; r.h = q.h / 2; r.v.resize((size_t)r.w * r.h);
        for (int j = 0; j < r.h; j++) for (int i = 0; i < r.w; i++)
            r.v[(size_t)j * r.w + i] = (uint8_t)((q.v[(size_t)(2 * j) * q.w + 2 * i] + q.v[(size_t)(2 * j) * q.w + 2 * i + 1] + q.v[(size_t)(2 * j + 1) * q.w + 2 * i] + q.v[(size_t)(2 * j + 1) * q.w + 2 * i + 1]) >> 2);
        return r;
    };
    const Plane qa = half(pa), qb = half(pb);
    const int bs = 8;
    const int bw = std::min(pa.w / bs, qa.w / 4), bh = std::min(pa.h / bs, qa.h / 4);
    if (bw < 2 || bh < 2) return false;
    auto search = [&](const Plane& P, const Plane& Q, int bsz, int bx, int by, int cx, int cy, int rng, int pen) {
        long best = -1; int bdx = 0, bdy = 0;
        for (int dy = cy - rng; dy <= cy + rng; dy++) {
            const int y0 = by * bsz + dy;
            if (y0 < 0 || y0 + bsz > Q.h) continue;
            for (int dx = cx - rng; dx <= cx + rng; dx++) {
                const int x0 = bx * bsz + dx;
                if (x0 < 0 || x0 + bsz > Q.w) continue;
                long sad = 0;
                for (int j = 0; j < bsz && (best < 0 || sad < best); j++) {
                    const uint8_t* a = &P.v[(size_t)(by * bsz + j) * P.w + (size_t)bx * bsz];
                    const uint8_t* b = &Q.v[(size_t)(y0 + j) * Q.w + (size_t)x0];
                    for (int i = 0; i < bsz; i++) sad += std::abs((int)a[i] - (int)b[i]);
                }
                sad += (long)(std::abs(dx) + std::abs(dy)) * pen;
                if (best < 0 || sad < best) { best = sad; bdx = dx; bdy = dy; }
            }
        }
        return std::make_pair(bdx, bdy);
    };
    auto sadAt = [&](const Plane& P, const Plane& Q, int bsz, int bx, int by, int dx, int dy) {
        const int y0 = by * bsz + dy, x0 = bx * bsz + dx;
        if (y0 < 0 || x0 < 0 || y0 + bsz > Q.h || x0 + bsz > Q.w) return (long)1 << 40;
        long sad = 0;
        for (int j = 0; j < bsz; j++) { const uint8_t* a = &P.v[(size_t)(by * bsz + j) * P.w + (size_t)bx * bsz]; const uint8_t* b = &Q.v[(size_t)(y0 + j) * Q.w + (size_t)x0]; for (int i = 0; i < bsz; i++) sad += std::abs((int)a[i] - (int)b[i]); }
        return sad;
    };
    // motion of every 32x32 block from picture P to picture Q (quarter-resolution units)
    auto estimate = [&](const Plane& P4, const Plane& Q4, const Plane& P8, const Plane& Q8, std::vector<int8_t>& vx, std::vector<int8_t>& vy) {
        vx.assign((size_t)bw * bh, 0); vy.assign((size_t)bw * bh, 0);
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                const auto c1 = search(P8, Q8, 4, bx, by, 0, 0, 20, 1);                 // eighth-resolution units
                auto f = search(P4, Q4, 8, bx, by, c1.first * 2, c1.second * 2, 2, 2);   // quarter-resolution units
                const long s0 = sadAt(P4, Q4, 8, bx, by, 0, 0), s1 = sadAt(P4, Q4, 8, bx, by, f.first, f.second);
                if (s1 * 10 > s0 * 7) f = {0, 0};   // no convincing match: the block stays put
                vx[(size_t)by * bw + bx] = (int8_t)std::max(-127, std::min(127, f.first)); vy[(size_t)by * bw + bx] = (int8_t)std::max(-127, std::min(127, f.second));
            }
    };
    // flat blocks (a clear sky, the inside of a plain object) match equally well with many vectors: let them follow the median motion
    // of their neighbours whenever that vector is about as good as their own
    auto smoothField = [&](const Plane& P4, const Plane& Q4, std::vector<int8_t>& vx, std::vector<int8_t>& vy) {
        for (int pass = 0; pass < 2; pass++) {
            std::vector<int8_t> nx = vx, ny = vy;
            for (int by = 0; by < bh; by++)
                for (int bx = 0; bx < bw; bx++) {
                    int ax[9], ay[9], n = 0;
                    for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) { const int yy = std::min(bh - 1, std::max(0, by + j)), xx = std::min(bw - 1, std::max(0, bx + i)); ax[n] = vx[(size_t)yy * bw + xx]; ay[n] = vy[(size_t)yy * bw + xx]; n++; }
                    std::nth_element(ax, ax + 4, ax + 9); std::nth_element(ay, ay + 4, ay + 9);
                    const int mx = ax[4], my = ay[4];
                    const int ox = vx[(size_t)by * bw + bx], oy = vy[(size_t)by * bw + bx];
                    if (mx == ox && my == oy) continue;
                    const long so = sadAt(P4, Q4, 8, bx, by, ox, oy), sm = sadAt(P4, Q4, 8, bx, by, mx, my);
                    if (sm <= so + 64 + so / 20) { nx[(size_t)by * bw + bx] = (int8_t)mx; ny[(size_t)by * bw + bx] = (int8_t)my; }
                }
            vx = nx; vy = ny;
        }
    };
    std::vector<int8_t> mvx, mvy, bvx, bvy;
    estimate(pa, pb, qa, qb, mvx, mvy);     // forward  A -> B
    estimate(pb, pa, qb, qa, bvx, bvy);     // backward B -> A
    smoothField(pa, pb, mvx, mvy);
    smoothField(pb, pa, bvx, bvy);
    // the motions that occur in the picture: the distinct block vectors, most frequent first (at most 6)
    std::vector<std::pair<int, int>> motions;
    {
        std::vector<std::pair<int, std::pair<int, int>>> hist;
        for (size_t i = 0; i < mvx.size(); i++) {
            if (!mvx[i] && !mvy[i]) continue;
            bool found = false;
            for (auto& hh : hist) if (std::abs(hh.second.first - mvx[i]) <= 3 && std::abs(hh.second.second - mvy[i]) <= 3) { hh.first++; found = true; break; }
            if (!found) hist.push_back({1, {mvx[i], mvy[i]}});
        }
        std::sort(hist.begin(), hist.end(), [](auto& a, auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < hist.size() && i < 6; i++) if (hist[i].first >= 2) motions.push_back({hist[i].second.first * 4, hist[i].second.second * 4});   // full-resolution pixels
    }
    // A scene cut inside the gap: even with the best motion the two pictures do not match. Morphing across a cut looks like a smear,
    // so show the nearer real picture instead.
    {
        double tot = 0;
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) tot += (double)std::min(sadAt(pa, pb, 8, bx, by, mvx[(size_t)by * bw + bx], mvy[(size_t)by * bw + bx]), sadAt(pa, pb, 8, bx, by, 0, 0)) / 64.0;
        if (getenv("DECT2_MVDEBUG")) fprintf(stderr, "avg block SAD per pixel %.1f\n", tot / ((double)bw * bh));
        if (cutCheckOnly && tot / ((double)bw * bh) <= 8.5) return false;   // no scene cut: the caller interpolates in another way
        if (tot / ((double)bw * bh) > 8.5) {
            for (int k = 0; k < count; k++) {
                const VideoFrame& src = (k + 1) * 2 <= count + 1 ? A : B;
                auto f = std::make_shared<VideoFrame>(src);
                f->interlaced = src.interlaced;
                out.push_back(std::move(f));
            }
            return true;
        }
    }
    if (getenv("DECT2_MVDEBUG")) { for (int by = 0; by < bh; by++) { for (int bx = 0; bx < bw; bx++) fprintf(stderr, "%3d,%3d ", mvx[(size_t)by * bw + bx], mvy[(size_t)by * bw + bx]); fprintf(stderr, "\n"); } }
    if (getenv("DECT2_MVDEBUG")) { for (int by = 0; by < bh; by++) { for (int bx = 0; bx < bw; bx++) fprintf(stderr, "%3d,%3d ", mvx[(size_t)by * bw + bx], mvy[(size_t)by * bw + bx]); fprintf(stderr, "\n"); } }
    // ---- synthesis. The vectors describe motion from picture A's grid, so for a picture at time t every moving block is projected
    // to where it will be (position + t * motion). Each 4x4 cell then chooses between "moves with that block" and "stays put",
    // whichever makes the two real pictures agree better at that cell (this also copes with covered and uncovered background).
    const int cw = w / 2, ch = h / 2;
    std::vector<uint8_t> aU((size_t)cw * ch), aV((size_t)cw * ch), bU((size_t)cw * ch), bV((size_t)cw * ch);
    for (int j = 0; j < ch; j++) for (int i = 0; i < cw; i++) {
        aU[(size_t)j * cw + i] = A.uv[(size_t)j * w + 2 * i]; aV[(size_t)j * cw + i] = A.uv[(size_t)j * w + 2 * i + 1];
        bU[(size_t)j * cw + i] = B.uv[(size_t)j * w + 2 * i]; bV[(size_t)j * cw + i] = B.uv[(size_t)j * w + 2 * i + 1];
    }
    const int cs = 4, gw = w / cs, gh = h / cs;
    std::vector<int16_t> cvx((size_t)gw * gh), cvy((size_t)gw * gh);
    std::vector<uint8_t> cset((size_t)gw * gh);
    for (int k = 0; k < count; k++) {
        const float t = (float)(k + 1) / (float)(count + 1);
        auto f = std::make_shared<VideoFrame>();
        f->w = w; f->h = h; f->bt709 = A.bt709; f->fullRange = A.fullRange; f->interlaced = false;
        f->y.resize((size_t)w * h); f->uv.resize(A.uv.size(), 128);
        std::fill(cset.begin(), cset.end(), 0);
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                const int fx = mvx[(size_t)by * bw + bx] * 4, fy = mvy[(size_t)by * bw + bx] * 4;
                if (!fx && !fy) continue;
                const int x0 = (int)std::lround(bx * 32 + t * fx), y0 = (int)std::lround(by * 32 + t * fy);
                for (int cy = std::max(0, y0 / cs); cy < std::min(gh, (y0 + 32) / cs); cy++)
                    for (int cx = std::max(0, x0 / cs); cx < std::min(gw, (x0 + 32) / cs); cx++) { cvx[(size_t)cy * gw + cx] = (int16_t)fx; cvy[(size_t)cy * gw + cx] = (int16_t)fy; cset[(size_t)cy * gw + cx] = 1; }
            }
        for (int cy = 0; cy < gh; cy++)
            for (int cx = 0; cx < gw; cx++) {
                // choose the motion that makes the two real pictures agree best at this cell (nearest-pixel SAD over an 8x8 window around it)
                float mx = 0, my = 0;
                if (!motions.empty() || cset[(size_t)cy * gw + cx]) {
                    auto score = [&](float px, float py) {
                        long sad = 0;
                        for (int j = -2; j < cs + 2; j += 2)
                            for (int i = -2; i < cs + 2; i += 2) {
                                const int x = std::min(w - 1, std::max(0, cx * cs + i)), y = std::min(h - 1, std::max(0, cy * cs + j));
                                const int xa = std::min(w - 1, std::max(0, (int)std::lround(x - t * px))), ya = std::min(h - 1, std::max(0, (int)std::lround(y - t * py)));
                                const int xb = std::min(w - 1, std::max(0, (int)std::lround(x + (1 - t) * px))), yb = std::min(h - 1, std::max(0, (int)std::lround(y + (1 - t) * py)));
                                sad += std::abs((int)A.y[(size_t)ya * w + xa] - (int)B.y[(size_t)yb * w + xb]);
                            }
                        return sad;
                    };
                    long best = score(0, 0);
                    // a cell that a moving block is projected onto prefers that motion unless the two real pictures clearly contradict it
                    // (an object that moves further than its own size leaves both pictures looking identical at its halfway position)
                    if (cset[(size_t)cy * gw + cx]) {
                        const float px = (float)cvx[(size_t)cy * gw + cx], py = (float)cvy[(size_t)cy * gw + cx];
                        const long sc = score(px, py);
                        if (sc <= best + 40) { best = sc; mx = px; my = py; }
                    }
                    if (mx == 0 && my == 0)
                        for (auto& m : motions) { const long sc = score((float)m.first, (float)m.second); if (sc * 10 < best * 8) { best = sc; mx = (float)m.first; my = (float)m.second; } }
                }
                for (int j = 0; j < cs; j++)
                    for (int i = 0; i < cs; i++) {
                        const int x = cx * cs + i, y = cy * cs + j;
                        const float va = bilin(A.y.data(), w, h, x - t * mx, y - t * my);
                        const float vb = bilin(B.y.data(), w, h, x + (1 - t) * mx, y + (1 - t) * my);
                        float wb = t;
                        if (std::fabs(va - vb) > 40.f) wb = t < 0.5f ? 0.f : 1.f;   // still disagreeing (occlusion): trust the nearer picture
                        f->y[(size_t)y * w + x] = (uint8_t)std::min(255.f, std::max(0.f, va * (1 - wb) + vb * wb + 0.5f));
                    }
                // chroma of this cell (2x2 samples)
                for (int j = 0; j < cs / 2; j++)
                    for (int i = 0; i < cs / 2; i++) {
                        const int x = cx * cs / 2 + i, y = cy * cs / 2 + j;
                        const float fx = mx * 0.5f, fy = my * 0.5f;
                        const float ua = bilin(aU.data(), cw, ch, x - t * fx, y - t * fy), ub = bilin(bU.data(), cw, ch, x + (1 - t) * fx, y + (1 - t) * fy);
                        const float va = bilin(aV.data(), cw, ch, x - t * fx, y - t * fy), vb = bilin(bV.data(), cw, ch, x + (1 - t) * fx, y + (1 - t) * fy);
                        const float wb = (std::fabs(ua - ub) + std::fabs(va - vb) > 40.f) ? (t < 0.5f ? 0.f : 1.f) : t;
                        f->uv[(size_t)y * w + 2 * x] = (uint8_t)std::min(255.f, std::max(0.f, ua * (1 - wb) + ub * wb + 0.5f));
                        f->uv[(size_t)y * w + 2 * x + 1] = (uint8_t)std::min(255.f, std::max(0.f, va * (1 - wb) + vb * wb + 0.5f));
                    }
            }
        out.push_back(std::move(f));
    }
    return true;
}


#ifndef __APPLE__
bool appleInterpolationAvailable() { return false; }
bool interpolateGapApple(const VideoFrame&, const VideoFrame&, int, std::vector<std::shared_ptr<VideoFrame>>&) { return false; }
#endif

void prepareInterpolation(int w, int h) {
    if (!appleInterpolationAvailable() || getenv("DECT2_SWINTERP") || w < 64 || h < 64) return;
    std::thread([w, h] {
        VideoFrame a, b;
        a.w = b.w = w; a.h = b.h = h;
        a.y.assign((size_t)w * h, 100); b.y.assign((size_t)w * h, 110);
        a.uv.assign((size_t)w * h / 2, 128); b.uv = a.uv;
        std::vector<std::shared_ptr<VideoFrame>> out;
        interpolateGapApple(a, b, 1, out);   // creates the session and runs the model once
    }).detach();
}

const char* interpolationBackend() {
    return appleInterpolationAvailable() && !getenv("DECT2_SWINTERP") ? "Apple ML" : "motion search";
}

bool interpolateGap(const VideoFrame& A, const VideoFrame& B, int count, std::vector<std::shared_ptr<VideoFrame>>& out) {
    if (appleInterpolationAvailable() && !getenv("DECT2_SWINTERP") && count >= 1 && A.w == B.w && A.h == B.h) {
        // a scene cut inside the gap cannot be interpolated by any model: show the nearer real picture (checked with the motion search)
        if (interpolateGapSoftware(A, B, count, out, true)) return true;
        if (interpolateGapApple(A, B, count, out)) return true;
    }
    return interpolateGapSoftware(A, B, count, out, false);
}

} // namespace dect2
