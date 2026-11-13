// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham SSTV send: picture files, fitting, the audio of a picture through the repo's SSTV modulator, CW ID, the gated send (ham_sstv.h).
#include "dect2/ham/ham_sstv.h"
#include "dect2/ham/ham_keyer.h"
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_sstv.h"
#include "dect2/common/png.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace dect2::ham {

namespace {
constexpr double kPi = 3.14159265358979323846;
uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

bool decodeBmp(const std::vector<uint8_t>& d, SstvPicture& o, std::string& err) {
    if (d.size() < 54) { err = "the BMP file is too short"; return false; }
    const uint32_t off = le32(&d[10]), hdr = le32(&d[14]);
    if (hdr < 40) { err = "this BMP header is not supported"; return false; }
    const int32_t w = (int32_t)le32(&d[18]), hs = (int32_t)le32(&d[22]);
    const int bpp = le16(&d[28]);
    const uint32_t comp = le32(&d[30]);
    if (w <= 0 || hs == 0 || w > 8192 || std::abs(hs) > 8192) { err = "the BMP size is not valid"; return false; }
    if (!(bpp == 24 && comp == 0) && !(bpp == 32 && (comp == 0 || comp == 3))) { err = "only uncompressed 24 / 32 bit BMP files are supported"; return false; }
    const int h = std::abs(hs);
    const size_t stride = ((size_t)w * (size_t)(bpp / 8) + 3) & ~(size_t)3;
    if ((size_t)off + stride * (size_t)h > d.size()) { err = "the BMP file is cut short"; return false; }
    o.w = w; o.h = h; o.rgb.assign((size_t)w * h * 3, 0);
    for (int y = 0; y < h; y++) {
        const uint8_t* row = &d[off + stride * (size_t)(hs > 0 ? h - 1 - y : y)];   // positive height: bottom-up
        for (int x = 0; x < w; x++) {
            const uint8_t* p = row + (size_t)x * (size_t)(bpp / 8);
            uint8_t* q = &o.rgb[((size_t)y * w + x) * 3];
            q[0] = p[2]; q[1] = p[1]; q[2] = p[0];
        }
    }
    return true;
}

bool decodePpm(const std::vector<uint8_t>& d, SstvPicture& o, std::string& err) {
    size_t i = 2;
    auto skip = [&]() {
        while (i < d.size()) {
            if (d[i] == '#') { while (i < d.size() && d[i] != '\n') i++; }
            else if (isspace(d[i])) i++;
            else break;
        }
    };
    auto num = [&](long& v) -> bool {
        skip();
        if (i >= d.size() || !isdigit(d[i])) return false;
        v = 0;
        while (i < d.size() && isdigit(d[i])) { v = v * 10 + (d[i] - '0'); i++; if (v > 1000000) return false; }
        return true;
    };
    const bool bin = d[1] == '6';
    long w, h, mx;
    if (!num(w) || !num(h) || !num(mx) || w <= 0 || h <= 0 || w > 8192 || h > 8192 || mx <= 0 || mx > 65535) { err = "the PPM header is not valid"; return false; }
    o.w = (int)w; o.h = (int)h; o.rgb.assign((size_t)w * h * 3, 0);
    const size_t n = o.rgb.size();
    if (bin) {
        i++;   // the one white space after the maximum
        const size_t bps = mx > 255 ? 2 : 1;
        if (i + n * bps > d.size()) { err = "the PPM file is cut short"; return false; }
        for (size_t k = 0; k < n; k++) {
            const long v = bps == 2 ? (d[i + 2 * k] << 8 | d[i + 2 * k + 1]) : d[i + k];
            o.rgb[k] = (uint8_t)std::lround(255.0 * v / mx);
        }
    } else {
        for (size_t k = 0; k < n; k++) { long v; if (!num(v)) { err = "the PPM file is cut short"; return false; } o.rgb[k] = (uint8_t)std::lround(255.0 * std::min(v, mx) / mx); }
    }
    return true;
}

// 8 kHz -> 48 kHz: windowed sinc, 6 phases (cut-off 3.6 kHz)
std::vector<float> up6(const std::vector<float>& x) {
    constexpr int N = 48;
    static const std::vector<double> h = [] {
        std::vector<double> v;
        for (int i = -N; i <= N; i++) {
            const double a = kPi * i * 2 * 3600.0 / 48000.0;
            v.push_back((i == 0 ? 1.0 : std::sin(a) / a) * (0.5 + 0.5 * std::cos(kPi * i / (N + 1))) * 2 * 3600.0 / 48000.0 * 6);
        }
        return v;
    }();
    std::vector<float> o(x.size() * 6, 0.f);
    for (size_t n = 0; n < o.size(); n++) {
        double acc = 0;
        for (int i = -N; i <= N; i++) {
            const long m = (long)n - i;
            if (m < 0 || m % 6) continue;
            const size_t j = (size_t)(m / 6);
            if (j < x.size()) acc += h[(size_t)(i + N)] * x[j];
        }
        o[n] = (float)acc;
    }
    return o;
}
}  // namespace

bool sstvDecodePicture(const std::vector<uint8_t>& d, SstvPicture& o, std::string& err) {
    err.clear();
    o = SstvPicture();
    if (d.size() >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!decodePng(d.data(), d.size(), w, h, rgba) || w <= 0 || h <= 0) { err = "the PNG file cannot be decoded"; return false; }
        o.w = w; o.h = h; o.rgb.resize((size_t)w * h * 3);
        for (size_t i = 0; i < (size_t)w * h; i++) for (int c = 0; c < 3; c++) o.rgb[i * 3 + c] = rgba[i * 4 + c];   // premultiplied: on black
        return true;
    }
    if (d.size() >= 2 && d[0] == 'B' && d[1] == 'M') return decodeBmp(d, o, err);
    if (d.size() >= 2 && d[0] == 'P' && (d[1] == '6' || d[1] == '3')) return decodePpm(d, o, err);
    err = "not a PNG, BMP or PPM picture (JPEG: save it as PNG first)";
    return false;
}

