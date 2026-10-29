// Digital video generator engine (videogen.h). One thread, "onair-videogen": source picture -> fit to the output size -> overlay ->
// video encoder, the sound -> one encoder per audio track, both into FFmpeg's MPEG-TS muxer (mux rate, service, PIDs), whose packets
// go to a .ts file (as fast as the encoder runs) or to tsout's paced UDP / RTP sender (in real time).
// The camera comes from VideoIn (listVideoInputs / start / copyYuv420), the microphone from AudioIn. Pictures and video files are
// decoded here, frame by frame, so a file is encoded faster than real time with its picture and sound in step. The test generator
// draws its test picture (drawTestCard) straight into the output frame, its message under the overlay.
#include "dect2/tv/videogen.h"
#include "dect2/audio/audioin.h"
#include "dect2/audio/videoin.h"
#include "dect2/common/thread_name.h"
#include "dect2/common/u8file.h"
#include "dect2/tv/tsout.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace dect2::vgen {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string averr(int e) {
    char b[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(e, b, sizeof b);
    return b;
}

// the test generator's colour bars, white, yellow, cyan, green, magenta, red, blue, black (BT.601, limited range)
constexpr std::array<uint8_t, 8> kBarY = {235, 210, 170, 145, 106, 81, 41, 16};
constexpr std::array<uint8_t, 8> kBarU = {128, 16, 166, 54, 202, 90, 240, 128};
constexpr std::array<uint8_t, 8> kBarV = {128, 146, 16, 34, 222, 240, 110, 128};

} // namespace

// The test generator's picture: the one of the transmitter's test transport stream (demo_ts.cpp, made for 640 x 360 at 25 fps), drawn
// here at any resolution. The bars and the ramp follow the width; the squares, the ball, the sweep and the second bar follow the height.
void drawTestCard(uint8_t* py, int ys, uint8_t* pu, int us, uint8_t* pv, int vs, int W, int H, int n, double fps, int picture) {
    const double t = (double)n / fps, sc = H / 360.0;
    const int cell = std::max(2, (int)std::lround(40 * sc)), scroll = n * std::max(1, (int)std::lround(2 * sc));
    const int mul = picture == 2 ? 1 : 3, off = picture == 2 ? 0 : n * std::max(1, (int)std::lround(6 * sc));   // the ramp stands still
    const int ballX = (int)(W * (0.5 + 0.42 * std::sin(1.3 * t))), ballY = (int)(H * (0.62 + 0.17 * std::sin(2.9 * t)));
    const int outer = (int)std::lround(46 * sc), inner = (int)std::lround(14 * sc);
    for (int row = 0; row < H; row++) {
        uint8_t* ly = py + (size_t)row * ys;
        uint8_t* lu = nullptr;
        uint8_t* lv = nullptr;
        if (!(row & 1)) { lu = pu + (size_t)(row / 2) * us; lv = pv + (size_t)(row / 2) * vs; }
        int band = 2;   // 0 bars, 1 ramp or sweep, 2 chequerboard with the ball
        if (picture == 1 || (picture == 0 && row < H * 0.38)) band = 0;
        else if (picture == 2 || row < H * 0.44) band = 1;
        for (int col = 0; col < W; col++) {
            uint8_t Y = 52, U = 128, V = 128;
            if (band == 0) {
                const size_t i = (size_t)(col * 8 / W);
                Y = kBarY[i]; U = kBarU[i]; V = kBarV[i];
            } else if (band == 1) {
                Y = (uint8_t)(16 + ((col * mul + off) % W) * 219 / W);
            } else {
                const int ex = col - ballX, ey = row - ballY, d2 = ex * ex + ey * ey;
                if (d2 < inner * inner) Y = 235;
                else if (d2 < outer * outer) { Y = 210; U = 90; V = 170; }
                else if (((col + scroll) / cell + row / cell) & 1) { Y = 70; U = 134; V = 122; }
            }
            ly[col] = Y;
            if (lu && !(col & 1)) { lu[col / 2] = U; lv[col / 2] = V; }
        }
    }
    // along the bottom, full once a second
    const int perSec = std::max(1, (int)std::lround(fps)), margin = std::max(1, (int)std::lround(20 * sc));
    const int len = (n % perSec) * (W - 2 * margin) / perSec;
    for (int row = H - (int)std::lround(22 * sc); row < H - (int)std::lround(12 * sc); row++)
        std::fill_n(py + (size_t)row * ys + margin, len, (uint8_t)235);
}

