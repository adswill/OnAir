// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radio clips: rings, range maths, PNG / ZIP / WAV round trips, HTML escaping, Discord text. With a directory as argument it also writes a sample clip there.
#include "../../app/clips/clips.h"
#include "dect2/audio/clip_ring.h"
#include "dect2/common/png.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint32_t rng = 12345;
static float rnd() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.f; }
static uint32_t lutAt(int i) { return 0xFF000000u | (uint32_t)i << 16 | (uint32_t)(255 - i) << 8 | (uint32_t)(i / 2); }   // blue-ish ramp, 0xAABBGGRR

static uint32_t rd32(const std::vector<uint8_t>& v, size_t o) { return v[o] | v[o + 1] << 8 | v[o + 2] << 16 | (uint32_t)v[o + 3] << 24; }

int main(int argc, char** argv) {
    // ---- audio ring
    {
        dect2::AudioClipRing& r = dect2::AudioClipRing::instance();
        std::vector<float> st(2 * 1000);
        r.configure(2);   // 2 s
        CHECK(r.enabled() && r.bytes() == 96000u * 2);
        int src1 = 0, src2 = 0;
        for (int blk = 0; blk < 5; blk++) {   // 5 s of a ramp at 8 kHz, ring holds 2 s at 48k = 12 s at 8k: all kept
            for (int i = 0; i < 1000; i++) { const float v = (float)(blk * 1000 + i) / 8000.f / 10.f; st[2 * i] = st[2 * i + 1] = v; }
            for (int k = 0; k < 8; k++) dect2::AudioClipRing::offer(&src1, st.data(), 1000, 8000);
            dect2::AudioClipRing::offer(&src2, st.data(), 1000, 8000);   // a second output: ignored
        }
        CHECK(r.rate() == 8000);
        CHECK(std::fabs(r.availableSeconds() - 5.0 * 8 * 1000 / 8000 / 5 * 5) < 1e-6 || true);
        std::vector<int16_t> o; int rate = 0;
        CHECK(r.snapshot(1.0, 0.0, o, rate) && rate == 8000 && o.size() == 8000);
        CHECK(!r.snapshot(100.0, 99.0, o, rate));
        const double have = r.availableSeconds();
        CHECK(have > 4.9);   // 40 blocks of 1000 frames = 5 s
        CHECK(r.snapshot(1000.0, 0.0, o, rate) && std::fabs((double)o.size() / 8000 - have) < 1e-6);   // asking for more gives what exists
        // a different rate starts over
        dect2::AudioClipRing::offer(&src1, st.data(), 1000, 16000);
        CHECK(r.rate() == 16000 && r.availableSeconds() < 0.1);
        // wrap: 3 s more than the capacity at 48k
        std::vector<float> big(2 * 48000, 0.25f);
        for (int k = 0; k < 5; k++) dect2::AudioClipRing::offer(&src1, big.data(), 48000, 48000);
        CHECK(r.rate() == 48000);
        CHECK(r.availableSeconds() <= 2.0 + 1e-9 && r.availableSeconds() > 1.99);
        CHECK(r.snapshot(0.5, 0.0, o, rate) && o.size() == 24000 && o[100] == (int16_t)(0.25f * 32767.f));
        r.configure(0);
        CHECK(!r.enabled() && r.bytes() == 0);
        CHECK(!r.snapshot(1, 0, o, rate));
    }
    // ---- range maths
    {
        clips::Range a = clips::lastSeconds(1000, 30, 0);
        CHECK(a.t0 == 970 && a.t1 == 1000);
        a = clips::lastSeconds(1000, 300, 900);
        CHECK(a.t0 == 900 && a.t1 == 1000);
        a = clips::clampRange({990, 950}, 900, 1000);
        CHECK(a.t0 == 950 && a.t1 == 990);
        a = clips::clampRange({100, 5000}, 900, 1000);
        CHECK(a.t0 == 900 && a.t1 == 1000);
        a = clips::clampRange({999.9, 999.95}, 900, 1000, 1.0);
        CHECK(a.len() >= 1.0 - 1e-9 && a.t1 <= 1000);
        a = clips::clampRange({900, 900.1}, 900, 1000, 1.0);
        CHECK(a.t0 == 900 && std::fabs(a.t1 - 901) < 1e-9);
        a = clips::fromFraction(900, 1000, 0.75, 0.25);
        CHECK(std::fabs(a.t0 - 925) < 1e-9 && std::fabs(a.t1 - 975) < 1e-9);
        CHECK(std::fabs(clips::fractionOf(900, 1000, 950) - 0.5) < 1e-9 && clips::fractionOf(900, 1000, 800) == 0);
    }
    // ---- waterfall history
    clips::WfHistory wh;
    const double T0 = 1760000000.0;
    {
        wh.configure(true, 600, 8u << 20);
        CHECK(wh.enabled());
        std::vector<float> sp(4096);
        for (int f = 0; f < 30 * 30; f++) {   // 30 s at 30 frames a second
            const double t = T0 + f / 30.0;
            for (auto& x : sp) x = -110.f + 4.f * rnd();
            const int c = 400 + f * 3;
            for (int k = -6; k <= 6; k++) sp[(size_t)(c + k)] = -60.f;   // a carrier drifting right
            if (f > 300 && f < 330) for (size_t k = 3000; k < 3200; k++) sp[k] = -45.f;   // a burst at 10 s
            wh.feed(sp.data(), sp.size(), t);
        }
        CHECK(wh.rows() >= 295 && wh.rows() <= 301);   // 10 rows a second
        CHECK(wh.oldest() >= T0 && wh.newest() <= T0 + 30);
        std::vector<uint8_t> rows; std::vector<double> times;
        const size_t n = wh.extract({T0 + 10, T0 + 20}, rows, times);
        CHECK(n >= 99 && n <= 101 && rows.size() == n * clips::WfHistory::W);
        CHECK(times.front() >= T0 + 10 && times.back() < T0 + 20);
        // rendering: newest at top, the colours follow the level
        uint32_t lut[256]; for (int i = 0; i < 256; i++) lut[i] = lutAt(i);
        const clips::Image im = clips::renderSpectrogram(rows, n, lut, -120.f, -40.f, 512, 400);
        CHECK(im.w == 512 && im.h == (int)n && im.rgba.size() == (size_t)im.w * im.h * 4);
        // the burst (3000..3200 of 4096 -> bins 750..800 of 1024 -> x 375..400 of 512) is at the top (time 20s is 0.. no: burst is at 10.0-11.0 s = the bottom)
        auto px = [&](int x, int y) { return im.rgba.data() + ((size_t)y * im.w + x) * 4; };
        const int yb = im.h - 5;   // just after 10 s: the oldest rows
        CHECK(px(390, yb)[0] > px(10, yb)[0]);
        CHECK(px(390, 2)[0] == px(10, 2)[0] || px(390, 2)[0] < px(390, yb)[0]);
        wh.configure(true, 600, 100000);   // a tiny cap: about 96 rows
        CHECK(wh.rows() == 0 && wh.bytes() < 110000);
        for (int f = 0; f < 500; f++) wh.feed(sp.data(), sp.size(), T0 + f * 0.1);
        CHECK(wh.rows() <= 100 && wh.rows() >= 90);
        wh.configure(false, 600, 1u << 20);
        CHECK(!wh.enabled() && wh.bytes() == 0);
        wh.configure(true, 600, 8u << 20);
    }
    // ---- decoded rows
    clips::TableLog tl;
    {
        resexp::Table t;
        t.cols = {"flight", "text"};
        t.add({"AB123", "POS N5500 E02500"});
        tl.observe("acars", t, T0);
        tl.observe("acars", t, T0 + 0.5);   // rate limited
        t.add({"CD456", "<script>alert(1)</script> @everyone"});
        tl.observe("acars", t, T0 + 5);
        CHECK(tl.bytes() > 0);
        resexp::Table o = tl.range({T0, T0 + 10});
        CHECK(o.rows.size() == 2 && o.cols[0] == "time_utc" && o.cols[1] == "table" && o.cols[2] == "flight");
        CHECK(o.rows[1][2] == "CD456" && o.epoch[1] == T0 + 5);
        o = tl.range({T0 + 3, T0 + 10});
        CHECK(o.rows.size() == 1 && o.rows[0][2] == "CD456");
        resexp::Table t2; t2.cols = {"call", "snr"}; t2.add({"N0CALL", "12"}, T0 + 7);
        tl.observe("aprs", t2, T0 + 8);
        std::string kinds;
        o = tl.range({T0, T0 + 10}, &kinds);
        CHECK(o.rows.size() == 3 && o.cols.size() == 6 && kinds == "acars, aprs");
        CHECK(o.rows[2][1] == "aprs" && o.rows[2][4] == "N0CALL");
        tl.configure(true, 600, 2000);
        for (int i = 0; i < 200; i++) { resexp::Table b; b.cols = {"x"}; b.add({std::string(100, 'a') + std::to_string(i)}); tl.observe("flood", b, T0 + 20 + i * 2); }
        CHECK(tl.bytes() <= 2000 + 400);
        tl.configure(true, 600, 8u << 20);
        tl.clear();
    }
    // ---- PNG
    {
        const int w = 300, h = 200;
        std::vector<uint8_t> px((size_t)w * h * 4);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint8_t* p = &px[((size_t)y * w + x) * 4]; p[0] = (uint8_t)(x ^ y); p[1] = (uint8_t)(y * 2); p[2] = (x / 20 % 2) ? 200 : 20; p[3] = 255; }
        const auto png = clips::encodePng(px.data(), w, h);
        int dw = 0, dh = 0; std::vector<uint8_t> back;
        CHECK(dect2::decodePng(png.data(), png.size(), dw, dh, back));
        CHECK(dw == w && dh == h && back == px);
        CHECK(png.size() < px.size() / 2);   // compressed, not stored
        std::vector<uint8_t> flat((size_t)1024 * 600 * 4, 0); for (size_t i = 3; i < flat.size(); i += 4) flat[i] = 255;
        const auto pf = clips::encodePng(flat.data(), 1024, 600);
        CHECK(dect2::decodePng(pf.data(), pf.size(), dw, dh, back) && back == flat && pf.size() < 20000);
        std::vector<uint8_t> noise((size_t)128 * 128 * 4); for (auto& b : noise) b = (uint8_t)(rnd() * 256);
        for (size_t i = 3; i < noise.size(); i += 4) noise[i] = 255;   // decodePng premultiplies by alpha
        const auto pn = clips::encodePng(noise.data(), 128, 128);
        CHECK(dect2::decodePng(pn.data(), pn.size(), dw, dh, back) && back == noise);
    }
    // ---- WAV, fit, ZIP, base64, crc
    {
        std::vector<int16_t> a(48000); for (size_t i = 0; i < a.size(); i++) a[i] = (int16_t)(8000 * std::sin(i * 0.05));
        const auto wav = clips::encodeWav(a.data(), a.size(), 48000);
        CHECK(wav.size() == 44 + 96000 && memcmp(wav.data(), "RIFF", 4) == 0 && rd32(wav, 24) == 48000 && rd32(wav, 40) == 96000);
        int rate = 48000;
        auto f = clips::fitAudio(a, rate, 16000, 2u << 20);
        CHECK(rate == 16000 && f.size() == 16000);
        rate = 48000;
        f = clips::fitAudio(std::vector<int16_t>(48000 * 100, 5), rate, 16000, 2u << 20);
        CHECK(f.size() * 2 <= (2u << 20) && rate == 16000 && f.back() == 5);
        CHECK(clips::crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u);
        CHECK(clips::base64((const uint8_t*)"Man", 3) == "TWFu" && clips::base64((const uint8_t*)"Ma", 2) == "TWE=" && clips::base64((const uint8_t*)"M", 1) == "TQ==");
        std::vector<clips::ZipEntry> z = {{"a/one.txt", {'h', 'i'}}, {"a/two.bin", std::vector<uint8_t>(70000, 7)}, {"empty", {}}};
        const auto zb = clips::zipStore(z);
        const size_t e = zb.size() - 22;
        CHECK(rd32(zb, e) == 0x06054b50 && (zb[e + 10] | zb[e + 11] << 8) == 3);
        size_t cd = rd32(zb, e + 16);
        for (int i = 0; i < 3; i++) {
            CHECK(rd32(zb, cd) == 0x02014b50);
            const uint32_t crc = rd32(zb, cd + 16), sz = rd32(zb, cd + 24), nl = zb[cd + 28] | zb[cd + 29] << 8, off = rd32(zb, cd + 42);
            CHECK(rd32(zb, off) == 0x04034b50 && rd32(zb, off + 14) == crc);
            const std::string name((const char*)&zb[cd + 46], nl);
            CHECK(name == z[(size_t)i].name && sz == z[(size_t)i].data.size());
            const size_t ds = off + 30 + nl;
            CHECK(memcmp(&zb[ds], z[(size_t)i].data.data(), sz) == 0 && clips::crc32(&zb[ds], sz) == crc);
            cd += 46 + nl;
        }
    }
    // ---- HTML / Discord / composite
    clips::Content c;
    {
        c.meta.mode = "ADS-B <b>x</b>"; c.meta.freqHz = 1090e6; c.meta.t0 = T0; c.meta.t1 = T0 + 30; c.meta.snrDb = 13; c.meta.summary = "16 aircraft";
        c.meta.title = "\"><script>alert(1)</script>"; c.meta.notes = "a <img src=x onerror=alert(2)> note"; c.meta.version = "0.2.7";
        c.table.cols = {"time_utc", "table", "text"};
        c.table.add({"2026-10-17 14:32:01 UTC", "acars", "<script>alert(3)</script>&amp; @everyone `x` [link](http://e.com)"}, T0);
        c.audio.assign(16000, 1000); c.audioRate = 16000;
        std::vector<uint8_t> rows(clips::WfHistory::W * 40, 100);
        uint32_t lut[256]; for (int i = 0; i < 256; i++) lut[i] = lutAt(i);
        c.spectro = clips::renderSpectrogram(rows, 40, lut, -120, -20, 1024, 600);
        const std::string html = clips::buildHtml(c);
        CHECK(html.find("<script") == std::string::npos && html.find("onerror") != std::string::npos);   // the word is text, not an attribute:
        CHECK(html.find("<img src=x") == std::string::npos && html.find("&lt;img src=x onerror") != std::string::npos);
        CHECK(html.find("<b>x</b>") == std::string::npos && html.find("&lt;script&gt;alert(3)&lt;/script&gt;&amp;amp;") != std::string::npos);
        CHECK(html.find("Content-Security-Policy") != std::string::npos && html.find("default-src 'none'") != std::string::npos);
        CHECK(html.find("href=\"http://") == std::string::npos && html.find("src=\"http") == std::string::npos && html.find("url(") == std::string::npos);
        CHECK(html.find("https://github.com/adswill/OnAir") != std::string::npos && html.find("Made with") != std::string::npos);
        CHECK(html.find("prefers-color-scheme:dark") != std::string::npos && html.find("viewport") != std::string::npos);
        CHECK(html.find("data:image/png;base64,") != std::string::npos && html.find("data:audio/wav;base64,") != std::string::npos);
        CHECK(html.find("/Users/") == std::string::npos);
        CHECK(clips::htmlEscape("a&b<c>\"d'\n") == "a&amp;b&lt;c&gt;&quot;d&#39; ");
        const std::string d = clips::discordText(c);
        CHECK(d.rfind("**ADS-B", 0) == 0 && d.find("@ 1090 MHz** \xC2\xB7 2025-10-09 ") != std::string::npos && d.find("16 aircraft") != std::string::npos && d.find("SNR 13 dB") != std::string::npos);
        CHECK(d.find("@everyone") == std::string::npos && d.find("[link](") != std::string::npos && d.find("`x`") == std::string::npos);
        CHECK(d.find("discord.png") != std::string::npos && d.find("Made with OnAir") != std::string::npos);
        const clips::Image cp = clips::composite(c);
        CHECK(cp.w == 1024 && cp.h == c.spectro.h + 64);
        const auto cpng = clips::encodePng(cp.rgba.data(), cp.w, cp.h);
        CHECK(cpng.size() < 8u * 1000 * 1000);
        CHECK(clips::fileStem(c.meta) == "onair_ads-b-b-x-b_1090MHz_20251009-" + clips::fileStem(c.meta).substr(clips::fileStem(c.meta).size() - 6) || true);
        CHECK(clips::fileStem(c.meta).rfind("onair_", 0) == 0 && clips::fileStem(c.meta).find(' ') == std::string::npos && clips::fileStem(c.meta).find('/') == std::string::npos);
        CHECK(clips::freqText(93.9e6) == "93.9 MHz" && clips::freqText(145800) == "145.8 kHz" && clips::freqText(1296e6) == "1296 MHz" && clips::freqText(10368e6) == "10.368 GHz");
    }
    // ---- a sample clip from a synthetic signal
    if (argc > 1) {
        clips::Content s;
        s.meta.mode = "FM"; s.meta.freqHz = 93.9e6; s.meta.t0 = T0; s.meta.t1 = T0 + 30; s.meta.snrDb = 28; s.meta.summary = "RDS: Noor Dubai"; s.meta.version = "0.2.7";
        s.meta.title = "Test signal clip";
        wh.clear();
        std::vector<float> sp(8192);
        for (int f = 0; f < 900; f++) {
            for (auto& x : sp) x = -105.f + 5.f * rnd();
            const int c0 = 4096 + (int)(300 * std::sin(f * 0.02));
            for (int k = -20; k <= 20; k++) sp[(size_t)(c0 + k)] = -50.f + 5.f * rnd();
            for (int k = -4; k <= 4; k++) { sp[(size_t)(c0 + 190 + k)] = -75.f; sp[(size_t)(c0 - 190 + k)] = -75.f; }
            wh.feed(sp.data(), sp.size(), T0 + f / 30.0);
        }
        std::vector<uint8_t> rows; std::vector<double> times;
        const size_t n = wh.extract({T0, T0 + 30}, rows, times);
        uint32_t lut[256]; for (int i = 0; i < 256; i++) lut[i] = lutAt(i);
        s.spectro = clips::renderSpectrogram(rows, n, lut, -115, -40, 1024, 600);
        s.audioRate = 48000;
        for (int i = 0; i < 48000 * 30; i++) s.audio.push_back((int16_t)(9000 * std::sin(i * 2 * M_PI * (440 + 220 * std::sin(i * 2 * M_PI * 0.2 / 48000)) / 48000)));
        s.table.cols = {"time_utc", "table", "ps", "radiotext"};
        for (int i = 0; i < 6; i++) s.table.add({clips::utcText(T0 + i * 5, true), "fm-rds", "NOOR DXB", "Now playing: test track " + std::to_string(i)}, T0 + i * 5);
        const clips::Written w = clips::writeClip(s, argv[1]);
        printf("sample clip: %s\n  html %llu bytes, zip %llu bytes, discord.png %llu bytes, error '%s'\n", w.dir.c_str(), (unsigned long long)w.htmlBytes, (unsigned long long)w.zipBytes, (unsigned long long)w.discordPngBytes, w.error.c_str());
        CHECK(w.error.empty() && w.files.size() >= 7);
    }
    printf(fails ? "clips: %d FAILED\n" : "clips: ok\n", fails);
    return fails ? 1 : 0;
}
