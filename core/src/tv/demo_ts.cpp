// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "dect2/tv/demo_ts.h"
#include "dect2/audio/videoin.h"
#include "dect2/common/diag.h"
#include "dect2/source/synth_media.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {

namespace {

constexpr int kW = 640, kH = 360, kFps = 25, kSeconds = 20, kRate = 48000;

struct Clip {
    std::vector<uint8_t> ts;
    double seconds = 0;
};

std::vector<DemoTsService> programmesOf(const DemoTsIdentity& id) {
    if (!id.programmes.empty()) {
        std::vector<DemoTsService> v = id.programmes;
        if (v.size() > 4) v.resize(4);
        for (size_t i = 0; i < v.size(); i++) if (v[i].name.empty()) v[i].name = "OnAir " + std::to_string(i + 1);
        return v;
    }
    std::vector<DemoTsService> v;
    if (id.services.empty()) {
        DemoTsService s; s.name = "OnAir"; s.picture = 0; v.push_back(s);
        return v;
    }
    for (size_t i = 0; i < id.services.size() && i < 4; i++) {
        DemoTsService s;
        s.name = id.services[i].empty() ? ("OnAir " + std::to_string(i + 1)) : id.services[i];
        s.picture = (int)i % 3;
        v.push_back(s);
    }
    return v;
}

void drawCard(AVFrame* f, int n, int svc, int picture, const char* name) {
    static const uint8_t bars[8][3] = {{235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34}, {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}};
    const double t = (double)n / kFps;
    const int rot = ((svc % 8) + 8) % 8;
    const int bx = (int)((0.5 + 0.42 * std::sin(t * 1.3 + svc)) * kW), by = (int)(kH * 0.62 + std::sin(t * 2.9 + svc * 0.7) * kH * 0.17);
    for (int y = 0; y < kH; y++) {
        for (int x = 0; x < kW; x++) {
            int Y, U, V;
            if (picture == 1) { const auto& b = bars[(x * 8 / kW + rot) & 7]; Y = b[0]; U = b[1]; V = b[2]; }
            else if (picture == 2) { Y = 16 + x * 219 / kW; U = V = 128; }
            else if (y < kH * 0.38) { const auto& b = bars[(x * 8 / kW + rot) & 7]; Y = b[0]; U = b[1]; V = b[2]; }
            else if (y < kH * 0.44) { Y = 16 + ((x * 3 + n * 6 + svc * 40) % kW) * 219 / kW; U = V = 128; }
            else {
                const bool sq = (((x + n * 2) / 40) + (y / 40) + svc) & 1;
                Y = sq ? 70 : 52; U = sq ? (uint8_t)(134 + svc * 8) : 128; V = sq ? (uint8_t)(122 - svc * 6) : 128;
                const int dx = x - bx, dy = y - by;
                if (dx * dx + dy * dy < 46 * 46) { Y = 210; U = 90; V = 170; }
                if (dx * dx + dy * dy < 14 * 14) { Y = 235; U = 128; V = 128; }
            }
            f->data[0][y * f->linesize[0] + x] = (uint8_t)Y;
            if (!(x & 1) && !(y & 1)) { f->data[1][(y / 2) * f->linesize[1] + x / 2] = (uint8_t)U; f->data[2][(y / 2) * f->linesize[2] + x / 2] = (uint8_t)V; }
        }
    }
    const int wbar = (n % kFps) * (kW - 40) / kFps;
    for (int y = kH - 22; y < kH - 12; y++) for (int x = 20; x < 20 + wbar; x++) f->data[0][y * f->linesize[0] + x] = 235;
    if (name && name[0]) {
        // a dark ident strip: the generator's MPEG-2 picture carries the service name as luma blocks (no font atlas here)
        for (int y = 8; y < 28; y++) for (int x = 8; x < kW - 8; x++) {
            f->data[0][y * f->linesize[0] + x] = 16;
            if (!(x & 1) && !(y & 1)) { f->data[1][(y / 2) * f->linesize[1] + x / 2] = 128; f->data[2][(y / 2) * f->linesize[2] + x / 2] = 128; }
        }
        for (int i = 0; name[i] && i < 24; i++) {
            const unsigned c = (unsigned char)name[i];
            const int x0 = 16 + i * 18;
            for (int yy = 0; yy < 14; yy++) for (int xx = 0; xx < 12; xx++) {
                const bool on = ((c + yy * 3 + xx) & 7) != 0 && yy > 1 && yy < 12 && xx > 1 && xx < 10;
                const int x = x0 + xx, y = 10 + yy;
                if (x >= 0 && x < kW) f->data[0][y * f->linesize[0] + x] = on ? 235 : 16;
            }
        }
    }
}

bool loadFileFrame(const std::string& path, AVFrame* vf, int n) {
    if (path.empty()) return false;
    AVFormatContext* ic = nullptr;
    if (avformat_open_input(&ic, path.c_str(), nullptr, nullptr) < 0) { DIAG_DEBUG(729891, "FFmpeg could not open the demo video file %s, so the demo picture is used instead.", path.c_str()); return false; }
    if (avformat_find_stream_info(ic, nullptr) < 0) { DIAG_DEBUG(611539, "FFmpeg could not read the streams of the demo video file %s, so the demo picture is used instead.", path.c_str()); avformat_close_input(&ic); return false; }
    const int si = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (si < 0) { DIAG_DEBUG(714846, "The demo video file %s has no video stream, so the demo picture is used instead.", path.c_str()); avformat_close_input(&ic); return false; }
    AVStream* st = ic->streams[si];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext* ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
    bool ok = ctx && avcodec_parameters_to_context(ctx, st->codecpar) >= 0 && avcodec_open2(ctx, codec, nullptr) >= 0;
    SwsContext* sws = nullptr;
    AVFrame* fr = av_frame_alloc();
    AVPacket* pk = av_packet_alloc();
    int got = 0;
    const int want = n % std::max(1, kFps * kSeconds);
    if (ok) {
        const int64_t ts = st->duration > 0 ? st->duration * want / (kFps * kSeconds) : 0;
        if (ts) av_seek_frame(ic, si, ts, AVSEEK_FLAG_BACKWARD);
        while (av_read_frame(ic, pk) >= 0) {
            if (pk->stream_index != si) { av_packet_unref(pk); continue; }
            if (avcodec_send_packet(ctx, pk) >= 0 && avcodec_receive_frame(ctx, fr) >= 0) {
                sws = sws_getContext(fr->width, fr->height, (AVPixelFormat)fr->format, kW, kH, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (sws) { sws_scale(sws, fr->data, fr->linesize, 0, fr->height, vf->data, vf->linesize); ok = true; got = 1; }
                av_packet_unref(pk);
                break;
            }
            av_packet_unref(pk);
        }
    }
    if (sws) sws_freeContext(sws);
    av_packet_free(&pk); av_frame_free(&fr);
    avcodec_free_context(&ctx); avformat_close_input(&ic);
    return got != 0;
}

AVCodecContext* openMp2() {
    const AVCodec* ac = avcodec_find_encoder(AV_CODEC_ID_MP2);
    if (!ac) return nullptr;
    for (AVSampleFormat sf : {AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_FLTP}) {
        AVCodecContext* ax = avcodec_alloc_context3(ac);
        ax->sample_rate = kRate; ax->bit_rate = 128000; ax->time_base = AVRational{1, kRate};
        av_channel_layout_default(&ax->ch_layout, 2);
        ax->sample_fmt = sf;
        if (avcodec_open2(ax, ac, nullptr) >= 0) return ax;
        avcodec_free_context(&ax);
    }
    return nullptr;
}

void fillBeep(AVCodecContext* ax, AVFrame* af, int64_t aPts, int svc) {
    const double f0 = 440.0 * std::pow(2.0, svc / 12.0);
    for (int i = 0; i < af->nb_samples; i++) {
        const double ts = (double)(aPts + i) / kRate;
        const double env = std::fmod(ts, 1.0) < 0.2 ? 0.25 : 0.0;
        const double s = env * std::sin(2 * M_PI * f0 * ts);
        if (ax->sample_fmt == AV_SAMPLE_FMT_S16) { int16_t* d = (int16_t*)af->data[0]; d[2 * i] = d[2 * i + 1] = (int16_t)(s * 32767); }
        else if (ax->sample_fmt == AV_SAMPLE_FMT_S16P) { ((int16_t*)af->data[0])[i] = ((int16_t*)af->data[1])[i] = (int16_t)(s * 32767); }
        else if (ax->sample_fmt == AV_SAMPLE_FMT_FLTP) { ((float*)af->data[0])[i] = ((float*)af->data[1])[i] = (float)s; }
        else { float* d = (float*)af->data[0]; d[2 * i] = d[2 * i + 1] = (float)s; }
    }
}

// the user's own sound (Synthetic content) instead of the beep: the looped PCM at 48 kHz stereo
void fillPcm(AVCodecContext* ax, AVFrame* af, SynthMediaAudio& pcm) {
    std::vector<float> st((size_t)af->nb_samples * 2);
    pcm.read(st.data(), af->nb_samples);
    for (int i = 0; i < af->nb_samples; i++) {
        const float l = std::max(-1.f, std::min(1.f, st[2 * i])), r = std::max(-1.f, std::min(1.f, st[2 * i + 1]));
        if (ax->sample_fmt == AV_SAMPLE_FMT_S16) { int16_t* d = (int16_t*)af->data[0]; d[2 * i] = (int16_t)(l * 32767); d[2 * i + 1] = (int16_t)(r * 32767); }
        else if (ax->sample_fmt == AV_SAMPLE_FMT_S16P) { ((int16_t*)af->data[0])[i] = (int16_t)(l * 32767); ((int16_t*)af->data[1])[i] = (int16_t)(r * 32767); }
        else if (ax->sample_fmt == AV_SAMPLE_FMT_FLTP) { ((float*)af->data[0])[i] = l; ((float*)af->data[1])[i] = r; }
        else { float* d = (float*)af->data[0]; d[2 * i] = l; d[2 * i + 1] = r; }
    }
}

bool encode(Clip& clip, const DemoTsIdentity& id, const SynthMedia* media = nullptr) {
    const auto progs = programmesOf(id);
    const int nsvc = (int)progs.size();
    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, "mpegts", nullptr) < 0 || !oc) { DIAG_ERROR(785331, "This FFmpeg build cannot write MPEG transport streams, so the demo transport stream is not made."); return false; }
    if (avio_open_dyn_buf(&oc->pb) < 0) { DIAG_ERROR(874202, "FFmpeg could not allocate the memory buffer for the demo transport stream, so it is not made."); avformat_free_context(oc); return false; }
    const AVCodec* vc = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!vc) { DIAG_ERROR(847513, "This FFmpeg build has no MPEG-2 video encoder, so the demo transport stream is not made."); uint8_t* b; avio_close_dyn_buf(oc->pb, &b); av_free(b); avformat_free_context(oc); return false; }

    struct ProgEnc { AVCodecContext *vx = nullptr, *ax = nullptr; AVStream *vs = nullptr, *as = nullptr; AVFrame *vf = nullptr, *af = nullptr; int64_t aPts = 0; };
    std::vector<ProgEnc> pe((size_t)nsvc);
    bool ok = true;
    double outSeconds = kSeconds;
    const std::string prov = id.provider.empty() ? "OnAir" : id.provider;
    av_dict_set(&oc->metadata, "service_provider", prov.c_str(), 0);
    av_dict_set(&oc->metadata, "service_name", progs[0].name.c_str(), 0);
    for (int s = 0; s < nsvc && ok; s++) {
        pe[s].vx = avcodec_alloc_context3(vc);
        pe[s].vx->width = kW; pe[s].vx->height = kH; pe[s].vx->pix_fmt = AV_PIX_FMT_YUV420P;
        pe[s].vx->time_base = AVRational{1, kFps}; pe[s].vx->framerate = AVRational{kFps, 1};
        pe[s].vx->bit_rate = 1200000; pe[s].vx->rc_max_rate = 1500000; pe[s].vx->rc_buffer_size = 1835008;
        pe[s].vx->gop_size = 12; pe[s].vx->max_b_frames = 0; pe[s].vx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        pe[s].ax = openMp2();
        if (!pe[s].ax || avcodec_open2(pe[s].vx, vc, nullptr) < 0) { ok = false; break; }
        pe[s].vs = avformat_new_stream(oc, nullptr);
        pe[s].as = avformat_new_stream(oc, nullptr);
        if (!pe[s].vs || !pe[s].as) { ok = false; break; }
        avcodec_parameters_from_context(pe[s].vs->codecpar, pe[s].vx); pe[s].vs->time_base = pe[s].vx->time_base;
        avcodec_parameters_from_context(pe[s].as->codecpar, pe[s].ax); pe[s].as->time_base = pe[s].ax->time_base;
        pe[s].vs->id = 0x100 + s * 0x10;   // own video PID per programme
        pe[s].as->id = 0x101 + s * 0x10;   // own audio PID
        const int sid = id.serviceId + s;
        AVProgram* pr = av_new_program(oc, sid);
        if (pr) {
            av_dict_set(&pr->metadata, "service_name", progs[s].name.c_str(), 0);
            av_dict_set(&pr->metadata, "service_provider", prov.c_str(), 0);
            av_program_add_stream_index(oc, sid, pe[s].vs->index);
            av_program_add_stream_index(oc, sid, pe[s].as->index);
        }
        pe[s].vf = av_frame_alloc();
        pe[s].vf->format = pe[s].vx->pix_fmt; pe[s].vf->width = kW; pe[s].vf->height = kH;
        av_frame_get_buffer(pe[s].vf, 0);
        pe[s].af = av_frame_alloc();
        pe[s].af->format = pe[s].ax->sample_fmt; pe[s].af->nb_samples = pe[s].ax->frame_size;
        av_channel_layout_copy(&pe[s].af->ch_layout, &pe[s].ax->ch_layout); pe[s].af->sample_rate = kRate;
        av_frame_get_buffer(pe[s].af, 0);
    }
    AVDictionary* opt = nullptr;
    av_dict_set(&opt, "pat_period", "0.2", 0); av_dict_set(&opt, "sdt_period", "0.5", 0);
    if (ok) ok = avformat_write_header(oc, &opt) >= 0;
    av_dict_free(&opt);

    if (ok) {
        AVPacket* pk = av_packet_alloc();
        auto drain = [&](AVCodecContext* cx, AVStream* st) {
            while (avcodec_receive_packet(cx, pk) >= 0) { av_packet_rescale_ts(pk, cx->time_base, st->time_base); pk->stream_index = st->index; av_interleaved_write_frame(oc, pk); }
        };
        // Synthetic content: the user's picture / video and sound replace the test card and the beep (one pass of the file, looped by the mux)
        std::unique_ptr<SynthMediaVideo> mv;
        std::unique_ptr<SynthMediaAudio> ma;
        double clipSeconds = kSeconds;
        if (media && media->active()) {
            std::string err;
            if (media->kind && !media->path.empty()) {
                mv = std::make_unique<SynthMediaVideo>();
                if (!mv->open(media->path, err)) { synthMediaSetStatus("Could not use the picture: " + err + ". The built-in test card is used."); DIAG(333549, "The synthetic content picture could not be loaded (%s), so the built-in test card is used.", err.c_str()); mv.reset(); }
            }
            if (!media->audio.empty() || media->kind == 2) {
                ma = std::make_unique<SynthMediaAudio>();
                if (!ma->load(*media, kSeconds, err)) { if (!media->audio.empty()) { synthMediaSetStatus("Could not use the sound: " + err + ". The built-in beep is used."); DIAG(936766, "The synthetic content sound could not be loaded (%s), so the built-in beep is used.", err.c_str()); } ma.reset(); }
            }
            if (mv && !mv->isStill() && mv->duration() > 0) clipSeconds = std::min<double>(kSeconds, std::max(1.0, mv->duration()));
            else if (mv && ma) clipSeconds = std::min<double>(kSeconds, std::max(1.0, ma->seconds()));
            else if (mv) clipSeconds = 4;
            if (mv || ma) synthMediaSetStatus(std::string("Carrying ") + (mv ? "your picture" : "the built-in test card") + (ma ? " and your sound" : " and the built-in beep") + ".");
        }
        const int frames = (int)std::lround(kFps * clipSeconds);
        outSeconds = (double)frames / kFps;
        std::vector<char> still((size_t)nsvc, 0);
        for (int s = 0; s < nsvc; s++) {
            if (progs[s].file.empty()) continue;
            av_frame_make_writable(pe[s].vf);
            still[s] = loadFileFrame(progs[s].file, pe[s].vf, 0) ? 1 : 0;
        }
        const bool useVideo = mv != nullptr;
        for (int n = 0; n < frames; n++) {
            for (int s = 0; s < nsvc; s++) {
                av_frame_make_writable(pe[s].vf);
                bool drawn = false;
                if (useVideo) drawn = mv->yuv420((double)n / kFps, kW, kH, pe[s].vf->data[0], pe[s].vf->linesize[0], pe[s].vf->data[1], pe[s].vf->linesize[1], pe[s].vf->data[2], pe[s].vf->linesize[2]);
                if (!drawn && !still[s]) drawCard(pe[s].vf, n, s, progs[s].picture, progs[s].name.c_str());
                pe[s].vf->pts = n;
                avcodec_send_frame(pe[s].vx, pe[s].vf); drain(pe[s].vx, pe[s].vs);
                while ((double)pe[s].aPts / kRate < (double)(n + 1) / kFps) {
                    av_frame_make_writable(pe[s].af);
                    if (ma) fillPcm(pe[s].ax, pe[s].af, *ma); else
                    fillBeep(pe[s].ax, pe[s].af, pe[s].aPts, s);
                    pe[s].af->pts = pe[s].aPts; pe[s].aPts += pe[s].af->nb_samples;
                    avcodec_send_frame(pe[s].ax, pe[s].af); drain(pe[s].ax, pe[s].as);
                }
            }
        }
        for (int s = 0; s < nsvc; s++) {
            avcodec_send_frame(pe[s].vx, nullptr); drain(pe[s].vx, pe[s].vs);
            avcodec_send_frame(pe[s].ax, nullptr); drain(pe[s].ax, pe[s].as);
        }
        av_write_trailer(oc);
        av_packet_free(&pk);
    }
    uint8_t* buf = nullptr;
    const int sz = avio_close_dyn_buf(oc->pb, &buf);
    oc->pb = nullptr;
    if (ok && sz > 188 * 100) { clip.ts.assign(buf, buf + sz / 188 * 188); clip.seconds = outSeconds; }
    else ok = false;
    av_free(buf);
    for (auto& p : pe) {
        av_frame_free(&p.vf); av_frame_free(&p.af);
        avcodec_free_context(&p.vx); avcodec_free_context(&p.ax);
    }
    avformat_free_context(oc);
    return ok;
}

