// CP16: fonts, the text cells, the transmitter's carriers and the character recognition (cp16_proto.h).
#include "dect2/cp16/cp16_proto.h"
#include "../hfdig/hfdig_kbd_tables.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <mutex>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../../third_party/imgui/imstb_truetype.h"

namespace dect2 {
namespace cp16 {

namespace {
const uint16_t kGbFlat[87 * 94] = {   // lead 0xA1 + row, trail 0xA1 + column
#include "../fm/fm_gb2312.inc"
};

constexpr double kTwoPi = 6.283185307179586;

struct TtFont {
    std::vector<uint8_t> data;
    stbtt_fontinfo info{};
    bool ok = false;
    bool load(std::vector<uint8_t> d, std::string& err) {
        ok = false;
        data = std::move(d);
        const int off = data.empty() ? -1 : stbtt_GetFontOffsetForIndex(data.data(), 0);
        if (off < 0 || !stbtt_InitFont(&info, data.data(), off)) { err = "Not a TrueType or OpenType font."; data.clear(); return false; }
        ok = true;
        return true;
    }
    // the character at 16 pixels to the em, the em box on the 16 rows (baseline at the typographic ascender)
    bool draw(uint32_t cp, Glyph& g) const {
        if (!ok || stbtt_FindGlyphIndex(&info, (int)cp) == 0) return false;
        const float sc = stbtt_ScaleForMappingEmToPixels(&info, 16.f);
        int asc = 0, desc = 0, gap = 0;
        if (!stbtt_GetFontVMetricsOS2(&info, &asc, &desc, &gap)) stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
        const float em = 16.f / sc;   // font units of the em
        float base = (float)asc * sc;
        if (asc - desc > 0 && (float)(asc - desc) > em * 1.05f) base = 16.f * (float)asc / (float)(asc - desc);   // a tall line box: fit it
        const int baseRow = (int)std::lround(base);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&info, (int)cp, sc, sc, &x0, &y0, &x1, &y1);
        const int w = x1 - x0, h = y1 - y0;
        g = Glyph();
        if (w <= 0 || h <= 0) return true;   // a blank character (the ideographic space)
        if (w > 64 || h > 64) return false;
        std::vector<unsigned char> bm((size_t)w * h);
        stbtt_MakeCodepointBitmap(&info, bm.data(), w, h, w, sc, sc, (int)cp);
        int lsb = 0, adv = 0;
        stbtt_GetCodepointHMetrics(&info, (int)cp, &adv, &lsb);
        // the advance is the cell: centre a narrower one (a proportional Latin letter) in the 16 columns
        const int advPx = (int)std::lround((float)adv * sc);
        const int shift = advPx > 0 && advPx < 16 ? (16 - advPx) / 2 : 0;
        for (int r = 0; r < h; r++) {
            const int row = baseRow + y0 + r;
            if (row < 0 || row >= kRows) continue;
            for (int c = 0; c < w; c++) {
                const int col = x0 + c + shift;
                if (col < 0 || col >= kCarriers) continue;
                if (bm[(size_t)r * w + c] >= 100) g.row[row] |= (uint16_t)(0x8000u >> col);
            }
        }
        return true;
    }
};

struct FontState {
    std::mutex mu;
    std::string userPath;
    std::vector<uint8_t> hzk;            // a raw HZK16 file (GB2312 order, 32 bytes a character)
    TtFont userTt, defTt;
    std::string defName;
    uint64_t gen = 1;                    // changes with every font change (the recognition cache)
};
FontState& fs() { static FontState s; return s; }

std::vector<uint8_t> readFile(const std::string& path, std::string& err) {
    std::vector<uint8_t> d;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "Cannot open the font file."; return d; }
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        d.insert(d.end(), buf, buf + n);
        if (d.size() > ((size_t)64 << 20)) { fclose(f); d.clear(); err = "The font file is larger than 64 MB."; return d; }
    }
    fclose(f);
    if (d.empty()) err = "The font file is empty.";
    return d;
}

