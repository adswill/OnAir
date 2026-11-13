// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radio clips, generators: PNG (deflate), WAV, STORE zip, base64, HTML, Discord text and picture, and writing the folder.
#include "clips.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace clips {

// ================================================================ CRC32 / Adler / base64
uint32_t crc32(const uint8_t* p, size_t n, uint32_t c) {
    static uint32_t tab[256];
    static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t x = i; for (int k = 0; k < 8; k++) x = (x & 1) ? 0xEDB88320u ^ (x >> 1) : x >> 1; tab[i] = x; } init = true; }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}
static uint32_t adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) { const size_t m = std::min<size_t>(n, 5552); for (size_t i = 0; i < m; i++) { a += p[i]; b += a; } a %= 65521; b %= 65521; p += m; n -= m; }
    return (b << 16) | a;
}
std::string base64(const uint8_t* p, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o += t[v >> 18]; o += t[(v >> 12) & 63];
        o += i + 1 < n ? t[(v >> 6) & 63] : '=';
        o += i + 2 < n ? t[v & 63] : '=';
    }
    return o;
}

// ================================================================ deflate (fixed Huffman, LZ77 with short hash chains) and PNG
namespace {
struct BitW {
    std::vector<uint8_t>& o; uint32_t acc = 0; int nb = 0;
    explicit BitW(std::vector<uint8_t>& v) : o(v) {}
    void bits(uint32_t v, int n) { acc |= v << nb; nb += n; while (nb >= 8) { o.push_back((uint8_t)acc); acc >>= 8; nb -= 8; } }
    void rev(uint32_t code, int n) { uint32_t r = 0; for (int i = 0; i < n; i++) r |= ((code >> i) & 1) << (n - 1 - i); bits(r, n); }   // Huffman codes go MSB first
    void flush() { if (nb > 0) { o.push_back((uint8_t)acc); acc = 0; nb = 0; } }
};
void lit(BitW& w, int v) {
    if (v < 144) w.rev(0x30 + v, 8); else if (v < 256) w.rev(0x190 + (v - 144), 9); else if (v < 280) w.rev(v - 256, 7); else w.rev(0xC0 + (v - 280), 8);
}
const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void deflateFixed(const uint8_t* d, size_t n, std::vector<uint8_t>& out) {
    BitW w(out);
    w.bits(1, 1); w.bits(1, 2);   // final block, fixed Huffman
    const int HB = 15;
    std::vector<int32_t> head((size_t)1 << HB, -1), prev(32768, -1);
    auto hash = [&](size_t i) { return (uint32_t)((d[i] * 506832829u ^ d[i + 1] * 2654435761u ^ d[i + 2] * 40503u) >> (32 - HB)) & ((1u << HB) - 1); };
    size_t i = 0;
    while (i < n) {
        int bestLen = 0; size_t bestDist = 0;
        if (i + 3 <= n) {
            const uint32_t h = hash(i);
            int32_t c = head[h];
            for (int depth = 0; depth < 24 && c >= 0 && i - (size_t)c <= 32768; depth++) {
                const size_t maxl = std::min<size_t>(258, n - i);
                size_t l = 0;
                while (l < maxl && d[(size_t)c + l] == d[i + l]) l++;
                if ((int)l > bestLen) { bestLen = (int)l; bestDist = i - (size_t)c; if (l == maxl) break; }
                const int32_t nx = prev[(size_t)c & 32767];
                if (nx >= c) break;
                c = nx;
            }
        }
        auto insert = [&](size_t p) { if (p + 3 <= n) { const uint32_t h = hash(p); prev[p & 32767] = head[h]; head[h] = (int32_t)p; } };
        if (bestLen >= 3) {
            int li = 28; while (kLenBase[li] > bestLen) li--;
            lit(w, 257 + li); if (kLenExtra[li]) w.bits((uint32_t)(bestLen - kLenBase[li]), kLenExtra[li]);
            int di = 29; while (kDistBase[di] > bestDist) di--;
            w.rev((uint32_t)di, 5); if (kDistExtra[di]) w.bits((uint32_t)(bestDist - kDistBase[di]), kDistExtra[di]);
            for (int k = 0; k < bestLen; k++) insert(i + (size_t)k);
            i += (size_t)bestLen;
        } else { lit(w, d[i]); insert(i); i++; }
    }
    lit(w, 256);
    w.flush();
}
void be32(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
void chunk(std::vector<uint8_t>& o, const char* type, const std::vector<uint8_t>& data) {
    be32(o, (uint32_t)data.size());
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    o.insert(o.end(), td.begin(), td.end());
    be32(o, crc32(td.data(), td.size()));
}
}  // namespace

std::vector<uint8_t> encodePng(const uint8_t* rgba, int w, int h) {
    std::vector<uint8_t> o = {137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<uint8_t> ihdr; be32(ihdr, (uint32_t)w); be32(ihdr, (uint32_t)h);
    ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(o, "IHDR", ihdr);
    const size_t stride = (size_t)w * 4;
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * (size_t)h);
    std::vector<uint8_t> f1(stride), f2(stride);
    for (int y = 0; y < h; y++) {
        const uint8_t* cur = rgba + (size_t)y * stride;
        const uint8_t* up = y ? cur - stride : nullptr;
        long s0 = 0, s1 = 0, s2 = 0;
        for (size_t x = 0; x < stride; x++) {
            s0 += std::abs((int8_t)cur[x]);
            f1[x] = (uint8_t)(cur[x] - (x >= 4 ? cur[x - 4] : 0)); s1 += std::abs((int8_t)f1[x]);
            f2[x] = (uint8_t)(cur[x] - (up ? up[x] : 0)); s2 += std::abs((int8_t)f2[x]);
        }
        if (s0 <= s1 && s0 <= s2) { raw.push_back(0); raw.insert(raw.end(), cur, cur + stride); }
        else if (s1 <= s2) { raw.push_back(1); raw.insert(raw.end(), f1.begin(), f1.end()); }
        else { raw.push_back(2); raw.insert(raw.end(), f2.begin(), f2.end()); }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    deflateFixed(raw.data(), raw.size(), z);
    be32(z, adler32(raw.data(), raw.size()));
    chunk(o, "IDAT", z);
    chunk(o, "IEND", {});
    return o;
}

// ================================================================ WAV, audio fitting, ZIP
static void le32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }
static void le16(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
std::vector<uint8_t> encodeWav(const int16_t* mono, size_t n, int rate) {
    std::vector<uint8_t> o;
    o.reserve(44 + n * 2);
    const uint32_t data = (uint32_t)(n * 2);
    o.insert(o.end(), {'R', 'I', 'F', 'F'}); le32(o, 36 + data); o.insert(o.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    le32(o, 16); le16(o, 1); le16(o, 1); le32(o, (uint32_t)rate); le32(o, (uint32_t)rate * 2); le16(o, 2); le16(o, 16);
    o.insert(o.end(), {'d', 'a', 't', 'a'}); le32(o, data);
    for (size_t i = 0; i < n; i++) le16(o, (uint16_t)mono[i]);
    return o;
}
std::vector<int16_t> fitAudio(const std::vector<int16_t>& in, int& rate, int maxRate, size_t maxBytes) {
    std::vector<int16_t> o;
    if (in.empty() || rate <= 0) return o;
    const int f = std::max(1, (rate + maxRate - 1) / maxRate);
    if (f == 1) o = in;
    else {
        o.resize(in.size() / (size_t)f);
        for (size_t i = 0; i < o.size(); i++) { int s = 0; for (int k = 0; k < f; k++) s += in[i * (size_t)f + (size_t)k]; o[i] = (int16_t)(s / f); }
        rate /= f;
    }
    const size_t keep = std::max<size_t>(1, maxBytes / 2);
    if (o.size() > keep) o.erase(o.begin(), o.end() - (long)keep);
    return o;
}
std::vector<uint8_t> zipStore(const std::vector<ZipEntry>& files) {
    std::vector<uint8_t> o, cd;
    for (const auto& f : files) {
        const uint32_t crc = crc32(f.data.data(), f.data.size()), off = (uint32_t)o.size();
        le32(o, 0x04034b50); le16(o, 20); le16(o, 0x0800); le16(o, 0); le16(o, 0); le16(o, 0x21); le32(o, crc);   // flags: UTF-8 names; time 0, date 1980-01-01
        le32(o, (uint32_t)f.data.size()); le32(o, (uint32_t)f.data.size()); le16(o, (uint32_t)f.name.size()); le16(o, 0);
        o.insert(o.end(), f.name.begin(), f.name.end());
        o.insert(o.end(), f.data.begin(), f.data.end());
        le32(cd, 0x02014b50); le16(cd, 20); le16(cd, 20); le16(cd, 0x0800); le16(cd, 0); le16(cd, 0); le16(cd, 0x21); le32(cd, crc);
        le32(cd, (uint32_t)f.data.size()); le32(cd, (uint32_t)f.data.size()); le16(cd, (uint32_t)f.name.size()); le16(cd, 0); le16(cd, 0); le16(cd, 0); le16(cd, 0); le32(cd, 0); le32(cd, off);
        cd.insert(cd.end(), f.name.begin(), f.name.end());
    }
    const uint32_t cdOff = (uint32_t)o.size();
    o.insert(o.end(), cd.begin(), cd.end());
    le32(o, 0x06054b50); le16(o, 0); le16(o, 0); le16(o, (uint32_t)files.size()); le16(o, (uint32_t)files.size()); le32(o, (uint32_t)cd.size()); le32(o, cdOff); le16(o, 0);
    return o;
}

// ================================================================ text helpers
std::string htmlEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        case '\'': o += "&#39;"; break;
        case '\n': case '\t': o += ' '; break;
        default: if (c < 32 || c == 127) o += ' '; else o += (char)c;
        }
    }
    return o;
}
std::string freqText(double hz) {
    char b[48];
    if (hz <= 0) return "";
    if (hz >= 3e9) snprintf(b, sizeof b, "%s GHz", resexp::num(hz / 1e9, 4).c_str());
    else if (hz >= 1e6) snprintf(b, sizeof b, "%s MHz", resexp::num(hz / 1e6, 4).c_str());
    else snprintf(b, sizeof b, "%s kHz", resexp::num(hz / 1e3, 3).c_str());
    return b;
}
std::string utcText(double t, bool seconds) {
    const std::time_t tt = (std::time_t)t;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char b[40];
    strftime(b, sizeof b, seconds ? "%Y-%m-%d %H:%M:%S UTC" : "%Y-%m-%d %H:%M UTC", &tm);
    return b;
}
static std::string stampOf(double t) {
    const std::time_t tt = (std::time_t)t;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
    return b;
}
static std::string slug(const std::string& s) {
    std::string o;
    for (unsigned char c : s) o += std::isalnum(c) ? (char)std::tolower(c) : '-';
    while (o.find("--") != std::string::npos) o.erase(o.find("--"), 1);
    while (!o.empty() && o.back() == '-') o.pop_back();
    while (!o.empty() && o.front() == '-') o.erase(o.begin());
    return o.empty() ? "radio" : o.substr(0, 24);
}
std::string fileStem(const Meta& m) {
    std::string f = freqText(m.freqHz);
    f.erase(std::remove(f.begin(), f.end(), ' '), f.end());
    return "onair_" + slug(m.mode) + (f.empty() ? "" : "_" + f) + "_" + stampOf(m.t0);
}
// radio text is untrusted: one short plain line, nothing Discord would turn into a ping, link, mention or format
static std::string plainLine(const std::string& s, size_t maxLen) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 32 || c == 127) c = ' ';
        if (c == '`' || c == '*' || c == '_' || c == '~' || c == '|' || c == '>' || c == '<' || c == '\\') c = '\'';
        if (c == '@') { o += "@\xE2\x80\x8B"; continue; }   // zero width space: no @everyone
        if (c == ' ' && !o.empty() && o.back() == ' ') continue;
        o += (char)c;
    }
    if (o.size() > maxLen) { size_t k = maxLen; while (k > 0 && ((unsigned char)o[k] & 0xC0) == 0x80) k--; o = o.substr(0, k) + "..."; }
    return o;
}
static std::string rowText(const resexp::Table& t, size_t i) {
    std::string s;
    for (size_t c = 2; c < t.cols.size() && c < t.rows[i].size(); c++) if (!t.rows[i][c].empty()) { if (!s.empty()) s += " "; s += t.rows[i][c]; }
    return s;
}
static std::string headline(const Meta& m) {
    std::string h = m.mode.empty() ? "Radio" : m.mode;
    if (m.freqHz > 0) h += " @ " + freqText(m.freqHz);
    return h;
}
static std::string detailLine(const Meta& m, bool markdownDot) {
    std::string s = utcText(m.t0);
    (void)markdownDot;
    if (!m.summary.empty()) s += " \xC2\xB7 " + m.summary;
    if (m.snrDb > -999) s += " \xC2\xB7 SNR " + resexp::num(m.snrDb, 0) + " dB";
    return s;
}

