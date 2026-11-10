// Audio capture: a ring for the microphone, a looped file (WAV / MP3 through FFmpeg), a tone, or silence.
#include "audioin_impl.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include "dect2/common/mathconst.h"
}

namespace dect2 {

namespace {
using dect2::math::kPi;

bool loadPcmWav(const std::string& path, std::vector<float>& stereo, int wantRate) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char riff[12];
    if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4)) { fclose(f); return false; }
    int16_t ch = 1, bits = 16;
    int32_t sr = 8000;
    std::vector<float> lft, rgt;   // two channels are kept apart (stereo I/Q files); one is copied to both, more are averaged
    for (;;) {
        char id[4];
        uint32_t n = 0;
        if (fread(id, 1, 4, f) != 4 || fread(&n, 4, 1, f) != 1) { fclose(f); return false; }
        if (!memcmp(id, "fmt ", 4)) {
            uint8_t fmt[16] = {};
            if (n < 16 || fread(fmt, 1, 16, f) != 16) { fclose(f); return false; }
            if (n > 16) fseek(f, (long)n - 16, SEEK_CUR);
            memcpy(&ch, fmt + 2, 2);
            memcpy(&sr, fmt + 4, 4);
            memcpy(&bits, fmt + 14, 2);
        } else if (!memcmp(id, "data", 4)) {
            if (bits != 16) { fclose(f); return false; }
            std::vector<int16_t> raw(n / 2);
            if (fread(raw.data(), 2, raw.size(), f) != raw.size()) { fclose(f); return false; }
            fclose(f);
            const int c = std::max(1, (int)ch);
            const size_t frames = raw.size() / (size_t)c;
            lft.resize(frames);
            rgt.resize(frames);
            for (size_t i = 0; i < frames; i++) {
                if (c == 2) { lft[i] = raw[i * 2] / 32768.f; rgt[i] = raw[i * 2 + 1] / 32768.f; continue; }
                float s = 0;
                for (int k = 0; k < c; k++) s += raw[i * (size_t)c + (size_t)k] / 32768.f;
                lft[i] = rgt[i] = s / (float)c;
            }
            break;
        } else fseek(f, (long)n, SEEK_CUR);
    }
    if (lft.empty()) return false;
    const double adv = (double)sr / (double)wantRate;
    stereo.resize((size_t)wantRate * 2 * lft.size() / (size_t)std::max(1, sr) + 4);
    size_t o = 0;
    double pos = 0;
    while (pos + 1 < (double)lft.size() && o + 1 < stereo.size()) {
        const size_t i = (size_t)pos, j = std::min(i + 1, lft.size() - 1);
        const float fr = (float)(pos - (double)i);
        stereo[o++] = lft[i] + (lft[j] - lft[i]) * fr;
        stereo[o++] = rgt[i] + (rgt[j] - rgt[i]) * fr;
        pos += adv;
    }
    stereo.resize(o);
    return o >= 2;
}

} // namespace

// An audio file opened with FFmpeg and decoded a packet at a time into interleaved stereo float at the session rate. A short file is
// decoded whole into memory; a longer one (more than kMemorySeconds) stays open and is decoded while it is sent, so it plays to its end
// and then loops like a short one (#471: a file longer than 10 minutes used to restart at the 10 minute mark).
struct AudioFileStream {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* ctx = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* pk = nullptr;
    AVFrame* fr = nullptr;
    AVChannelLayout outL{};
    int si = -1, wantRate = 48000;
    double seconds = 0;   // from the container, 0 when it does not say

