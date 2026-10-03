// DAB / DAB+ audio: superframe assembly (Reed-Solomon, firecode, access units) and decoding with FFmpeg.
#include "dect2/dab.h"
#include "dect2/audioout.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>

namespace dect2 {

struct DabAudio::Impl {
    mutable std::mutex mu;
    AudioOut out;
    bool silent = false, outStarted = false;
    DabAudioStats st;
    std::function<void(const uint8_t*, int, uint8_t)> auTap;
    float volume = 1.f; bool muted = false;

    // DAB+ superframe assembly
    std::deque<std::vector<uint8_t>> frames;
    bool synced = false;
    int since = 0, consecBad = 0;
    bool dabPlus = false;
    int bitrate = 0;

    // decoder
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    SwrContext* swr = nullptr;
    int swrRate = 0, swrFmt = -1, swrCh = 0;
    uint8_t openHeader = 0xFF;
    std::vector<float> pcm;

    ~Impl() { closeDecoder(); if (pkt) av_packet_free(&pkt); if (frame) av_frame_free(&frame); }

    void closeDecoder() {
        if (ctx) avcodec_free_context(&ctx);
        if (swr) swr_free(&swr);
        swrRate = 0; swrFmt = -1; swrCh = 0;
        openHeader = 0xFF;
    }

    // AudioSpecificConfig for DAB+ (AAC-LC with 960-sample frames, optionally with SBR / PS signalled explicitly)
    static std::vector<uint8_t> makeAsc(int dac, int sbr, int stereo, int ps) {
        const int coreRate = sbr ? (dac ? 24000 : 16000) : (dac ? 48000 : 32000);
        auto freqIdx = [](int hz) { return hz == 48000 ? 3 : hz == 32000 ? 5 : hz == 24000 ? 6 : 8; };
        const int chan = (stereo && !ps) ? 2 : 1;
        uint32_t v = 0; int bits = 0;
        auto put = [&](uint32_t x, int n) { v = (v << n) | (x & ((1u << n) - 1)); bits += n; };
        if (!sbr) { put(2, 5); put((uint32_t)freqIdx(coreRate), 4); put((uint32_t)chan, 4); }
        else {
            put(ps ? 29 : 5, 5); put((uint32_t)freqIdx(coreRate), 4); put((uint32_t)chan, 4);
            put((uint32_t)freqIdx(coreRate * 2), 4); put(2, 5);
        }
        put(1, 1); put(0, 1); put(0, 1);   // frameLengthFlag = 1 (960), dependsOnCoreCoder, extensionFlag
        std::vector<uint8_t> out((size_t)((bits + 7) / 8), 0);
        v <<= (out.size() * 8 - (size_t)bits);
        for (size_t i = 0; i < out.size(); i++) out[out.size() - 1 - i] = (uint8_t)(v >> (8 * i));
        return out;
    }

