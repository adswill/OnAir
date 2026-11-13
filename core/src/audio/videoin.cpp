// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "videoin_impl.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#include "dect2/atv/atv_std.h"
#include "dect2/tv/videogen.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {

void VideoIn::Impl::setRgb(int nw, int nh, const uint8_t* packed) {
    if (nw <= 0 || nh <= 0 || !packed) return;
    std::lock_guard<std::mutex> lk(mu);
    w = nw; h = nh;
    rgb.assign(packed, packed + (size_t)nw * (size_t)nh * 3);
    seq.fetch_add(1);
}

namespace {
bool sampleRgb(const VideoIn::Impl& I, int dw, int dh, int x, int y, float& r, float& g, float& b) {
    if (I.w <= 0 || I.h <= 0 || I.rgb.empty()) return false;
    const float fx = (x + 0.5f) * (float)I.w / (float)dw - 0.5f;
    const float fy = (y + 0.5f) * (float)I.h / (float)dh - 0.5f;
    const int x0 = std::max(0, std::min(I.w - 1, (int)std::floor(fx)));
    const int y0 = std::max(0, std::min(I.h - 1, (int)std::floor(fy)));
    const int x1 = std::min(I.w - 1, x0 + 1), y1 = std::min(I.h - 1, y0 + 1);
    const float tx = std::max(0.f, std::min(1.f, fx - x0)), ty = std::max(0.f, std::min(1.f, fy - y0));
    auto pix = [&](int px, int py) -> const uint8_t* { return I.rgb.data() + ((size_t)py * (size_t)I.w + (size_t)px) * 3; };
    const uint8_t *a = pix(x0, y0), *c = pix(x1, y0), *d = pix(x0, y1), *e = pix(x1, y1);
    auto lerp = [&](int k) {
        const float p = a[k] + (c[k] - a[k]) * tx, q = d[k] + (e[k] - d[k]) * tx;
        return (p + (q - p) * ty) / 255.f;
    };
    r = lerp(0); g = lerp(1); b = lerp(2);
    return true;
}

bool openFileLoop(VideoIn::Impl* I, const std::string& path, std::string& err) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) { err = "could not open this video file"; return false; }
    if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); err = "could not read this video file"; return false; }
    const int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (si < 0) { avformat_close_input(&fmt); err = "this file has no video"; return false; }
    AVStream* st = fmt->streams[si];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext* ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!ctx || avcodec_parameters_to_context(ctx, st->codecpar) < 0 || avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx); avformat_close_input(&fmt); err = "this video codec is not available"; return false;
    }
    SwsContext* sws = nullptr;
    AVFrame* fr = av_frame_alloc();
    AVPacket* pk = av_packet_alloc();
    std::vector<uint8_t> rgb;
    const AVRational rs = av_guess_frame_rate(fmt, st, nullptr);
    const double nominalDur = rs.num > 0 && rs.den > 0 ? av_q2d(av_inv_q(rs)) : 0.04;
    const double tb = av_q2d(st->time_base);
    I->wallStart = std::chrono::steady_clock::now();
    I->clock = -1;
    { std::lock_guard<std::mutex> lk(I->mu); I->pubPts = -1; I->pubDur = 0; I->fileDone = false; }
    I->fileTh = dect2::namedThread("onair-vidin", [=]() mutable {
        auto closeAll = [&] {
            if (sws) sws_freeContext(sws);
            av_frame_free(&fr); av_packet_free(&pk);
            avcodec_free_context(&ctx); avformat_close_input(&fmt);
            std::lock_guard<std::mutex> lk(I->mu);
            I->fileDone = true;
            I->cv.notify_all();
        };
        double loopBase = 0, zero = 0, lastRel = -nominalDur;   // the frame times go on rising over the file's loops
        bool haveZero = false;
        while (!I->stop.load()) {
            int rc = av_read_frame(fmt, pk);
            if (rc < 0) {
                av_seek_frame(fmt, si, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(ctx);
                loopBase += lastRel + nominalDur;
                lastRel = -nominalDur;
                if (av_read_frame(fmt, pk) < 0) break;
            }
            if (pk->stream_index != si) { av_packet_unref(pk); continue; }
            if (avcodec_send_packet(ctx, pk) < 0) { av_packet_unref(pk); continue; }
            av_packet_unref(pk);
            while (avcodec_receive_frame(ctx, fr) >= 0) {
                const int64_t ts = fr->best_effort_timestamp;
                double rel = lastRel + nominalDur;
                if (ts != AV_NOPTS_VALUE) { if (!haveZero) { zero = (double)ts * tb; haveZero = true; } rel = (double)ts * tb - zero; }
                if (rel < lastRel) rel = lastRel + nominalDur;   // a timestamp jump backwards in a damaged file
                lastRel = rel;
                const double pts = loopBase + rel;
                {   // hold the frame until its time comes
                    std::unique_lock<std::mutex> lk(I->mu);
                    while (!I->stop.load() && I->clockNow() + 1e-4 < pts) I->cv.wait_for(lk, std::chrono::milliseconds(5));
                }
                if (I->stop.load()) break;
                if (!sws || I->w != fr->width || I->h != fr->height) {
                    if (sws) sws_freeContext(sws);
                    sws = sws_getContext(fr->width, fr->height, (AVPixelFormat)fr->format, fr->width, fr->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    rgb.resize((size_t)fr->width * (size_t)fr->height * 3);
                }
                if (sws) {
                    uint8_t* dst[4] = {rgb.data(), nullptr, nullptr, nullptr};
                    int ls[4] = {fr->width * 3, 0, 0, 0};
                    sws_scale(sws, fr->data, fr->linesize, 0, fr->height, dst, ls);
                    I->setRgb(fr->width, fr->height, rgb.data());
                }
                std::lock_guard<std::mutex> lk(I->mu);
                I->pubPts = pts; I->pubDur = nominalDur;
                I->cv.notify_all();
            }
        }
        closeAll();
    });
    return true;
}
}