namespace {

// A planar YUV 4:2:0 picture we own
struct Yuv {
    int w = 0, h = 0;
    std::vector<uint8_t> y, u, v;
    void resize(int W, int H) {
        if (W == w && H == h) return;
        w = W; h = H;
        y.assign((size_t)W * H, 16);
        u.assign((size_t)(W / 2) * (H / 2), 128);
        v.assign((size_t)(W / 2) * (H / 2), 128);
    }
    void black() { std::fill(y.begin(), y.end(), 16); std::fill(u.begin(), u.end(), 128); std::fill(v.begin(), v.end(), 128); }
};

// Scales the part of `src` the plan names into `dst` (black around it). Both YUV 4:2:0.
struct Fitter {
    SwsContext* sws = nullptr;
    ~Fitter() { sws_freeContext(sws); }
    void run(const uint8_t* const sp[3], const int ss[3], int sw, int sh, Yuv& dst, Fit fit) {
        const FitPlan p = fitPlan(sw, sh, dst.w, dst.h, fit);
        if (p.sw < 2 || p.sh < 2 || p.dw < 2 || p.dh < 2) { dst.black(); return; }
        if (p.dw != dst.w || p.dh != dst.h) dst.black();
        sws = sws_getCachedContext(sws, p.sw, p.sh, AV_PIX_FMT_YUV420P, p.dw, p.dh, AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr, nullptr, nullptr);
        if (!sws) return;
        const uint8_t* in[3] = {sp[0] + (size_t)p.sy * ss[0] + p.sx, sp[1] + (size_t)(p.sy / 2) * ss[1] + p.sx / 2, sp[2] + (size_t)(p.sy / 2) * ss[2] + p.sx / 2};
        uint8_t* out[3] = {dst.y.data() + (size_t)p.dy * dst.w + p.dx, dst.u.data() + (size_t)(p.dy / 2) * (dst.w / 2) + p.dx / 2,
                           dst.v.data() + (size_t)(p.dy / 2) * (dst.w / 2) + p.dx / 2};
        const int os[3] = {dst.w, dst.w / 2, dst.w / 2};
        sws_scale(sws, in, ss, 0, p.sh, out, os);
    }
};

// A file read with FFmpeg: the video frames (as YUV 4:2:0 at their own size) or the sound (stereo float at a chosen rate), looped
struct MediaReader {
    AVFormatContext* fc = nullptr;
    AVCodecContext* dec = nullptr;
    int si = -1;
    bool video = true;
    AVPacket* pkt = nullptr;
    AVFrame* fr = nullptr;
    SwsContext* sws = nullptr;
    SwrContext* swr = nullptr;
    int rate = 48000;
    double loopBase = 0, lastT = 0, frameDur = 0.04;
    int loops = 0;
    int64_t framesThisLoop = 0;

    ~MediaReader() {
        av_frame_free(&fr);
        av_packet_free(&pkt);
        avcodec_free_context(&dec);
        avformat_close_input(&fc);
        sws_freeContext(sws);
        swr_free(&swr);
    }
    bool open(const std::string& path, bool wantVideo, int sampleRate, std::string& err) {
        video = wantVideo;
        rate = sampleRate;
        int r = avformat_open_input(&fc, path.c_str(), nullptr, nullptr);
        if (r < 0) { err = "cannot open " + path + ": " + averr(r); return false; }
        if ((r = avformat_find_stream_info(fc, nullptr)) < 0) { err = "cannot read " + path + ": " + averr(r); return false; }
        const AVCodec* c = nullptr;
        si = av_find_best_stream(fc, video ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO, -1, -1, &c, 0);
        if (si < 0 || !c) { err = std::string(video ? "no picture in " : "no sound in ") + path; return false; }
        dec = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(dec, fc->streams[si]->codecpar);
        dec->pkt_timebase = fc->streams[si]->time_base;
        if ((r = avcodec_open2(dec, c, nullptr)) < 0) { err = "cannot decode " + path + ": " + averr(r); return false; }
        const AVRational fr0 = fc->streams[si]->avg_frame_rate;
        if (fr0.num > 0 && fr0.den > 0) frameDur = av_q2d(av_inv_q(fr0));
        pkt = av_packet_alloc();
        fr = av_frame_alloc();
        if (!video) {
            AVChannelLayout st;
            av_channel_layout_default(&st, 2);
            if (swr_alloc_set_opts2(&swr, &st, AV_SAMPLE_FMT_FLT, rate, &dec->ch_layout, dec->sample_fmt, dec->sample_rate, 0, nullptr) < 0 || swr_init(swr) < 0) {
                err = "cannot convert the sound of " + path;
                return false;
            }
        }
        return true;
    }
    // the next decoded frame in `fr`; at the end the file starts again (false when it cannot)
    bool next() {
        for (int tries = 0; tries < 100000; tries++) {
            int r = avcodec_receive_frame(dec, fr);
            if (r >= 0) { framesThisLoop++; return true; }
            if (r == AVERROR_EOF) {
                if (framesThisLoop == 0) return false;   // nothing came out of the last pass: looping would never end
                framesThisLoop = 0;
                loops++;
                loopBase = lastT + frameDur;
                avcodec_flush_buffers(dec);
                if (av_seek_frame(fc, -1, fc->start_time != AV_NOPTS_VALUE ? fc->start_time : 0, AVSEEK_FLAG_BACKWARD) < 0) return false;
                continue;
            }
            r = av_read_frame(fc, pkt);
            if (r < 0) { avcodec_send_packet(dec, nullptr); continue; }
            if (pkt->stream_index == si) avcodec_send_packet(dec, pkt);
            av_packet_unref(pkt);
        }
        return false;
    }
    double frameTime() {
        const AVStream* st = fc->streams[si];
        const int64_t ts = fr->best_effort_timestamp;
        const int64_t s0 = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
        double t = ts == AV_NOPTS_VALUE ? lastT - loopBase + frameDur : (double)(ts - s0) * av_q2d(st->time_base);
        t += loopBase;
        lastT = t;
        return t;
    }
    // the frame in `fr` as YUV 4:2:0 at its own size
    bool toYuv(Yuv& out) {
        const int w = fr->width & ~1, h = fr->height & ~1;
        if (w < 2 || h < 2) return false;
        out.resize(w, h);
        sws = sws_getCachedContext(sws, fr->width, fr->height, (AVPixelFormat)fr->format, w, h, AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr, nullptr, nullptr);
        if (!sws) return false;
        uint8_t* d[3] = {out.y.data(), out.u.data(), out.v.data()};
        const int ds[3] = {w, w / 2, w / 2};
        sws_scale(sws, fr->data, fr->linesize, 0, fr->height, d, ds);
        return true;
    }
    // sound: append stereo float samples of the next frame
    bool audioInto(std::deque<float>& fifo) {
        if (!next()) return false;
        const int outN = swr_get_out_samples(swr, fr->nb_samples);
        std::vector<float> buf((size_t)std::max(0, outN) * 2);
        uint8_t* o[1] = {(uint8_t*)buf.data()};
        const int n = swr_convert(swr, o, outN, (const uint8_t**)fr->extended_data, fr->nb_samples);
        if (n > 0) fifo.insert(fifo.end(), buf.begin(), buf.begin() + (size_t)n * 2);
        return true;
    }
};

// Where the muxer's packets go
struct Sink {
    FILE* file = nullptr;
    OutputManager* net = nullptr;
    std::vector<uint8_t> part;   // bytes of an unfinished 188-byte packet
    uint64_t bytes = 0;
    bool failed = false;
    void write(const uint8_t* d, int n) {
        bytes += (uint64_t)n;
        if (file && fwrite(d, 1, (size_t)n, file) != (size_t)n) failed = true;
        if (net) {
            part.insert(part.end(), d, d + n);
            const size_t whole = part.size() / 188;
            if (whole) {
                net->packets(part.data(), whole, nullptr);
                part.erase(part.begin(), part.begin() + (std::ptrdiff_t)(whole * 188));
            }
        }
    }
};

#if LIBAVFORMAT_VERSION_MAJOR >= 61
int writePacket(void* op, const uint8_t* buf, int n) {
#else
int writePacket(void* op, uint8_t* buf, int n) {
#endif
    Sink* s = (Sink*)op;
    s->write(buf, n);
    return s->failed ? AVERROR(EIO) : n;
}

bool encoderTakes(const AVCodec* c, AVPixelFormat want) {
    const AVPixelFormat* f = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    if (avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_PIX_FORMAT, 0, (const void**)&f, nullptr) < 0) return false;
#else
    f = c->pix_fmts;
#endif
    if (!f) return true;
    for (; *f != AV_PIX_FMT_NONE; f++) if (*f == want) return true;
    return false;
}

AVSampleFormat firstSampleFmt(const AVCodec* c) {
    const AVSampleFormat* f = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    if (avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, (const void**)&f, nullptr) < 0) f = nullptr;
#else
    f = c->sample_fmts;
#endif
    return f ? f[0] : AV_SAMPLE_FMT_FLTP;
}