bool looksTrueType(const std::vector<uint8_t>& d) {
    if (d.size() < 12) return false;
    const uint32_t m = (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
    return m == 0x00010000u || m == 0x4F54544Fu /* OTTO */ || m == 0x74727565u /* true */ || m == 0x74746366u /* ttcf */;
}

bool hzkGlyph(const std::vector<uint8_t>& hzk, uint32_t cp, Glyph& g) {
    const uint32_t code = gb2312Code(cp);
    if (!code) return false;
    const size_t at = ((size_t)((code >> 8) - 0xA1) * 94 + ((code & 0xFF) - 0xA1)) * 32;
    if (at + 32 > hzk.size()) return false;
    for (int r = 0; r < kRows; r++) g.row[r] = (uint16_t)(hzk[at + 2 * r] << 8 | hzk[at + 2 * r + 1]);
    return true;
}

// the Hellschreiber mode's Latin glyphs (7 x 14) in the middle of the cell
bool builtinGlyph(uint32_t cp, Glyph& g) {
    uint32_t c = cp;
    if (c >= 0xFF01 && c <= 0xFF5E) c = c - 0xFF01 + 0x21;   // full-width forms: the same letters
    if (c == 0x3000) c = ' ';
    if (c > 0x7FF) return false;
    unsigned short out[14] = {};
    if (!kbd::hellGlyph((int)c, out)) return false;
    g = Glyph();
    for (int i = 0; i < 14; i++) g.row[i + 1] = (uint16_t)(out[i] >> 4);
    return true;
}
} // namespace

int Glyph::ink() const { int n = 0; for (uint16_t r : row) { uint16_t v = r; while (v) { n += v & 1; v >>= 1; } } return n; }

bool validParams(const Params& p, std::string* why) {
    auto no = [&](const char* s) { if (why) *why = s; return false; };
    if (!(p.spacingHz >= kMinSpacingHz && p.spacingHz <= kMaxSpacingHz)) return no("carrier spacing out of range");
    if (!(p.rowSec() >= kMinRowSec && p.rowSec() <= kMaxRowSec)) return no("character time out of range");
    if (!(p.baseHz >= 100 && p.baseHz + 15 * p.spacingHz <= 3900)) return no("the carriers must sit between 100 and 3900 Hz");
    return true;
}

uint32_t gb2312Code(uint32_t cp) {
    static const std::vector<std::pair<uint32_t, uint16_t>> idx = [] {
        std::vector<std::pair<uint32_t, uint16_t>> v;
        for (int a = 0; a < 87; a++)
            for (int b = 0; b < 94; b++)
                if (kGbFlat[a * 94 + b]) v.push_back({kGbFlat[a * 94 + b], (uint16_t)((0xA1 + a) << 8 | (0xA1 + b))});
        std::sort(v.begin(), v.end());
        return v;
    }();
    auto it = std::lower_bound(idx.begin(), idx.end(), std::make_pair(cp, (uint16_t)0));
    return it != idx.end() && it->first == cp ? it->second : 0;
}

const std::vector<uint32_t>& gb2312Repertoire() {
    static const std::vector<uint32_t> v = [] {
        std::vector<uint32_t> r;
        for (int a = 0; a < 87; a++)
            for (int b = 0; b < 94; b++)
                if (kGbFlat[a * 94 + b]) r.push_back(kGbFlat[a * 94 + b]);
        return r;
    }();
    return v;
}

bool loadFontFile(const std::string& path, std::string& err) {
    FontState& s = fs();
    if (path.empty()) {
        std::lock_guard<std::mutex> lk(s.mu);
        s.userPath.clear(); s.hzk.clear(); s.userTt = TtFont(); s.gen++;
        return true;
    }
    std::vector<uint8_t> d = readFile(path, err);
    if (d.empty()) return false;
    std::lock_guard<std::mutex> lk(s.mu);
    if (looksTrueType(d)) {
        TtFont t;
        if (!t.load(std::move(d), err)) return false;
        s.hzk.clear();
        s.userTt = std::move(t);
        s.userTt.info.data = s.userTt.data.data();
    } else if (d.size() % 32 == 0 && d.size() >= (size_t)32 * 94 * 9) {
        s.userTt = TtFont();
        s.hzk = std::move(d);
    } else {
        err = "Not a font OnAir can read: use an HZK16 dot font (GB2312, 32 bytes a character) or a .ttf / .otf file.";
        return false;
    }
    s.userPath = path;
    s.gen++;
    return true;
}

std::string fontFilePath() { std::lock_guard<std::mutex> lk(fs().mu); return fs().userPath; }

bool setDefaultFontData(std::vector<uint8_t> ttf, const std::string& name, std::string& err) {
    TtFont t;
    if (!t.load(std::move(ttf), err)) return false;
    FontState& s = fs();
    std::lock_guard<std::mutex> lk(s.mu);
    s.defTt = std::move(t);
    s.defTt.info.data = s.defTt.data.data();
    s.defName = name;
    s.gen++;
    return true;
}

std::string fontName() {
    FontState& s = fs();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.userPath.empty()) {
        const size_t p = s.userPath.find_last_of("/\\");
        return p == std::string::npos ? s.userPath : s.userPath.substr(p + 1);
    }
    return s.defTt.ok ? s.defName : std::string();
}

