// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/wb/wb_source.h"
#include "dect2/source/iq_file.h"
#include "dect2/source/bluefile.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace dect2 {
namespace wb {

namespace {

std::string lowerExt(const std::string& p) {
    const size_t d = p.find_last_of('.');
    if (d == std::string::npos) return "";
    std::string e = p.substr(d + 1);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

std::string baseName(const std::string& p) {
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

size_t bytesPer(FileFormat f) {
    switch (f) { case FileFormat::CS8: case FileFormat::CU8: return 2; case FileFormat::CI16: case FileFormat::CU16: return 4; default: return 8; }
}

const char* fmtName(FileFormat f) {
    switch (f) { case FileFormat::CS8: return "cs8"; case FileFormat::CU8: return "cu8"; case FileFormat::CI16: return "ci16"; case FileFormat::CU16: return "cu16"; default: return "cf32"; }
}

// ---- raw IQ file --------------------------------------------------------------------------------------------------------------

class RawSource : public Source {
public:
    RawSource(FILE* f, FileFormat fmt, bool big, uint64_t off, uint64_t bytes, double rate) : f_(f), fmt_(fmt), big_(big), off_(off), rate_(rate) {
        n_ = bytes / bytesPer(fmt);
    }
    ~RawSource() override { if (f_) fclose(f_); }
    uint64_t length() const override { return n_; }
    double rate() const override { return rate_; }
    size_t read(uint64_t start, size_t n, cf32* out) const override {
        if (start >= n_) return 0;
        n = (size_t)std::min<uint64_t>(n, n_ - start);
        const size_t bp = bytesPer(fmt_);
        std::vector<uint8_t> buf(n * bp);
        {
            std::lock_guard<std::mutex> lk(mu_);
#ifdef _WIN32
            if (_fseeki64(f_, (long long)(off_ + start * bp), SEEK_SET) != 0) return 0;
#else
            if (fseeko(f_, (off_t)(off_ + start * bp), SEEK_SET) != 0) return 0;
#endif
            const size_t got = fread(buf.data(), 1, buf.size(), f_) / bp;
            if (got < n) n = got;
        }
        const uint8_t* p = buf.data();
        switch (fmt_) {
        case FileFormat::CS8: for (size_t i = 0; i < n; i++) out[i] = cf32((int8_t)p[2 * i] * (1.f / 128), (int8_t)p[2 * i + 1] * (1.f / 128)); break;
        case FileFormat::CU8: for (size_t i = 0; i < n; i++) out[i] = cf32((p[2 * i] - 127.5f) * (1.f / 127.5f), (p[2 * i + 1] - 127.5f) * (1.f / 127.5f)); break;
        case FileFormat::CI16: case FileFormat::CU16: {
            const bool u = fmt_ == FileFormat::CU16;
            for (size_t i = 0; i < n; i++) {
                uint16_t a, b;
                if (big_) { a = (uint16_t)(p[4 * i] << 8 | p[4 * i + 1]); b = (uint16_t)(p[4 * i + 2] << 8 | p[4 * i + 3]); }
                else { a = (uint16_t)(p[4 * i] | p[4 * i + 1] << 8); b = (uint16_t)(p[4 * i + 2] | p[4 * i + 3] << 8); }
                out[i] = u ? cf32(((int)a - 32768) * (1.f / 32768), ((int)b - 32768) * (1.f / 32768)) : cf32((int16_t)a * (1.f / 32768), (int16_t)b * (1.f / 32768));
            }
            break;
        }
        default:
            if (!big_) memcpy((void*)out, p, n * 8);
            else for (size_t i = 0; i < n; i++) {
                uint32_t a, b;
                a = (uint32_t)p[8 * i] << 24 | (uint32_t)p[8 * i + 1] << 16 | (uint32_t)p[8 * i + 2] << 8 | p[8 * i + 3];
                b = (uint32_t)p[8 * i + 4] << 24 | (uint32_t)p[8 * i + 5] << 16 | (uint32_t)p[8 * i + 6] << 8 | p[8 * i + 7];
                float fa, fb; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
                out[i] = cf32(fa, fb);
            }
            break;
        }
        for (size_t i = 0; i < n; i++) if (!std::isfinite(out[i].real()) || !std::isfinite(out[i].imag())) out[i] = cf32(0, 0);
        return n;
    }
    void describe(const std::string& path) {
        path_ = path;
        name_ = baseName(path);
        char b[160];
        snprintf(b, sizeof b, "%s IQ, %.3f Msps, %.2f s, %llu samples", fmtName(fmt_), rate_ / 1e6, (double)n_ / std::max(rate_, 1.0), (unsigned long long)n_);
        desc_ = b;
    }
private:
    FILE* f_;
    FileFormat fmt_;
    bool big_;
    uint64_t off_, n_ = 0;
    double rate_;
    mutable std::mutex mu_;
};

// ---- decoded audio / memory ---------------------------------------------------------------------------------------------------

class MemSource : public Source {
public:
    // data: interleaved floats, channels per frame (1 or 2)
    MemSource(std::vector<float> data, int channels, double rate, bool audio) : d_(std::move(data)), ch_(channels), rate_(rate), audio_(audio) {
        channels_ = channels;
        real_ = true;
        n_ = d_.size() / (size_t)ch_;
    }
    uint64_t length() const override { return n_; }
    double rate() const override { return rate_; }
    size_t read(uint64_t start, size_t n, cf32* out) const override {
        if (start >= n_) return 0;
        n = (size_t)std::min<uint64_t>(n, n_ - start);
        const float* p = d_.data() + start * ch_;
        if (ch_ == 1) for (size_t i = 0; i < n; i++) out[i] = cf32(p[i], 0);
        else if (iq_) for (size_t i = 0; i < n; i++) out[i] = cf32(p[2 * i], p[2 * i + 1]);
        else for (size_t i = 0; i < n; i++) out[i] = cf32(0.5f * (p[2 * i] + p[2 * i + 1]), 0);
        return n;
    }
    void setStereoIq(bool on) override { iq_ = on && ch_ == 2; real_ = !iq_; refresh(); }
    bool stereoIq() const override { return iq_; }
    bool isAudio() const override { return audio_; }
    void refresh() {
        char b[200];
        if (!audio_) { snprintf(b, sizeof b, "memory, %.3f Msps, %.2f s", rate_ / 1e6, (double)n_ / std::max(rate_, 1.0)); desc_ = b; return; }
        snprintf(b, sizeof b, "audio, %.0f Hz, %s, %.2f s%s", rate_, ch_ == 1 ? "mono (real signal)" : iq_ ? "stereo as I/Q (complex)" : "stereo mixed to mono (real signal)", (double)n_ / std::max(rate_, 1.0),
                 ch_ == 2 && !iq_ ? ", left + right averaged" : "");
        desc_ = b;
    }
    void setNames(const std::string& path, const std::string& name) { path_ = path; name_ = name; }
private:
    std::vector<float> d_;
    int ch_;
    double rate_;
    bool audio_, iq_ = false;
    uint64_t n_ = 0;
};

std::shared_ptr<Source> decodeAudio(const std::string& path, const LoadOptions& o, std::string& err) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) { err = "cannot open the file"; return nullptr; }
    if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); err = "not a recognised audio file"; return nullptr; }
    const AVCodec* codec = nullptr;
    const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (idx < 0 || !codec) { avformat_close_input(&fmt); err = "no audio stream in the file"; return nullptr; }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, fmt->streams[idx]->codecpar);
    if (avcodec_open2(ctx, codec, nullptr) < 0) { avcodec_free_context(&ctx); avformat_close_input(&fmt); err = "cannot open the audio decoder"; return nullptr; }
    const double rate = ctx->sample_rate;
    const int chIn = ctx->ch_layout.nb_channels;
    const int ch = chIn >= 2 ? 2 : 1;
    SwrContext* swr = nullptr;
    AVChannelLayout outL;
    av_channel_layout_default(&outL, ch);
    swr_alloc_set_opts2(&swr, &outL, AV_SAMPLE_FMT_FLT, ctx->sample_rate, &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate, 0, nullptr);
    if (!swr || swr_init(swr) < 0) {
        swr_free(&swr); avcodec_free_context(&ctx); avformat_close_input(&fmt);
        err = "cannot convert the audio";
        return nullptr;
    }
    std::vector<float> data;
    const int64_t dur = fmt->duration > 0 ? fmt->duration : 0;
    if (dur > 0) data.reserve((size_t)((double)dur / AV_TIME_BASE * rate * ch * 1.02));
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    std::vector<float> tmp;
    auto drain = [&]() {
        while (avcodec_receive_frame(ctx, fr) >= 0) {
            const int maxOut = swr_get_out_samples(swr, fr->nb_samples) + 64;
            tmp.resize((size_t)maxOut * ch);
            uint8_t* o2[1] = {(uint8_t*)tmp.data()};
            const int got = swr_convert(swr, o2, maxOut, (const uint8_t**)fr->extended_data, fr->nb_samples);
            if (got > 0) data.insert(data.end(), tmp.begin(), tmp.begin() + (size_t)got * ch);
            av_frame_unref(fr);
        }
    };
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == idx && avcodec_send_packet(ctx, pkt) >= 0) drain();
        av_packet_unref(pkt);
    }
    avcodec_send_packet(ctx, nullptr);
    drain();
    av_frame_free(&fr); av_packet_free(&pkt);
    swr_free(&swr); avcodec_free_context(&ctx); avformat_close_input(&fmt);
    av_channel_layout_uninit(&outL);
    for (float& v : data) if (!std::isfinite(v)) v = 0;
    if (data.empty()) { err = "the file holds no audio"; return nullptr; }
    auto s = std::make_shared<MemSource>(std::move(data), ch, rate, true);
    s->setNames(path, baseName(path));
    s->setStereoIq(o.stereoIq);
    s->setCenterHz(o.centerHz);
    s->refresh();
    return s;
}

} // namespace