// stereo float -> the encoder's frame (mono is the mean of both sides)
bool fillAudio(AVFrame* f, const float* st, int n, int channels) {
    const AVSampleFormat fmt = (AVSampleFormat)f->format;
    for (int i = 0; i < n; i++) {
        float smp[2] = {st[2 * i], st[2 * i + 1]};
        if (channels == 1) smp[0] = 0.5f * (smp[0] + smp[1]);
        for (int c = 0; c < channels; c++) {
            const float x = std::clamp(smp[c], -1.f, 1.f);
            switch (fmt) {
            case AV_SAMPLE_FMT_FLTP: ((float*)f->data[c])[i] = x; break;
            case AV_SAMPLE_FMT_FLT: ((float*)f->data[0])[i * channels + c] = x; break;
            case AV_SAMPLE_FMT_S16P: ((int16_t*)f->data[c])[i] = (int16_t)std::lround(x * 32767.f); break;
            case AV_SAMPLE_FMT_S16: ((int16_t*)f->data[0])[i * channels + c] = (int16_t)std::lround(x * 32767.f); break;
            default: return false;
            }
        }
    }
    return true;
}

} // namespace

FitPlan fitPlan(int srcW, int srcH, int dstW, int dstH, Fit f) {
    FitPlan p;
    srcW &= ~1; srcH &= ~1; dstW &= ~1; dstH &= ~1;
    p.sw = srcW; p.sh = srcH; p.dw = dstW; p.dh = dstH;
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0 || f == Fit::Stretch) return p;
    const double sa = (double)srcW / srcH, da = (double)dstW / dstH;
    if (f == Fit::Letterbox) {
        if (sa > da) { p.dh = std::min(dstH, (int)std::lround(dstW / sa) & ~1); p.dy = ((dstH - p.dh) / 2) & ~1; }
        else { p.dw = std::min(dstW, (int)std::lround(dstH * sa) & ~1); p.dx = ((dstW - p.dw) / 2) & ~1; }
    } else {
        if (sa > da) { p.sw = std::min(srcW, (int)std::lround(srcH * da) & ~1); p.sx = ((srcW - p.sw) / 2) & ~1; }
        else { p.sh = std::min(srcH, (int)std::lround(srcW / da) & ~1); p.sy = ((srcH - p.sh) / 2) & ~1; }
    }
    return p;
}

bool isMulticast(const std::string& host) {
    const int a = atoi(host.c_str());
    return a >= 224 && a <= 239;
}

std::string streamUrl(Output o, const std::string& host, int port) {
    if (o == Output::File) return std::string();
    return std::string(o == Output::Rtp ? "rtp://" : "udp://") + host + ":" + std::to_string(port);
}

