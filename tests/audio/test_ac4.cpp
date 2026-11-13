// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AC-4 support of the ATSC 3.0 receiver (core/src/audio/ac4_dec.cpp, ac4_xcode.cpp, the AC-4 branch of atsc3_remux.cpp).
//   test_ac4 [dir-with-a-stock-libavcodec-and-libavutil]
// There is no real AC-4 stream and no AC-4 decoder in this repository, so what is checked is everything around the decoder:
//   - the signalling: codecs strings, MPD, the 'ac-4' sample entry and its 'dac4' box in an initialization segment
//   - the loader: a folder without libraries, with broken files, with a FFmpeg that has no "ac4" decoder: all fail with a message
//   - the whole path with a BORROWED decoder: ac4SetDecoderName("ac3") makes the loader use the AC-3 decoder of a stock FFmpeg (the folder
//     of the first argument), and the test stream is an AC-3 stream whose sample entry was relabelled 'ac-4' / 'dac4'. That proves loading,
//     calling the library through its stable interface, the remux branch, the AC-3 re-encoding and the time line. It does NOT prove that
//     any AC-4 bitstream decodes: that needs the patched FFmpeg and a real stream (not available here).
#include "dect2/audio/ac4_dec.h"
#include "dect2/atsc3/atsc3_remux.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
}

using namespace dect2;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static std::vector<uint8_t> slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void split(const std::vector<uint8_t>& f, std::vector<uint8_t>& init, std::vector<std::vector<uint8_t>>& segs) {
    size_t pos = 0;
    bool inInit = true;
    while (pos + 8 <= f.size()) {
        uint32_t sz = (f[pos] << 24) | (f[pos + 1] << 16) | (f[pos + 2] << 8) | f[pos + 3];
        if (sz < 8 || pos + sz > f.size()) break;
        char type[5] = {(char)f[pos + 4], (char)f[pos + 5], (char)f[pos + 6], (char)f[pos + 7], 0};
        bool moof = !strcmp(type, "moof");
        if (moof) { inInit = false; segs.emplace_back(); }
        if (inInit) init.insert(init.end(), f.begin() + pos, f.begin() + pos + sz);
        else if (!segs.empty() && strcmp(type, "sidx") && strcmp(type, "styp")) segs.back().insert(segs.back().end(), f.begin() + pos, f.begin() + pos + sz);
        pos += sz;
    }
}

struct MemIn { const uint8_t* d; size_t n, pos = 0; };
static int memRead(void* o, uint8_t* buf, int size) {
    MemIn* m = (MemIn*)o;
    if (m->pos >= m->n) return AVERROR_EOF;
    int k = (int)std::min<size_t>(size, m->n - m->pos);
    memcpy(buf, m->d + m->pos, k);
    m->pos += k;
    return k;
}

