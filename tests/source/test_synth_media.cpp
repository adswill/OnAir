// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Synthetic content" (synth_media.h): a picture and a sound made here are carried by the test signals and come out of the receivers.
//   DVB-T            generator (transport stream made from the files) -> DvbtReceiver -> FFmpeg decodes the MPEG-2 picture and the MP2 sound
//   analog TV (PAL)  makeModeSynth -> AtvReceiver: the picture and the sound carrier
//   FM broadcast     makeModeSynth -> FmReceiver: a stereo sound file as the programme
//   SSTV             the test audio -> the SSTV decoder: the picture
// And the built-in content is unchanged when nothing is chosen (the same samples as a config that never heard of the setting).
// NOT covered: the app's file dialog, DAB / DRM (they keep the built-in content), a real radio.
#include "dect2/atv/atv_rx.h"
#include "dect2/dvbt/dvbt_gen.h"
#include "dect2/dvbt/dvbt_rx.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/fm/fm_rx.h"
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_sstv.h"
#include "dect2/source/source.h"
#include "dect2/source/synth_media.h"
#include "dect2/tv/demo_ts.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static std::string g_dir;

// a picture with four flat colour areas (left to right, top to bottom): red, green, blue, yellow, and a darker frame
static void quadrant(int x, int y, int w, int h, uint8_t rgb[3]) {
    static const uint8_t c[4][3] = {{220, 40, 40}, {40, 200, 40}, {40, 40, 220}, {220, 220, 40}};
    const int q = (x * 2 / w) + 2 * (y * 2 / h);
    memcpy(rgb, c[q], 3);
}
static std::string writePpm(const std::string& name, int w, int h) {
    const std::string p = g_dir + "/" + name;
    std::ofstream f(p, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint8_t px[3]; quadrant(x, y, w, h, px); f.write((const char*)px, 3); }
    return p;
}
static void le32(std::ofstream& f, uint32_t v) { f.write((const char*)&v, 4); }
static void le16(std::ofstream& f, uint16_t v) { f.write((const char*)&v, 2); }
// a 16 bit PCM wav, 48 kHz: channel 1 a sine at hzL, channel 2 (stereo only) at hzR
static std::string writeWav(const std::string& name, bool stereo, double hzL, double hzR, double secs) {
    const std::string p = g_dir + "/" + name;
    std::ofstream f(p, std::ios::binary);
    const uint32_t n = (uint32_t)(secs * 48000), ch = stereo ? 2 : 1;
    f.write("RIFF", 4); le32(f, 36 + n * ch * 2); f.write("WAVEfmt ", 8); le32(f, 16); le16(f, 1); le16(f, (uint16_t)ch); le32(f, 48000); le32(f, 48000 * ch * 2); le16(f, (uint16_t)(ch * 2)); le16(f, 16);
    f.write("data", 4); le32(f, n * ch * 2);
    for (uint32_t i = 0; i < n; i++) {
        const double t = i / 48000.0;
        le16(f, (uint16_t)(int16_t)(0.5 * 32767 * std::sin(2 * M_PI * hzL * t)));
        if (stereo) le16(f, (uint16_t)(int16_t)(0.5 * 32767 * std::sin(2 * M_PI * hzR * t)));
    }
    return p;
}

// the amplitude of a tone in x (48 kHz)
static double toneAmp(const std::vector<float>& x, double hz, size_t from = 0) {
    const size_t n = x.size() > from ? x.size() - from : 0;
    double re = 0, im = 0;
    for (size_t i = 0; i < n; i++) { const double ph = 2 * M_PI * hz * (double)i / 48000.0; re += x[from + i] * std::cos(ph); im += x[from + i] * std::sin(ph); }
    return n ? 2 * std::sqrt(re * re + im * im) / (double)n : 0;
}

// the mean colour (0..255) of the middle of each quadrant of a w x h picture (bpp bytes a pixel), and the worst distance from the colour that was drawn
static double quadrantError(const uint8_t* rgb, int w, int h, int bpp, double xFrac0 = 0, double xFrac1 = 1, double yFrac0 = 0, double yFrac1 = 1) {
    double worst = 0;
    const int ax = (int)(xFrac0 * w), bx = (int)(xFrac1 * w), ay = (int)(yFrac0 * h), by = (int)(yFrac1 * h);
    for (int q = 0; q < 4; q++) {
        const int qx = q & 1, qy = q >> 1;
        const int x0 = ax + (bx - ax) * (qx * 4 + 1) / 8, x1 = ax + (bx - ax) * (qx * 4 + 3) / 8;   // the middle half of the quadrant
        const int y0 = ay + (by - ay) * (qy * 4 + 1) / 8, y1 = ay + (by - ay) * (qy * 4 + 3) / 8;
        double s[3] = {0, 0, 0}; int cnt = 0;
        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) { for (int k = 0; k < 3; k++) s[k] += rgb[((size_t)y * w + (size_t)x) * bpp + k]; cnt++; }
        uint8_t ref[3]; quadrant(qx * 2 + 1, qy * 2 + 1, 4, 4, ref);
        for (int k = 0; k < 3; k++) worst = std::max(worst, std::fabs(s[k] / std::max(1, cnt) - ref[k]));
    }
    return worst;
}