std::string vlcOpenUrl(Output o, const std::string& host, int port) {
    if (o == Output::File) return std::string();
    return std::string(o == Output::Rtp ? "rtp://@" : "udp://@") + (isMulticast(host) ? host : std::string()) + ":" + std::to_string(port);
}

double mediaSeconds(const std::string& path) {
    AVFormatContext* fc = nullptr;
    if (path.empty() || avformat_open_input(&fc, path.c_str(), nullptr, nullptr) < 0) return 0;
    double s = 0;
    if (avformat_find_stream_info(fc, nullptr) >= 0 && fc->duration > 0) s = (double)fc->duration / AV_TIME_BASE;
    avformat_close_input(&fc);
    return s;
}

struct VideoGenerator::Impl {
    mutable std::mutex mu;
    std::condition_variable cv;
    std::thread th;
    std::atomic<bool> stopFlag{false};
    bool active = false;
    GenStats st;
    Layout overlay;
    uint64_t overlaySerial = 0;
    std::vector<uint8_t> prev;
    int prevW = 0, prevH = 0;
    uint64_t prevSerial = 0;

    void fail(const std::string& e) { std::lock_guard<std::mutex> lk(mu); if (st.error.empty()) st.error = e; }
    void run(GenConfig c);
};

void VideoGenerator::Impl::run(GenConfig c) {
    const Settings& s = c.enc;
    const int W = s.width & ~1, H = s.height & ~1;
    const AVRational fr = av_d2q(std::clamp(s.fps, 0.5, 120.0), 1001);
    const double fps = av_q2d(fr);
    const bool live = c.source == Source::Camera || c.output != Output::File;
    const int64_t totalFrames = c.output == Output::File ? std::max<int64_t>(1, (int64_t)std::llround(c.seconds * fps)) : INT64_MAX;
    const Budget bud = estimate(s);

    // encoders
    const std::string venc = pickVideoEncoder(s.videoEncoder, videoEncoders());
    const int tracks = audioTrackCount(s);
    std::string aenc;
    if (tracks > 0) {
        // the chosen encoder, else the first this FFmpeg has (st.audioEncoder tells the window which); none at all is an error, not a
        // stream without the audio tracks the settings and the estimate count
        for (const EncoderInfo& e : audioEncoders()) if (e.name == s.audioEncoder) aenc = e.name;
        if (aenc.empty() && !audioEncoders().empty()) aenc = audioEncoders().front().name;
        if (aenc.empty()) { fail("this FFmpeg has no audio encoder: choose \"Video only\""); return; }
    }
    { std::lock_guard<std::mutex> lk(mu); st.videoEncoder = venc; st.audioEncoder = aenc; }

    // sources
    VideoIn cam;
    MediaReader vfile, afile;
    AudioIn mic;
    Yuv still, src, out;
    out.resize(W, H);
    Fitter fitter;
    std::string err;
    if (c.source == Source::Camera) {
        if (!cam.start(VideoIn::Camera, c.cameraId, err)) { fail(err.empty() ? "cannot open the camera" : err); return; }
    } else if (c.source != Source::Generator) {
        if (!vfile.open(c.path, true, s.sampleRate, err)) { fail(err); return; }
        if (c.source == Source::Picture) {
            if (!vfile.next() || !vfile.toYuv(still)) { fail("cannot decode the picture " + c.path); return; }
            const uint8_t* sp[3] = {still.y.data(), still.u.data(), still.v.data()};
            const int ss[3] = {still.w, still.w / 2, still.w / 2};
            fitter.run(sp, ss, still.w, still.h, out, c.fit);
        }
    }
    bool fileSound = false;
    if (tracks > 0 && c.audio == AudioSource::FromFile && c.source == Source::VideoFile) {
        std::string aerr;
        fileSound = afile.open(c.path, false, s.sampleRate, aerr);   // a file without sound gives silence
    }
    if (tracks > 0 && c.audio == AudioSource::Microphone) {
        if (!mic.start(AudioIn::Mic, s.sampleRate, c.micId, 0, err)) { fail(err.empty() ? "cannot open the microphone" : err); return; }
    }

    // output
    Sink sink;
    OutputManager net;
    if (c.output == Output::File) {
        sink.file = u8fopen(c.tsPath, "wb");
        if (!sink.file) { fail("cannot write " + c.tsPath + ": " + strerror(errno)); return; }
    } else {
        OutputConfig oc;
        oc.udp = true; oc.host = c.host; oc.port = c.port; oc.ttl = c.ttl; oc.rtp = c.output == Output::Rtp;
        net.configure(oc);
        const OutputStats os = net.stats();
        if (!os.error.empty()) { fail(os.error); return; }
        sink.net = &net;
    }

    AVFormatContext* oc = nullptr;
    avformat_alloc_output_context2(&oc, nullptr, "mpegts", nullptr);
    if (!oc) { fail("FFmpeg has no MPEG-TS muxer"); if (sink.file) fclose(sink.file); return; }
    const int ioSize = 188 * 64;
    uint8_t* ioBuf = (uint8_t*)av_malloc(ioSize);
    oc->pb = avio_alloc_context(ioBuf, ioSize, 1, &sink, nullptr, writePacket, nullptr);
    oc->flags |= AVFMT_FLAG_CUSTOM_IO;

    // video encoder
    const AVCodec* vc = avcodec_find_encoder_by_name(venc.c_str());
    AVCodecContext* vx = vc ? avcodec_alloc_context3(vc) : nullptr;
    AVFrame* vf = av_frame_alloc();
    SwsContext* toNv12 = nullptr;
    std::vector<AVCodecContext*> ax((size_t)tracks, nullptr);
    std::vector<AVStream*> as((size_t)tracks, nullptr);
    AVStream* vs = nullptr;
    AVFrame* af = av_frame_alloc();
    AVPacket* pk = av_packet_alloc();
    bool ok = vx != nullptr;
    if (!ok) fail("no video encoder " + venc);
    if (ok) {
        const EncoderCaps caps = capsOf(venc);
        vx->width = W; vx->height = H;
        vx->pix_fmt = encoderTakes(vc, AV_PIX_FMT_YUV420P) ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_NV12;
        vx->time_base = av_inv_q(fr); vx->framerate = fr;
        vx->sample_aspect_ratio = AVRational{1, 1};
        const int gop = s.frames == FrameTypes::IOnly ? 1 : std::max(1, (int)std::lround(s.gopSeconds * fps));
        vx->gop_size = gop;
        int bf = s.frames == FrameTypes::IPB && caps.bFrames ? 2 : 0;
        if (caps.profile && s.profile == 0) bf = 0;   // Baseline has no B-frames
        vx->max_b_frames = std::min(bf, gop - 1);
        const int64_t rate = (int64_t)effectiveVideoKbps(s) * 1000;
        const bool mpeg2 = codecOf(venc) == Codec::Mpeg2;
        const int64_t vbvCap = mpeg2 ? (W <= 720 && H <= 576 ? 1835008 : 9781248) : INT64_MAX;   // MPEG-2 Main Level / High Level
        AVDictionary* o = nullptr;
        RateControl rc = s.rc;
        if (rc == RateControl::Crf && !caps.crf) rc = RateControl::ConstrainedVbr;
        if (rc == RateControl::Cbr) {
            vx->bit_rate = vx->rc_min_rate = vx->rc_max_rate = rate;
            vx->rc_buffer_size = (int)std::min<int64_t>(rate, vbvCap);
            if (venc == "libx264") av_dict_set(&o, "nal-hrd", "cbr", 0);   // filler data keeps the rate even on a still picture
            if (venc.find("_nvenc") != std::string::npos) av_dict_set(&o, "rc", "cbr", 0);
        } else if (rc == RateControl::ConstrainedVbr) {
            vx->bit_rate = rate;
            vx->rc_max_rate = rate * 3 / 2;
            vx->rc_buffer_size = (int)std::min<int64_t>(rate * 3 / 2, vbvCap);
            if (venc.find("_nvenc") != std::string::npos) av_dict_set(&o, "rc", "vbr", 0);
        } else {
            av_dict_set_int(&o, "crf", s.crf, 0);
            vx->rc_max_rate = rate * 2;   // capped, so the mux rate the estimate gives is enough
            vx->rc_buffer_size = (int)(rate * 2);
        }
        if (mpeg2) {
            // vbv_delay is 16 bits of 90 kHz: a buffer longer than about 0.7 s of the rate makes the stream VBR in the headers
            vx->rc_buffer_size = (int)std::min<int64_t>(vx->rc_buffer_size, vx->rc_max_rate * 7 / 10);
            vx->rc_initial_buffer_occupancy = vx->rc_buffer_size * 3 / 4;
            // a busy picture (camera noise) can still take longer than vbv_delay holds, and FFmpeg aborts on it in CBR: VBR headers
            // then (vbv_delay 0xFFFF); the rate control keeps the set rate, the muxer pads the rest with null packets
            if (vx->rc_min_rate && !mpeg2CbrHeaders(W, H, vx->rc_min_rate)) vx->rc_min_rate = 0;
        }
        if (caps.preset) av_dict_set(&o, "preset", kPresets[std::clamp(s.preset, 0, kNumPresets - 1)], 0);
        if (caps.zeroLatency && s.zeroLatency) av_dict_set(&o, "tune", "zerolatency", 0);
        if (caps.profile) av_dict_set(&o, "profile", kProfiles[std::clamp(s.profile, 0, kNumProfiles - 1)], 0);
        if (venc == "libx264") av_dict_set(&o, "x264-params", "scenecut=0", 0);   // the GOP the user set, not one the scene cuts choose
        if (venc == "libx265") av_dict_set(&o, "x265-params", rc == RateControl::Cbr ? "log-level=error:scenecut=0:strict-cbr=1" : "log-level=error:scenecut=0", 0);
        const int r = avcodec_open2(vx, vc, &o);
        av_dict_free(&o);
        if (r < 0) { fail("cannot open the video encoder " + venc + ": " + averr(r)); ok = false; }
    }
    if (ok) {
        vs = avformat_new_stream(oc, nullptr);
        avcodec_parameters_from_context(vs->codecpar, vx);
        vs->time_base = vx->time_base;
        if (!s.autoPids) vs->id = s.videoPid;
        vf->format = vx->pix_fmt; vf->width = W; vf->height = H;
        if (av_frame_get_buffer(vf, 0) < 0) { fail("out of memory"); ok = false; }
        if (vx->pix_fmt == AV_PIX_FMT_NV12) toNv12 = sws_getContext(W, H, AV_PIX_FMT_YUV420P, W, H, AV_PIX_FMT_NV12, SWS_POINT, nullptr, nullptr, nullptr);
    }
    // audio encoders: the same sound on every track
    const AVCodec* acodec = aenc.empty() ? nullptr : avcodec_find_encoder_by_name(aenc.c_str());
    for (int t = 0; ok && t < tracks && acodec; t++) {
        AVCodecContext* a = avcodec_alloc_context3(acodec);
        a->sample_fmt = firstSampleFmt(acodec);
        a->sample_rate = s.sampleRate;
        a->bit_rate = (int64_t)s.audioKbps * 1000;
        av_channel_layout_default(&a->ch_layout, std::clamp(s.channels, 1, 2));
        a->time_base = AVRational{1, s.sampleRate};
        const int r = avcodec_open2(a, acodec, nullptr);
        ax[(size_t)t] = a;
        if (r < 0) { fail("cannot open the audio encoder " + aenc + ": " + averr(r)); ok = false; break; }
        AVStream* st2 = avformat_new_stream(oc, nullptr);
        avcodec_parameters_from_context(st2->codecpar, a);
        st2->time_base = a->time_base;
        if (!s.autoPids) st2->id = s.audioPid + t;
        if (tracks > 1) av_dict_set(&st2->metadata, "title", ("Track " + std::to_string(t + 1)).c_str(), 0);
        as[(size_t)t] = st2;
    }
    if (ok && tracks > 0 && ax[0]) {
        af->format = ax[0]->sample_fmt;
        af->nb_samples = ax[0]->frame_size > 0 ? ax[0]->frame_size : 1152;
        av_channel_layout_copy(&af->ch_layout, &ax[0]->ch_layout);
        af->sample_rate = s.sampleRate;
        if (av_frame_get_buffer(af, 0) < 0) { fail("out of memory"); ok = false; }
    }
    if (ok) {
        oc->max_delay = 700000;   // the PCR runs 0.7 s ahead of the decoding times (FFmpeg's -muxdelay default), room for a large I-frame
        av_dict_set(&oc->metadata, "service_name", s.serviceName.c_str(), 0);
        av_dict_set(&oc->metadata, "service_provider", s.provider.c_str(), 0);
        AVDictionary* mo = nullptr;
        av_dict_set_int(&mo, "muxrate", (int64_t)bud.mux * 1000, 0);
        av_dict_set_int(&mo, "mpegts_service_id", s.serviceId, 0);
        if (!s.autoPids) av_dict_set_int(&mo, "mpegts_pmt_start_pid", s.pmtPid, 0);
        av_dict_set(&mo, "pcr_period", "40", 0);
        av_dict_set(&mo, "pat_period", "0.1", 0);
        av_dict_set(&mo, "sdt_period", "0.5", 0);
        const int r = avformat_write_header(oc, &mo);
        av_dict_free(&mo);
        if (r < 0) { fail("cannot start the transport stream: " + averr(r)); ok = false; }
    }

    if (ok) {
        auto drain = [&](AVCodecContext* cx, AVStream* stream) {
            while (avcodec_receive_packet(cx, pk) >= 0) {
                av_packet_rescale_ts(pk, cx->time_base, stream->time_base);
                pk->stream_index = stream->index;
                if (av_interleaved_write_frame(oc, pk) < 0) sink.failed = true;
            }
        };
        auto noError = [&] { std::lock_guard<std::mutex> lk(mu); return st.error.empty(); };
        OverlayRenderer ovr;
        ovr.setFont(c.font);
        // the generator's message: one centred line in the chequerboard under the bars, smaller when it would not fit
        std::vector<uint8_t> msg;
        if (c.source == Source::Generator && !c.message.empty() && ovr.hasFont()) {
            Layout ml;
            Block b;
            b.text = c.message;
            b.fontPx = std::max(8.0f, H * 0.07f);
            const float wide = ovr.blockRect(b, s, W, H).w;
            if (wide > W * 0.94f) b.fontPx = std::max(8.0f, b.fontPx * W * 0.94f / wide);
            const Rect r = ovr.blockRect(b, s, W, H);
            b.x = std::max(0.0f, (W - r.w) / 2 / W);
            b.y = 0.47f;
            ml.blocks.push_back(b);
            ovr.render(ml, s, 0, W, H, msg);
        }
        Layout lay;
        uint64_t laySerial = ~0ull;
        std::vector<uint8_t> ovl;
        bool ovlStatic = false, ovlEmpty = true;
        std::deque<float> fifo;   // file sound, stereo float
        std::vector<float> abuf;
        int64_t aPts = 0;
        double phase = 0;
        bool haveFileFrame = false, fileEnded = false, pending = false;
        double nextFileT = 0;
        SwsContext* prevSws = nullptr;
        auto lastPreview = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        const auto t0 = std::chrono::steady_clock::now();
        int late = 0;
        for (int64_t n = 0; n < totalFrames && !stopFlag; n++) {
            const double t = (double)n / fps;
            if (live) {
                const auto due = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(t));
                if (std::chrono::steady_clock::now() > due + std::chrono::milliseconds(200)) late++;
                else std::this_thread::sleep_until(due);
            }
            // the picture
            if (c.source == Source::Camera) {
                const int cw = cam.width() & ~1, ch = cam.height() & ~1;
                if (cw >= 2 && ch >= 2) {
                    src.resize(cw, ch);
                    if (cam.copyYuv420(cw, ch, src.y.data(), src.u.data(), src.v.data())) {
                        const uint8_t* sp[3] = {src.y.data(), src.u.data(), src.v.data()};
                        const int ss[3] = {src.w, src.w / 2, src.w / 2};
                        fitter.run(sp, ss, src.w, src.h, out, c.fit);
                    }
                }
            } else if (c.source == Source::Generator) {
                drawTestCard(out.y.data(), W, out.u.data(), W / 2, out.v.data(), W / 2, W, H, (int)(n % (1 << 30)), fps, c.pattern);
                if (!msg.empty()) OverlayRenderer::blendYuv420(msg.data(), W, H, out.y.data(), W, out.u.data(), W / 2, out.v.data(), W / 2);
            } else if (c.source == Source::Picture) {
                const uint8_t* sp[3] = {still.y.data(), still.u.data(), still.v.data()};
                const int ss[3] = {still.w, still.w / 2, still.w / 2};
                fitter.run(sp, ss, still.w, still.h, out, c.fit);   // again: the overlay drew on the last one
            } else {
                // the newest file frame at or before t (frames are repeated or skipped to reach the output rate); the decoded frame
                // after it waits in the reader (pending) until its time comes
                if (!haveFileFrame && !fileEnded) {
                    if (vfile.next()) { vfile.frameTime(); haveFileFrame = vfile.toYuv(src); }   // the first frame is shown from t = 0
                    else fileEnded = true;
                }
                while (!fileEnded) {
                    if (!pending) {
                        if (!vfile.next()) { fileEnded = true; break; }
                        nextFileT = vfile.frameTime();
                        pending = true;
                    }
                    if (nextFileT > t) break;
                    vfile.toYuv(src);
                    pending = false;
                }
                if (haveFileFrame && src.w >= 2) {
                    const uint8_t* sp[3] = {src.y.data(), src.u.data(), src.v.data()};
                    const int ss[3] = {src.w, src.w / 2, src.w / 2};
                    fitter.run(sp, ss, src.w, src.h, out, c.fit);
                }
            }
            // the overlay
            {
                std::lock_guard<std::mutex> lk(mu);
                if (overlaySerial != laySerial) { lay = overlay; laySerial = overlaySerial; ovlStatic = false; }
            }
            if (!lay.blocks.empty()) {
                const bool rolling = std::any_of(lay.blocks.begin(), lay.blocks.end(), [](const Block& b) { return b.kind == BlockKind::Rolling; });
                if (!ovlStatic || rolling) { ovr.render(lay, s, t, W, H, ovl); ovlStatic = true; ovlEmpty = false; }
                if (!ovlEmpty) OverlayRenderer::blendYuv420(ovl.data(), W, H, out.y.data(), W, out.u.data(), W / 2, out.v.data(), W / 2);
            }
            // encode the picture
            if (av_frame_make_writable(vf) < 0) { fail("out of memory"); break; }
            if (vx->pix_fmt == AV_PIX_FMT_NV12 && toNv12) {
                const uint8_t* sp[3] = {out.y.data(), out.u.data(), out.v.data()};
                const int ss[3] = {W, W / 2, W / 2};
                sws_scale(toNv12, sp, ss, 0, H, vf->data, vf->linesize);
            } else {
                av_image_copy_plane(vf->data[0], vf->linesize[0], out.y.data(), W, W, H);
                av_image_copy_plane(vf->data[1], vf->linesize[1], out.u.data(), W / 2, W / 2, H / 2);
                av_image_copy_plane(vf->data[2], vf->linesize[2], out.v.data(), W / 2, W / 2, H / 2);
            }
            vf->pts = n;
            if (avcodec_send_frame(vx, vf) < 0) { fail("the video encoder stopped"); break; }
            drain(vx, vs);
            // the sound up to the end of this picture
            if (tracks > 0 && ax[0]) {
                const int fs = af->nb_samples;
                abuf.assign((size_t)fs * 2, 0.f);
                while ((double)aPts / s.sampleRate < (double)(n + 1) / fps) {
                    if (c.audio == AudioSource::Tone) {
                        for (int i = 0; i < fs; i++) {
                            const float x = 0.1f * (float)std::sin(phase);   // -20 dBFS
                            phase += 2 * kPi * c.toneHz / s.sampleRate;
                            abuf[2 * (size_t)i] = abuf[2 * (size_t)i + 1] = x;
                        }
                        phase = std::fmod(phase, 2 * kPi);
                    } else if (c.audio == AudioSource::Microphone) {
                        mic.read(abuf.data(), fs);
                    } else if (fileSound) {
                        while (fifo.size() < (size_t)fs * 2 && afile.audioInto(fifo)) {}
                        const size_t m = std::min(fifo.size(), (size_t)fs * 2);
                        std::fill(abuf.begin(), abuf.end(), 0.f);
                        std::copy(fifo.begin(), fifo.begin() + (std::ptrdiff_t)m, abuf.begin());
                        fifo.erase(fifo.begin(), fifo.begin() + (std::ptrdiff_t)m);
                    } else {
                        std::fill(abuf.begin(), abuf.end(), 0.f);
                    }
                    for (int k = 0; k < tracks; k++) {
                        if (av_frame_make_writable(af) < 0 || !fillAudio(af, abuf.data(), fs, ax[(size_t)k]->ch_layout.nb_channels)) { fail("the sound format is not supported"); break; }
                        af->pts = aPts;
                        avcodec_send_frame(ax[(size_t)k], af);
                        drain(ax[(size_t)k], as[(size_t)k]);
                    }
                    aPts += fs;
                }
            }
            if (!noError()) break;   // an audio encoder failed
            if (sink.net) net.burstDone(1.0 / fps);
            if (sink.failed) { fail(c.output == Output::File ? "cannot write " + c.tsPath : "cannot send the stream"); break; }
            // the preview for the window, about 8 times a second
            const auto now = std::chrono::steady_clock::now();
            if (now - lastPreview > std::chrono::milliseconds(120)) {
                lastPreview = now;
                const int pw = std::min(W, 640) & ~1, ph = std::max(2, (int)std::lround((double)pw * H / W) & ~1);
                prevSws = sws_getCachedContext(prevSws, W, H, AV_PIX_FMT_YUV420P, pw, ph, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (prevSws) {
                    std::vector<uint8_t> rgba((size_t)pw * ph * 4);
                    const uint8_t* sp[3] = {out.y.data(), out.u.data(), out.v.data()};
                    const int ss[3] = {W, W / 2, W / 2};
                    uint8_t* d[1] = {rgba.data()};
                    const int ds[1] = {pw * 4};
                    sws_scale(prevSws, sp, ss, 0, H, d, ds);
                    std::lock_guard<std::mutex> lk(mu);
                    prev.swap(rgba); prevW = pw; prevH = ph; prevSerial++;
                }
            }
            {
                std::lock_guard<std::mutex> lk(mu);
                st.frames = (uint64_t)n + 1;
                st.seconds = (double)(n + 1) / fps;
                st.bytes = sink.bytes;
                st.kbps = st.seconds > 0 ? (double)sink.bytes * 8 / st.seconds / 1000.0 : 0;
                st.progress = c.output == Output::File ? (double)(n + 1) / (double)totalFrames : 0;
                st.lateFrames = late;
            }
        }
        sws_freeContext(prevSws);
        // flush the encoders and close the stream
        avcodec_send_frame(vx, nullptr);
        drain(vx, vs);
        for (int k = 0; k < tracks; k++) if (ax[(size_t)k]) { avcodec_send_frame(ax[(size_t)k], nullptr); drain(ax[(size_t)k], as[(size_t)k]); }
        av_write_trailer(oc);
    }
    avio_flush(oc->pb);
    if (sink.net) net.burstDone(1.0 / fps);
    sws_freeContext(toNv12);
    av_packet_free(&pk);
    av_frame_free(&vf);
    av_frame_free(&af);
    avcodec_free_context(&vx);
    for (AVCodecContext*& a : ax) avcodec_free_context(&a);
    if (oc->pb) { av_freep(&oc->pb->buffer); avio_context_free(&oc->pb); }
    avformat_free_context(oc);
    if (sink.file) { if (fclose(sink.file) != 0) fail("cannot write " + c.tsPath); }
    if (sink.net) {
        // let the paced sender empty its queue before the socket closes
        for (int i = 0; i < 50 && !stopFlag; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        net.close();
    }
    cam.stop();
    mic.stop();
    std::lock_guard<std::mutex> lk(mu);
    st.bytes = sink.bytes;
    st.finished = st.error.empty();
}

VideoGenerator::VideoGenerator() : p_(new Impl) {}
VideoGenerator::~VideoGenerator() { stop(); }

bool VideoGenerator::start(const GenConfig& c, std::string& err) {
    if (running()) { err = "already running"; return false; }
    if (p_->th.joinable()) p_->th.join();
    if (c.output == Output::File && c.tsPath.empty()) { err = "choose a .ts file to write"; return false; }
    if ((c.source == Source::Picture || c.source == Source::VideoFile) && c.path.empty()) { err = "choose a picture or video file"; return false; }
    if (c.source == Source::Generator && (c.pattern < 0 || c.pattern > 2)) { err = "the test pattern must be 0 to 2"; return false; }
    if (c.output != Output::File && (c.port <= 0 || c.port > 65535)) { err = "the port must be 1 to 65535"; return false; }
    if (c.enc.width < 16 || c.enc.height < 16) { err = "the picture is too small"; return false; }
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->st = GenStats();
        p_->st.running = true;
        p_->active = true;
        p_->overlay = c.overlay;
        p_->overlaySerial++;
        p_->prevW = p_->prevH = 0;
    }
    p_->stopFlag = false;
    Impl* I = p_.get();
    p_->th = namedThread("onair-videogen", [I](GenConfig cfg) {
        I->run(std::move(cfg));
        std::lock_guard<std::mutex> lk(I->mu);
        I->st.running = false;
        I->active = false;
        I->cv.notify_all();
    }, c);
    return true;
}

void VideoGenerator::stop() {
    p_->stopFlag = true;
    if (p_->th.joinable()) p_->th.join();
}

bool VideoGenerator::running() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->active;
}

bool VideoGenerator::wait(double timeoutSeconds) {
    std::unique_lock<std::mutex> lk(p_->mu);
    return p_->cv.wait_for(lk, std::chrono::duration<double>(timeoutSeconds), [this] { return !p_->active; });
}

GenStats VideoGenerator::stats() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->st;
}

void VideoGenerator::setOverlay(const Layout& l) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->overlay = l;
    p_->overlaySerial++;
}

bool VideoGenerator::preview(std::vector<uint8_t>& rgba, int& w, int& h, uint64_t& serial) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->prevSerial == serial || p_->prev.empty()) return false;
    rgba = p_->prev;
    w = p_->prevW; h = p_->prevH;
    serial = p_->prevSerial;
    return true;
}

} // namespace dect2::vgen
