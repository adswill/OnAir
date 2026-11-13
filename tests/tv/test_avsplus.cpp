// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AVS+ add-on plumbing (core/src/tv/avsplus_dec.cpp). No AVS+ stream and no AVS+ decoder exist here, so what is checked is everything around the
// decoder:
//   - the loader: a folder without libraries, with broken files, with a FFmpeg that has no such decoder: all fail with a plain message
//   - a BORROWED decoder: avsPlusSetDecoderName("mpeg1video") makes the loader use the MPEG-1 decoder of a stock FFmpeg (the folder of the first
//     argument) as a stand-in for the add-on; a few synthetic pictures are encoded, sent as packets and must come back with their times in order
//   - hot pickup: a library folder that is empty and then gets the libraries (symlinks) is picked up by avsPlusPoll without a restart
//   - the messages for the panel
// It does NOT prove that any AVS+ bitstream decodes: that needs the patched FFmpeg and a real stream.
//   test_avsplus [dir-with-a-stock-libavcodec-and-libavutil]
#include "dect2/tv/avsplus_dec.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

using namespace dect2;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Pkt { std::vector<uint8_t> d; int64_t pts; bool key; };

static std::vector<Pkt> encodeSynthetic(int n, int w, int h) {
    std::vector<Pkt> out;
    const AVCodec* c = avcodec_find_encoder(AV_CODEC_ID_MPEG1VIDEO);
    if (!c) return out;
    AVCodecContext* e = avcodec_alloc_context3(c);
    e->width = w; e->height = h; e->time_base = AVRational{1, 25}; e->framerate = AVRational{25, 1};
    e->pix_fmt = AV_PIX_FMT_YUV420P; e->gop_size = 3; e->max_b_frames = 0; e->bit_rate = 400000;
    if (avcodec_open2(e, c, nullptr) < 0) { avcodec_free_context(&e); return out; }
    AVFrame* f = av_frame_alloc();
    f->format = e->pix_fmt; f->width = w; f->height = h;
    av_frame_get_buffer(f, 0);
    AVPacket* p = av_packet_alloc();
    for (int i = 0; i <= n; i++) {
        if (i < n) {
            av_frame_make_writable(f);
            for (int y = 0; y < h; y++) memset(f->data[0] + (size_t)y * f->linesize[0], (y * 3 + i * 9) & 255, w);
            for (int y = 0; y < h / 2; y++) { memset(f->data[1] + (size_t)y * f->linesize[1], 100 + i, w / 2); memset(f->data[2] + (size_t)y * f->linesize[2], 150 - i, w / 2); }
            f->pts = i;
        }
        if (avcodec_send_frame(e, i < n ? f : nullptr) < 0) break;
        while (avcodec_receive_packet(e, p) >= 0) {
            out.push_back({std::vector<uint8_t>(p->data, p->data + p->size), 90000LL * 1000 + p->pts * 3600, (p->flags & AV_PKT_FLAG_KEY) != 0});
            av_packet_unref(p);
        }
    }
    av_packet_free(&p); av_frame_free(&f); avcodec_free_context(&e);
    return out;
}