std::string clipKey(const DemoTsIdentity& id, const SynthMedia* media) {
    std::string k = (media && media->active() ? "media:" + media->key() + "|" : std::string()) + id.provider + "|" + id.network + "|" + std::to_string(id.serviceId);
    const auto p = programmesOf(id);
    for (const auto& s : p) k += "|" + s.name + ":" + std::to_string(s.picture) + ":" + s.file + ":" + s.camera + (s.liveCamera ? ":cam" : "");
    return k;
}

const Clip& clip(const DemoTsIdentity& id) {
    static std::mutex mu;
    static std::map<std::string, Clip> clips;
    std::lock_guard<std::mutex> lk(mu);
    const SynthMedia* media = SynthMediaScope::current();
    Clip& c = clips[clipKey(id, media)];
    if (c.ts.empty()) {
        if (media && media->active()) {
            if (!encode(c, id, media)) {   // FFmpeg could not make it: the built-in programme carries on, and the user is told
                synthMediaSetStatus("Your file could not be encoded to MPEG-2 here, so the built-in test card and beep are used.");
                DIAG(178592, "The synthetic content could not be encoded to a transport stream, so the built-in test card and beep are used.");
                c.ts.clear();
                encode(c, id);
            }
        } else encode(c, id);
    }
    return c;
}

