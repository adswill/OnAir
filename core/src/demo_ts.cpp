#include "dect2/demo_ts.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace dect2 {

namespace {

constexpr int kW = 640, kH = 360, kFps = 25, kSeconds = 20, kRate = 48000;

struct Clip {
    std::vector<uint8_t> ts;   // whole packets
    double seconds = 0;
};

// One test card picture: colour bars on top, a sweeping ramp, a bouncing ball and a moving checkerboard. Writes planar YUV 4:2:0.
void drawCard(AVFrame* f, int n) {
    static const uint8_t bars[8][3] = {{235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34}, {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}};
    const double t = (double)n / kFps;
    const int bx = (int)((0.5 + 0.42 * std::sin(t * 1.3)) * kW), by = (int)(kH * 0.62 + std::sin(t * 2.9) * kH * 0.17);
    for (int y = 0; y < kH; y++) {
        for (int x = 0; x < kW; x++) {
            int Y, U, V;
            if (y < kH * 0.38) { const auto& b = bars[x * 8 / kW]; Y = b[0]; U = b[1]; V = b[2]; }
            else if (y < kH * 0.44) { Y = 16 + ((x * 3 + n * 6) % kW) * 219 / kW; U = V = 128; }
            else {
                const bool sq = (((x + n * 2) / 40) + (y / 40)) & 1;
                Y = sq ? 70 : 52; U = sq ? 134 : 128; V = sq ? 122 : 128;
                const int dx = x - bx, dy = y - by;
                if (dx * dx + dy * dy < 46 * 46) { Y = 210; U = 90; V = 170; }
                if (dx * dx + dy * dy < 14 * 14) { Y = 235; U = 128; V = 128; }
            }
            f->data[0][y * f->linesize[0] + x] = (uint8_t)Y;
            if (!(x & 1) && !(y & 1)) { f->data[1][(y / 2) * f->linesize[1] + x / 2] = (uint8_t)U; f->data[2][(y / 2) * f->linesize[2] + x / 2] = (uint8_t)V; }
        }
    }
    // a progress bar for the second, so that frozen pictures are obvious
    const int w = (n % kFps) * (kW - 40) / kFps;
    for (int y = kH - 22; y < kH - 12; y++) for (int x = 20; x < 20 + w; x++) f->data[0][y * f->linesize[0] + x] = 235;
}

bool encode(Clip& clip) {
    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, "mpegts", nullptr) < 0 || !oc) return false;
    if (avio_open_dyn_buf(&oc->pb) < 0) { avformat_free_context(oc); return false; }
    const AVCodec* vc = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    const AVCodec* ac = avcodec_find_encoder(AV_CODEC_ID_MP2);
    if (!vc || !ac) { uint8_t* b; avio_close_dyn_buf(oc->pb, &b); av_free(b); avformat_free_context(oc); return false; }

    AVCodecContext* vx = avcodec_alloc_context3(vc);
    vx->width = kW; vx->height = kH; vx->pix_fmt = AV_PIX_FMT_YUV420P;
    vx->time_base = AVRational{1, kFps}; vx->framerate = AVRational{kFps, 1};
    vx->bit_rate = 1400000; vx->rc_max_rate = 1600000; vx->rc_buffer_size = 1835008; vx->gop_size = 12; vx->max_b_frames = 0;
    vx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    // the MP2 encoder takes 16-bit integer or float samples depending on the FFmpeg version: try them in turn
    AVCodecContext* ax = nullptr;
    bool aOpen = false;
    for (AVSampleFormat sf : {AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_FLTP}) {
        avcodec_free_context(&ax);
        ax = avcodec_alloc_context3(ac);
        ax->sample_rate = kRate; ax->bit_rate = 128000; ax->time_base = AVRational{1, kRate};
        av_channel_layout_default(&ax->ch_layout, 2);
        ax->sample_fmt = sf;
        if (avcodec_open2(ax, ac, nullptr) >= 0) { aOpen = true; break; }
    }
    bool ok = aOpen && avcodec_open2(vx, vc, nullptr) >= 0;

    AVStream* vs = ok ? avformat_new_stream(oc, nullptr) : nullptr;
    AVStream* as = ok ? avformat_new_stream(oc, nullptr) : nullptr;
    if (vs && as) {
        avcodec_parameters_from_context(vs->codecpar, vx); vs->time_base = vx->time_base;
        avcodec_parameters_from_context(as->codecpar, ax); as->time_base = ax->time_base;
        av_dict_set(&oc->metadata, "service_name", "OnAir demo", 0);
        av_dict_set(&oc->metadata, "service_provider", "OnAir", 0);
        AVDictionary* opt = nullptr;
        av_dict_set(&opt, "pat_period", "0.2", 0); av_dict_set(&opt, "sdt_period", "0.5", 0);
        ok = avformat_write_header(oc, &opt) >= 0;
        av_dict_free(&opt);
    } else ok = false;

    if (ok) {
        AVFrame* vf = av_frame_alloc();
        vf->format = vx->pix_fmt; vf->width = kW; vf->height = kH;
        av_frame_get_buffer(vf, 0);
        AVFrame* af = av_frame_alloc();
        af->format = ax->sample_fmt; af->nb_samples = ax->frame_size; av_channel_layout_copy(&af->ch_layout, &ax->ch_layout); af->sample_rate = kRate;
        av_frame_get_buffer(af, 0);
        AVPacket* pk = av_packet_alloc();
        const int frames = kFps * kSeconds;
        int64_t aPts = 0;
        auto drain = [&](AVCodecContext* cx, AVStream* st) {
            while (avcodec_receive_packet(cx, pk) >= 0) { av_packet_rescale_ts(pk, cx->time_base, st->time_base); pk->stream_index = st->index; av_interleaved_write_frame(oc, pk); }
        };
        for (int n = 0; n < frames; n++) {
            av_frame_make_writable(vf);
            drawCard(vf, n);
            vf->pts = n;
            avcodec_send_frame(vx, vf); drain(vx, vs);
            // audio up to the same time: a short 440 Hz beep at the start of every second
            while ((double)aPts / kRate < (double)(n + 1) / kFps) {
                av_frame_make_writable(af);
                for (int i = 0; i < af->nb_samples; i++) {
                    const double ts = (double)(aPts + i) / kRate;
                    const double env = std::fmod(ts, 1.0) < 0.2 ? 0.25 : 0.0;
                    const double s = env * std::sin(2 * M_PI * 440.0 * ts);
                    if (ax->sample_fmt == AV_SAMPLE_FMT_S16) { int16_t* d = (int16_t*)af->data[0]; d[2 * i] = d[2 * i + 1] = (int16_t)(s * 32767); }
                    else if (ax->sample_fmt == AV_SAMPLE_FMT_S16P) { ((int16_t*)af->data[0])[i] = ((int16_t*)af->data[1])[i] = (int16_t)(s * 32767); }
                    else if (ax->sample_fmt == AV_SAMPLE_FMT_FLTP) { ((float*)af->data[0])[i] = ((float*)af->data[1])[i] = (float)s; }
                    else if (ax->sample_fmt == AV_SAMPLE_FMT_FLT) { float* d = (float*)af->data[0]; d[2 * i] = d[2 * i + 1] = (float)s; }
                }
                af->pts = aPts; aPts += af->nb_samples;
                avcodec_send_frame(ax, af); drain(ax, as);
            }
        }
        avcodec_send_frame(vx, nullptr); drain(vx, vs);
        avcodec_send_frame(ax, nullptr); drain(ax, as);
        av_write_trailer(oc);
        av_packet_free(&pk); av_frame_free(&vf); av_frame_free(&af);
    }
    uint8_t* buf = nullptr;
    const int sz = avio_close_dyn_buf(oc->pb, &buf);
    oc->pb = nullptr;
    if (ok && sz > 188 * 100) { clip.ts.assign(buf, buf + sz / 188 * 188); clip.seconds = kSeconds; }
    else ok = false;
    av_free(buf);
    avcodec_free_context(&vx); avcodec_free_context(&ax);
    avformat_free_context(oc);
    return ok;
}

const Clip& clip() {
    static Clip c;
    static std::once_flag once;
    std::call_once(once, [] { encode(c); });
    return c;
}

} // namespace

std::function<void(uint8_t*)> demoTsSource(double netBitrate) {
    const Clip& c = clip();
    const size_t n = c.ts.size() / 188;
    // share of the channel's packets that carry the clip; the rest are null packets
    const double rho = n && netBitrate > 0 ? std::min(1.0, (double)n * 188 * 8 / c.seconds / netBitrate) : 0.0;
    auto pos = std::make_shared<size_t>(0);
    auto acc = std::make_shared<double>(0.5);
    return [=, &c](uint8_t* pkt) {
        *acc += rho;
        if (n && *acc >= 1.0) {
            *acc -= 1.0;
            memcpy(pkt, c.ts.data() + (*pos) * 188, 188);
            *pos = (*pos + 1) % n;
            return;
        }
        memset(pkt, 0xFF, 188);
        pkt[0] = 0x47; pkt[1] = 0x1F; pkt[2] = 0xFF; pkt[3] = 0x10;
    };
}

} // namespace dect2