// 3 s of a 1 kHz tone in stereo, AC-3 encoded, in a fragmented MP4; returns the raw AC-3 frames too
static bool makeAc3Fmp4(std::vector<uint8_t>& mp4, std::vector<std::vector<uint8_t>>& frames) {
    const AVCodec* c = avcodec_find_encoder_by_name("ac3");
    if (!c) return false;
    AVCodecContext* e = avcodec_alloc_context3(c);
    e->sample_rate = 48000; e->sample_fmt = AV_SAMPLE_FMT_FLTP; e->bit_rate = 192000;
    av_channel_layout_default(&e->ch_layout, 2);
    e->time_base = AVRational{1, 48000};
    if (avcodec_open2(e, c, nullptr) < 0) return false;
    AVFormatContext* oc = nullptr;
    avformat_alloc_output_context2(&oc, nullptr, "mp4", nullptr);
    if (!oc || avio_open_dyn_buf(&oc->pb) < 0) return false;
    AVStream* st = avformat_new_stream(oc, nullptr);
    avcodec_parameters_from_context(st->codecpar, e);
    st->time_base = AVRational{1, 48000};
    AVDictionary* o = nullptr;
    av_dict_set(&o, "movflags", "frag_custom+empty_moov+delay_moov+default_base_moof", 0);
    if (avformat_write_header(oc, &o) < 0) return false;
    AVFrame* f = av_frame_alloc();
    AVPacket* p = av_packet_alloc();
    int64_t pts = 0;
    int k = 0;
    for (int i = 0; i < 3 * 48000 / 1536; i++) {
        f->nb_samples = 1536; f->format = AV_SAMPLE_FMT_FLTP; f->sample_rate = 48000;
        av_channel_layout_copy(&f->ch_layout, &e->ch_layout);
        av_frame_get_buffer(f, 0);
        for (int ch = 0; ch < 2; ch++)
            for (int n = 0; n < 1536; n++) ((float*)f->extended_data[ch])[n] = 0.5f * (float)sin(2 * M_PI * 1000.0 * (double)(pts + n) / 48000.0);
        f->pts = pts; pts += 1536;
        avcodec_send_frame(e, f);
        av_frame_unref(f);
        while (avcodec_receive_packet(e, p) >= 0) {
            frames.emplace_back(p->data, p->data + p->size);
            p->stream_index = 0;
            av_packet_rescale_ts(p, e->time_base, st->time_base);
            av_interleaved_write_frame(oc, p);
            if (++k % 10 == 0) av_write_frame(oc, nullptr);   // a fragment every 10 frames
            av_packet_unref(p);
        }
    }
    avcodec_send_frame(e, nullptr);
    while (avcodec_receive_packet(e, p) >= 0) { frames.emplace_back(p->data, p->data + p->size); p->stream_index = 0; av_packet_rescale_ts(p, e->time_base, st->time_base); av_interleaved_write_frame(oc, p); av_packet_unref(p); }
    av_write_trailer(oc);
    uint8_t* buf = nullptr;
    int n = avio_close_dyn_buf(oc->pb, &buf);
    mp4.assign(buf, buf + n);
    av_free(buf);
    oc->pb = nullptr;
    avformat_free_context(oc);
    av_packet_free(&p);
    av_frame_free(&f);
    avcodec_free_context(&e);
    return !frames.empty();
}

// 'ac-3' -> 'ac-4' and 'dac3' -> 'dac4' (with a real ac4_dsi_v1: version 1, bitstream version 2, 48 kHz, frame rate index 3, 2 presentations)
static void relabel(std::vector<uint8_t>& init) {
    for (size_t i = 4; i + 8 < init.size(); i++) {
        if (!memcmp(&init[i], "ac-3", 4)) init[i + 3] = '4';
        if (!memcmp(&init[i], "dac3", 4)) {
            init[i + 3] = '4';
            const uint32_t v = (1u << 21) | (2u << 14) | (1u << 13) | (3u << 9) | 2u;
            init[i + 4] = (uint8_t)(v >> 16); init[i + 5] = (uint8_t)(v >> 8); init[i + 6] = (uint8_t)v;
        }
    }
}

static std::vector<uint8_t> runRemux(const std::vector<std::pair<std::vector<uint8_t>, std::vector<std::vector<uint8_t>>>>& comps, Atsc3Remux& rx) {
    std::vector<uint8_t> ts;
    for (size_t i = 0; i < comps.size(); i++) rx.addComponent((int)i + 1);
    std::thread reader([&] {
        uint8_t buf[8192];
        for (;;) { int n = rx.read(buf, sizeof buf); if (n <= 0) break; ts.insert(ts.end(), buf, buf + n); }
    });
    for (size_t i = 0; i < comps.size(); i++) rx.push((int)i + 1, comps[i].first);
    size_t n = 0;
    for (auto& c : comps) n = std::max(n, c.second.size());
    for (size_t i = 0; i < n; i++) {
        for (size_t k = 0; k < comps.size(); k++) if (i < comps[k].second.size()) rx.push((int)k + 1, comps[k].second[i]);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    for (int i = 0; i < 100; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); size_t a = ts.size(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); if (a == ts.size() && a > 0) break; }
    rx.stop();
    reader.join();
    return ts;
}