    bool openAac(uint8_t hdr) {
        closeDecoder();
        const int dac = (hdr >> 6) & 1, sbr = (hdr >> 5) & 1, stereo = (hdr >> 4) & 1, ps = (hdr >> 3) & 1;
        codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
        if (!codec) return false;
        ctx = avcodec_alloc_context3(codec);
        const std::vector<uint8_t> asc = makeAsc(dac, sbr, stereo, ps);
        ctx->extradata = (uint8_t*)av_mallocz(asc.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        std::memcpy(ctx->extradata, asc.data(), asc.size());
        ctx->extradata_size = (int)asc.size();
        if (avcodec_open2(ctx, codec, nullptr) < 0) { avcodec_free_context(&ctx); return false; }
        openHeader = hdr;
        st.codec = sbr && ps ? "HE-AAC v2" : sbr ? "HE-AAC" : "AAC-LC";
        st.channels = stereo ? 2 : 1;
        return true;
    }

    bool openMp2() {
        closeDecoder();
        codec = avcodec_find_decoder(AV_CODEC_ID_MP2);
        if (!codec) return false;
        ctx = avcodec_alloc_context3(codec);
        if (avcodec_open2(ctx, codec, nullptr) < 0) { avcodec_free_context(&ctx); return false; }
        st.codec = "MP2";
        return true;
    }

    void emit(const float* stereo, int frames) {
        st.pcmFrames += (uint64_t)frames;
        st.decoding = true;
        if (silent) return;
        if (!outStarted) {
            out.setStartThreshold(12000);
            if (out.start(48000)) outStarted = true;
            out.setVolume(volume); out.setMuted(muted);
        }
        if (outStarted) out.write(stereo, frames);
    }

    void decodePacket(const uint8_t* data, int n) {
        if (!ctx) return;
        if (!pkt) pkt = av_packet_alloc();
        if (!frame) frame = av_frame_alloc();
        if (av_new_packet(pkt, n) < 0) return;
        std::memcpy(pkt->data, data, (size_t)n);
        const int s = avcodec_send_packet(ctx, pkt);
        av_packet_unref(pkt);
        if (s < 0) return;
        while (avcodec_receive_frame(ctx, frame) == 0) {
            const int nb = frame->nb_samples, ch = frame->ch_layout.nb_channels;
            if (!swr || swrRate != frame->sample_rate || swrFmt != frame->format || swrCh != ch) {
                if (swr) swr_free(&swr);
                AVChannelLayout outL = AV_CHANNEL_LAYOUT_STEREO;
                swr_alloc_set_opts2(&swr, &outL, AV_SAMPLE_FMT_FLT, 48000, &frame->ch_layout, (AVSampleFormat)frame->format, frame->sample_rate, 0, nullptr);
                if (!swr || swr_init(swr) < 0) { if (swr) swr_free(&swr); av_frame_unref(frame); return; }
                swrRate = frame->sample_rate; swrFmt = frame->format; swrCh = ch;
                st.sampleRate = frame->sample_rate; st.channels = ch;
            }
            const int maxOut = (int)((int64_t)nb * 48000 / std::max(1, frame->sample_rate)) + 64;
            pcm.resize((size_t)maxOut * 2);
            uint8_t* o[1] = {(uint8_t*)pcm.data()};
            const int got = swr_convert(swr, o, maxOut, (const uint8_t**)frame->extended_data, nb);
            if (got > 0) emit(pcm.data(), got);
            av_frame_unref(frame);
        }
    }

    // ---- DAB+ superframe
    bool superframe(const std::deque<std::vector<uint8_t>>& frames) {
        const int fb = (int)frames.front().size();
        const int total = fb * 5, N = total / 120;
        if (total % 120 || N < 1 || N > 24) return false;
        std::vector<uint8_t> sf((size_t)total), payload((size_t)N * 110);
        for (int f = 0; f < 5; f++) std::memcpy(&sf[(size_t)f * fb], frames[(size_t)f].data(), (size_t)fb);
        int corrected = 0;
        for (int i = 0; i < N; i++) {
            uint8_t cw[120];
            for (int j = 0; j < 120; j++) cw[j] = sf[(size_t)j * N + i];
            const int r = dab::rsDecode120(cw);
            if (r < 0) return false;
            corrected += r;
            for (int j = 0; j < 110; j++) payload[(size_t)j * N + i] = cw[j];
        }
        if (dab::fireCode(&payload[2], 9) != (uint16_t)((payload[0] << 8) | payload[1])) return false;
        const uint8_t hdr = payload[2];
        const int dac = (hdr >> 6) & 1, sbr = (hdr >> 5) & 1;
        const int nAu = dac ? (sbr ? 3 : 6) : (sbr ? 2 : 4);
        static const int firstStart[7] = {0, 0, 5, 6, 8, 0, 11};
        std::vector<int> starts;
        starts.push_back(firstStart[nAu]);
        for (int k = 0; k < nAu - 1; k++) {
            const int bit = 12 * k, byte = 3 + bit / 8;
            starts.push_back((bit % 8 == 0) ? ((payload[(size_t)byte] << 4) | (payload[(size_t)byte + 1] >> 4))
                                            : (((payload[(size_t)byte] & 0x0F) << 8) | payload[(size_t)byte + 1]));
        }
        starts.push_back((int)payload.size());
        for (int k = 0; k < nAu; k++) if (starts[(size_t)k + 1] <= starts[(size_t)k] || starts[(size_t)k + 1] > (int)payload.size()) return false;
        std::lock_guard<std::mutex> lk(mu);
        st.rsCorrected += (uint64_t)corrected;
        if (openHeader != (hdr & 0x7F)) { if (!openAac(hdr & 0x7F)) return true; openHeader = hdr & 0x7F; }
        for (int k = 0; k < nAu; k++) {
            const uint8_t* au = &payload[(size_t)starts[(size_t)k]];
            const int len = starts[(size_t)k + 1] - starts[(size_t)k];
            if (len >= 3 && dab::crc16(au, len - 2) == (uint16_t)((au[len - 2] << 8) | au[len - 1])) {
                st.auOk++;
                if (auTap) auTap(au, len - 2, hdr);
                decodePacket(au, len - 2);
            } else st.auBad++;
        }
        return true;
    }
};

DabAudio::DabAudio() : p_(new Impl) {}
DabAudio::~DabAudio() = default;

void DabAudio::select(int sub, bool dabPlus, int bitrate) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& I = *p_;
    I.closeDecoder();
    I.frames.clear();
    I.synced = false; I.since = 0; I.consecBad = 0;
    I.dabPlus = dabPlus; I.bitrate = bitrate;
    I.st = DabAudioStats();
    I.st.sub = sub; I.st.dabPlus = dabPlus; I.st.bitrate = bitrate;
    if (!dabPlus && sub >= 0) I.openMp2();
    if (I.outStarted) I.out.flush();
}

void DabAudio::flushSync() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->frames.clear(); p_->synced = false; p_->since = 0;
}