    ~AudioFileStream() {
        if (fr) av_frame_free(&fr);
        if (pk) av_packet_free(&pk);
        if (swr) swr_free(&swr);
        if (ctx) avcodec_free_context(&ctx);
        if (fmt) avformat_close_input(&fmt);
        av_channel_layout_uninit(&outL);
    }
    bool open(const std::string& path, int rate) {
        wantRate = rate;
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) { fmt = nullptr; return false; }
        if (avformat_find_stream_info(fmt, nullptr) < 0) return false;
        si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (si < 0) return false;
        AVStream* st = fmt->streams[si];
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (!ctx || avcodec_parameters_to_context(ctx, st->codecpar) < 0 || avcodec_open2(ctx, codec, nullptr) < 0) return false;
        av_channel_layout_default(&outL, 2);
        swr_alloc_set_opts2(&swr, &outL, AV_SAMPLE_FMT_FLT, wantRate, &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate, 0, nullptr);
        if (!swr || swr_init(swr) < 0) return false;
        pk = av_packet_alloc();
        fr = av_frame_alloc();
        if (!pk || !fr) return false;
        if (st->duration > 0 && st->time_base.den > 0) seconds = (double)st->duration * av_q2d(st->time_base);
        else if (fmt->duration > 0) seconds = (double)fmt->duration / AV_TIME_BASE;
        return true;
    }
    void append(AVFrame* f, std::vector<float>& stereo) {
        uint8_t* out[1] = {nullptr};
        const int maxOut = (int)((int64_t)f->nb_samples * wantRate / std::max(1, ctx->sample_rate) + 256);
        const size_t at = stereo.size();
        stereo.resize(at + (size_t)maxOut * 2);
        out[0] = (uint8_t*)(stereo.data() + at);
        const int got = swr_convert(swr, out, maxOut, (const uint8_t**)f->extended_data, f->nb_samples);
        stereo.resize(at + (size_t)std::max(0, got) * 2);
    }
    // Decodes the next packet of the audio stream, appending to `stereo`. false at the end of the file (the decoder is drained then).
    bool decodeSome(std::vector<float>& stereo) {
        while (av_read_frame(fmt, pk) >= 0) {
            if (pk->stream_index != si) { av_packet_unref(pk); continue; }
            if (avcodec_send_packet(ctx, pk) == 0)
                while (avcodec_receive_frame(ctx, fr) == 0) append(fr, stereo);
            av_packet_unref(pk);
            return true;
        }
        avcodec_send_packet(ctx, nullptr);
        while (avcodec_receive_frame(ctx, fr) == 0) append(fr, stereo);
        return false;
    }
    // Back to the start for the next loop.
    bool rewind() {
        if (av_seek_frame(fmt, si, 0, AVSEEK_FLAG_BACKWARD) < 0 && av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_BYTE) < 0) return false;
        avcodec_flush_buffers(ctx);
        return true;
    }
};

namespace {

constexpr double kMemorySeconds = 600;   // a file up to this long is decoded whole into memory (48 kHz stereo float: 230 MB at most)

// Opens the file: a short one is decoded into `stereo` (and `stream` is left empty), a long one is returned in `stream`, ready to play.
bool loadFfmpeg(const std::string& path, std::vector<float>& stereo, int wantRate, std::shared_ptr<AudioFileStream>& stream) {
    stream.reset();
    stereo.clear();
    auto d = std::make_shared<AudioFileStream>();
    if (!d->open(path, wantRate)) return false;
    const size_t cap = (size_t)(wantRate * 2 * kMemorySeconds);
    if (d->seconds <= kMemorySeconds) {   // short, or the container does not say: decode until the end or the limit
        while (d->decodeSome(stereo))
            if (stereo.size() > cap) break;
        if (stereo.size() <= cap) return stereo.size() >= 2;
        stereo.clear();
        stereo.shrink_to_fit();
        if (!d->rewind()) return false;
    }
    stream = std::move(d);
    return true;
}

void measure(std::atomic<float>& peak, const float* s, int n, float gain) {
    float p = peak.load(std::memory_order_relaxed) * 0.92f;
    for (int i = 0; i < n * 2; i++) p = std::max(p, std::fabs(s[i] * gain));
    peak.store(std::min(p, 1.f), std::memory_order_relaxed);
}
}

bool AudioIn::Impl::setResample(double devRate, double wantRate) {
    useRs = false;
    if (std::fabs(devRate - wantRate) < 0.5) return true;
    if (!rs.configure(devRate, wantRate)) return false;
    rs.reset();
    useRs = true;
    return true;
}

