// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AudioIn: a 30 s file and a 700 s file (past the 600 s in-memory limit) play sample for sample to their end, then loop (#471).
// The file is mono, so FFmpeg spreads it at -3 dB to both channels.
#include "dect2/audio/audioin.h"
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <filesystem>
static float val(long k) { return (float)(((k * 7) % 20000) - 10000) / 32768.f; }
static void writeWav(const char* p, int rate, long n) {
    FILE* f = fopen(p, "wb"); uint32_t ds = n * 2, rs = 36 + ds; uint16_t ch = 1, bps = 16, fmt = 1, ba = 2; uint32_t br = rate * 2, fl = 16, sr = rate;
    fwrite("RIFF", 1, 4, f); fwrite(&rs, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fl, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bps, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&ds, 4, 1, f);
    for (long k = 0; k < n; k++) { int16_t v = (int16_t)std::lround(val(k) * 32768.f); fwrite(&v, 2, 1, f); }
    fclose(f);
}
static int check(const char* path, int rate, double sec) {
    const long n = (long)(rate * sec);
    writeWav(path, rate, n);
    dect2::AudioIn a; std::string err;
    if (!a.start(dect2::AudioIn::File, rate, path, 0, err)) { printf("start failed: %s\n", err.c_str()); return 1; }
    printf("%.0f s file: fileSeconds %.2f\n", sec, a.fileSeconds());
    if (std::fabs(a.fileSeconds() - sec) > 0.05) { printf("  wrong length\n"); remove(path); return 1; }
    std::vector<float> buf(2 * 4096); long k = 0, bad = 0, firstBad = -1; const long total = n + n / 3;   // past the end, into the loop
    while (k < total) {
        const int m = 4096; a.read(buf.data(), m);
        for (int i = 0; i < m && k < total; i++, k++) { const float e = val(k % n) * 0.70710678f; if (std::fabs(buf[2 * i] - e) > 2e-3f || std::fabs(buf[2 * i + 1] - e) > 2e-3f) { if (firstBad < 0) firstBad = k; bad++; } }
    }
    printf("  frames %ld, mismatches %ld (first at %.2f s)\n", total, bad, firstBad < 0 ? -1.0 : firstBad / (double)rate);
    remove(path);
    return bad > 0;
}
int main() {
    int r = 0;
    const std::string dir = std::filesystem::temp_directory_path().string();
    r |= check((dir + "/onair_audiofile_short.wav").c_str(), 8000, 30);
    r |= check((dir + "/onair_audiofile_long.wav").c_str(), 8000, 700);
    printf(r ? "FAIL\n" : "PASS\n"); return r;
}
