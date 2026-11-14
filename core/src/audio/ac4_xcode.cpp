// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AC-4 access units -> AC-3 frames: decoded with the optional AC-4 library (ac4_dec.h), mixed to stereo or 5.1 and encoded by the FFmpeg
// inside OnAir, so that the transport stream of the ATSC 3.0 receiver carries audio the player can decode.
#include "dect2/audio/ac4_dec.h"
#include "dect2/common/diag.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace dect2 {

struct Ac4ToAc3::Impl {
    Ac4Decoder dec;
    AVCodecContext* enc = nullptr;
    SwrContext* swr = nullptr;
    int swrInCh = 0, swrInRate = 0;
    int outCh = 2;
    std::string err;
    std::vector<std::vector<float>> q;   // planar samples waiting for a whole AC-3 frame
    int64_t anchor = 0;                  // 90 kHz time of the first sample counted in `consumed`
    int64_t consumed = 0;                // samples (48 kHz) taken by the encoder since the anchor
    int64_t queued = 0;                  // samples (48 kHz) put in the queue since the anchor
    bool anchored = false;
    AVPacket* pkt = nullptr;
    AVFrame* frm = nullptr;

    ~Impl() {
        if (swr) swr_free(&swr);
        if (enc) avcodec_free_context(&enc);
        if (pkt) av_packet_free(&pkt);
        if (frm) av_frame_free(&frm);
    }

    bool setupSwr(int inCh, int inRate) {
        if (swr) swr_free(&swr);
        AVChannelLayout in = {}, out = {};
        av_channel_layout_default(&in, inCh);
        av_channel_layout_copy(&out, &enc->ch_layout);
        const int r = swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_FLTP, enc->sample_rate, &in, AV_SAMPLE_FMT_FLT, inRate, 0, nullptr);
        av_channel_layout_uninit(&in);
        av_channel_layout_uninit(&out);
        if (r < 0 || !swr || swr_init(swr) < 0) { if (swr) swr_free(&swr); err = "cannot set up the channel mixer"; return false; }
        swrInCh = inCh; swrInRate = inRate;
        return true;
    }

    void encodeQueued(std::vector<Ac4TsPacket>& out) {
        const int fs = enc->frame_size > 0 ? enc->frame_size : 1536;
        while ((int)q[0].size() >= fs) {
            av_frame_unref(frm);
            frm->nb_samples = fs;
            frm->format = AV_SAMPLE_FMT_FLTP;
            frm->sample_rate = enc->sample_rate;
            av_channel_layout_copy(&frm->ch_layout, &enc->ch_layout);
            if (av_frame_get_buffer(frm, 0) < 0) return;
            for (int c = 0; c < outCh; c++) memcpy(frm->extended_data[c], q[c].data(), (size_t)fs * sizeof(float));
            for (int c = 0; c < outCh; c++) q[c].erase(q[c].begin(), q[c].begin() + fs);
            if (avcodec_send_frame(enc, frm) < 0) return;
            const int64_t t0 = anchor + av_rescale(consumed, 90000, enc->sample_rate);
            consumed += fs;
            while (avcodec_receive_packet(enc, pkt) >= 0) {
                Ac4TsPacket o;
                o.data.assign(pkt->data, pkt->data + pkt->size);
                o.pts90 = t0;
                o.samples = fs;
                out.push_back(std::move(o));
                av_packet_unref(pkt);
            }
        }
    }
};

Ac4ToAc3::Ac4ToAc3() : p_(new Impl) {}
Ac4ToAc3::~Ac4ToAc3() = default;

int Ac4ToAc3::outChannels() const { return p_->outCh; }
int Ac4ToAc3::outSampleRate() const { return 48000; }
int Ac4ToAc3::outBitRate() const { return p_->outCh >= 6 ? 448000 : 192000; }
std::string Ac4ToAc3::error() const { return p_->err.empty() ? p_->dec.error() : p_->err; }

bool Ac4ToAc3::open(int channelsHint) {
    Impl& I = *p_;
    if (!I.dec.open()) return false;
    const AVCodec* c = avcodec_find_encoder_by_name("ac3");
    if (!c) { DIAG(235471, "This FFmpeg has no AC-3 encoder, so AC-4 audio cannot be converted for a receiver."); I.err = "no AC-3 encoder in this FFmpeg"; return false; }
    I.enc = avcodec_alloc_context3(c);
    if (!I.enc) return false;
    I.outCh = channelsHint >= 6 ? 6 : 2;
    I.enc->sample_rate = 48000;
    I.enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    I.enc->bit_rate = I.outCh >= 6 ? 448000 : 192000;
    av_channel_layout_default(&I.enc->ch_layout, I.outCh);
    I.enc->time_base = AVRational{1, 48000};
    if (avcodec_open2(I.enc, c, nullptr) < 0) { avcodec_free_context(&I.enc); I.err = "the AC-3 encoder would not open"; return false; }
    I.pkt = av_packet_alloc();
    I.frm = av_frame_alloc();
    I.q.assign((size_t)I.outCh, {});
    return I.pkt && I.frm;
}

void Ac4ToAc3::push(const uint8_t* data, size_t size, int64_t pts90, std::vector<Ac4TsPacket>& out) {
    Impl& I = *p_;
    if (!I.enc) return;
    std::vector<float> pcm;
    int ch = 0, rate = 0;
    const int n = I.dec.decode(data, size, pcm, ch, rate);
    ac4NoteDecode(n >= 0, ch, rate);
    if (n <= 0) return;
    if (!I.swr || I.swrInCh != ch || I.swrInRate != rate) { if (!I.setupSwr(ch, rate)) return; }
    // the time line: the first access unit sets it; a jump of more than 200 ms (a splice, a lost segment) sets it again
    const int64_t expect = I.anchored ? I.anchor + av_rescale(I.queued, 90000, I.enc->sample_rate) : pts90;
    if (!I.anchored || std::llabs(pts90 - expect) > 90 * 200) {
        I.anchored = true;
        I.anchor = pts90;
        I.consumed = I.queued = 0;
        for (auto& v : I.q) v.clear();
    }
    const int maxOut = swr_get_out_samples(I.swr, n) + 64;
    std::vector<std::vector<float>> tmp((size_t)I.outCh, std::vector<float>((size_t)maxOut));
    std::vector<uint8_t*> planes((size_t)I.outCh);
    for (int c = 0; c < I.outCh; c++) planes[c] = (uint8_t*)tmp[c].data();
    const uint8_t* in[1] = {(const uint8_t*)pcm.data()};
    const int got = swr_convert(I.swr, planes.data(), maxOut, in, n);
    if (got <= 0) return;
    for (int c = 0; c < I.outCh; c++) I.q[c].insert(I.q[c].end(), tmp[c].begin(), tmp[c].begin() + got);
    I.queued += got;
    I.encodeQueued(out);
}

} // namespace dect2