VideoIn::VideoIn() : p_(std::make_unique<Impl>()) {}
VideoIn::~VideoIn() { stop(); }

int VideoIn::width() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->w; }
int VideoIn::height() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->h; }

bool VideoIn::start(Kind k, const std::string& pathOrDevice, std::string& err) {
    stop();
    kind_ = k;
    p_->stop = false;
    p_->seq = 0;
    p_->w = p_->h = 0;
    p_->rgb.clear();
    p_->genId = 0;
    if (k == File) {
        if (int gid = 0; vgen::parseGeneratorSpec(pathOrDevice, gid)) {   // the Digital video generator of that number, while it sends to the transmitter
            p_->genId = gid;
            Impl* impl = p_.get();
            vgen::setGeneratorFrameSink(gid, [impl](int w, int h, const uint8_t* rgb) { impl->setRgb(w, h, rgb); });
            running_ = true;
            return true;
        }
        if (pathOrDevice.empty()) { err = "choose a video file"; return false; }
        if (!openFileLoop(p_.get(), pathOrDevice, err)) return false;
        running_ = true;
        return true;
    }
    if (k == Camera) {
        if (!videoInCameraStart(p_.get(), pathOrDevice, err)) return false;
        running_ = true;
        return true;
    }
    err = "no video source";
    return false;
}

void VideoIn::stop() {
    p_->stop = true;
    if (p_->genId > 0) { vgen::setGeneratorFrameSink(p_->genId, {}); p_->genId = 0; }
    videoInCameraStop(p_.get());
    if (p_->fileTh.joinable()) p_->fileTh.join();
    running_ = false;
    kind_ = None;
}

void VideoIn::setClock(double seconds, int waitMs) {
    if (kind_ != File || !(seconds >= 0) || p_->genId > 0) return;
    p_->clock = seconds;
    std::unique_lock<std::mutex> lk(p_->mu);
    p_->cv.notify_all();
    if (waitMs > 0) p_->cv.wait_for(lk, std::chrono::milliseconds(waitMs), [&] { return p_->stop.load() || p_->fileDone || (p_->pubPts >= 0 && p_->pubPts + p_->pubDur > seconds); });
}

bool VideoIn::copyYuv(int dw, int dh, float* y, float* u, float* v) {
    if (dw <= 0 || dh <= 0 || !y) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->rgb.empty()) return false;
    for (int row = 0; row < dh; row++) {
        for (int col = 0; col < dw; col++) {
            float r, g, b;
            if (!sampleRgb(*p_, dw, dh, col, row, r, g, b)) return false;
            float Y, U, V;
            atvRgbToYuv(r, g, b, Y, U, V);
            const size_t o = (size_t)row * (size_t)dw + (size_t)col;
            y[o] = Y;
            if (u) u[o] = U;
            if (v) v[o] = V;
        }
    }
    return true;
}

bool VideoIn::copyYuv420(int dw, int dh, uint8_t* y, uint8_t* u, uint8_t* v, int yStride, int uStride, int vStride) {
    if (dw <= 0 || dh <= 0 || !y || !u || !v) return false;
    if (yStride <= 0) yStride = dw;
    if (uStride <= 0) uStride = dw / 2;
    if (vStride <= 0) vStride = dw / 2;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->rgb.empty()) return false;
    for (int row = 0; row < dh; row++) {
        for (int col = 0; col < dw; col++) {
            float r, g, b;
            if (!sampleRgb(*p_, dw, dh, col, row, r, g, b)) return false;
            const int Y = std::max(16, std::min(235, (int)std::lround(16 + 219 * (0.299f * r + 0.587f * g + 0.114f * b))));
            y[(size_t)row * (size_t)yStride + (size_t)col] = (uint8_t)Y;
            if (!(col & 1) && !(row & 1)) {
                const float yy = 0.299f * r + 0.587f * g + 0.114f * b;
                const int uu = std::max(16, std::min(240, (int)std::lround(128 + 112 * (b - yy) / 0.886f)));
                const int vv = std::max(16, std::min(240, (int)std::lround(128 + 112 * (r - yy) / 0.701f)));
                u[(size_t)(row / 2) * (size_t)uStride + (size_t)(col / 2)] = (uint8_t)uu;
                v[(size_t)(row / 2) * (size_t)vStride + (size_t)(col / 2)] = (uint8_t)vv;
            }
        }
    }
    return true;
}

std::vector<VideoInputDevice> listVideoInputs() { return videoInListCameras(); }

} // namespace dect2
