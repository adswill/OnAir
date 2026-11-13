// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/wb/wb_export.h"
#include "dect2/source/iq_file.h"
#include "dect2/wb/wb_dsp.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "dect2/common/endian.h"

namespace dect2 {
namespace wb {

namespace {

void le32(uint8_t* p, uint32_t v) { dect2::endian::storeLe32(p, v); }
void le16(uint8_t* p, uint16_t v) { dect2::endian::storeLe16(p, v); }

void wavHeader(uint8_t* h, int fmtTag, int channels, int rate, int bits, uint64_t dataBytes) {
    memcpy(h, "RIFF", 4); le32(h + 4, (uint32_t)(36 + dataBytes)); memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16); le16(h + 20, (uint16_t)fmtTag); le16(h + 22, (uint16_t)channels); le32(h + 24, (uint32_t)rate);
    le32(h + 28, (uint32_t)((uint64_t)rate * channels * bits / 8)); le16(h + 32, (uint16_t)(channels * bits / 8)); le16(h + 34, (uint16_t)bits);
    memcpy(h + 36, "data", 4); le32(h + 40, (uint32_t)dataBytes);
}

std::string stripExt(const std::string& p) {
    const size_t d = p.find_last_of('.'), s = p.find_last_of("/\\");
    if (d == std::string::npos || (s != std::string::npos && d < s)) return p;
    return p.substr(0, d);
}

} // namespace

const char* exportExt(int fmt) { return fmt == ExWav ? "wav" : fmt == ExCf32 ? "cf32" : "sigmf-data"; }
std::string withExt(const std::string& path, const std::string& ext) { return stripExt(path) + "." + ext; }

bool writeTextFile(const std::string& path, const std::string& text, std::string& err) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { err = "cannot write " + path; return false; }
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    return true;
}

bool writeWavMono16(const std::string& path, const std::vector<float>& a, int rate, std::string& err) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { err = "cannot write " + path; return false; }
    uint8_t h[44];
    wavHeader(h, 1, 1, rate, 16, (uint64_t)a.size() * 2);
    fwrite(h, 1, 44, f);
    std::vector<int16_t> o(a.size());
    for (size_t i = 0; i < a.size(); i++) o[i] = (int16_t)std::lrint(std::max(-1.f, std::min(1.f, a[i])) * 32767.f);
    fwrite(o.data(), 2, o.size(), f);
    fclose(f);
    return true;
}

bool exportSelection(const Source& s, uint64_t t0, uint64_t t1, double f0, double f1, const std::string& pathIn, int fmt, std::string& err,
                     const std::function<bool(double)>& progress) {
    t1 = std::min<uint64_t>(t1, s.length());
    if (t1 <= t0) { err = "nothing selected"; return false; }
    const bool band = f1 > f0 && !(f0 <= (s.real() ? 0 : -s.rate() / 2) + 1 && f1 >= s.rate() / 2 - 1);
    std::vector<cf32> ex;
    double rate = s.rate();
    double centre = s.centerHz();
    bool real = s.real();
    const uint64_t n = t1 - t0;
    if (band) {
        Extracted e;
        const double bw = f1 - f0;
        if (!extractBand(s, t0, t1, 0.5 * (f0 + f1), bw, std::min(bw * 2.5, s.rate()), e, 1u << 27, progress)) { err = "nothing in the band"; return false; }
        ex = std::move(e.x); rate = e.rate; centre += 0.5 * (f0 + f1); real = false;
    }
    std::string path = pathIn;
    std::string base = stripExt(path);
    if (fmt == ExSigmf) path = base + ".sigmf-data";
    else if (fmt == ExCf32) path = base + ".cf32";
    else path = base + ".wav";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { err = "cannot write " + path; return false; }
    const bool mono = real && fmt == ExWav;
    const uint64_t total = band ? ex.size() : n;
    uint64_t bytes = total * (mono ? 4 : 8);
    if (fmt == ExWav && bytes > 0xF0000000ull) { fclose(f); err = "too long for a WAV file (4 GB): use cf32 or SigMF"; return false; }
    if (fmt == ExWav) { uint8_t h[44]; wavHeader(h, 3, mono ? 1 : 2, (int)std::lround(rate), 32, bytes); fwrite(h, 1, 44, f); }
    const size_t chunk = 1u << 18;
    std::vector<cf32> buf(chunk);
    std::vector<float> fl(chunk);
    uint64_t pos = 0;
    while (pos < total) {
        const size_t want = (size_t)std::min<uint64_t>(chunk, total - pos);
        size_t got;
        if (band) { std::copy(ex.begin() + (long)pos, ex.begin() + (long)(pos + want), buf.begin()); got = want; }
        else got = s.read(t0 + pos, want, buf.data());
        if (!got) break;
        if (mono) { for (size_t i = 0; i < got; i++) fl[i] = buf[i].real(); fwrite(fl.data(), 4, got, f); }
        else fwrite(buf.data(), 8, got, f);
        pos += got;
        if (progress && !progress((double)pos / (double)total)) { fclose(f); err = "cancelled"; return false; }
    }
    fclose(f);
    if (fmt == ExSigmf) {
        char d[200];
        snprintf(d, sizeof d, "OnAir signal workbench: %s, %.3f s from %.3f s%s", s.name().c_str(), (double)total / rate, (double)t0 / s.rate(), band ? ", one band moved to baseband" : "");
        if (!writeSigmfMeta(base + ".sigmf-meta", FileFormat::CF32, false, rate, centre, 0, d, err)) return false;
    }
    return true;
}

} // namespace wb
} // namespace dect2
