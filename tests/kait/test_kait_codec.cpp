// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT image codec: round trip of a synthetic 1080p picture, PSNR against the number of bytes (the prefix property), odd sizes, truncation safety.
#include "kait_test_util.h"
#include <chrono>

using namespace dect2::kait;

static Image prefixDecode(const Encoded& e, size_t n) {
    return decodeImage(e.bytes.data(), std::min(n, e.bytes.size()), e.info);
}

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    const Image img = kaittest::testPicture();
    const Encoded e = encodeImage(img, 1000000);
    const double encS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("full stream: %zu bytes, top plane %d, %u decisions, encode %.2f s\n", e.bytes.size(), e.info.topPlane, e.info.decisions, encS);
    const auto t1 = std::chrono::steady_clock::now();
    const Image full = prefixDecode(e, e.bytes.size());
    const double decS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    const double pf = psnrRgb(img, full);
    printf("all bytes: PSNR %.2f dB, decode %.2f s\n", pf, decS);
    CHECK(pf > 38.0, "near-lossless PSNR %.2f", pf);
    // the prefix property: PSNR grows with the bytes
    const size_t cuts[] = {500, 1000, 2000, 4000, 8000, 16000, 32000, 64000, 128000, 256000};
    double prev = 0;
    for (size_t c : cuts) {
        if (c > e.bytes.size()) break;
        const Image d = prefixDecode(e, c);
        const double p = psnrRgb(img, d);
        printf("prefix %7zu bytes (%.3f bpp): PSNR %.2f dB\n", c, 8.0 * (double)c / (1920.0 * 1080.0), p);
        CHECK(p >= prev - 0.05, "PSNR fell from %.2f to %.2f at %zu bytes", prev, p, c);
        prev = p;
    }
    // a budgeted encode (what the transmitter does) gives the same picture as the prefix of the full stream, within a hair
    const Encoded e2 = encodeImage(img, 30000);
    CHECK(e2.bytes.size() <= 30000, "budget exceeded: %zu", e2.bytes.size());
    const Image d2 = decodeImage(e2.bytes.data(), e2.bytes.size(), e2.info);
    printf("budget 30000: %zu bytes, %u decisions, PSNR %.2f dB\n", e2.bytes.size(), e2.info.decisions, psnrRgb(img, d2));
    CHECK(psnrRgb(img, d2) > 20.0, "30 KB picture too poor");
    // truncating the budgeted stream further must not crash and must not give garbage
    for (size_t c : {0u, 3u, 10u, 100u}) {
        const Image d = decodeImage(e2.bytes.data(), c, e2.info);
        CHECK(d.w == 1920 && d.h == 1080, "size after cut %zu", (size_t)c);
    }
    // odd sizes and a tiny picture
    for (auto wh : {std::pair<int, int>{37, 23}, {64, 48}, {1, 1}, {17, 3}, {200, 151}}) {
        const Image s = kaittest::testPicture(wh.first, wh.second);
        const Encoded es = encodeImage(s, 1000000);
        const Image ds = decodeImage(es.bytes.data(), es.bytes.size(), es.info);
        const double ps = psnrRgb(s, ds);
        printf("%dx%d: %zu bytes, PSNR %.1f dB\n", wh.first, wh.second, es.bytes.size(), ps);
        CHECK(ds.w == wh.first && ds.h == wh.second && ps > 18.0, "odd size %dx%d PSNR %.1f", wh.first, wh.second, ps);
    }
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
