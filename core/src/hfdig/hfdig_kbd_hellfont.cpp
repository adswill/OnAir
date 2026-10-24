// Hellschreiber: characters from a font file the user chose (hfdig_kbd_tables.h, hellLoadUserFont). The built-in glyphs cover ASCII, accented Latin,
// Cyrillic and Greek; every other code point (Chinese, Japanese, Korean, symbols, emoji in a monochrome font) is drawn from the font file, if
// it has the character. Nothing is bundled: the user downloads a font (e.g. GNU Unifont, Noto Sans CJK) and picks the file.
// The glyph is drawn into the rows 2 .. 11 of the 14 (the receiver finds the column boundary from the blank rows 0, 1, 12, 13) and at most
// 16 columns wide, thresholded: a monochrome picture of ten pixels height.
#include "hfdig_kbd_tables.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../../third_party/imgui/imstb_truetype.h"

namespace dect2 {
namespace kbd {
namespace {
std::mutex gMu;
std::vector<unsigned char> gData;
stbtt_fontinfo gInfo;
bool gLoaded = false;
std::string gPath;
constexpr int kInkRows = 10, kFirstRow = 2, kMaxCols = 16;
} // namespace

bool hellLoadUserFont(const std::string& path, std::string& err) {
    std::lock_guard<std::mutex> lk(gMu);
    gLoaded = false; gPath.clear(); gData.clear();
    if (path.empty()) return true;   // cleared
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "Cannot open the font file."; return false; }
    std::vector<unsigned char> d;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        d.insert(d.end(), buf, buf + n);
        if (d.size() > (size_t)64 << 20) { fclose(f); err = "The font file is larger than 64 MB."; return false; }
    }
    fclose(f);
    int off = stbtt_GetFontOffsetForIndex(d.data(), 0);
    if (off < 0 || !stbtt_InitFont(&gInfo, d.data(), off)) { err = "Not a TrueType or OpenType (glyf) font. Use a .ttf or .otf file."; return false; }
    gData = std::move(d);
    gInfo.data = gData.data();
    gLoaded = true; gPath = path;
    return true;
}

std::string hellUserFontPath() { std::lock_guard<std::mutex> lk(gMu); return gPath; }

bool hellUserGlyph(int cp, unsigned short out[14]) {
    std::lock_guard<std::mutex> lk(gMu);
    if (!gLoaded || cp < 0x80 || stbtt_FindGlyphIndex(&gInfo, cp) == 0) return false;
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&gInfo, &asc, &desc, &gap);
    if (asc - desc <= 0) return false;
    const float sc = (float)kInkRows / (float)(asc - desc);
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&gInfo, cp, sc, sc, &x0, &y0, &x1, &y1);
    const int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0 || w > 4 * kMaxCols || h > 4 * kInkRows) return false;   // nothing to draw (a space), or not sensible
    std::vector<unsigned char> bm((size_t)w * h);
    stbtt_MakeCodepointBitmap(&gInfo, bm.data(), w, h, w, sc, sc, cp);
    for (int i = 0; i < 14; i++) out[i] = 0;
    const int base = kFirstRow + (int)std::lround(asc * sc);   // the baseline row
    bool any = false;
    for (int r = 0; r < h; r++) {
        const int row = base + y0 + r;
        if (row < kFirstRow || row >= kFirstRow + kInkRows) continue;
        for (int c = 0; c < w && c < kMaxCols; c++)
            if (bm[(size_t)r * w + c] >= 64) { out[row] |= (unsigned short)(1u << (15 - c)); any = true; }
    }
    return any;
}

} // namespace kbd
} // namespace dect2