void AudioIn::Impl::push(const float* in, uint32_t nFrames, int chans) {
    if (chans == 1 || useRs) {   // a mono device is copied to both channels; a rate change goes through the polyphase resampler
        rsIn.resize(nFrames);
        for (uint32_t i = 0; i < nFrames; i++) rsIn[i] = chans == 1 ? cf32(in[i], in[i]) : cf32(in[2 * i], in[2 * i + 1]);
        if (useRs) {
            rsOut.clear();
            rs.process(rsIn.data(), nFrames, rsOut);
            conv.resize(rsOut.size() * 2);
            for (size_t i = 0; i < rsOut.size(); i++) { conv[2 * i] = rsOut[i].real(); conv[2 * i + 1] = rsOut[i].imag(); }
        } else {
            conv.resize((size_t)nFrames * 2);
            for (uint32_t i = 0; i < nFrames; i++) { conv[2 * i] = rsIn[i].real(); conv[2 * i + 1] = rsIn[i].imag(); }
        }
        pushStereo(conv.data(), (uint32_t)(conv.size() / 2));
        return;
    }
    pushStereo(in, nFrames);
}

void AudioIn::Impl::pushStereo(const float* in, uint32_t nFrames) {
    const uint64_t wr = w.load(std::memory_order_relaxed), rd = r.load(std::memory_order_acquire);
    const int space = (int)(frames - (wr - rd));
    const int n = std::min((int)nFrames, std::max(0, space));
    if (n < (int)nFrames) overrun += nFrames - (uint32_t)n;
    const float g = gain.load(std::memory_order_relaxed);
    const size_t mask = frames - 1;
    for (int i = 0; i < n; i++) {
        const size_t k = (size_t)((wr + i) & mask) * 2;
        ring[k] = in[2 * i] * g;
        ring[k + 1] = in[2 * i + 1] * g;
    }
    w.store(wr + (uint64_t)n, std::memory_order_release);
    measure(peak, in, n, g);
}

AudioIn::AudioIn() : p_(new Impl) {
    p_->frames = 1 << 16;
    p_->ring.assign(p_->frames * 2, 0.f);
}
AudioIn::~AudioIn() { stop(); }

bool AudioIn::start(Kind k, int sampleRate, const std::string& pathOrDevice, double toneHz, std::string& err) {
    stop();
    rate_ = sampleRate > 0 ? sampleRate : 48000;
    kind_ = k;
    p_->toneHz = toneHz > 0 ? toneHz : 1000;
    p_->tph = 0;
    p_->file.clear();
    p_->filePos = 0;
    p_->stream.reset();
    p_->streamBuf.clear();
    p_->streamPos = 0;
    p_->w = 0;
    p_->r = 0;
    p_->peak = 0;
    p_->overrun = 0;
    p_->actualRate = 0;
    p_->useRs = false;
    if (k == Mic) {
        if (!audioInMicStart(p_.get(), rate_, pathOrDevice, err)) { kind_ = None; return false; }
        if (p_->actualRate > 0) rate_ = p_->actualRate;   // the card would not run at the rate asked for: the real one is reported
        running_ = true;
        return true;
    }
    if (k == File) {
        if (!loadFfmpeg(pathOrDevice, p_->file, rate_, p_->stream) && !loadPcmWav(pathOrDevice, p_->file, rate_)) {
            err = "could not open the audio file (WAV or MP3)";
            kind_ = None;
            return false;
        }
        running_ = true;
        return true;
    }
    if (k == Tone || k == Silence) {
        running_ = true;
        return true;
    }
    err = "no audio source";
    kind_ = None;
    return false;
}

void AudioIn::stop() {
    if (kind_ == Mic) audioInMicStop(p_.get());
    running_ = false;
    kind_ = None;
    p_->file.clear();
    p_->stream.reset();
    p_->streamBuf.clear();
    p_->streamPos = 0;
}

void AudioIn::setMuted(bool m) { p_->muted.store(m); }
void AudioIn::setGain(float g) { p_->gain.store(g); }
float AudioIn::peak() const { return p_->peak.load(); }
uint64_t AudioIn::overruns() const { return p_->overrun.load(); }
double AudioIn::fileSeconds() const {
    if (kind_ != File || rate_ <= 0) return 0.0;
    if (p_->stream) return p_->stream->seconds;
    return (double)(p_->file.size() / 2) / (double)rate_;
}

