// The web panel of every mode: the hand-written part (the common block, the size cap, the value writers, PNG and base64 for pictures).
// The per-mode exporters and schemas are generated into web_schema_gen.cpp by tools/gen_web_schema.py.
#include "dect2/results/web_schema.h"
#include "dect2/results/rx_json.h"
#include "dect2/t2/t2rx.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dect2::webschema {
namespace detail {

void vnum(std::string& s, double v) {
    if (!std::isfinite(v)) { s += "null"; return; }
    if (v == std::floor(v) && std::fabs(v) < 1e15) { char b[32]; snprintf(b, sizeof b, "%.0f", v); s += b; return; }
    char b[32];
    snprintf(b, sizeof b, "%.6g", v);
    s += b;
}
void vu64(std::string& s, unsigned long long v) { s += std::to_string(v); }
void vi64(std::string& s, long long v) { s += std::to_string(v); }
void vbool(std::string& s, bool v) { s += v ? "true" : "false"; }

// UTF-8 validation: a bad sequence becomes '?', control characters are escaped; cut at maxBytes on a character boundary
void vstr(std::string& s, const std::string& v, size_t maxBytes) {
    s += '"';
    size_t i = 0, out = 0;
    const size_t n = v.size();
    while (i < n && out < maxBytes) {
        const unsigned char c = (unsigned char)v[i];
        size_t len = 1;
        bool ok = true;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0 && c >= 0xC2) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) len = 4;
        else ok = false;
        if (ok && i + len > n) ok = false;
        for (size_t k = 1; ok && k < len; k++) if (((unsigned char)v[i + k] & 0xC0) != 0x80) ok = false;
        if (ok && len == 3) { const unsigned char c1 = (unsigned char)v[i + 1]; if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) ok = false; }
        if (ok && len == 4) { const unsigned char c1 = (unsigned char)v[i + 1]; if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) ok = false; }
        if (!ok) { s += '?'; i++; out++; continue; }
        if (len == 1) {
            switch (c) {
            case '"': s += "\\\""; break;
            case '\\': s += "\\\\"; break;
            case '\n': s += "\\n"; break;
            case '\r': s += "\\r"; break;
            case '\t': s += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); s += b; }
                else s += (char)c;
            }
        } else s.append(v, i, len);
        i += len;
        out += len;
    }
    s += '"';
}

void vcplx(std::string& s, float re, float im) { s += '['; vnum(s, re); s += ','; vnum(s, im); s += ']'; }

void vconst(std::string& s, const float* iq, size_t n, size_t maxPoints) {
    s += '[';
    if (n && maxPoints) {
        const size_t step = n > maxPoints ? (n + maxPoints - 1) / maxPoints : 1;
        bool first = true;
        for (size_t i = 0; i < n; i += step) {
            if (!first) s += ',';
            first = false;
            vnum(s, iq[2 * i]); s += ','; vnum(s, iq[2 * i + 1]);
        }
    }
    s += ']';
}

std::string base64(const uint8_t* p, size_t n) {
    static const char* tb = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t a = p[i], b = i + 1 < n ? p[i + 1] : 0, c = i + 2 < n ? p[i + 2] : 0;
        const uint32_t v = (a << 16) | (b << 8) | c;
        o += tb[(v >> 18) & 63]; o += tb[(v >> 12) & 63];
        o += i + 1 < n ? tb[(v >> 6) & 63] : '=';
        o += i + 2 < n ? tb[v & 63] : '=';
    }
    return o;
}

static uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
    static uint32_t tb[256];
    static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; tb[i] = c; } init = true; }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = tb[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
static uint32_t adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}
static void be32(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
static void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    be32(out, (uint32_t)data.size());
    const size_t at = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    be32(out, crc32(out.data() + at, out.size() - at));
}

std::vector<uint8_t> pngStored(const uint8_t* px, int w, int h, int channels) {
    std::vector<uint8_t> out;
    if (!px || w <= 0 || h <= 0 || (channels != 1 && channels != 3) || (size_t)w * (size_t)h > 4u * 1024 * 1024) return out;
    const size_t stride = (size_t)w * channels;
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * h);
    for (int y = 0; y < h; y++) { raw.push_back(0); raw.insert(raw.end(), px + y * stride, px + y * stride + stride); }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t off = 0; off < raw.size() || off == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        const bool last = off + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)(n & 0xFF)); z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xFF)); z.push_back((uint8_t)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
        if (last) break;
    }
    be32(z, adler32(raw.data(), raw.size()));
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.insert(out.end(), sig, sig + 8);
    std::vector<uint8_t> ihdr;
    be32(ihdr, (uint32_t)w); be32(ihdr, (uint32_t)h);
    ihdr.push_back(8); ihdr.push_back(channels == 1 ? 0 : 2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}

