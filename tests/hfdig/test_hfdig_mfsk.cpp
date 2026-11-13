// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MFSK text and pictures (hfdig_kbd_mfsk.cpp): the variants of fldigi's mfsk.cxx (MFSK16 .. MFSK128, the L modes), the picture mode that Shortwave
// Radiogram uses (header "Pic:WxH[C][pN];", then one tone per pixel) in grey and colour at X1 / X2 / X4, and the real-world cases: tuning error,
// sample clock error, noise, fading, a receiver centre that is off, text before and after a picture.
// The transmitter is written from the same fldigi source as the receiver (header, flush, 352 sample prologue, pixels, flush); a loopback shows the
// two agree, the impairments are what is judged. The timing of the first pixel was calibrated against that transmitter (kPicDelay in the receiver).
//
// DECT2_MFSK_WAV=<8 kHz 16 bit mono wav> [DECT2_MFSK_VARIANT=n (default 6 = MFSK32)] [DECT2_MFSK_CENTRE=hz] [DECT2_MFSK_AUTO=1] [DECT2_MFSK_START=s]
// [DECT2_MFSK_SEC=s] [DECT2_MFSK_PICDIR=dir] decodes a recording (a Shortwave Radiogram programme), prints the text and writes the pictures as PPM files.
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_kbd.h"
#include "dect2/hfdig/hfdig_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Noise {
    uint64_t s = 4242;
    double u() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return ((s >> 11) + 0.5) / 9007199254740992.0; }
    double g() { return std::sqrt(-2 * std::log(u())) * std::cos(6.283185307179586 * u()); }
};

enum { kMfsk16 = 3, kMfsk31 = 5, kMfsk32 = 6, kMfsk64 = 7, kMfsk128 = 8, kMfsk64L = 9, kMfsk128L = 10 };

struct Imp {
    double snrDb = 40;        // signal power over the noise power in 2500 Hz
    double offsetHz = 0;      // transmitter off its nominal audio centre (a mistuned dial)
    double ppm = 0;           // transmitter sample clock error
    double fadeDepth = 0;     // 0 none, 1: a second path 0.75 ms later that turns slowly, so a notch sweeps through the signal
    double fadeHz = 0.3;
    double sec = 60;
    double centre = -1;       // receiver centre (-1: the transmitter's)
    bool autoCentre = false;
};

struct Rx {
    std::string text;
    std::vector<std::shared_ptr<const KbdPicture>> pics;
    double sec = 0;           // audio seconds until `need` was seen (or all)
    bool found = false;
};

// Runs the transmitter through the impairments into the decoder until `need` is in the text (and, when `wantPics` pictures are there) or `im.sec` is up
static Rx run(KbdTxConfig tx, const Imp& im, const std::string& need, size_t wantPics = 0) {
    Rx r;
    tx.offsetHz = im.offsetHz;
    tx.clockPpm = im.ppm;
    auto gen = makeKbdTx(tx);
    // signal power from a copy of the transmitter (without the picture: its power is the same tone)
    double p = 0; size_t n = 0;
    {
        KbdTxConfig t2 = tx; t2.picture.reset();
        auto g2 = makeKbdTx(t2);
        std::vector<float> b(8000);
        for (int k = 0; k < 12; k++) { g2->generate(b.data(), b.size()); if (k < 4) continue; for (float v : b) { p += (double)v * v; n++; } }
    }
    const double sigma = tx.amp > 0 ? std::sqrt(p / n / std::pow(10.0, im.snrDb / 10.0) * 4000.0 / 2500.0) : 0.1;
    auto dec = makeKbdDecoder();
    KbdConfig k = tx.cfg;
    k.autoCentre = im.autoCentre;
    if (im.centre > 0) k.centreHz = im.centre;
    dec->setConfig(k);
    Noise nz;
    std::vector<float> buf(800), delayLine(64, 0.f);
    size_t dl = 0;
    double fphase = 0;
    HfdigKbdTelemetry t;
    for (int blk = 0; blk < (int)(im.sec * 10); blk++) {
        gen->generate(buf.data(), buf.size());
        if (im.fadeDepth > 0) {
            for (float& v : buf) {
                delayLine[dl] = v;
                const float d = delayLine[(dl + delayLine.size() - 6) % delayLine.size()];
                dl = (dl + 1) % delayLine.size();
                fphase += 6.283185307179586 * im.fadeHz / 8000.0;
                v = (float)(v + im.fadeDepth * 0.95 * d * std::cos(fphase)) / 1.5f;
            }
        }
        for (float& v : buf) v += (float)(sigma * nz.g());
        dec->feedAudio(buf.data(), buf.size());
        r.sec = (blk + 1) * 0.1;
        if (blk % 5 == 4) {
            dec->telemetry(t);
            if (!need.empty() && t.text.find(need) != std::string::npos && t.picHistory.size() >= wantPics) { r.found = true; break; }
        }
    }
    dec->telemetry(t);
    r.text = t.text; r.pics = t.picHistory;
    if (!need.empty() && t.text.find(need) != std::string::npos && t.picHistory.size() >= wantPics) r.found = true;
    return r;
}