// ---- FFmpeg decode of the transport stream the receiver produced: the first picture (RGB 640 x 360) and the sound (mono, 48 kHz)
struct Decoded { bool pic = false; std::vector<uint8_t> rgb; std::vector<float> audio; int w = 0, h = 0; };
static Decoded decodeTs(const std::vector<uint8_t>& ts) {
    Decoded d;
    const std::string path = g_dir + "/rx.ts";
    { std::ofstream f(path, std::ios::binary); f.write((const char*)ts.data(), (std::streamsize)ts.size()); }
    AVFormatContext* ic = nullptr;
    if (avformat_open_input(&ic, path.c_str(), nullptr, nullptr) < 0) return d;
    avformat_find_stream_info(ic, nullptr);
    const int vi = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0), ai = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    auto open = [&](int si) -> AVCodecContext* {
        if (si < 0) return nullptr;
        const AVCodec* c = avcodec_find_decoder(ic->streams[si]->codecpar->codec_id);
        AVCodecContext* x = c ? avcodec_alloc_context3(c) : nullptr;
        if (x && (avcodec_parameters_to_context(x, ic->streams[si]->codecpar) < 0 || avcodec_open2(x, c, nullptr) < 0)) avcodec_free_context(&x);
        return x;
    };
    AVCodecContext *vx = open(vi), *ax = open(ai);
    SwrContext* swr = nullptr;
    SwsContext* sws = nullptr;
    AVFrame* fr = av_frame_alloc(); AVPacket* pk = av_packet_alloc();
    while (av_read_frame(ic, pk) >= 0) {
        AVCodecContext* x = pk->stream_index == vi ? vx : pk->stream_index == ai ? ax : nullptr;
        if (x && avcodec_send_packet(x, pk) >= 0) {
            while (avcodec_receive_frame(x, fr) >= 0) {
                if (x == vx && !d.pic) {
                    d.w = fr->width; d.h = fr->height;
                    d.rgb.assign((size_t)d.w * d.h * 3, 0);
                    sws = sws_getContext(d.w, d.h, (AVPixelFormat)fr->format, d.w, d.h, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    uint8_t* dst[4] = {d.rgb.data(), nullptr, nullptr, nullptr}; int ls[4] = {d.w * 3, 0, 0, 0};
                    sws_scale(sws, fr->data, fr->linesize, 0, d.h, dst, ls);
                    d.pic = true;
                } else if (x == ax) {
                    if (!swr) {
                        AVChannelLayout out; av_channel_layout_default(&out, 1);
                        swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_FLT, 48000, &ax->ch_layout, ax->sample_fmt, ax->sample_rate, 0, nullptr);
                        swr_init(swr);
                    }
                    std::vector<float> tmp((size_t)fr->nb_samples * 2 + 64);
                    uint8_t* o[1] = {(uint8_t*)tmp.data()};
                    const int got = swr_convert(swr, o, (int)tmp.size(), (const uint8_t**)fr->extended_data, fr->nb_samples);
                    if (got > 0) d.audio.insert(d.audio.end(), tmp.begin(), tmp.begin() + got);
                }
                av_frame_unref(fr);
            }
        }
        av_packet_unref(pk);
    }
    av_frame_free(&fr); av_packet_free(&pk); swr_free(&swr); if (sws) sws_freeContext(sws);
    avcodec_free_context(&vx); avcodec_free_context(&ax); avformat_close_input(&ic);
    return d;
}