void nullPkt(uint8_t* pkt) {
    memset(pkt, 0xFF, 188);
    pkt[0] = 0x47; pkt[1] = 0x1F; pkt[2] = 0xFF; pkt[3] = 0x10;
}

struct LiveMux {
    std::mutex mu;
    std::deque<uint8_t> q;
    std::atomic<bool> stop{false};
    std::thread th;
    std::vector<std::unique_ptr<VideoIn>> cams;
    ~LiveMux() { stop = true; if (th.joinable()) th.join(); }

    // FFmpeg 7 made the data of the write callback const; older versions (Ubuntu 24.04 has 6.1) want it without
#if LIBAVFORMAT_VERSION_MAJOR >= 61
    static int avioWrite(void* opaque, const uint8_t* buf, int size) {
#else
    static int avioWrite(void* opaque, uint8_t* buf, int size) {
#endif
        auto* m = (LiveMux*)opaque;
        std::lock_guard<std::mutex> lk(m->mu);
        m->q.insert(m->q.end(), buf, buf + size);
        while (m->q.size() > 188 * 800) m->q.erase(m->q.begin(), m->q.begin() + 188);   // ~0.1 s at 12 Mbps
        return size;
    }

    bool start(const DemoTsIdentity& id) {
        auto progs = programmesOf(id);
        cams.resize(progs.size());
        for (size_t i = 0; i < progs.size(); i++) {
            const bool cam = progs[i].liveCamera || !progs[i].camera.empty();
            if (!cam && progs[i].file.empty()) continue;
            cams[i] = std::make_unique<VideoIn>();
            std::string err;
            if (!cams[i]->start(cam ? VideoIn::Camera : VideoIn::File, cam ? progs[i].camera : progs[i].file, err))
                cams[i].reset();
        }
        th = dect2::namedThread("onair-demots", [this, id, progs] {
            AVFormatContext* oc = nullptr;
            if (avformat_alloc_output_context2(&oc, nullptr, "mpegts", nullptr) < 0) return;
            unsigned char* iob = (unsigned char*)av_malloc(4096);
            oc->pb = avio_alloc_context(iob, 4096, 1, this, nullptr, avioWrite, nullptr);
            oc->flags |= AVFMT_FLAG_CUSTOM_IO;
            const AVCodec* vc = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
            struct PE { AVCodecContext *vx = nullptr, *ax = nullptr; AVStream *vs = nullptr, *as = nullptr; AVFrame *vf = nullptr, *af = nullptr; int64_t aPts = 0; };
            std::vector<PE> pe(progs.size());
            bool ok = vc != nullptr;
            const std::string prov = id.provider.empty() ? "OnAir" : id.provider;
            for (size_t s = 0; s < progs.size() && ok; s++) {
                pe[s].vx = avcodec_alloc_context3(vc);
                pe[s].vx->width = kW; pe[s].vx->height = kH; pe[s].vx->pix_fmt = AV_PIX_FMT_YUV420P;
                pe[s].vx->time_base = AVRational{1, kFps}; pe[s].vx->framerate = AVRational{kFps, 1};
                pe[s].vx->bit_rate = 1200000; pe[s].vx->gop_size = 12; pe[s].vx->max_b_frames = 0;
                pe[s].ax = openMp2();
                if (!pe[s].ax || avcodec_open2(pe[s].vx, vc, nullptr) < 0) { ok = false; break; }
                pe[s].vs = avformat_new_stream(oc, nullptr); pe[s].as = avformat_new_stream(oc, nullptr);
                avcodec_parameters_from_context(pe[s].vs->codecpar, pe[s].vx); pe[s].vs->time_base = pe[s].vx->time_base;
                avcodec_parameters_from_context(pe[s].as->codecpar, pe[s].ax); pe[s].as->time_base = pe[s].ax->time_base;
                pe[s].vs->id = 0x100 + (int)s * 0x10;
                pe[s].as->id = 0x101 + (int)s * 0x10;
                const int sid = id.serviceId + (int)s;
                AVProgram* pr = av_new_program(oc, sid);
                if (pr) {
                    av_dict_set(&pr->metadata, "service_name", progs[s].name.c_str(), 0);
                    av_dict_set(&pr->metadata, "service_provider", prov.c_str(), 0);
                    av_program_add_stream_index(oc, sid, pe[s].vs->index);
                    av_program_add_stream_index(oc, sid, pe[s].as->index);
                }
                pe[s].vf = av_frame_alloc(); pe[s].vf->format = AV_PIX_FMT_YUV420P; pe[s].vf->width = kW; pe[s].vf->height = kH; av_frame_get_buffer(pe[s].vf, 0);
                pe[s].af = av_frame_alloc(); pe[s].af->format = pe[s].ax->sample_fmt; pe[s].af->nb_samples = pe[s].ax->frame_size;
                av_channel_layout_copy(&pe[s].af->ch_layout, &pe[s].ax->ch_layout); pe[s].af->sample_rate = kRate; av_frame_get_buffer(pe[s].af, 0);
            }
            AVDictionary* opt = nullptr;
            av_dict_set(&opt, "pat_period", "0.2", 0); av_dict_set(&opt, "sdt_period", "0.5", 0);
            if (ok) ok = avformat_write_header(oc, &opt) >= 0;
            av_dict_free(&opt);
            AVPacket* pk = av_packet_alloc();
            auto drain = [&](AVCodecContext* cx, AVStream* st) {
                while (avcodec_receive_packet(cx, pk) >= 0) { av_packet_rescale_ts(pk, cx->time_base, st->time_base); pk->stream_index = st->index; av_interleaved_write_frame(oc, pk); }
            };
            int n = 0;
            while (ok && !stop.load()) {
                for (size_t s = 0; s < progs.size(); s++) {
                    av_frame_make_writable(pe[s].vf);
                    bool got = false;
                    if (s < cams.size() && cams[s]) cams[s]->setClock((double)n / kFps);   // a file programme plays at its own speed
                    if (s < cams.size() && cams[s])
                        got = cams[s]->copyYuv420(kW, kH, pe[s].vf->data[0], pe[s].vf->data[1], pe[s].vf->data[2],
                                                 pe[s].vf->linesize[0], pe[s].vf->linesize[1], pe[s].vf->linesize[2]);
                    if (!got) drawCard(pe[s].vf, n, (int)s, progs[s].picture, progs[s].name.c_str());
                    pe[s].vf->pts = n;
                    avcodec_send_frame(pe[s].vx, pe[s].vf); drain(pe[s].vx, pe[s].vs);
                    while ((double)pe[s].aPts / kRate < (double)(n + 1) / kFps) {
                        av_frame_make_writable(pe[s].af);
                        fillBeep(pe[s].ax, pe[s].af, pe[s].aPts, (int)s);
                        pe[s].af->pts = pe[s].aPts; pe[s].aPts += pe[s].af->nb_samples;
                        avcodec_send_frame(pe[s].ax, pe[s].af); drain(pe[s].ax, pe[s].as);
                    }
                }
                n++;
                std::this_thread::sleep_for(std::chrono::milliseconds(1000 / kFps));
            }
            av_packet_free(&pk);
            for (auto& p : pe) { av_frame_free(&p.vf); av_frame_free(&p.af); avcodec_free_context(&p.vx); avcodec_free_context(&p.ax); }
            if (oc) {
                av_write_trailer(oc);
                av_freep(&oc->pb->buffer);
                avio_context_free(&oc->pb);
                avformat_free_context(oc);
            }
        });
        return true;
    }

    void pop(uint8_t* pkt) {
        std::lock_guard<std::mutex> lk(mu);
        if (q.size() >= 188) {
            std::copy(q.begin(), q.begin() + 188, pkt);
            q.erase(q.begin(), q.begin() + 188);
            return;
        }
        nullPkt(pkt);
    }
};

bool wantsLive(const DemoTsIdentity& id) {
    for (const auto& p : programmesOf(id))
        if (p.liveCamera || !p.camera.empty() || !p.file.empty()) return true;
    return false;
}

} // namespace

std::function<void(uint8_t*)> demoTsSource(double netBitrate) {
    return demoTsSource(netBitrate, DemoTsIdentity());
}

std::function<void(uint8_t*)> demoTsSource(double netBitrate, const DemoTsIdentity& id) {
    if (wantsLive(id)) {
        auto live = std::make_shared<LiveMux>();
        live->start(id);
        const double bps = netBitrate > 1e6 ? netBitrate : 20e6;
        auto acc = std::make_shared<double>(0.5);
        const double rho = std::min(1.0, 4e6 / bps);   // live video share of the channel
        return [live, acc, rho](uint8_t* pkt) {
            *acc += rho;
            if (*acc >= 1.0) { *acc -= 1.0; live->pop(pkt); return; }
            nullPkt(pkt);
        };
    }
    const Clip& c = clip(id);
    const size_t n = c.ts.size() / 188;
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
        nullPkt(pkt);
    };
}

} // namespace dect2