static std::shared_ptr<KbdPicture> testPicture(int w, int h, bool colour, int spp) {
    auto p = std::make_shared<KbdPicture>();
    p->width = w; p->height = h; p->colour = colour; p->spp = spp;
    p->rgb.resize((size_t)w * h * 3);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint8_t* d = &p->rgb[((size_t)y * w + x) * 3];
        int r = 20 + x * 200 / (w - 1), g = 20 + y * 200 / (h - 1), b = ((x / 8 + y / 8) & 1) ? 220 : 40;
        if ((x - w / 2) * (x - w / 2) + (y - h / 2) * (y - h / 2) < (h / 4) * (h / 4)) { r = 250; g = 30; b = 30; }
        d[0] = (uint8_t)r; d[1] = (uint8_t)g; d[2] = (uint8_t)b;
    }
    return p;
}

// mean absolute difference of the received picture from what was sent (grey pictures are compared with the grey of the original); 255 when the sizes differ
static double picError(const KbdPicture& got, const KbdPicture& sent) {
    if (got.width != sent.width || got.height != sent.height || got.colour != sent.colour || !got.done) return 255;
    double e = 0; size_t n = 0;
    for (size_t i = 0; i < got.rgb.size(); i++) {
        int ref = sent.rgb[i];
        if (!sent.colour) { const size_t px = i / 3; ref = (31 * sent.rgb[px * 3] + 61 * sent.rgb[px * 3 + 1] + 8 * sent.rgb[px * 3 + 2]) / 100; }
        e += std::abs(ref - (int)got.rgb[i]); n++;
    }
    return e / (double)n;
}

static KbdTxConfig txFor(int variant, const std::string& text) {
    KbdTxConfig c;
    c.cfg.family = KbdMfsk; c.cfg.variant = variant;
    c.text = text;
    return c;
}

// DECT2_MFSK_WAV: a real recording
static int wavFile(const char* f) {
    FILE* fp = fopen(f, "rb");
    if (!fp) { printf("cannot open %s\n", f); return 1; }
    fseek(fp, 44, SEEK_SET);
    std::vector<int16_t> a(1 << 20);
    std::vector<float> x;
    size_t m;
    while ((m = fread(a.data(), 2, a.size(), fp)) > 0) for (size_t i = 0; i < m; i++) x.push_back(a[i] / 32768.f);
    fclose(fp);
    const double t0 = getenv("DECT2_MFSK_START") ? atof(getenv("DECT2_MFSK_START")) : 0;
    const double len = getenv("DECT2_MFSK_SEC") ? atof(getenv("DECT2_MFSK_SEC")) : 1e9;
    KbdConfig k;
    k.family = KbdMfsk;
    k.variant = getenv("DECT2_MFSK_VARIANT") ? atoi(getenv("DECT2_MFSK_VARIANT")) : kMfsk32;
    k.centreHz = getenv("DECT2_MFSK_CENTRE") ? atof(getenv("DECT2_MFSK_CENTRE")) : 1500;
    k.autoCentre = getenv("DECT2_MFSK_AUTO") != nullptr;
    auto dec = makeKbdDecoder();
    dec->setConfig(k);
    HfdigKbdTelemetry t;
    size_t shown = 0, saved = 0;
    const size_t i0 = (size_t)(t0 * 8000), i1 = std::min(x.size(), i0 + (size_t)(len * 8000));
    for (size_t i = i0; i < i1; i += 800) {
        dec->feedAudio(x.data() + i, std::min<size_t>(800, i1 - i));
        if (((i - i0) / 800) % 50 == 49) {
            dec->telemetry(t);
            if (t.text.size() > shown) { printf("[%6.1fs lock%d off%.0f c%.0f] %s\n", i / 8000.0, t.state, t.offsetHz, t.centreHz, t.text.substr(shown).c_str()); shown = t.text.size(); }
            while (saved < t.picHistory.size() && getenv("DECT2_MFSK_PICDIR")) {
                const auto& pic = *t.picHistory[t.picHistory.size() - 1 - saved];
                char path[512];
                snprintf(path, sizeof path, "%s/pic%zu.ppm", getenv("DECT2_MFSK_PICDIR"), saved);
                if (FILE* o = fopen(path, "wb")) { fprintf(o, "P6\n%d %d\n255\n", pic.width, pic.height); fwrite(pic.rgb.data(), 1, pic.rgb.size(), o); fclose(o); }
                printf("picture %zu: %dx%d %s\n", saved, pic.width, pic.height, pic.colour ? "colour" : "grey");
                saved++;
            }
        }
    }
    return 0;
}

