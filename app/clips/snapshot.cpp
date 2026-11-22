// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Share-ready snapshot of the spectrum and waterfall: the newest spectrum row as a trace, the waterfall under it, a caption strip with
// frequency, mode, time, an optional place and the OnAir version. Pure pixels, no GUI (docs/SNAPSHOT.md).
#include "clips.h"
#include <algorithm>

namespace clips {

std::string snapCaption(const SnapCaption& c) {
    std::string s = c.mode.empty() ? "Radio" : c.mode;
    if (c.freqHz > 0) s += " @ " + freqText(c.freqHz);
    s += " | " + utcText(c.epoch);
    if (!c.place.empty()) s += " | " + c.place;
    s += " | OnAir" + (c.version.empty() ? std::string() : " " + c.version);
    return s;
}

std::string snapshotFileName(const SnapCaption& c) {
    Meta m;
    m.mode = c.mode; m.freqHz = c.freqHz; m.t0 = c.epoch;
    return fileStem(m) + ".png";
}

static void put(Image& im, int x, int y, uint32_t rgb) {
    if (x < 0 || y < 0 || x >= im.w || y >= im.h) return;
    uint8_t* p = &im.rgba[((size_t)y * im.w + x) * 4];
    p[0] = rgb & 255; p[1] = (rgb >> 8) & 255; p[2] = (rgb >> 16) & 255; p[3] = 255;
}

Image shareSnapshot(const std::vector<uint8_t>& lv, int w, int nRows, const uint32_t* lut, const SnapCaption& cap, int outW) {
    const int W = std::max(320, outW);
    const bool have = w > 0 && nRows > 0 && lv.size() >= (size_t)w * (size_t)nRows;
    const int traceH = W / 6, strip = 76;
    const int wfH = have ? std::min(std::max(nRows, 60), W / 2) : 60;
    Image im;
    im.w = W; im.h = traceH + wfH + strip;
    im.rgba.assign((size_t)im.w * im.h * 4, 255);
    for (int y = 0; y < im.h; y++) for (int x = 0; x < W; x++) put(im, x, y, 0x1C1812);   // the dark background (RGB 0xRRGGBB read as BGR below)
    // trace panel: level of the newest row per output column (the highest level of the columns it covers)
    if (have) {
        int prev = traceH - 1;
        for (int x = 0; x < W; x++) {
            const int a = (int)((long long)x * w / W), b = std::max(a + 1, (int)((long long)(x + 1) * w / W));
            int m = 0;
            for (int k = a; k < b && k < w; k++) m = std::max<int>(m, lv[(size_t)k]);
            const int y = traceH - 1 - m * (traceH - 2) / 255;
            for (int yy = std::min(y, prev); yy <= std::max(y, prev); yy++) put(im, x, yy, 0x50D0FF);
            for (int yy = y; yy < traceH; yy += 2) put(im, x, yy, 0x3A2A14);   // faint fill under the line
            prev = y;
        }
        // waterfall: newest row at the top, each output pixel the highest level it covers
        for (int y = 0; y < wfH; y++) {
            const int r0 = (int)((long long)y * nRows / wfH), r1 = std::max(r0 + 1, (int)((long long)(y + 1) * nRows / wfH));
            for (int x = 0; x < W; x++) {
                const int a = (int)((long long)x * w / W), b = std::max(a + 1, (int)((long long)(x + 1) * w / W));
                int m = 0;
                for (int r = r0; r < r1 && r < nRows; r++) for (int k = a; k < b && k < w; k++) m = std::max<int>(m, lv[(size_t)r * w + k]);
                const uint32_t c = lut[m];   // 0xAABBGGRR
                uint8_t* p = &im.rgba[((size_t)(traceH + y) * W + x) * 4];
                p[0] = c & 255; p[1] = (c >> 8) & 255; p[2] = (c >> 16) & 255; p[3] = 255;
            }
        }
    }
    // caption strip
    const int y0 = traceH + wfH;
    for (int y = y0; y < im.h; y++) for (int x = 0; x < W; x++) { uint8_t* p = &im.rgba[((size_t)y * W + x) * 4]; p[0] = 18; p[1] = 24; p[2] = 32; p[3] = 255; }
    std::string head = cap.mode.empty() ? "Radio" : cap.mode;
    if (cap.freqHz > 0) head += " @ " + freqText(cap.freqHz);
    std::string detail = utcText(cap.epoch);
    if (!cap.place.empty()) detail += " | " + cap.place;
    drawText(im, 12, y0 + 8, asciiOnly(head), 3, 0xFFFFFF);
    drawText(im, 12, y0 + 40, asciiOnly(detail), 2, 0xD8C8A8);
    const std::string wm = "OnAir" + (cap.version.empty() ? std::string() : " " + cap.version);
    const int ww = drawText(im, 0, 0, wm, 2, 0, true);
    drawText(im, W - ww - 12, y0 + 52, wm, 2, 0xE0B050);
    return im;
}

}  // namespace clips
