// Display aspect ratio: the pure computation, and an MPEG-2 clip with a sample aspect ratio decoded by FFmpeg the way the player does.
#include "dect2/player.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}
#include <cmath>
#include <cstdio>
#include <string>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static bool near(double a, double b) { return std::fabs(a - b) < 1e-3; }

// encodes 8 pictures of w x h with the given SAR into an MPEG-TS file
static bool makeClip(const std::string& path, int w, int h, AVRational sar) {
    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, "mpegts", path.c_str()) < 0) return false;
    const AVCodec* vc = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!vc) return false;
    AVCodecContext* vx = avcodec_alloc_context3(vc);
    vx->width = w; vx->height = h; vx->pix_fmt = AV_PIX_FMT_YUV420P; vx->sample_aspect_ratio = sar;
    vx->time_base = AVRational{1, 25}; vx->framerate = AVRational{25, 1}; vx->bit_rate = 800000; vx->gop_size = 4; vx->max_b_frames = 0;
    vx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(vx, vc, nullptr) < 0) return false;
    AVStream* st = avformat_new_stream(oc, nullptr);
    avcodec_parameters_from_context(st->codecpar, vx); st->time_base = vx->time_base;
    if (avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE) < 0 || avformat_write_header(oc, nullptr) < 0) return false;
    AVFrame* f = av_frame_alloc(); f->format = vx->pix_fmt; f->width = w; f->height = h; av_frame_get_buffer(f, 0);
    AVPacket* pk = av_packet_alloc();
    auto drain = [&] { while (avcodec_receive_packet(vx, pk) == 0) { av_packet_rescale_ts(pk, vx->time_base, st->time_base); pk->stream_index = 0; av_interleaved_write_frame(oc, pk); av_packet_unref(pk); } };
    for (int i = 0; i < 8; i++) {
        av_frame_make_writable(f);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) f->data[0][y * f->linesize[0] + x] = (uint8_t)(x + y + i * 9);
        for (int y = 0; y < h / 2; y++) for (int x = 0; x < w / 2; x++) { f->data[1][y * f->linesize[1] + x] = 128; f->data[2][y * f->linesize[2] + x] = 128; }
        f->pts = i; avcodec_send_frame(vx, f); drain();
    }
    avcodec_send_frame(vx, nullptr); drain();
    av_write_trailer(oc); avio_closep(&oc->pb);
    av_packet_free(&pk); av_frame_free(&f); avcodec_free_context(&vx); avformat_free_context(oc);
    return true;
}

// the first decoded picture's display aspect, computed as the player does
static double decodedAspect(const std::string& path, int& w, int& h) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return -1;
    avformat_find_stream_info(fmt, nullptr);
    const int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const AVCodec* c = avcodec_find_decoder(fmt->streams[vi]->codecpar->codec_id);
    AVCodecContext* cx = avcodec_alloc_context3(c);
    avcodec_parameters_to_context(cx, fmt->streams[vi]->codecpar); avcodec_open2(cx, c, nullptr);
    AVPacket* pk = av_packet_alloc(); AVFrame* f = av_frame_alloc();
    double r = -1;
    while (r < 0 && av_read_frame(fmt, pk) >= 0) {
        if (pk->stream_index == vi && avcodec_send_packet(cx, pk) == 0 && avcodec_receive_frame(cx, f) == 0) {
            AVRational sar = av_guess_sample_aspect_ratio(fmt, fmt->streams[vi], f);
            if (sar.num <= 0 || sar.den <= 0) sar = f->sample_aspect_ratio;
            w = f->width; h = f->height;
            r = dect2::displayAspect(f->width, f->height, sar.num, sar.den);
        }
        av_packet_unref(pk);
    }
    av_packet_free(&pk); av_frame_free(&f); avcodec_free_context(&cx); avformat_close_input(&fmt);
    return r;
}

int main() {
    using dect2::displayAspect;
    CHECK(near(displayAspect(720, 576, 64, 45), 16.0 / 9));       // PAL 16:9 anamorphic
    CHECK(near(displayAspect(720, 576, 16, 15), 4.0 / 3));        // PAL 4:3
    CHECK(near(displayAspect(1440, 1080, 4, 3), 16.0 / 9));       // HD anamorphic
    CHECK(near(displayAspect(1920, 1080, 1, 1), 16.0 / 9));
    CHECK(near(displayAspect(1920, 1080, 0, 0), 16.0 / 9));       // unknown: square pixels
    CHECK(near(displayAspect(720, 576, 0, 1), 720.0 / 576));
    CHECK(near(displayAspect(720, 576, -3, 4), 720.0 / 576));
    CHECK(near(displayAspect(720, 576, 1, 1000), 720.0 / 576));   // absurd SAR ignored
    CHECK(near(displayAspect(1920, 1088, 1, 1), 1920.0 / 1088));  // coded height: callers pass the cropped size
    CHECK(near(displayAspect(0, 0, 4, 3), 1.0));
    int w = 0, h = 0;
    const std::string p1 = "/tmp/dect2_aspect_a.ts", p2 = "/tmp/dect2_aspect_b.ts";
    if (makeClip(p1, 720, 576, AVRational{64, 45})) {
        const double a = decodedAspect(p1, w, h);
        CHECK(w == 720 && h == 576); CHECK(std::fabs(a - 16.0 / 9) < 0.02);
        printf("clip 720x576 SAR 64:45 -> %.4f\n", a);
    } else { printf("FAIL clip a\n"); fails++; }
    if (makeClip(p2, 720, 576, AVRational{16, 15})) {
        const double a = decodedAspect(p2, w, h);
        CHECK(std::fabs(a - 4.0 / 3) < 0.02);
        printf("clip 720x576 SAR 16:15 -> %.4f\n", a);
    } else { printf("FAIL clip b\n"); fails++; }
    remove(p1.c_str()); remove(p2.c_str());
    printf(fails ? "aspect: %d FAILED\n" : "aspect: ok\n", fails);
    return fails ? 1 : 0;
}