void DabAudio::push(const uint8_t* frame, int n) {
    Impl& I = *p_;
    if (I.st.sub < 0) return;
    if (!I.dabPlus) {
        std::lock_guard<std::mutex> lk(I.mu);
        I.st.frames++;
        I.decodePacket(frame, n);
        return;
    }
    std::deque<std::vector<uint8_t>> window;
    {
        std::lock_guard<std::mutex> lk(I.mu);
        I.st.frames++;
        I.frames.emplace_back(frame, frame + n);
        if (I.frames.size() > 5) I.frames.pop_front();
        if (I.frames.size() < 5) return;
        if (I.synced && ++I.since < 5) return;
        window = I.frames;
    }
    const bool ok = I.superframe(window);
    std::lock_guard<std::mutex> lk(I.mu);
    if (ok) { I.st.superframesOk++; I.synced = true; I.since = 0; I.consecBad = 0; }
    else if (I.synced) { I.st.superframesBad++; I.since = 0; if (++I.consecBad >= 3) { I.synced = false; I.consecBad = 0; } }
}

DabAudioStats DabAudio::stats() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    DabAudioStats s = p_->st;
    if (p_->outStarted) { s.bufferedMs = p_->out.bufferedFrames() / 48; s.underruns = p_->out.underruns(); }
    return s;
}

void DabAudio::setVolume(float v) { p_->volume = v; if (p_->outStarted) p_->out.setVolume(v); }
void DabAudio::setMuted(bool m) { p_->muted = m; if (p_->outStarted) p_->out.setMuted(m); }
void DabAudio::setAuTap(std::function<void(const uint8_t*, int, uint8_t)> cb) { p_->auTap = std::move(cb); }
void DabAudio::setSilent(bool s) { p_->silent = s; }

} // namespace dect2