int main() {
    if (getenv("DECT2_MFSK_WAV")) return wavFile(getenv("DECT2_MFSK_WAV"));
    static const std::string kText = "RADIOGRAM TEST 1234 THE QUICK BROWN FOX ";
    static const std::string kNeed = "FOX RADIOGRAM TEST 1234 THE QUICK";

    {   // every variant that goes on the air, clean: the text comes out
        static const double symSec[11] = {2048 / 8000.0, 1024 / 8000.0, 1024 / 11025.0, 512 / 8000.0, 512 / 11025.0, 256 / 8000.0, 256 / 8000.0, 128 / 8000.0, 64 / 8000.0, 128 / 8000.0, 64 / 8000.0};
        for (int v = 0; v < kbdVariantCount(KbdMfsk); v++) {
            Imp im; im.snrDb = 30;
            im.sec = symSec[v] * (v >= kMfsk64L ? 3500 : 700) + 20;
            Rx r = run(txFor(v, kText), im, kNeed);
            CHECK(r.found, "%s clean: [%.60s]", kbdVariantName(KbdMfsk, v), r.text.c_str());
            printf("%s clean: %.1f s\n", kbdVariantName(KbdMfsk, v), r.sec);
        }
        CHECK(kbdVariantCount(KbdMfsk) == 11 && std::string(kbdVariantName(KbdMfsk, kMfsk128)) == "MFSK128" && std::string(kbdVariantName(KbdMfsk, kMfsk64L)) == "MFSK64L", "variant list");
    }
    {   // impairments of the two Shortwave Radiogram modes
        struct C { const char* name; Imp im; bool ok; };
        std::vector<C> cs;
        { Imp i; i.sec = 50; i.offsetHz = 20; cs.push_back({"tuning +20 Hz", i, true}); }
        { Imp i; i.sec = 50; i.offsetHz = -60; cs.push_back({"tuning -60 Hz", i, true}); }
        { Imp i; i.sec = 50; i.ppm = 200; cs.push_back({"clock +200 ppm", i, true}); }
        { Imp i; i.sec = 50; i.ppm = -300; cs.push_back({"clock -300 ppm", i, true}); }
        { Imp i; i.sec = 90; i.snrDb = 0; cs.push_back({"noise 0 dB", i, true}); }
        { Imp i; i.sec = 90; i.snrDb = -3; cs.push_back({"noise -3 dB", i, true}); }
        { Imp i; i.sec = 90; i.snrDb = 12; i.fadeDepth = 1; i.fadeHz = 0.3; cs.push_back({"fading 0.3 Hz", i, true}); }
        { Imp i; i.sec = 90; i.snrDb = 12; i.fadeDepth = 1; i.fadeHz = 1.5; cs.push_back({"fading 1.5 Hz", i, true}); }
        { Imp i; i.sec = 60; i.offsetHz = 300; i.autoCentre = true; cs.push_back({"find signal, +300 Hz", i, true}); }
        { Imp i; i.sec = 60; i.centre = 1200; i.autoCentre = true; cs.push_back({"find signal, receiver 300 Hz off", i, true}); }
        { Imp i; i.sec = 60; i.offsetHz = -250; i.autoCentre = true; i.snrDb = 6; cs.push_back({"find signal, -250 Hz, 6 dB", i, true}); }
        for (int v : {kMfsk32, kMfsk64}) for (const C& c : cs) {
            Rx r = run(txFor(v, kText), c.im, kNeed);
            printf("%s %-34s %s %.1f s\n", kbdVariantName(KbdMfsk, v), c.name, r.found ? "ok  " : "FAIL", r.sec);
            CHECK(r.found == c.ok, "%s %s: [%.70s]", kbdVariantName(KbdMfsk, v), c.name, r.text.c_str());
        }
    }
    {   // pictures: grey and colour, X1 X2 X4, on every picture mode
        struct P { int v, w, h; bool col; int spp; };
        const P ps[] = {{kMfsk32, 48, 32, false, 8}, {kMfsk32, 48, 32, true, 8}, {kMfsk32, 40, 30, false, 4}, {kMfsk32, 40, 30, true, 2},
                        {kMfsk16, 48, 32, false, 8}, {kMfsk16, 40, 24, true, 8}, {kMfsk64, 48, 32, false, 8}, {kMfsk64, 64, 40, true, 8},
                        {kMfsk64, 48, 32, true, 4}, {kMfsk128, 48, 32, false, 8}, {kMfsk128, 48, 32, true, 2}};
        for (const P& p : ps) {
            auto pic = testPicture(p.w, p.h, p.col, p.spp);
            KbdTxConfig tx = txFor(p.v, kText);
            tx.picture = pic;
            Imp im; im.snrDb = 30; im.sec = 150;
            Rx r = run(tx, im, "FOX RADIOGRAM TEST 1234", 1);
            const double e = r.pics.empty() ? 255 : picError(*r.pics[0], *pic);
            printf("%s %dx%d %s X%d: error %.2f\n", kbdVariantName(KbdMfsk, p.v), p.w, p.h, p.col ? "colour" : "grey", 8 / p.spp, e);
            CHECK(e < (p.spp == 8 ? (p.v == kMfsk16 ? 8.0 : 5.0) : p.spp == 4 ? 7.0 : 15.0), "%s %dx%d %s spp %d: error %.2f", kbdVariantName(KbdMfsk, p.v), p.w, p.h, p.col ? "colour" : "grey", p.spp, e);
        }
    }
    {   // pictures under impairments, and the text after the picture
        struct C { const char* name; Imp im; double maxErr; };
        std::vector<C> cs;
        { Imp i; i.offsetHz = 25; cs.push_back({"tuning +25 Hz", i, 5}); }
        { Imp i; i.offsetHz = -40; cs.push_back({"tuning -40 Hz", i, 5}); }
        { Imp i; i.ppm = 150; cs.push_back({"clock +150 ppm", i, 6}); }
        { Imp i; i.snrDb = 10; cs.push_back({"noise 10 dB", i, 18}); }
        { Imp i; i.offsetHz = 150; i.autoCentre = true; cs.push_back({"find signal, +150 Hz", i, 5}); }
        for (const C& c : cs) {
            auto pic = testPicture(48, 32, true, 8);
            KbdTxConfig tx = txFor(kMfsk32, kText);
            tx.picture = pic;
            Imp im = c.im; im.sec = 150;
            Rx r = run(tx, im, "FOX RADIOGRAM TEST 1234", 1);
            const double e = r.pics.empty() ? 255 : picError(*r.pics[0], *pic);
            printf("MFSK32 picture, %-26s error %.2f\n", c.name, e);
            CHECK(e < c.maxErr, "MFSK32 picture %s: error %.2f", c.name, e);
        }
        // the text goes on after the picture
        auto pic = testPicture(40, 30, false, 8);
        KbdTxConfig tx = txFor(kMfsk32, "ALPHA BRAVO CHARLIE DELTA ECHO FOXTROT ");
        tx.picture = pic; tx.pictureAfterChars = 20;
        Imp im; im.snrDb = 30; im.sec = 200;
        Rx r = run(tx, im, "FOXTROT ALPHA BRAVO CHARLIE DELTA ECHO FOXTROT ALPHA", 1);
        CHECK(r.found && r.text.find("Pic:40x30;") != std::string::npos, "text after the picture: found %d [%.100s]", (int)r.found, r.text.c_str());
        // noise alone: no picture, no header
        auto dec = makeKbdDecoder();
        KbdConfig kc = txFor(kMfsk32, "").cfg;
        kc.autoCentre = true;
        dec->setConfig(kc);
        Noise nz;
        std::vector<float> buf(800);
        for (int b = 0; b < 1200; b++) { for (float& v : buf) v = (float)(0.1 * nz.g()); dec->feedAudio(buf.data(), buf.size()); }
        HfdigKbdTelemetry t;
        dec->telemetry(t);
        CHECK(t.picHistory.empty() && !t.picLive, "noise alone gave a picture");
        CHECK(t.text.find("Pic:") == std::string::npos, "noise alone gave a picture header");
    }
    return testSummary("test_hfdig_mfsk");
}
