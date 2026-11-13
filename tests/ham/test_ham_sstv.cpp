// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham SSTV send: pictures from PNG, BMP and PPM files (written here), fitting with crop and letterbox, the picture as audio in Robot 36,
// Martin M1, Scottie S1 and PD120 and back through the repo's SSTV decoder (HfdigSstv) with noise and a sound card clock error, the CW ID and
// the gate. Nothing is transmitted: the rig is a recording fake.
#include "dect2/ham/ham_sstv.h"
#include "dect2/hfdig/hfdig_sstv.h"
#include "../common/test_util.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
using namespace dect2;
using namespace dect2::ham;

struct Rec : HamRigIo {
    std::vector<float> a;
    int ptt1 = 0, ptt0 = 0;
    bool ptt(bool on, std::string&) override { (on ? ptt1 : ptt0)++; return true; }
    bool beginAudio(int, std::string&) override { a.clear(); return true; }
    int write(const float* m, int n) override { a.insert(a.end(), m, m + n); return n; }
    int pending() override { return 0; }
    void flushAudio() override {}
    void endAudio() override {}
};
static HamTxCheck gate(double dial) {
    HamTxCheck g; g.licenceConfirmed = true; g.armed = true; g.call = "LY1ABC"; g.grid = "KO24"; g.dialHz = dial; return g;
}
struct Gauss { uint64_t s = 99; double u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return ((s >> 11) + 0.5) / 9007199254740992.0; } double g() { return std::sqrt(-2 * std::log(u())) * std::cos(6.283185307179586 * u()); } };

static SstvPicture pattern(int w, int h) {
    SstvPicture p; p.w = w; p.h = h; p.rgb.resize((size_t)w * h * 3);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint8_t* q = &p.rgb[((size_t)y * w + x) * 3];
        q[0] = (uint8_t)(255 * x / (w - 1)); q[1] = (uint8_t)(255 * y / (h - 1)); q[2] = ((x / 40 + y / 40) % 2) ? 220 : 30;
    }
    return p;
}
static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }
static void put32be(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; i--) v.push_back((uint8_t)(x >> (8 * i))); }
static std::vector<uint8_t> bmp(const SstvPicture& p) {
    const size_t stride = ((size_t)p.w * 3 + 3) & ~(size_t)3;
    std::vector<uint8_t> v = {'B', 'M'};
    put32(v, (uint32_t)(54 + stride * p.h)); put32(v, 0); put32(v, 54); put32(v, 40); put32(v, (uint32_t)p.w); put32(v, (uint32_t)p.h);
    v.push_back(1); v.push_back(0); v.push_back(24); v.push_back(0); put32(v, 0); put32(v, 0); put32(v, 2835); put32(v, 2835); put32(v, 0); put32(v, 0);
    for (int y = p.h - 1; y >= 0; y--) {
        for (int x = 0; x < p.w; x++) { const uint8_t* q = &p.rgb[((size_t)y * p.w + x) * 3]; v.push_back(q[2]); v.push_back(q[1]); v.push_back(q[0]); }
        for (size_t k = (size_t)p.w * 3; k < stride; k++) v.push_back(0);
    }
    return v;
}
static uint32_t crc32(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) { c ^= d[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}
// a PNG with one IDAT of stored (uncompressed) deflate blocks
static std::vector<uint8_t> png(const SstvPicture& p) {
    std::vector<uint8_t> raw;
    for (int y = 0; y < p.h; y++) { raw.push_back(0); raw.insert(raw.end(), p.rgb.begin() + (long)((size_t)y * p.w * 3), p.rgb.begin() + (long)((size_t)(y + 1) * p.w * 3)); }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t o = 0; o < raw.size(); o += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - o);
        z.push_back(o + n == raw.size() ? 1 : 0);
        z.push_back((uint8_t)n); z.push_back((uint8_t)(n >> 8)); z.push_back((uint8_t)~n); z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + (long)o, raw.begin() + (long)(o + n));
    }
    uint32_t a = 1, b = 0; for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    put32be(z, b << 16 | a);
    std::vector<uint8_t> v = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto chunk = [&](const char* t, const std::vector<uint8_t>& d) {
        put32be(v, (uint32_t)d.size());
        std::vector<uint8_t> c(t, t + 4); c.insert(c.end(), d.begin(), d.end());
        v.insert(v.end(), c.begin(), c.end());
        put32be(v, crc32(c.data(), c.size()));
    };
    std::vector<uint8_t> ih; put32be(ih, (uint32_t)p.w); put32be(ih, (uint32_t)p.h); ih.push_back(8); ih.push_back(2); ih.push_back(0); ih.push_back(0); ih.push_back(0);
    chunk("IHDR", ih); chunk("IDAT", z); chunk("IEND", {});
    return v;
}
static std::string writeFile(const std::string& name, const std::vector<uint8_t>& d) {
    const std::string p = (std::filesystem::temp_directory_path() / name).string();
    std::ofstream f(p, std::ios::binary); f.write((const char*)d.data(), (std::streamsize)d.size());
    return p;
}
static double meanErr(const SstvPicture& a, const std::vector<uint8_t>& b, int rows) {
    double e = 0; size_t n = 0;
    for (int y = 0; y < rows; y++) for (int x = 0; x < a.w; x++) for (int c = 0; c < 3; c++) { const size_t i = ((size_t)y * a.w + x) * 3 + c; e += std::fabs((double)a.rgb[i] - b[i]); n++; }
    return n ? e / (double)n : 999;
}

