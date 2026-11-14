// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Synthetic content" (see synth_media.h): decoding of the user's picture, video and sound files for the test signals.
#include "dect2/source/synth_media.h"
#include "dect2/common/diag.h"
#include "dect2/source/source.h"
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
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>

namespace dect2 {

namespace {

std::mutex g_statusMu;
std::string g_status;
thread_local const SynthMedia* t_current = nullptr;

std::string fileStamp(const std::string& path) {
    if (path.empty()) return "";
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    const auto n = std::filesystem::file_size(path, ec);
    return path + "@" + std::to_string(ec ? 0 : (long long)t.time_since_epoch().count()) + ":" + std::to_string((unsigned long long)n);
}

std::string baseName(const std::string& p) {
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

} // namespace

std::string SynthMedia::key() const { return std::to_string(kind) + "|" + (kind ? fileStamp(path) : std::string()) + "|" + fileStamp(audio); }

std::string synthMediaStatus() { std::lock_guard<std::mutex> lk(g_statusMu); return g_status; }
void synthMediaSetStatus(const std::string& s) { std::lock_guard<std::mutex> lk(g_statusMu); g_status = s; }

int synthMediaSupport(const SynthConfig& c, std::string* note) {
    auto say = [&](int bits, const char* n) { if (note) *note = n; return bits; };
    const char* ts = "Encoded as MPEG-2 video and MP2 sound in the transport stream the signal carries (the picture is scaled to 640 x 360).";
    switch (c.mode) {
    case 0:
        if (c.dab) return say(0, "DAB needs its own audio encoder, which OnAir does not include: the built-in ensemble is kept.");
        return say(kSynthMediaPicture | kSynthMediaAudio, ts);
    case 3: case 6: case 732: case 743: case 8: case 9: case 42: case 156: case 748:
        return say(kSynthMediaPicture | kSynthMediaAudio, ts);
    case 10:
        if (c.modeOpt[0] == 13 || c.modeOpt[0] == 14) return say(0, "NBTV and mechanical television keep their built-in test picture.");
        return say(kSynthMediaPicture | kSynthMediaAudio, "The picture modulates the vision carrier and the sound the sound carrier.");
    case 7: return say(kSynthMediaAudio, "The sound file is the programme of the station (stereo, with RDS).");
    case 57: return say(kSynthMediaAudio, "The sound file is the programme of the AM, NFM, WFM and DSB test signals (single sideband keeps its tone).");
    case 27:
        if (c.modeOpt[0] == 1) return say(kSynthMediaPicture, "The picture is sent as the SSTV image (scaled to the size of the chosen SSTV mode).");
        return say(0, "This signal carries no picture or sound.");
    case 4: return say(0, "DAB needs its own audio encoder, which OnAir does not include: the built-in ensemble is kept.");
    case 5: return say(0, "ATSC 3.0 needs HEVC and AC-4 encoders, which OnAir does not include: the built-in test card is kept.");
    case 12: return say(0, "DRM needs an AAC or xHE-AAC encoder, which OnAir does not include: the built-in programme is kept.");
    case 23: return say(0, "HD Radio needs the patented HDC codec, which OnAir does not include: the built-in programme is kept.");
    default: return say(0, "This mode's test signal has no picture or sound to replace.");
    }
}

SynthMedia synthMediaOf(const SynthConfig& c) {
    SynthMedia m;
    const int sup = synthMediaSupport(c);
    if (sup & kSynthMediaPicture) { m.kind = c.mediaKind == 1 || c.mediaKind == 2 ? c.mediaKind : 0; if (m.kind) m.path = c.mediaPath; }
    if (sup & kSynthMediaAudio) m.audio = c.mediaAudio;
    if (!(sup & kSynthMediaAudio) && !(sup & kSynthMediaPicture)) return SynthMedia();
    if (m.kind && m.path.empty()) m.kind = 0;
    // a video's own sound is used by the sound modes too
    if ((sup & kSynthMediaAudio) && !(sup & kSynthMediaPicture) && c.mediaKind == 2 && m.audio.empty()) m.audio = c.mediaPath;
    return m;
}

SynthMediaScope::SynthMediaScope(const SynthConfig& cfg) : m_(synthMediaOf(cfg)), prev_(t_current) { t_current = m_.active() ? &m_ : nullptr; }
SynthMediaScope::SynthMediaScope(const SynthMedia& m) : m_(m), prev_(t_current) { t_current = m_.active() ? &m_ : nullptr; }
SynthMediaScope::~SynthMediaScope() { t_current = prev_; }
const SynthMedia* SynthMediaScope::current() { return t_current; }

// ---------------------------------------------------------------- video
struct SynthMediaVideo::Impl {
    AVFormatContext* ic = nullptr;
    AVCodecContext* cx = nullptr;
    AVStream* st = nullptr;
    int si = -1;
    AVFrame *cur = nullptr, *pend = nullptr, *fr = nullptr;
    AVPacket* pk = nullptr;
    SwsContext* sws = nullptr;
    double dur = 0, lastT = -1, curT = 0, pendT = 0, fps = 25;
    bool still = false, haveCur = false, havePend = false, draining = false;
    int64_t t0 = 0;
    ~Impl() {
        if (sws) sws_freeContext(sws);
        av_frame_free(&cur); av_frame_free(&pend); av_frame_free(&fr); av_packet_free(&pk);
        avcodec_free_context(&cx); avformat_close_input(&ic);
    }
    double ptsSec(const AVFrame* f, double prev) const {
        const int64_t p = f->best_effort_timestamp;
        if (p == AV_NOPTS_VALUE) return prev + 1.0 / fps;
        return std::max(0.0, (double)(p - t0) * av_q2d(st->time_base));
    }
    bool decodeNext(AVFrame* dst, double prevT, double& t) {
        for (;;) {
            const int r = avcodec_receive_frame(cx, fr);
            if (r >= 0) { av_frame_unref(dst); av_frame_ref(dst, fr); av_frame_unref(fr); t = ptsSec(dst, prevT); return true; }
            if (r != AVERROR(EAGAIN)) return false;   // end of the stream (or an error)
            if (draining) return false;
            const int rd = av_read_frame(ic, pk);
            if (rd < 0) { avcodec_send_packet(cx, nullptr); draining = true; continue; }
            if (pk->stream_index == si) avcodec_send_packet(cx, pk);
            av_packet_unref(pk);
        }
    }
    void rewind() {
        av_seek_frame(ic, si, t0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(cx);
        draining = false; havePend = false; haveCur = false;
    }
    bool seek(double t) {
        if (dur > 0 && !still) t = std::fmod(t, dur);
        if (t < 0) t = 0;
        if (!still && haveCur && t < lastT - 1e-3) rewind();
        lastT = t;
        for (;;) {
            if (havePend) {
                if (pendT <= t + 1e-4 || !haveCur) { std::swap(cur, pend); curT = pendT; haveCur = true; havePend = false; }
                else break;
            }
            double ft;
            if (!decodeNext(pend, haveCur ? curT : -1.0 / fps, ft)) break;
            pendT = ft; havePend = true;
        }
        return haveCur;
    }
    // where the frame goes in dw x dh (even numbers): no distortion, black bars
    void fit(int dw, int dh, int& ox, int& oy, int& w, int& h) const {
        const double sa = (double)cur->width / std::max(1, cur->height);
        w = dw; h = (int)std::lround(dw / sa) & ~1;
        if (h > dh) { h = dh; w = (int)std::lround(dh * sa) & ~1; }
        w = std::max(2, std::min(dw, w)); h = std::max(2, std::min(dh, h));
        ox = ((dw - w) / 2) & ~1; oy = ((dh - h) / 2) & ~1;
    }
};

SynthMediaVideo::SynthMediaVideo() : p_(new Impl) {}
SynthMediaVideo::~SynthMediaVideo() = default;

bool SynthMediaVideo::open(const std::string& path, std::string& err) {
    Impl& d = *p_;
    if (avformat_open_input(&d.ic, path.c_str(), nullptr, nullptr) < 0) { err = "FFmpeg cannot open " + baseName(path); return false; }
    if (avformat_find_stream_info(d.ic, nullptr) < 0) { err = "FFmpeg cannot read the streams of " + baseName(path); return false; }
    d.si = av_find_best_stream(d.ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (d.si < 0) { err = baseName(path) + " has no picture"; return false; }
    d.st = d.ic->streams[d.si];
    const AVCodec* codec = avcodec_find_decoder(d.st->codecpar->codec_id);
    d.cx = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!d.cx || avcodec_parameters_to_context(d.cx, d.st->codecpar) < 0 || avcodec_open2(d.cx, codec, nullptr) < 0) { err = "FFmpeg has no decoder for the picture of " + baseName(path); return false; }
    d.fr = av_frame_alloc(); d.cur = av_frame_alloc(); d.pend = av_frame_alloc(); d.pk = av_packet_alloc();
    d.t0 = d.st->start_time != AV_NOPTS_VALUE ? d.st->start_time : 0;
    const AVRational fr = av_guess_frame_rate(d.ic, d.st, nullptr);
    if (fr.num > 0 && fr.den > 0) d.fps = std::max(1.0, std::min(120.0, av_q2d(fr)));
    const char* fn = d.ic->iformat && d.ic->iformat->name ? d.ic->iformat->name : "";
    d.still = strstr(fn, "image") || strstr(fn, "_pipe") || d.st->nb_frames == 1 || d.ic->duration <= 0;
    d.dur = d.still || d.ic->duration <= 0 ? 0 : (double)d.ic->duration / AV_TIME_BASE;
    if (!d.seek(0)) { err = "FFmpeg could not decode a picture from " + baseName(path); return false; }
    return true;
}

bool SynthMediaVideo::isStill() const { return p_->still; }
double SynthMediaVideo::duration() const { return p_->dur; }
int SynthMediaVideo::width() const { return p_->cx ? p_->cx->width : 0; }
int SynthMediaVideo::height() const { return p_->cx ? p_->cx->height : 0; }

bool SynthMediaVideo::rgb(double t, int dw, int dh, std::vector<uint8_t>& out) {
    Impl& d = *p_;
    if (dw < 2 || dh < 2 || !d.seek(t)) return false;
    out.assign((size_t)dw * dh * 3, 0);
    int ox, oy, w, h;
    d.fit(dw, dh, ox, oy, w, h);
    d.sws = sws_getCachedContext(d.sws, d.cur->width, d.cur->height, (AVPixelFormat)d.cur->format, w, h, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!d.sws) return false;
    uint8_t* dst[4] = {out.data() + ((size_t)oy * dw + ox) * 3, nullptr, nullptr, nullptr};
    int ls[4] = {dw * 3, 0, 0, 0};
    sws_scale(d.sws, d.cur->data, d.cur->linesize, 0, d.cur->height, dst, ls);
    return true;
}

bool SynthMediaVideo::yuv420(double t, int dw, int dh, uint8_t* y, int ys, uint8_t* u, int us, uint8_t* v, int vs) {
    Impl& d = *p_;
    if (dw < 2 || dh < 2 || !d.seek(t)) return false;
    for (int r = 0; r < dh; r++) memset(y + (size_t)r * ys, 16, (size_t)dw);
    for (int r = 0; r < dh / 2; r++) { memset(u + (size_t)r * us, 128, (size_t)dw / 2); memset(v + (size_t)r * vs, 128, (size_t)dw / 2); }
    int ox, oy, w, h;
    d.fit(dw, dh, ox, oy, w, h);
    d.sws = sws_getCachedContext(d.sws, d.cur->width, d.cur->height, (AVPixelFormat)d.cur->format, w, h, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!d.sws) return false;
    uint8_t* dst[4] = {y + (size_t)oy * ys + ox, u + (size_t)(oy / 2) * us + ox / 2, v + (size_t)(oy / 2) * vs + ox / 2, nullptr};
    int ls[4] = {ys, us, vs, 0};
    sws_scale(d.sws, d.cur->data, d.cur->linesize, 0, d.cur->height, dst, ls);
    return true;
}

// ---------------------------------------------------------------- sound
namespace {
bool decodeSound(const std::string& path, double maxSec, std::vector<float>& pcm, std::string& err) {
    AVFormatContext* ic = nullptr;
    if (avformat_open_input(&ic, path.c_str(), nullptr, nullptr) < 0) { err = "FFmpeg cannot open " + baseName(path); return false; }
    struct Close { AVFormatContext*& c; ~Close() { avformat_close_input(&c); } } closer{ic};
    if (avformat_find_stream_info(ic, nullptr) < 0) { err = "FFmpeg cannot read the streams of " + baseName(path); return false; }
    const int si = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (si < 0) { err = baseName(path) + " has no sound"; return false; }
    const AVCodec* codec = avcodec_find_decoder(ic->streams[si]->codecpar->codec_id);
    AVCodecContext* cx = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!cx || avcodec_parameters_to_context(cx, ic->streams[si]->codecpar) < 0 || avcodec_open2(cx, codec, nullptr) < 0) {
        avcodec_free_context(&cx); err = "FFmpeg has no decoder for the sound of " + baseName(path); return false;
    }
    SwrContext* swr = nullptr;
    AVChannelLayout out; av_channel_layout_default(&out, 2);
    AVChannelLayout in = cx->ch_layout;
    if (in.nb_channels <= 0) av_channel_layout_default(&in, 2);
    if (swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_FLT, 48000, &in, cx->sample_fmt, cx->sample_rate, 0, nullptr) < 0 || !swr || swr_init(swr) < 0) {
        swr_free(&swr); avcodec_free_context(&cx); err = "FFmpeg cannot resample the sound of " + baseName(path); return false;
    }
    AVFrame* fr = av_frame_alloc(); AVPacket* pk = av_packet_alloc();
    const size_t cap = (size_t)(maxSec * 48000.0) * 2;
    std::vector<float> tmp;
    auto take = [&](const uint8_t** inb, int n) {
        const int oc = swr_get_out_samples(swr, n) + 64;
        tmp.assign((size_t)oc * 2, 0.f);
        uint8_t* o[1] = {(uint8_t*)tmp.data()};
        const int got = swr_convert(swr, o, oc, inb, n);
        if (got > 0) pcm.insert(pcm.end(), tmp.begin(), tmp.begin() + (size_t)got * 2);
    };
    auto drain = [&]() { while (avcodec_receive_frame(cx, fr) >= 0) { take((const uint8_t**)fr->extended_data, fr->nb_samples); av_frame_unref(fr); } };
    while (pcm.size() < cap && av_read_frame(ic, pk) >= 0) {
        if (pk->stream_index == si && avcodec_send_packet(cx, pk) >= 0) drain();
        av_packet_unref(pk);
    }
    if (pcm.size() < cap) { avcodec_send_packet(cx, nullptr); drain(); take(nullptr, 0); }
    if (pcm.size() > cap) pcm.resize(cap);
    av_frame_free(&fr); av_packet_free(&pk); swr_free(&swr); avcodec_free_context(&cx);
    av_channel_layout_uninit(&out);
    if (pcm.size() < 2 * 480) { err = baseName(path) + " has no usable sound"; return false; }
    return true;
}
} // namespace

bool SynthMediaAudio::load(const SynthMedia& m, double maxSeconds, std::string& err) {
    pcm_.clear(); pos_ = 0;
    const std::string& p = !m.audio.empty() ? m.audio : (m.kind == 2 ? m.path : std::string());
    if (p.empty()) { err = "no sound file"; return false; }
    if (!decodeSound(p, std::max(1.0, maxSeconds), pcm_, err)) { pcm_.clear(); return false; }
    return true;
}

int SynthMediaAudio::read(float* st, int frames) {
    if (pcm_.empty()) { std::fill(st, st + (size_t)frames * 2, 0.f); return frames; }
    for (int i = 0; i < frames; i++) {
        st[2 * i] = pcm_[pos_]; st[2 * i + 1] = pcm_[pos_ + 1];
        pos_ += 2; if (pos_ >= pcm_.size()) pos_ = 0;
    }
    return frames;
}

float SynthMediaAudio::mono(double t) const {
    if (pcm_.empty()) return 0.f;
    const size_t n = pcm_.size() / 2;
    double x = std::fmod(t * 48000.0, (double)n);
    if (x < 0) x += (double)n;
    const size_t i = (size_t)x, j = (i + 1) % n;
    const float f = (float)(x - (double)i);
    const float a = 0.5f * (pcm_[2 * i] + pcm_[2 * i + 1]), b = 0.5f * (pcm_[2 * j] + pcm_[2 * j + 1]);
    return a + (b - a) * f;
}

void SynthMediaAudio::setPeak(float peak) {
    float mx = 0.f;
    for (float v : pcm_) mx = std::max(mx, std::fabs(v));
    if (mx > 1e-6f) for (float& v : pcm_) v *= peak / mx;
}

// ---------------------------------------------------------------- shared copies for the generators
std::shared_ptr<SynthMediaAudio> synthMediaAudioFor(const SynthConfig& cfg, double maxSeconds) {
    const SynthMedia m = synthMediaOf(cfg);
    if (m.audio.empty() && !(m.kind == 2 && !m.path.empty())) return nullptr;
    auto a = std::make_shared<SynthMediaAudio>();
    std::string err;
    if (!a->load(m, maxSeconds, err)) {
        synthMediaSetStatus("Could not use the sound: " + err + ". The built-in sound is used.");
        DIAG(773241, "The synthetic content sound could not be loaded (%s), so the built-in sound is used.", err.c_str());
        return nullptr;
    }
    synthMediaSetStatus("Carrying the sound of " + baseName(!m.audio.empty() ? m.audio : m.path) + ".");
    return a;
}

std::shared_ptr<SynthMediaVideo> synthMediaVideoFor(const SynthConfig& cfg) {
    const SynthMedia m = synthMediaOf(cfg);
    if (!m.kind || m.path.empty()) return nullptr;
    auto v = std::make_shared<SynthMediaVideo>();
    std::string err;
    if (!v->open(m.path, err)) {
        synthMediaSetStatus("Could not use the picture: " + err + ". The built-in picture is used.");
        DIAG(237193, "The synthetic content picture could not be loaded (%s), so the built-in picture is used.", err.c_str());
        return nullptr;
    }
    synthMediaSetStatus("Carrying " + baseName(m.path) + ".");
    return v;
}

std::vector<uint8_t> synthMediaPicture(const SynthConfig& cfg, int w, int h) {
    auto v = synthMediaVideoFor(cfg);
    std::vector<uint8_t> rgb;
    if (!v || !v->rgb(0, w, h, rgb)) return {};
    return rgb;
}

} // namespace dect2