void vimage(std::string& s, const uint8_t* px, size_t n, int w, int h) {
    if (!px || w <= 0 || h <= 0) { s += "null"; return; }
    const size_t area = (size_t)w * (size_t)h;
    int ch = n == area ? 1 : n == 3 * area ? 3 : 0;
    if (!ch) { s += "null"; return; }
    // big pictures are box-downscaled to at most 1024 pixels wide
    std::vector<uint8_t> small;
    int f = 1;
    while (w / f > 1024) f++;
    if (f > 1) {
        const int ow = (w + f - 1) / f, oh = (h + f - 1) / f;
        small.assign((size_t)ow * oh * ch, 0);
        for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) for (int c = 0; c < ch; c++) {
            uint32_t acc = 0, cnt = 0;
            for (int dy = 0; dy < f && y * f + dy < h; dy++) for (int dx = 0; dx < f && x * f + dx < w; dx++) { acc += px[((size_t)(y * f + dy) * w + (x * f + dx)) * ch + c]; cnt++; }
            small[((size_t)y * ow + x) * ch + c] = (uint8_t)(cnt ? acc / cnt : 0);
        }
        px = small.data(); w = ow; h = oh;
    }
    const std::vector<uint8_t> png = pngStored(px, w, h, ch);
    if (png.empty()) { s += "null"; return; }
    s += "\"data:image/png;base64,";
    s += base64(png.data(), png.size());
    s += '"';
}
} // namespace detail

const char* commonSchemaJson() {
    return R"JSON([{"n":"standard","k":"num","l":"Standard"},{"n":"standardName","k":"text","l":"Mode"},{"n":"state","k":"num","l":"State (0 searching, 1 converging, 2 locked)"},)JSON"
           R"JSON({"n":"locked","k":"bool","l":"Locked"},{"n":"snrDb","k":"num","l":"Signal-to-noise ratio","u":"dB"},{"n":"snrKind","k":"text","l":"SNR measured as"},)JSON"
           R"JSON({"n":"cfoHz","k":"num","l":"Carrier offset","u":"Hz"},{"n":"blocksOk","k":"num","l":"Blocks OK"},{"n":"blocksBad","k":"num","l":"Blocks bad"},)JSON"
           R"JSON({"n":"quality","k":"obj","l":"Signal quality"},{"n":"details","k":"obj","l":"Details"}])JSON";
}

bool panelJson(const RxTelemetry& rx, std::string& out, size_t maxBytes) {
    out.clear();
    out += "{\"standard\":" + std::to_string(rx.standard) + ",\"common\":";
    out += rxTelemetryJson(rx, nullptr);
    out += ",\"fields\":";
    std::string f;
    const bool has = putModeFields(rx, rx.standard, f);
    if (!has) f = "{}";
    if (out.size() + f.size() + 2 > maxBytes) f = "{\"truncated\":true}";
    out += f;
    out += '}';
    return has;
}

std::string allSchemasJson() {
    std::string o = "{\"common\":";
    o += commonSchemaJson();
    for (int s : schemaStandards()) { o += ",\"" + std::to_string(s) + "\":"; o += schemaJson(s); }
    o += '}';
    return o;
}

std::string webModesJson() {
    int n = 0, ng = 0;
    const WebModeRow* r = webModeRows(n);
    const char* const* g = webModeGroups(ng);
    std::string o = "{\"groups\":[";
    for (int i = 0; i < ng; i++) { if (i) o += ','; detail::vstr(o, g[i], 200); }
    o += "],\"modes\":[";
    for (int i = 0; i < n; i++) {
        if (i) o += ',';
        o += "{\"family\":" + std::to_string(r[i].family) + ",\"name\":";
        detail::vstr(o, r[i].name, 200);
        o += ",\"group\":" + std::to_string(r[i].group) + ",\"blurb\":";
        detail::vstr(o, r[i].blurb, 1000);
        o += ",\"sub\":";
        detail::vstr(o, r[i].sub, 64);
        o += ",\"engine\":" + std::to_string(r[i].engineStd) + ",\"standard\":" + std::to_string(r[i].standard) + ",\"schema\":" + (schemaJson(r[i].standard) ? "true" : "false") + "}";
    }
    o += "]}";
    return o;
}
} // namespace dect2::webschema