bool isAudioExtension(const std::string& path) {
    const std::string e = lowerExt(path);
    static const char* k[] = {"wav", "wave", "mp3", "mp2", "flac", "ogg", "oga", "opus", "m4a", "aac", "wma", "aif", "aiff", "caf", "mka", "mp4", "webm", "amr", "au", "snd"};
    for (const char* x : k) if (e == x) return true;
    return false;
}

bool isRawIqExtension(const std::string& path) {
    const std::string e = lowerExt(path);
    static const char* k[] = {"cs8", "cu8", "cf32", "fc32", "cfile", "ci16", "cs16", "cu16", "sigmf", "sigmf-meta", "sigmf-data", "iq", "raw", "bin", "u8", "s8", "tmp", "prm", "blue", "bluefile"};
    for (const char* x : k) if (e == x) return true;
    return false;
}

std::shared_ptr<Source> Source::fromSamples(std::vector<cf32> x, double rate, bool real, double centerHz, const std::string& name) {
    // held as interleaved floats: real signals take one channel, complex ones two (read as I/Q)
    std::vector<float> d;
    int ch = 1;
    if (real) { d.resize(x.size()); for (size_t i = 0; i < x.size(); i++) d[i] = x[i].real(); }
    else { ch = 2; d.resize(x.size() * 2); for (size_t i = 0; i < x.size(); i++) { d[2 * i] = x[i].real(); d[2 * i + 1] = x[i].imag(); } }
    auto s = std::make_shared<MemSource>(std::move(d), ch, rate, false);
    if (!real) s->setStereoIq(true);
    s->setCenterHz(centerHz);
    s->setNames("", name);
    s->refresh();
    return s;
}