int main(int argc, char** argv) {
    const std::string libDir = argc > 1 ? argv[1] : "";

    // ---- signalling ----
    CHECK(audioCodecFromCodecs("ac-4.02.01.01") == AudioCodecKind::Ac4, "ac-4 codecs string");
    CHECK(audioCodecFromCodecs("hvc1.2.4.L120.B0, ac-4.02.01.00") == AudioCodecKind::Ac4, "AC-4 after a video codec");
    CHECK(audioCodecFromCodecs("mp4a.40.2") == AudioCodecKind::Aac && audioCodecFromCodecs("ec-3") == AudioCodecKind::Eac3 && audioCodecFromCodecs("ac-3") == AudioCodecKind::Ac3, "AAC, E-AC-3, AC-3 codecs strings");
    CHECK(audioCodecFromCodecs("mhm1.0x0D") == AudioCodecKind::MpegH && audioCodecFromCodecs("hev1.1.6.L93.B0") == AudioCodecKind::Unknown, "MPEG-H, and a video codec is no audio");
    { Ac4Codecs c = parseAc4Codecs("ac-4.02.01.01"); CHECK(c.valid && c.bitstreamVersion == 2 && c.presentationVersion == 1 && c.mdcompat == 1, "ac-4.02.01.01 fields"); }
    { Ac4Codecs c = parseAc4Codecs("ac-3"); CHECK(!c.valid, "ac-3 is not ac-4"); }
    const std::string mpd =
        "<MPD><Period><AdaptationSet contentType=\"video\"><Representation id=\"v\" codecs=\"hev1.2.4.L120.B0\"/></AdaptationSet>"
        "<AdaptationSet contentType=\"audio\" lang=\"en\"><Representation id=\"a1\" codecs='ac-4.02.01.01'/></AdaptationSet>"
        "<AdaptationSet contentType=\"audio\" lang=\"es\"><Representation id=\"a2\" codecs=\"mp4a.40.2\"/></AdaptationSet></Period></MPD>";
    { auto a = mpdAc4Codecs(mpd); CHECK(a.size() == 1 && a[0] == "ac-4.02.01.01", "AC-4 in an MPD"); }
    { auto k = mpdAudioKinds(mpd); CHECK(k.size() == 2 && k[0] == AudioCodecKind::Ac4 && k[1] == AudioCodecKind::Aac, "audio kinds of an MPD"); }
    CHECK(mpdAc4Codecs("<MPD/>").empty(), "no AC-4 in an empty MPD");
    { const uint8_t d[3] = {0x20 | 0x00, 0x00, 0x00}; (void)d; }
    {
        const uint32_t v = (1u << 21) | (2u << 14) | (1u << 13) | (3u << 9) | 2u;
        const uint8_t b[3] = {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        Ac4Dsi d = parseDac4(b, 3);
        CHECK(d.valid && d.dsiVersion == 1 && d.bitstreamVersion == 2 && d.fsIndex == 1 && d.frameRateIndex == 3 && d.presentations == 2 && d.sampleRate() == 48000, "dac4 fields");
        CHECK(!parseDac4(b, 2).valid, "short dac4");
    }
    CHECK(!findAc4InInit(nullptr, 0), "no init segment");
    { const uint8_t junk[16] = {0, 0, 0, 200, 'm', 'o', 'o', 'v'}; CHECK(!findAc4InInit(junk, sizeof junk), "a damaged box is not a crash"); }

    // ---- the real AAC init segment from the repository is not AC-4 ----
    {
        auto af = slurp("tests/data/route_audio.mp4");
        std::vector<uint8_t> init; std::vector<std::vector<uint8_t>> segs;
        split(af, init, segs);
        CHECK(!init.empty() && !findAc4InInit(init.data(), init.size()), "the AAC init segment has no AC-4");
    }

    // ---- the loader without a library ----
    {
        const fs::path empty = fs::temp_directory_path() / "dect2_ac4_empty";
        fs::create_directories(empty);
        ac4SetLibraryDir(empty.string());
        Ac4LibInfo li = ac4Library();
        CHECK(!li.found && !li.hasDecoder && !li.message.empty(), "an empty folder: not found, with a message");
        Ac4Decoder dec;
        CHECK(!dec.open() && !dec.error().empty() && !dec.isOpen(), "the decoder does not open and says why");
        std::vector<float> pcm; int ch = 0, rate = 0;
        const uint8_t junk[16] = {0};
        CHECK(dec.decode(junk, 16, pcm, ch, rate) < 0, "decoding with no library fails cleanly");
        // broken files with the right names
        const fs::path bad = fs::temp_directory_path() / "dect2_ac4_bad";
        fs::create_directories(bad);
#if defined(_WIN32)
        const char* n1 = "avcodec-99.dll"; const char* n2 = "avutil-99.dll";
#elif defined(__APPLE__)
        const char* n1 = "libavcodec.99.dylib"; const char* n2 = "libavutil.99.dylib";
#else
        const char* n1 = "libavcodec.so.99"; const char* n2 = "libavutil.so.99";
#endif
        { std::ofstream(bad / n1) << "this is not a library"; std::ofstream(bad / n2) << "nor is this"; }
        ac4SetLibraryDir(bad.string());
        li = ac4Library(true);
        CHECK(!li.found && !li.message.empty(), "files that are no libraries: not found, no crash");
        fs::remove_all(empty); fs::remove_all(bad);
    }

    // ---- remux without the library: the AC-4 sound is left out, the picture stays, and it is said ----
    std::vector<uint8_t> ac4mp4;
    std::vector<std::vector<uint8_t>> ac3frames;
    const bool made = makeAc3Fmp4(ac4mp4, ac3frames);
    CHECK(made, "an AC-3 test stream could be made");
    if (!made) { printf("%d failure(s)\n", fails); return 1; }
    std::vector<uint8_t> ainit; std::vector<std::vector<uint8_t>> aseg;
    split(ac4mp4, ainit, aseg);
    relabel(ainit);
    printf("  test stream: %zu AC-3 frames, init %zu bytes + %zu fragments\n", ac3frames.size(), ainit.size(), aseg.size());
    CHECK(!ainit.empty() && aseg.size() >= 3, "the test stream is fragmented");
    {
        Ac4Dsi dsi; int ch = 0;
        CHECK(findAc4InInit(ainit.data(), ainit.size(), &dsi, &ch), "the 'ac-4' sample entry is found");
        CHECK(ch == 2 && dsi.valid && dsi.sampleRate() == 48000 && dsi.presentations == 2, "its channel count and dac4");
    }
    {
        const fs::path empty = fs::temp_directory_path() / "dect2_ac4_empty2";
        fs::create_directories(empty);
        ac4SetLibraryDir(empty.string());
        ac4StatusReset();
        auto vf = slurp("tests/data/route_video.mp4");
        std::vector<uint8_t> vinit; std::vector<std::vector<uint8_t>> vseg;
        split(vf, vinit, vseg);
        Atsc3Remux rx;
        auto ts = runRemux({{vinit, vseg}, {ainit, aseg}}, rx);
        auto dr = rx.droppedCodecs();
        CHECK(dr.size() == 1 && dr[0] == "AC-4", "AC-4 is listed as dropped");
        CHECK(rx.error().empty() && rx.streamCount() == 1 && ts.size() > 188 * 20, "the picture is still delivered, alone");
        Ac4Status s = ac4Status();
        CHECK(s.seen && s.dropped && !s.hasDecoder && s.detail.find("AC-4") != std::string::npos, "the status says AC-4 was seen and dropped");
        printf("  no library: %s | %s\n", dr.empty() ? "-" : dr[0].c_str(), s.detail.c_str());
        // audio only: it fails with an explanation
        Atsc3Remux rx2;
        rx2.setStartupWaitMs(300);
        runRemux({{ainit, aseg}}, rx2);
        CHECK(rx2.error().find("AC-4") != std::string::npos, "audio only: the error names AC-4");
        fs::remove_all(empty);
    }

    // ---- with a real libavcodec that has no 'ac4' decoder ----
    const bool haveLibs = !libDir.empty() && fs::is_directory(libDir);
    if (!haveLibs) printf("SKIP: no FFmpeg library folder given: the loader and borrowed-decoder tests are not run\n");
    else {
        ac4SetDecoderName("ac4");
        ac4SetLibraryDir(libDir);
        Ac4LibInfo li = ac4Library(true);
        if (!li.found) { printf("SKIP: no loadable libavcodec/libavutil pair in %s (%s)\n", libDir.c_str(), li.message.c_str()); }
        else {
            printf("  library: %s (avcodec %d.%d.%d)\n", li.path.c_str(), li.avcodecVersion >> 16, (li.avcodecVersion >> 8) & 255, li.avcodecVersion & 255);
            CHECK(li.found && !li.hasDecoder && li.message.find("no \"ac4\" decoder") != std::string::npos, "a stock FFmpeg: found, no AC-4 decoder, and it says so");
            Ac4Decoder d0;
            CHECK(!d0.open(), "so the decoder does not open");

            // ---- the whole path with the AC-3 decoder borrowed from that library ----
            ac4SetDecoderName("ac3");
            li = ac4Library(true);
            CHECK(li.found && li.hasDecoder, "the borrowed decoder is found");
            Ac4Decoder dec;
            CHECK(dec.open(), "it opens");
            // decode the AC-3 frames directly: a 1 kHz tone, 2 channels, 48 kHz
            std::vector<float> all;
            int ch = 0, rate = 0;
            for (auto& f : ac3frames) { std::vector<float> pcm; int n = dec.decode(f.data(), f.size(), pcm, ch, rate); if (n > 0) all.insert(all.end(), pcm.begin(), pcm.end()); }
            CHECK(ch == 2 && rate == 48000, "decoded as 2 channels at 48 kHz (channel layout and rate through AVOptions)");
            CHECK(all.size() >= 2 * 1536 * (ac3frames.size() - 3), "nearly all samples came out");
            auto goertzel = [&](double hz) {
                double re = 0, im = 0;
                size_t n = all.size() / 2;
                for (size_t i = n / 4; i < n * 3 / 4; i++) { double a = 2 * M_PI * hz * (double)i / 48000.0; re += all[2 * i] * cos(a); im += all[2 * i] * sin(a); }
                return sqrt(re * re + im * im) / (double)(n / 2) * 2;
            };
            const double g1 = goertzel(1000), g3 = goertzel(3000);
            printf("  tone through the loaded library: 1 kHz amplitude %.3f, 3 kHz %.4f\n", g1, g3);
            CHECK(g1 > 0.35 && g1 < 0.65 && g3 < 0.02, "the 1 kHz tone is intact");
            std::vector<float> junkpcm;
            const uint8_t junk[64] = {1, 2, 3};
            CHECK(dec.decode(junk, 64, junkpcm, ch, rate) <= 0, "garbage does not crash and gives no sound");

            // the remux: AC-3 frames under an 'ac-4' label -> decoded by the loaded library -> encoded as AC-3 in the transport stream
            ac4StatusReset();
            Atsc3Remux rx;
            auto ts = runRemux({{ainit, aseg}}, rx);
            printf("  remux: %zu bytes of transport stream, %d stream(s), error '%s'\n", ts.size(), rx.streamCount(), rx.error().c_str());
            CHECK(rx.error().empty() && rx.streamCount() == 1 && rx.droppedCodecs().empty(), "one stream, nothing dropped");
            CHECK(ts.size() > 188 * 20 && ts.size() % 188 == 0 && ts[0] == 0x47, "transport stream packets");
            MemIn mi{ts.data(), ts.size()};
            unsigned char* iobuf = (unsigned char*)av_malloc(1 << 16);
            AVIOContext* io = avio_alloc_context(iobuf, 1 << 16, 0, &mi, memRead, nullptr, nullptr);
            AVFormatContext* ic = avformat_alloc_context();
            ic->pb = io;
            CHECK(avformat_open_input(&ic, "", av_find_input_format("mpegts"), nullptr) >= 0 && avformat_find_stream_info(ic, nullptr) >= 0, "the transport stream reads back");
            int aud = -1;
            for (unsigned i = 0; ic && i < ic->nb_streams; i++) if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) aud = (int)i;
            CHECK(aud >= 0 && ic->streams[aud]->codecpar->codec_id == AV_CODEC_ID_AC3 && ic->streams[aud]->codecpar->sample_rate == 48000 && ic->streams[aud]->codecpar->ch_layout.nb_channels == 2, "AC-3, 48 kHz, stereo");
            long frames = 0;
            int64_t first = -1, last = -1;
            std::vector<float> back;
            if (aud >= 0) {
                const AVCodec* ad = avcodec_find_decoder(AV_CODEC_ID_AC3);
                AVCodecContext* ac = avcodec_alloc_context3(ad);
                avcodec_parameters_to_context(ac, ic->streams[aud]->codecpar);
                avcodec_open2(ac, ad, nullptr);
                AVPacket* pk = av_packet_alloc();
                AVFrame* fr = av_frame_alloc();
                while (av_read_frame(ic, pk) >= 0) {
                    if (pk->stream_index == aud) {
                        frames++;
                        int64_t t = av_rescale_q(pk->pts, ic->streams[aud]->time_base, AVRational{1, 1000});
                        if (first < 0) first = t;
                        last = t;
                        if (avcodec_send_packet(ac, pk) >= 0)
                            while (avcodec_receive_frame(ac, fr) >= 0) { for (int i = 0; i < fr->nb_samples; i++) back.push_back(((float*)fr->extended_data[0])[i]); av_frame_unref(fr); }
                    }
                    av_packet_unref(pk);
                }
                av_frame_free(&fr); av_packet_free(&pk); avcodec_free_context(&ac);
            }
            printf("  AC-3 frames out: %ld (in: %zu), %lld..%lld ms, %zu samples decoded\n", frames, ac3frames.size(), (long long)first, (long long)last, back.size());
            CHECK(frames >= (long)ac3frames.size() - 4 && frames <= (long)ac3frames.size() + 1, "about as many AC-3 frames out as in");
            const double span = (double)(last - first);
            CHECK(span > 0.8 * (double)(ac3frames.size() - 1) * 32.0 && span < 1.2 * (double)(ac3frames.size() - 1) * 32.0, "the time line runs at real time (32 ms per frame)");
            {
                double re = 0, im = 0, re3 = 0, im3 = 0;
                size_t n = back.size();
                for (size_t i = n / 4; i < n * 3 / 4; i++) {
                    double a = 2 * M_PI * 1000.0 * (double)i / 48000.0, b = 2 * M_PI * 3000.0 * (double)i / 48000.0;
                    re += back[i] * cos(a); im += back[i] * sin(a); re3 += back[i] * cos(b); im3 += back[i] * sin(b);
                }
                const double m = (double)(n / 2);
                const double a1 = sqrt(re * re + im * im) / m * 2, a3 = sqrt(re3 * re3 + im3 * im3) / m * 2;
                printf("  tone after decode, re-encode and the transport stream: 1 kHz %.3f, 3 kHz %.4f\n", a1, a3);
                CHECK(a1 > 0.3 && a1 < 0.7 && a3 < 0.03, "the 1 kHz tone survives the whole path");
            }
            avformat_close_input(&ic);
            Ac4Status st = ac4Status();
            printf("  status: seen %d, access units %llu, decoded %llu, failed %llu, %d ch %d Hz\n", st.seen, (unsigned long long)st.accessUnits, (unsigned long long)st.decoded, (unsigned long long)st.failed, st.channels, st.sampleRate);
            CHECK(st.seen && !st.dropped && st.hasDecoder && st.decoded > 0 && st.channels == 2 && st.sampleRate == 48000, "the status counts the decoded frames");
        }
    }
    ac4SetDecoderName("ac4");
    ac4SetLibraryDir("");
    printf(fails ? "%d failure(s)\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
