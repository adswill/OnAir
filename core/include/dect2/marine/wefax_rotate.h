// Weather fax picture rotation: quarter turns clockwise (0..3), for the live picture, the saved PNGs and the gallery. Pure and header-only.
// Row-major pictures of any pixel type (8-bit grey or 32-bit pixels). A turn count outside 0..3 wraps (-1 = one turn counter-clockwise).
#pragma once
#include <cstddef>
#include <vector>

namespace dect2 {

inline int wefaxTurns(int turns) { return ((turns % 4) + 4) % 4; }

// Output size of the rotated picture
inline void wefaxRotatedSize(int w, int h, int turns, int& ow, int& oh) {
    if (wefaxTurns(turns) & 1) { ow = h; oh = w; } else { ow = w; oh = h; }
}

// Rotates src (w x h) by `turns` quarter turns clockwise into out (resized); ow / oh get the new size. src and out must be different buffers.
template <class T>
void wefaxRotate(const T* src, int w, int h, int turns, std::vector<T>& out, int& ow, int& oh) {
    const int t = wefaxTurns(turns);
    wefaxRotatedSize(w, h, t, ow, oh);
    out.assign((size_t)ow * (size_t)oh, T());
    if (w <= 0 || h <= 0 || !src) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int nx, ny;
            switch (t) {
            case 1: nx = h - 1 - y; ny = x; break;               // 90 clockwise
            case 2: nx = w - 1 - x; ny = h - 1 - y; break;       // 180
            case 3: nx = y; ny = w - 1 - x; break;               // 90 counter-clockwise
            default: nx = x; ny = y; break;
            }
            out[(size_t)ny * (size_t)ow + (size_t)nx] = src[(size_t)y * (size_t)w + (size_t)x];
        }
}

} // namespace dect2