bool sstvLoadPicture(const std::string& path, SstvPicture& o, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() > 64u << 20) { err = "the picture file is too large"; return false; }
    return sstvDecodePicture(d, o, err);
}

SstvPicture sstvFit(const SstvPicture& in, int w, int h, bool fill) {
    SstvPicture o;
    o.w = w; o.h = h; o.rgb.assign((size_t)w * h * 3, 0);
    if (in.w <= 0 || in.h <= 0 || in.rgb.size() < (size_t)in.w * in.h * 3) return o;
    const double sx = (double)w / in.w, sy = (double)h / in.h;
    const double s = fill ? std::max(sx, sy) : std::min(sx, sy);
    const double ox = (w - in.w * s) / 2, oy = (h - in.h * s) / 2;   // where the source's corner lands
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const double u = (x + 0.5 - ox) / s - 0.5, v = (y + 0.5 - oy) / s - 0.5;
            if (u < -0.5 || v < -0.5 || u > in.w - 0.5 || v > in.h - 0.5) continue;   // a bar
            const int x0 = std::clamp((int)std::floor(u), 0, in.w - 1), y0 = std::clamp((int)std::floor(v), 0, in.h - 1);
            const int x1 = std::min(x0 + 1, in.w - 1), y1 = std::min(y0 + 1, in.h - 1);
            const double fx = std::clamp(u - x0, 0.0, 1.0), fy = std::clamp(v - y0, 0.0, 1.0);
            for (int c = 0; c < 3; c++) {
                auto px = [&](int xx, int yy) { return (double)in.rgb[((size_t)yy * in.w + xx) * 3 + c]; };
                const double val = (px(x0, y0) * (1 - fx) + px(x1, y0) * fx) * (1 - fy) + (px(x0, y1) * (1 - fx) + px(x1, y1) * fx) * fy;
                o.rgb[((size_t)y * w + x) * 3 + c] = (uint8_t)std::clamp(std::lround(val), 0L, 255L);
            }
        }
    return o;
}

int sstvModeByName(const std::string& nameIn) {
    auto norm = [](std::string s) { std::string o; for (char c : s) if (isalnum((unsigned char)c)) o += (char)tolower((unsigned char)c); return o; };
    const std::string n = norm(nameIn);
    for (int i = 0; i < sstvModeCount(); i++) if (norm(sstvModeInfo(i).name) == n) return i;
    return -1;
}

double sstvSeconds(int mode) {
    if (mode < 0 || mode >= sstvModeCount()) return 0;
    const SstvModeInfo& m = sstvModeInfo(mode);
    const bool pd = std::string(m.name).rfind("PD", 0) == 0;
    return 0.91 + m.lineMs * (pd ? m.height / 2 : m.height) / 1000.0;
}

std::vector<float> sstvRender(const SstvPicture& pic, const SstvTxCfg& c, std::string& err) {
    err.clear();
    if (c.mode < 0 || c.mode >= sstvModeCount()) { err = "unknown SSTV mode"; return {}; }
    const SstvModeInfo& m = sstvModeInfo(c.mode);
    if (pic.w <= 0 || pic.h <= 0) { err = "no picture"; return {}; }
    SstvPicture f = sstvFit(pic, m.width, m.height, c.fill);
    if (!c.stamp.empty()) sstvStampText(f.w, f.h, f.rgb, c.stamp);
    auto gen = makeSstvTx(c.mode, f.rgb);
    if (!gen) { err = "no SSTV modulator"; return {}; }
    // the modulator repeats the picture: take one cycle (it starts with 0.4 s of silence, ends with 1.5 s of it)
    std::vector<float> x8, b(800);
    size_t lastOn = 0;
    bool seen = false;
    const size_t maxN = (size_t)((sstvSeconds(c.mode) + 4) * 8000);
    while (x8.size() < maxN) {
        gen->generate(b.data(), b.size());
        for (float v : b) { x8.push_back(v); if (v != 0.f) { lastOn = x8.size(); seen = true; } }
        if (seen && x8.size() > lastOn + 8000) break;
    }
    if (!seen) { err = "the modulator gave no audio"; return {}; }
    x8.resize(lastOn + 400);
    std::vector<float> a = up6(x8);
    if (!c.cwId.empty()) {   // CW identification after half a second
        KeyerCfg k; k.wpm = c.cwWpm;
        const auto cw = keyerAudio(keyerText(c.cwId, k, 0), c.cwHz, kKbdRate);
        a.insert(a.end(), (size_t)(0.5 * kKbdRate), 0.f);
        a.insert(a.end(), cw.begin(), cw.end());
    }
    float pk = 0;
    for (float v : a) pk = std::max(pk, std::fabs(v));
    if (pk > 0) for (auto& v : a) v /= pk;
    return a;
}

bool sstvSend(HamSlotTx& tx, TxDuty& duty, const HamTxCheck& gateIn, const SstvPicture& pic, const SstvTxCfg& c, double level, double now, std::string& err) {
    std::vector<float> a = sstvRender(pic, c, err);
    if (a.empty()) return false;
    const double dur = (double)a.size() / kKbdRate;
    HamTxCheck g = gateIn;
    g.audioHz = 1100 - 50;
    g.bwHz = kSstvBwHz;
    if (!tx.sendAudio(g, std::move(a), kSstvBwHz, level, err)) return false;
    duty.add(now, dur);
    return true;
}

}  // namespace dect2::ham