int main() {
    std::string err;
    const SstvPicture src = pattern(400, 300);
    // ---- files ----
    {
        SstvPicture a, b, c;
        CHECK(sstvLoadPicture(writeFile("onair_sstv_test.bmp", bmp(src)), a, err) && a.w == 400 && a.h == 300 && a.rgb == src.rgb, "BMP reads back (%s)", err.c_str());
        CHECK(sstvLoadPicture(writeFile("onair_sstv_test.png", png(src)), b, err) && b.w == 400 && b.rgb == src.rgb, "PNG reads back (%s)", err.c_str());
        std::string ppm = "P6\n# a comment\n400 300\n255\n";
        std::vector<uint8_t> pp(ppm.begin(), ppm.end()); pp.insert(pp.end(), src.rgb.begin(), src.rgb.end());
        CHECK(sstvLoadPicture(writeFile("onair_sstv_test.ppm", pp), c, err) && c.rgb == src.rgb, "PPM reads back (%s)", err.c_str());
        std::vector<uint8_t> junk = {0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3};
        CHECK(!sstvDecodePicture(junk, a, err) && err.find("JPEG") != std::string::npos, "JPEG refused with a hint: %s", err.c_str());
        CHECK(!sstvLoadPicture("/nonexistent/x.png", a, err), "missing file");
        std::vector<uint8_t> cut = bmp(src); cut.resize(1000);
        CHECK(!sstvDecodePicture(cut, a, err), "a cut BMP is refused");
    }
    // ---- fitting ----
    {
        const SstvPicture wide = pattern(640, 240);   // 8:3 into 320 x 256
        const SstvPicture lb = sstvFit(wide, 320, 256, false), cr = sstvFit(wide, 320, 256, true);
        CHECK(lb.rgb[0] == 0 && lb.rgb[((size_t)128 * 320 + 160) * 3 + 2] > 0, "letterbox: black bar on top, picture in the middle");
        CHECK(cr.rgb[1] < 30 && cr.rgb[2] > 0, "crop fills the frame");
        CHECK(sstvModeByName("martin m1") >= 0 && sstvModeByName("PD120") >= 0 && sstvModeByName("nope") < 0, "mode names");
        CHECK(std::fabs(sstvSeconds(sstvModeByName("Martin M1")) - 115) < 2 && std::fabs(sstvSeconds(sstvModeByName("Robot 36")) - 37) < 2, "durations M1 %.1f R36 %.1f",
              sstvSeconds(sstvModeByName("Martin M1")), sstvSeconds(sstvModeByName("Robot 36")));
    }
    // ---- audio through the receiver ----
    for (const char* name : {"Robot 36", "Martin M1", "Scottie S1", "PD120"}) {
        SstvTxCfg c; c.mode = sstvModeByName(name); c.stamp = "LY1ABC";
        const std::vector<float> a = sstvRender(src, c, err);
        CHECK(!a.empty(), "%s: render (%s)", name, err.c_str());
        if (a.empty()) continue;
        const SstvModeInfo& m = sstvModeInfo(c.mode);
        CHECK(std::fabs((double)a.size() / 48000 - sstvSeconds(c.mode) - 0.45) < 1.5, "%s: %.1f s of audio for %.1f s", name, (double)a.size() / 48000, sstvSeconds(c.mode));
        // 48 kHz -> 8 kHz with a 40 ppm faster sound card, noise at 20 dB in 3 kHz
        const double r = 6.0 * (1 + 40e-6);
        std::vector<float> x;
        Gauss g;
        const int N = 64;
        for (double t = 0; t + N + 1 < (double)a.size(); t += r) {
            const long c0 = (long)t;
            double acc = 0, ws = 0;
            for (long k = -N; k <= N; k++) {
                const long i = c0 + k; if (i < 0 || i >= (long)a.size()) continue;
                const double u = (double)i - t, s = u == 0 ? 1.0 : std::sin(3.141592653589793 * u * 7000.0 / 48000) / (3.141592653589793 * u * 7000.0 / 48000);
                const double w = 0.5 + 0.5 * std::cos(3.141592653589793 * u / (N + 1));
                acc += a[(size_t)i] * s * w; ws += s * w;
            }
            x.push_back((float)(0.5 * acc / ws + std::sqrt(0.125 / 100 * 4000.0 / 3000) * g.g()));
        }
        x.insert(x.end(), 16000, 0.f);
        auto dec = makeSstvDecoder();
        for (size_t i = 0; i < x.size(); i += 800) dec->feedAudio(&x[i], std::min<size_t>(800, x.size() - i));
        HfdigSstvTelemetry t;
        dec->telemetry(t);
        const auto img = t.history.empty() ? t.image : t.history.front();
        CHECK(img && img->mode == m.name, "%s: decoded as %s", name, img ? img->mode.c_str() : "nothing");
        if (!img) continue;
        SstvPicture want = sstvFit(src, m.width, m.height, true);
        sstvStampText(want.w, want.h, want.rgb, "LY1ABC");
        const double e = meanErr(want, img->rgb, std::min(img->lines, m.height));
        printf("%-11s %5.1f s  %d of %d lines, VIS %d, mean error %.1f levels, slant %.0f ppm\n", name, (double)a.size() / 48000, img->lines, m.height, t.visCode, e, img->slantPpm);
        CHECK(img->lines >= m.height - 2, "%s: %d of %d lines", name, img->lines, m.height);
        CHECK(e < 22, "%s: picture differs by %.1f levels on average", name, e);
    }
    // ---- CW ID and the gate ----
    {
        SstvTxCfg c; c.mode = sstvModeByName("Robot 36");
        const auto plain = sstvRender(src, c, err);
        c.cwId = "DE LY1ABC";
        const auto withId = sstvRender(src, c, err);
        CHECK(withId.size() > plain.size() + 48000 * 3, "CW ID adds %.1f s", (double)(withId.size() - plain.size()) / 48000);
        Rec r; HamSlotTx tx(r); TxDuty duty;
        HamTxCheck g = gate(14.227e6); g.armed = false;
        CHECK(!sstvSend(tx, duty, g, src, c, 0.4, 0, err) && r.ptt1 == 0, "disarmed: nothing keyed");
        CHECK(sstvSend(tx, duty, gate(14.349e6), src, c, 0.4, 0, err) && r.ptt1 == 1, "over the 20 m edge: a warning, sent");
        CHECK(sstvSend(tx, duty, gate(14.227e6), src, c, 0.4, 0, err) && r.ptt1 == 2 && r.ptt0 == 2, "14.230 SSTV armed: sent (%s)", err.c_str());
        TxDuty full; full.add(0, 290);
        CHECK(sstvSend(tx, full, gate(14.227e6), src, c, 0.4, 290, err), "a full duty counter does not refuse (3.2)");
        SstvPicture none;
        CHECK(sstvRender(none, c, err).empty(), "no picture refused");
    }
    return testSummary("ham_sstv");
}