int main(int argc, char** argv) {
    const fs::path tmp = fs::temp_directory_path() / "avsplus_test_dir";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    // ---- messages and the version check
    CHECK(avsPlusAbiOk(60u << 16) && avsPlusAbiOk(63u << 16 | 5) && !avsPlusAbiOk(57u << 16) && !avsPlusAbiOk(70u << 16), "ABI check");
    {
        AvsPlusLibInfo none, found, ok;
        found.found = true; found.message = "boom";
        ok.hasDecoder = ok.found = true;
        CHECK(avsPlusNote(none).find("install the AVS+ add-on") != std::string::npos, "missing note: %s", avsPlusNote(none).c_str());
        CHECK(avsPlusNote(found).find("boom") != std::string::npos, "unusable note: %s", avsPlusNote(found).c_str());
        CHECK(avsPlusNote(ok).empty(), "no note when the add-on works");
    }

    // ---- an empty folder, and a folder with junk that only looks like libraries
    avsPlusSetDecoderName("");
    avsPlusSetLibraryDir(tmp.string());
    {
        const AvsPlusLibInfo li = avsPlusLibrary(true);
        CHECK(!li.found && !li.hasDecoder && !li.message.empty(), "empty folder: found=%d '%s'", li.found, li.message.c_str());
        CHECK(!avsPlusAvailable(), "available from an empty folder");
        AvsPlusDecoder d;
        CHECK(!d.open() && !d.error().empty(), "open without an add-on");
        std::ofstream((tmp / "libavcodec.so.99").string()) << "not a library";
        std::ofstream((tmp / "libavutil.so.99").string()) << "not a library";
        std::ofstream((tmp / "libavcodec.99.dylib").string()) << "not a library";
        std::ofstream((tmp / "libavutil.99.dylib").string()) << "not a library";
        const AvsPlusLibInfo lj = avsPlusLibrary(true);
        CHECK(!lj.found && !lj.hasDecoder, "junk files counted as a library");
        fs::remove_all(tmp, ec); fs::create_directories(tmp, ec);
    }

    if (argc < 2 || !fs::exists(argv[1])) { printf("no stock FFmpeg folder given: loader tests with real libraries skipped\n"); printf(fails ? "FAILED\n" : "OK\n"); return fails ? 1 : 0; }
    const fs::path stock = argv[1];

    // ---- a real FFmpeg without the decoder: found, but no decoder, and a message that says so
    avsPlusSetDecoderName("definitely_not_a_decoder");
    avsPlusSetLibraryDir(stock.string());
    {
        const AvsPlusLibInfo li = avsPlusLibrary(true);
        CHECK(li.found && !li.hasDecoder && li.message.find("definitely_not_a_decoder") != std::string::npos, "no-decoder case: found=%d has=%d '%s'", li.found, li.hasDecoder, li.message.c_str());
        CHECK(avsPlusNote(li).find("add-on cannot be used") != std::string::npos, "note for an unusable add-on: %s", avsPlusNote(li).c_str());
    }

    // ---- the borrowed decoder: the whole call path
    avsPlusSetDecoderName("mpeg1video");
    avsPlusSetLibraryDir(stock.string());
    {
        const AvsPlusLibInfo li = avsPlusLibrary(true);
        CHECK(li.found && li.hasDecoder && avsPlusAbiOk((unsigned)li.avcodecVersion), "stand-in decoder: found=%d has=%d '%s'", li.found, li.hasDecoder, li.message.c_str());
        const std::vector<Pkt> pk = encodeSynthetic(8, 64, 48);
        CHECK(pk.size() >= 7, "encoded %zu packets", pk.size());
        AvsPlusDecoder d;
        CHECK(d.open(), "open: %s", d.error().c_str());
        int got = 0;
        int64_t last = INT64_MIN;
        bool ordered = true, sized = true, firstKey = false;
        for (const Pkt& p : pk) {
            CHECK(d.send(p.d.data(), p.d.size(), p.pts, p.key), "send: %s", d.error().c_str());
            AvsPicture pic;
            while (d.receive(pic)) {
                if (got == 0) firstKey = pic.key;
                got++;
                if (pic.w != 64 || pic.h != 48 || (pic.format != AV_PIX_FMT_YUV420P && pic.format != AV_PIX_FMT_YUVJ420P) || !pic.data[0] || !pic.data[1] || !pic.data[2] || pic.linesize[0] < 64) sized = false;
                if (pic.pts <= last) ordered = false;
                last = pic.pts;
            }
        }
        CHECK(got >= (int)pk.size() - 2, "pictures out %d of %zu", got, pk.size());
        CHECK(sized, "picture size / format / planes");
        CHECK(ordered, "picture times not increasing");
        CHECK(firstKey, "the first picture was not marked as a key picture");
        d.flush();
        CHECK(!d.send(nullptr, 0, 0, false), "empty packet accepted");
    }

    // ---- hot pickup: the folder is empty when the player looks, the libraries arrive later
    {
        const fs::path hot = tmp / "hot";
        fs::create_directories(hot, ec);
        avsPlusSetDecoderName("mpeg1video");
        avsPlusSetLibraryDir(hot.string());
        CHECK(!avsPlusPoll(0), "poll found a decoder in an empty folder");
        CHECK(!avsPlusPoll(3600), "a poll inside the interval looked again");
        int linked = 0;
        for (auto& e : fs::directory_iterator(stock, ec)) {
            const std::string n = e.path().filename().string();
            if (n.rfind("libav", 0) == 0 || n.rfind("libsw", 0) == 0 || n.rfind("avcodec", 0) == 0 || n.rfind("avutil", 0) == 0) { fs::create_symlink(fs::absolute(e.path()), hot / n, ec); if (!ec) linked++; }
        }
        CHECK(linked > 0, "nothing to link from %s", stock.string().c_str());
        CHECK(!avsPlusPoll(3600), "the folder was not looked at again before the interval ran out");
        CHECK(avsPlusPoll(0), "the new libraries were not picked up: '%s'", avsPlusLibrary().message.c_str());
        AvsPlusDecoder d;
        CHECK(d.open(), "open after pickup: %s", d.error().c_str());
    }

    avsPlusSetLibraryDir("");
    avsPlusSetDecoderName("");
    fs::remove_all(tmp, ec);
    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