static void testDvbt(const std::string& png, const std::string& wav) {
    SynthConfig sc;   // mode 0 = DVB-T / DVB-T2, as the synthetic source builds it
    sc.dvbt = true; sc.mediaKind = 1; sc.mediaPath = png; sc.mediaAudio = wav;
    const double fn = nativeRateHz(8);
    dvbt::Params p; p.mode = dvbt::k2K; p.guard = dvbt::kGi8; p.mod = dvbt::kQpsk; p.crHp = p.crLp = dvbt::kR12;
    std::unique_ptr<dvbt::Generator> gen;
    { SynthMediaScope scope(sc); gen = std::make_unique<dvbt::Generator>(p, demoTsSource(dvbt::netBitrate(p, fn))); }
    DvbtReceiver rx;
    rx.configure(fn, 8);
    std::vector<uint8_t> ts;
    rx.setPacketCallback([&](const uint8_t* pk, size_t n, double) {
        for (size_t i = 0; i < n; i++) { const uint8_t* q = pk + i * 188; if (!(q[1] & 0x80) && !(((q[1] & 0x1F) << 8 | q[2]) == 0x1FFF)) ts.insert(ts.end(), q, q + 188); }
    });
    std::vector<cf32> sym;
    size_t fed = 0;
    while (fed < (size_t)(2.4 * fn)) { gen->nextSymbol(sym); rx.feed(sym.data(), sym.size()); fed += sym.size(); }
    rx.flush();
    printf("DVB-T: %zu transport packets came out of the receiver\n", ts.size() / 188);
    const Decoded d = decodeTs(ts);
    CHECK(d.pic && d.w == 640 && d.h == 360, "DVB-T: no 640 x 360 picture decoded (%d, %d x %d)", (int)d.pic, d.w, d.h);
    if (d.pic) {
        const double e = quadrantError(d.rgb.data(), d.w, d.h, 3);
        printf("DVB-T: the picture's four colours are within %.1f levels of the file's\n", e);
        CHECK(e < 45, "DVB-T: the decoded picture is not the file's (colour error %.1f)", e);
    }
    CHECK(d.audio.size() > 48000 / 2, "DVB-T: %zu sound samples", d.audio.size());
    if (d.audio.size() > 48000 / 2) {
        const size_t from = d.audio.size() / 4;   // past the start-up of the decoder
        double tot = 0; for (size_t i = from; i < d.audio.size(); i++) tot += (double)d.audio[i] * d.audio[i];
        const double rms = std::sqrt(tot / (double)(d.audio.size() - from)), a = toneAmp(d.audio, 1500, from) / std::sqrt(2.0);
        printf("DVB-T: sound rms %.3f, 1.5 kHz component %.3f\n", rms, a);
        CHECK(rms > 0.1 && a > 0.8 * rms, "DVB-T: the sound is not the file's 1.5 kHz tone (rms %.3f, tone %.3f)", rms, a);
    }
}

static void testAtv(const std::string& ppm, const std::string& wav) {
    SynthConfig sc; sc.mode = 10; sc.snrDb = 45; sc.mediaKind = 1; sc.mediaPath = ppm; sc.mediaAudio = wav;   // PAL B/G, the system 0 of the mode
    const double rate = 10e6;
    auto ms = makeModeSynth(10, sc, rate);
    CHECK(ms != nullptr, "ATV: no test signal");
    if (!ms) return;
    AtvReceiver rx;
    rx.setSilent(true);
    std::vector<float> audio;
    rx.setAudioTap([&](const float* l, const float*, size_t n) { audio.insert(audio.end(), l, l + n); });
    rx.configure(rate);
    std::vector<cf32> buf(65536);
    std::shared_ptr<const AtvFrame> last;
    uint64_t fseq = 0;
    for (size_t done = 0; done < (size_t)(3.0 * rate); done += buf.size()) {
        ms->generate(buf.data(), buf.size());
        for (auto& s : buf) { auto q = [](float v) { return std::round(std::min(127.f, std::max(-128.f, v * 128.f))) / 128.f; }; s = cf32(q(s.real()), q(s.imag())); }
        rx.feed(buf.data(), buf.size());
        while (auto f = rx.frame(fseq)) last = f;
    }
    CHECK(last && last->width == 768 && last->height == 576, "ATV: no 768 x 576 picture");
    if (last) {
        // the active picture sits inside the frame: judge the middle (the quadrants of the file, a 4:3 picture)
        const double e = quadrantError(last->rgba.data(), last->width, last->height, 4, 0.1, 0.9, 0.1, 0.9);
        printf("ATV: the picture's four colours are within %.1f levels of the file's\n", e);
        CHECK(e < 60, "ATV: the decoded picture is not the file's (colour error %.1f)", e);
    }
    const size_t from = audio.size() / 3;
    const double a = toneAmp(audio, 1500, from), b = toneAmp(audio, 1000, from);
    printf("ATV: sound %zu samples, 1.5 kHz %.3f, built-in 1 kHz %.3f\n", audio.size(), a, b);
    CHECK(audio.size() > 48000 && a > 0.05 && a > 5 * b, "ATV: the sound carrier does not carry the file's tone (1.5 kHz %.3f, 1 kHz %.3f)", a, b);
}