int AudioIn::readAvailable(float* stereo, int n) {
    if (n <= 0) return 0;
    if (kind_ != Mic) return read(stereo, n);   // a file, tone or silence is always ready
    const bool mute = p_->muted.load();
    const uint64_t wr = p_->w.load(std::memory_order_acquire), rd = p_->r.load(std::memory_order_relaxed);
    const int have = (int)std::min<uint64_t>(wr - rd, (uint64_t)n);
    const size_t mask = p_->frames - 1;
    for (int i = 0; i < have; i++) {
        const size_t k = (size_t)((rd + i) & mask) * 2;
        stereo[2 * i] = mute ? 0 : p_->ring[k];
        stereo[2 * i + 1] = mute ? 0 : p_->ring[k + 1];
    }
    p_->r.store(rd + (uint64_t)have, std::memory_order_release);
    return have;
}

int AudioIn::read(float* stereo, int n) {
    if (n <= 0) return 0;
    const float g = p_->gain.load();
    const bool mute = p_->muted.load();
    if (kind_ == File && p_->stream) {   // a long file, decoded as it goes; at its end it starts again
        AudioFileStream& d = *p_->stream;
        std::vector<float>& b = p_->streamBuf;
        int i = 0, emptyLoops = 0;
        while (i < n) {
            if (p_->streamPos + 1 >= b.size()) {
                b.clear();
                p_->streamPos = 0;
                if (!d.decodeSome(b) && b.empty()) {   // the end: loop (a file that gives nothing twice in a row is sent as silence)
                    if (++emptyLoops > 1 || !d.rewind()) break;
                    continue;
                }
                if (b.size() < 2) continue;
                emptyLoops = 0;
            }
            stereo[2 * i] = mute ? 0 : b[p_->streamPos] * g;
            stereo[2 * i + 1] = mute ? 0 : b[p_->streamPos + 1] * g;
            p_->streamPos += 2;
            i++;
        }
        for (; i < n; i++) stereo[2 * i] = stereo[2 * i + 1] = 0;
        measure(p_->peak, stereo, n, 1.f);
        return n;
    }
    if (kind_ == File && p_->file.size() >= 2) {   // (a file of one value would divide by zero below)
        for (int i = 0; i < n; i++) {
            const size_t k = p_->filePos % (p_->file.size() / 2);
            const float l = p_->file[k * 2] * g, r = p_->file[k * 2 + 1] * g;
            stereo[2 * i] = mute ? 0 : l;
            stereo[2 * i + 1] = mute ? 0 : r;
            p_->filePos++;
        }
        measure(p_->peak, stereo, n, 1.f);
        return n;
    }
    if (kind_ == Tone) {
        const double w = 2 * kPi * p_->toneHz / rate_;
        for (int i = 0; i < n; i++) {
            const float s = mute ? 0.f : 0.5f * (float)std::sin(p_->tph) * g;
            stereo[2 * i] = stereo[2 * i + 1] = s;
            p_->tph += w;
            if (p_->tph > 2 * kPi) p_->tph -= 2 * kPi;
        }
        measure(p_->peak, stereo, n, 1.f);
        return n;
    }
    if (kind_ == Silence) {
        memset(stereo, 0, (size_t)n * 2 * sizeof(float));
        p_->peak.store(p_->peak.load() * 0.9f);
        return n;
    }
    if (kind_ == Mic) {
        const uint64_t wr = p_->w.load(std::memory_order_acquire), rd = p_->r.load(std::memory_order_relaxed);
        const int have = (int)std::min<uint64_t>(wr - rd, (uint64_t)n);
        const size_t mask = p_->frames - 1;
        for (int i = 0; i < have; i++) {
            const size_t k = (size_t)((rd + i) & mask) * 2;
            stereo[2 * i] = mute ? 0 : p_->ring[k];
            stereo[2 * i + 1] = mute ? 0 : p_->ring[k + 1];
        }
        for (int i = have; i < n; i++) stereo[2 * i] = stereo[2 * i + 1] = 0;
        p_->r.store(rd + (uint64_t)have, std::memory_order_release);
        return n;
    }
    memset(stereo, 0, (size_t)n * 2 * sizeof(float));
    return n;
}

std::vector<AudioInputDevice> listAudioInputs() {
    std::vector<AudioInputDevice> v;
    v.push_back({"", "Default input"});
    auto rest = audioInListMic();
    v.insert(v.end(), rest.begin(), rest.end());
    return v;
}

} // namespace dect2