std::string discordText(const Content& c) {
    const Meta& m = c.meta;
    std::string hl = plainLine(m.mode.empty() ? "Radio" : m.mode, 40);
    if (m.freqHz > 0) hl += " @ " + freqText(m.freqHz);
    std::string s = "**" + hl + "** \xC2\xB7 " + plainLine(detailLine(m, true), 140) + "\n";
    if (!m.title.empty()) s += "> " + plainLine(m.title, 120) + "\n";
    if (!m.notes.empty()) s += "> " + plainLine(m.notes, 300) + "\n";
    const resexp::Table& t = c.table;
    if (c.wantMessages && !t.rows.empty()) {
        std::vector<size_t> idx;
        for (size_t i = 0; i < t.rows.size(); i++) if (!rowText(t, i).empty()) idx.push_back(i);
        std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return rowText(t, a).size() > rowText(t, b).size(); });
        if (idx.size() > 5) idx.resize(5);
        std::sort(idx.begin(), idx.end());
        if (!idx.empty()) {
            s += "Notable:\n";
            for (size_t i : idx) s += "`" + plainLine(rowText(t, i), 90) + "`\n";
        }
    }
    s += "Attach: ";
    std::string a;
    if (c.wantImage && c.spectro.w > 0) a += "discord.png";
    if (c.wantAudio && !c.audio.empty()) a += std::string(a.empty() ? "" : ", ") + "audio.wav";
    a += std::string(a.empty() ? "" : ", ") + "clip.html";
    s += a + "\n*Made with OnAir* <" + kRepoUrl + ">\n";
    return s;
}