static void testFm(const std::string& wav) {
    SynthConfig sc; sc.mode = 7; sc.snrDb = 60; sc.mediaAudio = wav;
    const double rate = 4e6;
    auto ms = makeModeSynth(7, sc, rate);
    CHECK(ms != nullptr, "FM: no test signal");
    if (!ms) return;
    FmReceiver rx;
    rx.setSilent(true);
    rx.configure(rate);
    std::vector<float> l, r;
    rx.setAudioTap([&](const float* a, const float* b, size_t n) { l.insert(l.end(), a, a + n); r.insert(r.end(), b, b + n); });
    std::vector<cf32> buf(65536);
    for (size_t done = 0; done < (size_t)(2.5 * rate); done += buf.size()) { ms->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    const size_t from = l.size() / 3;
    const double l15 = toneAmp(l, 1500, from), l10 = toneAmp(l, 1000, from), r25 = toneAmp(r, 2500, from), r30 = toneAmp(r, 3000, from);
    printf("FM: left 1.5 kHz %.3f (1 kHz %.3f), right 2.5 kHz %.3f (3 kHz %.3f)\n", l15, l10, r25, r30);
    CHECK(l15 > 0.05 && l15 > 8 * l10, "FM: the left channel is not the file's 1.5 kHz (%.3f against %.3f)", l15, l10);
    CHECK(r25 > 0.05 && r25 > 8 * r30, "FM: the right channel is not the file's 2.5 kHz (%.3f against %.3f)", r25, r30);
}

static void testSstv(const std::string& ppm) {
    SynthConfig sc; sc.mediaKind = 1; sc.mediaPath = ppm;
    int m = 0;
    for (int i = 0; i < sstvModeCount(); i++) if (std::string(sstvModeInfo(i).name) == "Robot 36") m = i;
    sc.modeOpt[1] = m;
    const SstvModeInfo& mi = sstvModeInfo(m);
    auto gen = makeSstvTestAudio(sc);
    auto dec = makeSstvDecoder();
    std::vector<float> buf(800);
    HfdigSstvTelemetry t;
    for (int blk = 0; blk < 600; blk++) {
        gen->generate(buf.data(), buf.size());
        dec->feedAudio(buf.data(), buf.size());
        if (blk % 10 == 0) { dec->telemetry(t); if (t.picturesDone >= 1) break; }
    }
    dec->telemetry(t);
    CHECK(t.picturesDone >= 1 && t.image && t.image->complete, "SSTV: no complete picture");
    if (t.picturesDone >= 1 && t.image && t.image->complete) {
        const double e = quadrantError(t.image->rgb.data(), t.image->width, t.image->height, 3);
        printf("SSTV %s: the picture's four colours are within %.1f levels of the file's\n", mi.name, e);
        CHECK(e < 50, "SSTV: the decoded picture is not the file's (colour error %.1f)", e);
    }
}

// nothing chosen: the signal is exactly the one a config that never heard of the setting makes
static void testBuiltinUnchanged() {
    SynthConfig a, b; a.mode = 7; b.mode = 7; b.mediaKind = 0; b.mediaPath = "ignored.png";   // a path without a kind is not used
    auto x = makeModeSynth(7, a, 2e6), y = makeModeSynth(7, b, 2e6);
    std::vector<cf32> bx(20000), by(20000);
    x->generate(bx.data(), bx.size()); y->generate(by.data(), by.size());
    CHECK(memcmp(bx.data(), by.data(), bx.size() * sizeof(cf32)) == 0, "FM: the built-in signal changed");
    SynthConfig d; d.mode = 4;   // DAB keeps the built-in content whatever is chosen
    d.mediaAudio = "x.wav"; d.mediaKind = 1; d.mediaPath = "x.png";
    CHECK(synthMediaSupport(d) == 0 && !synthMediaOf(d).active(), "DAB: a file was accepted");
    SynthConfig e; e.mode = 7; CHECK(synthMediaSupport(e) == kSynthMediaAudio, "FM takes sound only");
    // a file that is not there: the built-in content carries on and the status says so
    SynthConfig f; f.mode = 7; f.mediaAudio = g_dir + "/missing.wav";
    auto z = makeModeSynth(7, f, 2e6);
    CHECK(z != nullptr && synthMediaStatus().find("built-in") != std::string::npos, "a missing file: '%s'", synthMediaStatus().c_str());
}

int main() {
    g_dir = (std::filesystem::temp_directory_path() / ("onair_synth_media_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
    std::filesystem::create_directories(g_dir);
    const std::string wavMono = writeWav("tone.wav", false, 1500, 0, 3), wavStereo = writeWav("stereo.wav", true, 1500, 2500, 3);
    const std::string ppm16x9 = writePpm("quad169.ppm", 640, 360), ppm4x3 = writePpm("quad43.ppm", 640, 480);
    testBuiltinUnchanged();
    testFm(wavStereo);
    testSstv(ppm4x3);
    testAtv(ppm4x3, wavMono);
    testDvbt(ppm16x9, wavMono);
    std::error_code ec; std::filesystem::remove_all(g_dir, ec);
    return testSummary("synth_media");
}