bool glyphFor(uint32_t cp, Glyph& g) {
    g = Glyph();
    if (cp == 0x3000 || cp == ' ') return true;
    FontState& s = fs();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.hzk.empty() && hzkGlyph(s.hzk, cp, g)) return true;
        if (s.userTt.ok && s.userTt.draw(cp, g)) return true;
        if (s.defTt.ok && s.defTt.draw(cp, g)) return true;
    }
    if (builtinGlyph(cp, g)) return true;
    g = Glyph();   // a box: the reader sees that a character was sent
    for (int r = 2; r <= 13; r++) g.row[r] = 0x2004;
    g.row[2] = g.row[13] = 0x3FFC;
    return false;
}

uint32_t fullWidth(uint32_t cp) {
    if (cp >= 0x21 && cp <= 0x7E) return cp - 0x21 + 0xFF01;
    if (cp == ' ') return 0x3000;
    return cp;
}

std::vector<uint32_t> utf8ToCodepoints(const std::string& s) {
    std::vector<uint32_t> v;
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = (uint8_t)s[i];
        uint32_t cp = c;
        int n = 0;
        if (c >= 0xF0) { cp = c & 7; n = 3; } else if (c >= 0xE0) { cp = c & 15; n = 2; } else if (c >= 0xC0) { cp = c & 31; n = 1; } else if (c >= 0x80) { i++; continue; }
        bool bad = false;
        for (int k = 1; k <= n; k++) {
            if (i + (size_t)k >= s.size() || ((uint8_t)s[i + k] & 0xC0) != 0x80) { bad = true; break; }
            cp = cp << 6 | ((uint8_t)s[i + k] & 0x3F);
        }
        if (bad) { i++; continue; }
        v.push_back(cp);
        i += (size_t)n + 1;
    }
    return v;
}