// ================================================================ composite picture (spectrogram + caption strip)
namespace {
const uint8_t kFont[95][5] = {
{0,0,0,0,0},{0,0,0x5F,0,0},{0,7,0,7,0},{0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,8,0x64,0x62},{0x36,0x49,0x56,0x20,0x50},{0,5,3,0,0},
{0,0x1C,0x22,0x41,0},{0,0x41,0x22,0x1C,0},{0x14,8,0x3E,8,0x14},{8,8,0x3E,8,8},{0,0x50,0x30,0,0},{8,8,8,8,8},{0,0x60,0x60,0,0},{0x20,0x10,8,4,2},
{0x3E,0x51,0x49,0x45,0x3E},{0,0x42,0x7F,0x40,0},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{1,0x71,9,5,3},
{0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1E},{0,0x36,0x36,0,0},{0,0x56,0x36,0,0},{8,0x14,0x22,0x41,0},{0x14,0x14,0x14,0x14,0x14},{0,0x41,0x22,0x14,8},{2,1,0x51,9,6},
{0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,9,9,9,1},{0x3E,0x41,0x49,0x49,0x7A},
{0x7F,8,8,8,0x7F},{0,0x41,0x7F,0x41,0},{0x20,0x40,0x41,0x3F,1},{0x7F,8,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},{0x7F,2,0x0C,2,0x7F},{0x7F,4,8,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
{0x7F,9,9,9,6},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,9,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{1,1,0x7F,1,1},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
{0x63,0x14,8,0x14,0x63},{7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43},{0,0x7F,0x41,0x41,0},{2,4,8,0x10,0x20},{0,0x41,0x41,0x7F,0},{4,2,1,2,4},{0x40,0x40,0x40,0x40,0x40},
{0,1,2,4,0},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{8,0x7E,9,1,2},{0x0C,0x52,0x52,0x52,0x3E},
{0x7F,8,4,4,0x78},{0,0x44,0x7D,0x40,0},{0x20,0x40,0x44,0x3D,0},{0x7F,0x10,0x28,0x44,0},{0,0x41,0x7F,0x40,0},{0x7C,4,0x18,4,0x78},{0x7C,8,4,4,0x78},{0x38,0x44,0x44,0x44,0x38},
{0x7C,0x14,0x14,0x14,8},{8,0x14,0x14,0x18,0x7C},{0x7C,8,4,4,8},{0x48,0x54,0x54,0x54,0x20},{4,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0,8,0x36,0x41,0},{0,0,0x7F,0,0},{0,0x41,0x36,8,0},{0x10,8,8,0x10,8}};

// draws ASCII text (other bytes as '?') at scale s; returns the width in pixels
int drawText(Image& im, int x, int y, const std::string& t, int s, uint32_t rgb, bool measureOnly = false) {
    int cx = x;
    for (unsigned char ch : t) {
        const int g = ch >= 32 && ch < 127 ? ch - 32 : '?' - 32;
        if (!measureOnly)
            for (int col = 0; col < 5; col++)
                for (int row = 0; row < 7; row++)
                    if (kFont[g][col] >> row & 1)
                        for (int dy = 0; dy < s; dy++)
                            for (int dx = 0; dx < s; dx++) {
                                const int px = cx + col * s + dx, py = y + row * s + dy;
                                if (px < 0 || py < 0 || px >= im.w || py >= im.h) continue;
                                uint8_t* p = &im.rgba[((size_t)py * im.w + px) * 4];
                                p[0] = rgb & 255; p[1] = (rgb >> 8) & 255; p[2] = (rgb >> 16) & 255; p[3] = 255;
                            }
        cx += 6 * s;
    }
    return cx - x;
}
std::string asciiOnly(std::string s) { for (size_t p; (p = s.find("\xC2\xB7")) != std::string::npos;) s.replace(p, 2, "|"); std::string o; for (unsigned char c : s) o += c >= 32 && c < 127 ? (char)c : (c >= 0x80 && (c & 0xC0) != 0x80 ? '?' : (c < 0x80 ? ' ' : '\0')); o.erase(std::remove(o.begin(), o.end(), '\0'), o.end()); return o; }
}  // namespace

Image composite(const Content& c) {
    const int W = 1024, strip = 64;
    Image sp = c.spectro;
    if (sp.w <= 0 || sp.h <= 0) { sp.w = W; sp.h = 160; sp.rgba.assign((size_t)W * 160 * 4, 0); for (size_t i = 3; i < sp.rgba.size(); i += 4) sp.rgba[i] = 255; }
    Image im;
    im.w = W; im.h = std::min(sp.h, 900) + strip;
    im.rgba.assign((size_t)im.w * im.h * 4, 255);
    for (int y = 0; y < im.h - strip; y++)
        for (int x = 0; x < W; x++) {
            const int sx = x * sp.w / W, sy = y * sp.h / (im.h - strip);
            memcpy(&im.rgba[((size_t)y * W + x) * 4], &sp.rgba[((size_t)sy * sp.w + sx) * 4], 4);
        }
    const int y0 = im.h - strip;
    for (int y = y0; y < im.h; y++) for (int x = 0; x < W; x++) { uint8_t* p = &im.rgba[((size_t)y * W + x) * 4]; p[0] = 18; p[1] = 24; p[2] = 32; p[3] = 255; }
    drawText(im, 12, y0 + 8, asciiOnly(headline(c.meta)), 3, 0xFFFFFF);
    drawText(im, 12, y0 + 38, asciiOnly(detailLine(c.meta, false)), 2, 0xD8C8A8);
    const std::string wm = "Made with OnAir";
    const int ww = drawText(im, 0, 0, wm, 2, 0, true);
    drawText(im, W - ww - 12, y0 + 40, wm, 2, 0xE0B050);
    return im;
}

// ================================================================ the one-file HTML clip
std::string buildHtml(const Content& c) {
    const Meta& m = c.meta;
    std::string h;
    h.reserve(64 * 1024);
    const std::string head = headline(m);
    h += "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
         "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src data:; media-src data:; style-src 'unsafe-inline'\">\n"
         "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<meta name=\"color-scheme\" content=\"light dark\">\n";
    h += "<title>" + htmlEscape("OnAir clip: " + head) + "</title>\n<style>\n"
         ":root{--bg:#fff;--fg:#1c2330;--mut:#5b6678;--card:#f2f4f8;--line:#d5dae3;--acc:#b36b00}\n"
         "@media (prefers-color-scheme:dark){:root{--bg:#0f141b;--fg:#e6e9ef;--mut:#9aa5b8;--card:#18202b;--line:#2a3442;--acc:#e0b050}}\n"
         "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}\n"
         "main{max-width:980px;margin:0 auto;padding:16px}h1{font-size:1.4rem;margin:.2em 0}.meta{color:var(--mut);margin:0 0 .8em}\n"
         ".card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px;margin:12px 0}\n"
         "img{max-width:100%;height:auto;display:block;border-radius:6px;image-rendering:auto}audio{width:100%}\n"
         ".tw{overflow-x:auto}table{border-collapse:collapse;width:100%;font:13px/1.35 ui-monospace,Menlo,Consolas,monospace}\n"
         "th,td{text-align:left;padding:4px 8px;border-bottom:1px solid var(--line);white-space:nowrap}th{color:var(--mut);position:sticky;top:0;background:var(--card)}\n"
         "footer{color:var(--mut);font-size:.85rem;margin:20px 0}a{color:var(--acc)}\n</style></head><body><main>\n";
    h += "<h1>" + htmlEscape(m.title.empty() ? head : m.title) + "</h1>\n";
    if (!m.title.empty()) h += "<p class=\"meta\"><strong>" + htmlEscape(head) + "</strong></p>\n";
    h += "<p class=\"meta\">" + htmlEscape(utcText(m.t0, true) + " to " + utcText(m.t1, true) + "  (" + resexp::num(m.t1 - m.t0, 0) + " s)");
    if (!m.summary.empty()) h += " &middot; " + htmlEscape(m.summary);
    if (m.snrDb > -999) h += " &middot; SNR " + htmlEscape(resexp::num(m.snrDb, 0)) + " dB";
    if (!m.grid.empty()) h += " &middot; " + htmlEscape(m.grid);
    h += "</p>\n";
    if (!m.notes.empty()) h += "<div class=\"card\">" + htmlEscape(m.notes) + "</div>\n";
    if (c.wantImage && c.spectro.w > 0) {
        const auto png = encodePng(c.spectro.rgba.data(), c.spectro.w, c.spectro.h);
        h += "<div class=\"card\"><img alt=\"Spectrogram (newest at the top)\" src=\"data:image/png;base64," + base64(png.data(), png.size()) + "\"></div>\n";
    }
    if (c.wantAudio && !c.audio.empty() && c.audioRate > 0) {
        int rate = c.audioRate;
        const auto a = fitAudio(c.audio, rate, 16000, 2u << 20);
        const auto wav = encodeWav(a.data(), a.size(), rate);
        h += "<div class=\"card\"><audio controls preload=\"none\" src=\"data:audio/wav;base64," + base64(wav.data(), wav.size()) + "\"></audio></div>\n";
    }
    if (c.wantMessages && !c.table.rows.empty()) {
        const size_t maxRows = 1000;
        h += "<div class=\"card tw\"><table><thead><tr>";
        for (const auto& col : c.table.cols) h += "<th>" + htmlEscape(col) + "</th>";
        h += "</tr></thead><tbody>\n";
        for (size_t i = 0; i < c.table.rows.size() && i < maxRows; i++) {
            h += "<tr>";
            for (size_t k = 0; k < c.table.cols.size(); k++) h += "<td>" + htmlEscape(k < c.table.rows[i].size() ? c.table.rows[i][k] : std::string()) + "</td>";
            h += "</tr>\n";
        }
        h += "</tbody></table>";
        if (c.table.rows.size() > maxRows) h += "<p class=\"meta\">" + std::to_string(c.table.rows.size() - maxRows) + " more rows are in messages.csv</p>";
        h += "</div>\n";
    }
    h += "<footer>Made with <a href=\"" + std::string(kRepoUrl) + "\" rel=\"noopener noreferrer\">OnAir</a>" + (m.version.empty() ? "" : " " + htmlEscape(m.version)) + "</footer>\n</main></body></html>\n";
    return h;
}

// ================================================================ writing the folder
static bool putFile(const std::string& path, const std::vector<uint8_t>& d) {
    FILE* f = dect2::u8fopen(path, "wb");
    if (!f) return false;
    const bool ok = fwrite(d.data(), 1, d.size(), f) == d.size();
    return fclose(f) == 0 && ok;
}
Written writeClip(const Content& c, const std::string& parent) {
    Written w;
    try {
        const std::string stem = fileStem(c.meta);
        const std::string dir = parent + "/" + stem;
        std::error_code ec;
        std::filesystem::create_directories(dect2::u8file(dir), ec);
        if (ec) { w.error = "cannot create " + dir; return w; }
        w.dir = dir;
        std::vector<ZipEntry> z;
        auto add = [&](const std::string& name, std::vector<uint8_t> data) {
            if (!putFile(dir + "/" + name, data)) { if (w.error.empty()) w.error = "cannot write " + name; return; }
            w.files.push_back(name);
            z.push_back({stem + "/" + name, std::move(data)});
        };
        auto str = [](const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); };
        const std::string html = buildHtml(c);
        add("clip.html", str(html)); w.htmlBytes = html.size();
        if (c.wantImage && c.spectro.w > 0) {
            add("spectrogram.png", encodePng(c.spectro.rgba.data(), c.spectro.w, c.spectro.h));
            const Image cp = composite(c);
            auto png = encodePng(cp.rgba.data(), cp.w, cp.h);
            if (png.size() < 8u * 1000 * 1000) { w.discordPngBytes = png.size(); add("discord.png", std::move(png)); }
        }
        if (c.wantAudio && !c.audio.empty() && c.audioRate > 0) {
            int rate = c.audioRate;
            const auto a = fitAudio(c.audio, rate, 24000, 4u << 20);
            add("audio.wav", encodeWav(a.data(), a.size(), rate));
        }
        if (c.wantMessages && !c.table.rows.empty()) { add("messages.csv", str(resexp::toCsv(c.table))); add("messages.json", str(resexp::toJson(c.table))); }
        add("discord.txt", str(discordText(c)));
        const auto zip = zipStore(z);
        w.zip = parent + "/" + stem + ".zip";
        if (!putFile(w.zip, zip)) { if (w.error.empty()) w.error = "cannot write the zip"; w.zip.clear(); } else w.zipBytes = zip.size();
        w.html = dir + "/clip.html";
    } catch (const std::exception& e) { w.error = e.what(); }
    return w;
}

}  // namespace clips