std::shared_ptr<Source> Source::open(const std::string& path, const LoadOptions& o, std::string& err) {
    err.clear();
    FILE* probe = fopen(path.c_str(), "rb");
    if (!probe) { err = "cannot open the file"; return nullptr; }
    fseek(probe, 0, SEEK_END);
    const long long size = ftell(probe);
    fclose(probe);
    if (size <= 0) { err = "the file is empty"; return nullptr; }
    const std::string ext = lowerExt(path);
    bool audio = o.audio;
    // an IQ file with a description (SigMF, IQ WAV): the rate and format come from it. A WAV is decoded unless it is large
    IqFileInfo info;
    std::string perr;
    const bool wavBig = ext == "wav" && size > 48ll * 1024 * 1024;
    if (!audio && (ext == "sigmf-meta" || ext == "sigmf-data" || ext == "sigmf" || wavBig || blue::isBlueExtension(path))) {
        if (probeIqFile(path, info, perr)) {
            FILE* f = fopen(info.dataPath.c_str(), "rb");
            if (!f) { err = "cannot open the data file " + info.dataPath; return nullptr; }
            fseek(f, 0, SEEK_END);
            const long long dsz = ftell(f);
            const uint64_t end = info.endBytes ? std::min<uint64_t>(info.endBytes, (uint64_t)dsz) : (uint64_t)dsz;
            const uint64_t bytes = end > info.offsetBytes ? end - info.offsetBytes : 0;
            const double rate = info.sampleRate > 0 ? info.sampleRate : o.sampleRate;
            if (rate <= 0) { fclose(f); err = "the file does not say its sample rate: type it in"; return nullptr; }
            auto s = std::make_shared<RawSource>(f, info.format, info.bigEndian, info.offsetBytes, bytes, rate);
            s->setCenterHz(info.centerHz > 0 ? info.centerHz : o.centerHz);
            s->describe(path);
            return s;
        }
        if (!perr.empty() && ext != "wav") { err = perr; return nullptr; }
    }
    if (audio || (isAudioExtension(path) && !isRawIqExtension(path))) {
        auto s = decodeAudio(path, o, err);
        if (s) return s;
        if (!audio && !isRawIqExtension(path) && ext != "wav" && ext != "mp3") return nullptr;
        return nullptr;
    }
    // raw samples
    FileFormat fmt = o.format >= 0 ? (FileFormat)o.format : (ext == "ci16" || ext == "cs16") ? FileFormat::CI16 : ext == "cu16" ? FileFormat::CU16 : guessFormat(path);
    double rate = o.sampleRate > 0 ? o.sampleRate : guessSampleRate(path);
    if (rate <= 0) { err = "this is a raw file: type its sample rate (and format) in"; return nullptr; }
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open the file"; return nullptr; }
    auto s = std::make_shared<RawSource>(f, fmt, o.bigEndian, 0, (uint64_t)size, rate);
    s->setCenterHz(o.centerHz);
    s->describe(path);
    if (s->length() == 0) { err = "the file holds no samples"; return nullptr; }
    return s;
}

} // namespace wb
} // namespace dect2