std::string codepointToUtf8(uint32_t cp) {
    std::string o;
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
    else { o += (char)(0xF0 | cp >> 18); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
    return o;
}

std::vector<uint32_t> textCells(const std::string& utf8) {
    std::vector<uint32_t> out;
    for (uint32_t cp : utf8ToCodepoints(utf8)) {
        if (cp == '\t' || cp == '\n' || cp == '\r') cp = ' ';
        if (cp < 0x20 || cp == 0x7F) continue;
        out.push_back(fullWidth(cp));
    }
    return out;
}

std::vector<uint16_t> messageRows(const std::vector<uint32_t>& cells) {
    std::vector<uint16_t> rows;
    rows.reserve(cells.size() * kCellRows);
    for (uint32_t cp : cells) {
        Glyph g;
        glyphFor(cp, g);
        for (int r = 0; r < kRows; r++) rows.push_back(g.row[r]);
        rows.push_back(0);
    }
    return rows;
}

// ---------------------------------------------------------------- modulator

Modulator::Modulator(const Params& p, double rate, double amp, uint32_t seed) : p_(p), rate_(rate), amp_(amp) {
    spr_ = p_.rowSec() * rate_;
    uint32_t x = seed ? seed : 1;
    for (int c = 0; c < kCarriers; c++) {
        x = x * 1664525u + 1013904223u;
        ph_[c] = kTwoPi * (double)(x >> 8) / 16777216.0;
        dph_[c] = kTwoPi * p_.carrierHz(c) / rate_;
    }
}

void Modulator::push(uint16_t row) {
    if (next_ > 4096) { rows_.erase(rows_.begin(), rows_.begin() + (long)next_); next_ = 0; }
    rows_.push_back(row);
}

size_t Modulator::generate(std::complex<float>* out, size_t n) {
    const double edge = std::max(1.0, std::min(0.06 * spr_, 0.008 * rate_));
    size_t k = 0;
    while (k < n && next_ < rows_.size()) {
        const uint16_t cur = rows_[next_];
        const uint16_t chg = (uint16_t)(cur ^ prev_);
        // the run of samples left in this row
        const double left = spr_ - pos_;
        size_t run = (size_t)std::max(1.0, std::ceil(left - 1e-9));
        run = std::min(run, n - k);
        for (size_t i = 0; i < run; i++) {
            const double t = pos_ + (double)i;
            float re = 0, im = 0;
            const double ramp = t < edge ? 0.5 - 0.5 * std::cos(3.141592653589793 * (t + 0.5) / edge) : 1.0;
            for (int c = 0; c < kCarriers; c++) {
                const uint16_t m = (uint16_t)(0x8000u >> c);
                double a;
                if (chg & m) a = (cur & m) ? ramp : 1.0 - ramp;
                else a = (cur & m) ? 1.0 : 0.0;
                if (a > 0) {
                    const double ph = ph_[c] + dph_[c] * (double)i;
                    re += (float)(a * std::cos(ph));
                    im += (float)(a * std::sin(ph));
                }
            }
            out[k + i] = std::complex<float>(re * (float)amp_, im * (float)amp_);
        }
        for (int c = 0; c < kCarriers; c++) ph_[c] = std::fmod(ph_[c] + dph_[c] * (double)run, kTwoPi);
        pos_ += (double)run;
        k += run;
        if (pos_ >= spr_ - 1e-9) { pos_ -= spr_; prev_ = cur; next_++; }
    }
    return k;
}

// ---------------------------------------------------------------- recognition

namespace {
struct Cand { uint32_t cp; uint64_t w[4]; float mean, norm; };
struct Lib { uint64_t gen = 0; std::vector<Cand> all; };
std::mutex gLibMu;
Lib gLib;

void bitsOf(const Glyph& g, uint64_t w[4]) {
    for (int i = 0; i < 4; i++) w[i] = (uint64_t)g.row[4 * i] << 48 | (uint64_t)g.row[4 * i + 1] << 32 | (uint64_t)g.row[4 * i + 2] << 16 | g.row[4 * i + 3];
}
Cand cand(uint32_t cp, const Glyph& g) {
    Cand c{};
    c.cp = cp;
    bitsOf(g, c.w);
    const int n = g.ink();
    c.mean = (float)n / 256.f;
    c.norm = std::sqrt((float)n * (1 - c.mean) * (1 - c.mean) + (float)(256 - n) * c.mean * c.mean);
    return c;
}
const std::vector<Cand>& library() {
    uint64_t gen;
    { std::lock_guard<std::mutex> lk(fs().mu); gen = fs().gen; }
    std::lock_guard<std::mutex> lk(gLibMu);
    if (gLib.gen == gen) return gLib.all;
    gLib.all.clear();
    for (uint32_t cp : gb2312Repertoire()) {
        if (cp == 0x3000) continue;
        Glyph g;
        if (!glyphFor(cp, g) || g.ink() < 3) continue;
        gLib.all.push_back(cand(cp, g));
    }
    gLib.gen = gen;
    return gLib.all;
}
inline int popc(uint64_t x) { return __builtin_popcountll(x); }
} // namespace

void clearGlyphCache() { std::lock_guard<std::mutex> lk(gLibMu); gLib.gen = 0; }

Match recognise(const float dots[kRows][kCarriers], const std::vector<uint32_t>* only) {
    Match best;
    float mean = 0;
    for (int r = 0; r < kRows; r++) for (int c = 0; c < kCarriers; c++) mean += dots[r][c];
    mean /= 256.f;
    float var = 0;
    uint64_t w[4] = {};
    for (int r = 0; r < kRows; r++)
        for (int c = 0; c < kCarriers; c++) {
            const float d = dots[r][c] - mean;
            var += d * d;
            if (dots[r][c] >= 0.5f) w[r / 4] |= 1ull << (63 - ((r % 4) * 16 + c));
        }
    if (var < 1e-6f) return best;
    const float vn = std::sqrt(var);
    std::vector<Cand> own;
    if (only) for (uint32_t cp : *only) { Glyph g; if (glyphFor(cp, g) && g.ink() >= 3) own.push_back(cand(cp, g)); }
    const std::vector<Cand>& lib = only ? own : library();
    // the 48 nearest by Hamming distance of the thresholded dots, then the soft correlation
    constexpr size_t kKeep = 48;
    std::vector<std::pair<int, size_t>> closest;
    closest.reserve(lib.size());
    for (size_t i = 0; i < lib.size(); i++) {
        const Cand& c = lib[i];
        closest.push_back({popc(c.w[0] ^ w[0]) + popc(c.w[1] ^ w[1]) + popc(c.w[2] ^ w[2]) + popc(c.w[3] ^ w[3]), i});
    }
    const size_t keep = std::min(kKeep, closest.size());
    std::partial_sort(closest.begin(), closest.begin() + (long)keep, closest.end());
    for (size_t k = 0; k < keep; k++) {
        const Cand& c = lib[closest[k].second];
        if (c.norm <= 0) continue;
        float s = 0;
        for (int r = 0; r < kRows; r++) {
            const uint64_t rowBits = c.w[r / 4] >> (48 - (r % 4) * 16);
            for (int col = 0; col < kCarriers; col++) {
                const float b = ((rowBits >> (15 - col)) & 1) ? 1.f - c.mean : -c.mean;
                s += b * (dots[r][col] - mean);
            }
        }
        const float ncc = s / (c.norm * vn);
        if (ncc > best.score) { best.second = best.score; best.score = ncc; best.cp = c.cp; }
        else if (ncc > best.second) best.second = ncc;
    }
    return best;
}

} // namespace cp16
} // namespace dect2
