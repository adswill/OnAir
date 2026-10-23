// DAB / DAB+ audio: superframe assembly (Reed-Solomon, firecode, access units) and decoding with FFmpeg.
#include "dect2/dab/dab.h"
#include "dect2/common/endian.h"
#include "dect2/audio/audioout.h"
#include "dect2/dab/dab_pad.h"
#include "dect2/dab/dab_playout.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include "dect2/common/endian.h"
#endif

namespace dect2 {

// ---- faad2, loaded at run time when it is installed (it is not shipped): FFmpeg's AAC decoder has no SBR and no parametric stereo for the 960
// sample frames of DAB+, faad2 has. The few calls and structures used are declared here, as faad2's headers lay them out (neaacdec.h).
namespace {
struct FaadConfig { unsigned char defObjectType; unsigned long defSampleRate; unsigned char outputFormat, downMatrix, useOldADTSFormat, dontUpSampleImplicitSBR; };
struct FaadFrameInfo {
    unsigned long bytesconsumed, samples;
    unsigned char channels, error;
    unsigned long samplerate;
    unsigned char sbr, object_type, header_type, num_front_channels, num_side_channels, num_back_channels, num_lfe_channels, channel_position[64], ps;
    unsigned char spare[256];     // room for what a newer faad2 adds at the end
};
struct Faad {
    typedef void* (*Open)();
    typedef FaadConfig* (*GetConf)(void*);
    typedef unsigned char (*SetConf)(void*, FaadConfig*);
    typedef char (*Init2)(void*, unsigned char*, unsigned long, unsigned long*, unsigned char*);
    typedef void* (*Decode)(void*, FaadFrameInfo*, unsigned char*, unsigned long);
    typedef void (*Close)(void*);
    Open open = nullptr; GetConf getConf = nullptr; SetConf setConf = nullptr; Init2 init2 = nullptr; Decode decode = nullptr; Close close = nullptr;
    bool ok = false;
    Faad() {
#ifdef _WIN32
        for (const char* n : {"libfaad.dll", "libfaad2.dll", "libfaad-2.dll", "libfaad2-2.dll", "faad2.dll", "faad.dll"}) { if ((h = (void*)LoadLibraryA(n))) break; }
        auto sym = [&](const char* s) { return h ? (void*)GetProcAddress((HMODULE)h, s) : nullptr; };
#else
        for (const char* n : {"libfaad.so.2", "libfaad.so", "libfaad.2.dylib", "libfaad.dylib", "/opt/homebrew/lib/libfaad.dylib", "/usr/local/lib/libfaad.dylib"}) { if ((h = dlopen(n, RTLD_NOW | RTLD_LOCAL))) break; }
        auto sym = [&](const char* s) { return h ? dlsym(h, s) : nullptr; };
#endif
        open = (Open)sym("NeAACDecOpen"); getConf = (GetConf)sym("NeAACDecGetCurrentConfiguration"); setConf = (SetConf)sym("NeAACDecSetConfiguration");
        init2 = (Init2)sym("NeAACDecInit2"); decode = (Decode)sym("NeAACDecDecode"); close = (Close)sym("NeAACDecClose");
        ok = open && getConf && setConf && init2 && decode && close;
    }
    void* h = nullptr;
    static Faad& get() { static Faad f; return f; }
};
std::atomic<int> gAacBackend{0};   // 0 faad2 for the streams with SBR when it is there, 1 FFmpeg always, 2 faad2 always (when there)
} // namespace

bool dabFaadAvailable() { return Faad::get().ok; }

DabAacParams dabAacParams(uint8_t h) {
    DabAacParams p;
    const int dac = (h >> 6) & 1;
    p.valid = true;
    p.sbr = (h >> 5) & 1; p.stereo = (h >> 4) & 1; p.ps = (h >> 3) & 1; p.surround = h & 7;
    if (p.ps && !p.sbr) p.valid = false;   // parametric stereo needs SBR
    p.outRate = dac ? 48000 : 32000;
    p.coreRate = p.sbr ? p.outRate / 2 : p.outRate;
    p.auPerSuperframe = dac ? (p.sbr ? 3 : 6) : (p.sbr ? 2 : 4);
    p.coreChannels = (p.stereo && !p.ps) ? 2 : 1;
    p.outChannels = (p.stereo || p.ps) ? 2 : 1;
    p.profile = p.sbr && p.ps ? "HE-AAC v2 (AAC-LC + SBR + PS)" : p.sbr ? "HE-AAC (AAC-LC + SBR)" : "AAC-LC";
    return p;
}

struct DabAudio::Impl {
    mutable std::mutex mu;
    AudioOut out;
    // the last decoded samples (stereo), for the spectrum of the interface
    static constexpr int kRing = 16384;
    mutable std::mutex pmu;
    std::vector<float> ring = std::vector<float>((size_t)kRing * 2, 0.f);
    uint64_t ringTotal = 0;
    bool silent = false, outStarted = false;
    DabAudioStats st;
    DabPadDecoder pad;
    uint64_t logoEpoch = 0;
    uint64_t slideEpoch = 0;     // pictures of the services selected before, so that the sequence number never goes back
    std::function<void(const uint8_t*, int, uint8_t)> auTap;
    std::function<void(const float*, int)> pcmTap;
    // playout: concealment, rate control and gap filling (dab_playout.h)
    DabPlayout play;
    std::vector<float> pout;
    std::function<int()> fillProbe;      // tests: the frames queued in a simulated output (else the real one's)
    bool everFull = false;               // the output has once been filled to the start level (it plays)
    bool filling = false;                // gaps were filled since the last audio
    // After a loss that went silent, one good frame alone is not played: a stretch of bad reception would otherwise come out as short blips
    // between the silences. Two good ones in a row are.
    int goodRun = 0;
    bool hold = false;
    int auFrames = 1920;                 // 48 kHz frames of one access unit of the current stream
    int lastNau = 3;
    float volume = 1.f; bool muted = false;

    // DAB+ superframe assembly
    std::deque<std::vector<uint8_t>> frames;
    std::vector<uint8_t> mp2buf;   // DAB at 24 kHz: the first half of an MP2 frame (48 ms, two logical frames)
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
    void* faad = nullptr;                 // a faad2 decoder, when that one decodes
    SwrContext* swrF = nullptr;           // its float output to 48 kHz stereo
    int swrFRate = 0, swrFCh = 0;
    uint8_t openHeader = 0xFF;
    std::vector<float> pcm;

    ~Impl() { closeDecoder(); if (pkt) av_packet_free(&pkt); if (frame) av_frame_free(&frame); }

    void closeDecoder() {
        if (ctx) avcodec_free_context(&ctx);
        if (swr) swr_free(&swr);
        if (swrF) swr_free(&swrF);
        if (faad) { Faad::get().close(faad); faad = nullptr; }
        swrRate = 0; swrFmt = -1; swrCh = 0; swrFRate = 0; swrFCh = 0;
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
        const int backend = gAacBackend.load();
        if (backend != 1 && (sbr || backend == 2) && Faad::get().ok) {   // faad2: SBR and PS work with DAB+'s frames
            Faad& F = Faad::get();
            faad = F.open();
            if (faad) {
                FaadConfig* c = F.getConf(faad);
                c->outputFormat = 4;   // FAAD_FMT_FLOAT
                c->downMatrix = 0;
                c->dontUpSampleImplicitSBR = 0;
                F.setConf(faad, c);
                std::vector<uint8_t> asc = makeAsc(dac, sbr, stereo, ps);
                unsigned long rate = 0;
                unsigned char ch = 0;
                if (F.init2(faad, asc.data(), (unsigned long)asc.size(), &rate, &ch) >= 0) {
                    openHeader = hdr;
                    st.codec = sbr && ps ? "HE-AAC v2" : sbr ? "HE-AAC" : "AAC-LC";
                    st.decoder = "faad2";
                    st.channels = (stereo || ps) ? 2 : 1;
                    return true;
                }
                F.close(faad); faad = nullptr;
            }
        }
        st.decoder = "FFmpeg";
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

    // ---- recording (all of it under mu)
    struct Rec {
        bool active = false;
        DabRecFormat fmt = DabRecFormat::Wav;
        std::string path, error;
        FILE* f = nullptr;              // WAV, MP2
        FILE* log = nullptr;            // the dynamic label log
        AVFormatContext* oc = nullptr;  // DAB+ (MP4)
        int64_t pts = 0, auTicks = 960;
        bool muxOpen = false;
        uint64_t bytes = 0, pcmFrames = 0, units = 0;
        double unitSec = 0.024;         // seconds of one unit written (MP2 frame, AU)
        std::string lastDl;
    } rec;

    static void le32(uint8_t* p, uint32_t v) { dect2::endian::storeLe32(p, v); }
    void wavHeader(uint32_t dataBytes) {
        uint8_t h[44];
        std::memcpy(h, "RIFF", 4); le32(h + 4, 36 + dataBytes); std::memcpy(h + 8, "WAVEfmt ", 8); le32(h + 16, 16);
        h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0; le32(h + 24, 48000); le32(h + 28, 48000 * 4); h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
        std::memcpy(h + 36, "data", 4); le32(h + 40, dataBytes);
        std::fseek(rec.f, 0, SEEK_SET);
        std::fwrite(h, 1, 44, rec.f);
    }
    void recStop() {
        if (!rec.active) return;
        if (rec.fmt == DabRecFormat::Wav && rec.f) { wavHeader((uint32_t)std::min<uint64_t>(rec.pcmFrames * 4, 0xFFFFFFFFu - 64)); }
        if (rec.oc) {
            if (rec.muxOpen) av_write_trailer(rec.oc);
            if (rec.oc->pb) avio_closep(&rec.oc->pb);
            avformat_free_context(rec.oc);
            rec.oc = nullptr;
        }
        if (rec.f) { std::fclose(rec.f); rec.f = nullptr; }
        if (rec.log) { std::fclose(rec.log); rec.log = nullptr; }
        rec.active = false;
        rec.muxOpen = false;
    }
    void recFail(const char* why) { rec.error = why; recStop(); }
    void recWav(const float* stereo, int frames) {
        std::vector<int16_t> b((size_t)frames * 2);
        for (size_t i = 0; i < b.size(); i++) b[i] = (int16_t)std::lrintf(std::max(-1.f, std::min(1.f, stereo[i])) * 32767.f);
        if (std::fwrite(b.data(), 2, b.size(), rec.f) != b.size()) { recFail("write error"); return; }
        rec.pcmFrames += (uint64_t)frames;
        rec.bytes = rec.pcmFrames * 4 + 44;
    }
    // length of the MPEG audio layer II frame that starts at p (0 = not a frame)
    static int mp2FrameLen(const uint8_t* p, int n, double* secs = nullptr) {
        if (n < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return 0;
        const int ver = (p[1] >> 3) & 3, layer = (p[1] >> 1) & 3, bi = p[2] >> 4, si = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
        if (layer != 2 || ver == 1 || bi == 0 || bi == 15 || si == 3) return 0;
        static const int br1[15] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384};
        static const int br2[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
        static const int sr1[3] = {44100, 48000, 32000};
        const bool lsf = ver != 3;
        const int sr = sr1[si] / (ver == 3 ? 1 : ver == 2 ? 2 : 4);
        const int len = 144 * (lsf ? br2[bi] : br1[bi]) * 1000 / sr + pad;
        if (secs) *secs = 1152.0 / sr;
        return len <= n ? len : 0;
    }
    void recMp2(const uint8_t* frame, int n) {
        const int len = mp2FrameLen(frame, n, &rec.unitSec);
        if (len <= 0) return;
        if (std::fwrite(frame, 1, (size_t)len, rec.f) != (size_t)len) { recFail("write error"); return; }
        rec.bytes += (uint64_t)len;
        rec.units++;
    }
    bool recOpenAac(uint8_t hdr) {
        const int dac = (hdr >> 6) & 1, sbr = (hdr >> 5) & 1, stereo = (hdr >> 4) & 1, ps = (hdr >> 3) & 1;
        const int coreRate = sbr ? (dac ? 24000 : 16000) : (dac ? 48000 : 32000), outRate = sbr ? coreRate * 2 : coreRate;
        const std::vector<uint8_t> asc = makeAsc(dac, sbr, stereo, ps);
        if (avformat_alloc_output_context2(&rec.oc, nullptr, "mp4", rec.path.c_str()) < 0 || !rec.oc) { recFail("no MP4 muxer"); return false; }
        AVStream* s = avformat_new_stream(rec.oc, nullptr);
        if (!s) { recFail("no stream"); return false; }
        AVCodecParameters* cp = s->codecpar;
        cp->codec_type = AVMEDIA_TYPE_AUDIO; cp->codec_id = AV_CODEC_ID_AAC; cp->sample_rate = outRate; cp->frame_size = 960 * (sbr ? 2 : 1);
        av_channel_layout_default(&cp->ch_layout, (stereo && !ps) ? 2 : 1);
        cp->extradata = (uint8_t*)av_mallocz(asc.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        std::memcpy(cp->extradata, asc.data(), asc.size());
        cp->extradata_size = (int)asc.size();
        s->time_base = AVRational{1, outRate};
        rec.auTicks = 960 * (sbr ? 2 : 1);
        rec.unitSec = 960.0 / coreRate;
        if (avio_open(&rec.oc->pb, rec.path.c_str(), AVIO_FLAG_WRITE) < 0) { recFail("cannot create the file"); return false; }
        AVDictionary* o = nullptr;
        av_dict_set(&o, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);   // playable even if the program stops before the end
        const int r = avformat_write_header(rec.oc, &o);
        av_dict_free(&o);
        if (r < 0) { recFail("cannot write the header"); return false; }
        rec.muxOpen = true;
        return true;
    }
    void recAu(const uint8_t* au, int n, uint8_t hdr) {
        if (!rec.muxOpen && !recOpenAac(hdr & 0x7F)) return;
        AVPacket* pk = av_packet_alloc();
        if (!pk || av_new_packet(pk, n) < 0) { if (pk) av_packet_free(&pk); return; }
        std::memcpy(pk->data, au, (size_t)n);
        pk->stream_index = 0; pk->pts = pk->dts = rec.pts; pk->duration = rec.auTicks; pk->flags |= AV_PKT_FLAG_KEY;
        rec.pts += rec.auTicks;
        const int r = av_interleaved_write_frame(rec.oc, pk);
        av_packet_free(&pk);
        if (r < 0) { recFail("write error"); return; }
        rec.bytes += (uint64_t)n;
        rec.units++;
    }
    void recLabel() {
        if (!rec.log) return;
        std::string t = pad.label();
        for (const DabDlpTag& g : pad.dlPlus()) t += std::string("  [") + dabDlpTypeName(g.type) + ": " + g.text + "]";
        if (t == rec.lastDl) return;
        rec.lastDl = t;
        const time_t now = std::time(nullptr);
        struct tm m;
#ifdef _WIN32
        localtime_s(&m, &now);
#else
        localtime_r(&now, &m);
#endif
        char ts[24];
        std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &m);
        std::fprintf(rec.log, "%s\t%s\n", ts, t.c_str());
        std::fflush(rec.log);
    }

    void emit(const float* stereo, int frames) {
        st.pcmFrames += (uint64_t)frames;
        st.decoding = true;
        {
            std::lock_guard<std::mutex> lk(pmu);
            for (int i = 0; i < frames; i++) {
                const size_t w = (size_t)(ringTotal++ % kRing) * 2;
                ring[w] = stereo[2 * i]; ring[w + 1] = stereo[2 * i + 1];
            }
        }
        if (rec.active && rec.fmt == DabRecFormat::Wav) recWav(stereo, frames);
        if (pcmTap) pcmTap(stereo, frames);
        if (silent) return;
        if (!outStarted) {
            out.setStartThreshold(DabAudio::kStartFrames);
            if (out.start(48000)) outStarted = true;
            out.setVolume(volume); out.setMuted(muted);
        }
        if (outStarted) out.write(stereo, frames);
    }

    // frames queued in the output, -1 when there is none (tests without a probe)
    int fillNow() const {
        if (fillProbe) return fillProbe();
        if (outStarted) return out.bufferedFrames();
        return -1;
    }
    void deliverPlayout() {
        if (!pout.empty()) emit(pout.data(), (int)(pout.size() / 2));
        pout.clear();
    }
    // decoded audio, 48 kHz stereo: through the playout (splice, rate control) to the output
    void audioGood(const float* stereo, int frames) {
        if (hold) { play.lost(frames, pout); deliverPlayout(); return; }
        const int fill = fillNow();
        if (fill >= 0) {
            play.setFill(fill);
            if (fill >= DabAudio::kStartFrames) everFull = true;
            // after the gaps were filled: back up to the working level with silence before the audio fades in, so the next burst
            // spacing does not empty the buffer again
            if (filling && fill < DabPlayout::kTarget - 2400) play.lost(DabPlayout::kTarget - fill, pout);
        }
        filling = false;
        play.good(stereo, frames, pout);
        deliverPlayout();
    }
    // `frames` of audio lost: concealment
    void audioLost(int frames) {
        goodRun = 0;
        play.lost(frames, pout);
        deliverPlayout();
    }

    // float samples of any rate and one or two channels to the 48 kHz stereo of the output
    int emitFloat(const float* in, int frames, int ch, int rate) {
        if (rate == 48000 && ch == 2) { audioGood(in, frames); return frames; }
        if (!swrF || swrFRate != rate || swrFCh != ch) {
            if (swrF) swr_free(&swrF);
            AVChannelLayout inL, outL = AV_CHANNEL_LAYOUT_STEREO;
            av_channel_layout_default(&inL, ch);
            swr_alloc_set_opts2(&swrF, &outL, AV_SAMPLE_FMT_FLT, 48000, &inL, AV_SAMPLE_FMT_FLT, rate, 0, nullptr);
            if (!swrF || swr_init(swrF) < 0) { if (swrF) swr_free(&swrF); return 0; }
            swrFRate = rate; swrFCh = ch;
            st.sampleRate = rate; st.channels = ch;
        }
        const int maxOut = (int)((int64_t)frames * 48000 / std::max(1, rate)) + 64;
        pcm.resize((size_t)maxOut * 2);
        uint8_t* o[1] = {(uint8_t*)pcm.data()};
        const uint8_t* ip[1] = {(const uint8_t*)in};
        const int got = swr_convert(swrF, o, maxOut, ip, frames);
        if (got > 0) audioGood(pcm.data(), got);
        return std::max(0, got);
    }

    // decodes one access unit or MP2 frame; returns the 48 kHz frames put out (0: it did not decode)
    int decodePacket(const uint8_t* data, int n) {
        if (faad) {
            FaadFrameInfo fi;
            std::memset(&fi, 0, sizeof fi);
            std::vector<uint8_t> in(data, data + n);
            const void* out = Faad::get().decode(faad, &fi, in.data(), (unsigned long)n);
            if (fi.error == 0 && out && fi.samples > 0 && fi.channels >= 1 && fi.channels <= 2) {
                st.sbrDecoded = fi.sbr != 0; st.psDecoded = fi.ps != 0;
                return emitFloat((const float*)out, (int)(fi.samples / fi.channels), fi.channels, (int)fi.samplerate);
            }
            return 0;
        }
        if (!ctx) return 0;
        if (!pkt) pkt = av_packet_alloc();
        if (!frame) frame = av_frame_alloc();
        if (av_new_packet(pkt, n) < 0) return 0;
        std::memcpy(pkt->data, data, (size_t)n);
        const int s = avcodec_send_packet(ctx, pkt);
        av_packet_unref(pkt);
        if (s < 0) return 0;
        int put = 0;
        while (avcodec_receive_frame(ctx, frame) == 0) {
            const int nb = frame->nb_samples, ch = frame->ch_layout.nb_channels;
            if (!swr || swrRate != frame->sample_rate || swrFmt != frame->format || swrCh != ch) {
                if (swr) swr_free(&swr);
                AVChannelLayout outL = AV_CHANNEL_LAYOUT_STEREO;
                swr_alloc_set_opts2(&swr, &outL, AV_SAMPLE_FMT_FLT, 48000, &frame->ch_layout, (AVSampleFormat)frame->format, frame->sample_rate, 0, nullptr);
                if (!swr || swr_init(swr) < 0) { if (swr) swr_free(&swr); av_frame_unref(frame); return put; }
                swrRate = frame->sample_rate; swrFmt = frame->format; swrCh = ch;
                st.sampleRate = frame->sample_rate; st.channels = ch;
            }
            const int maxOut = (int)((int64_t)nb * 48000 / std::max(1, frame->sample_rate)) + 64;
            pcm.resize((size_t)maxOut * 2);
            uint8_t* o[1] = {(uint8_t*)pcm.data()};
            const int got = swr_convert(swr, o, maxOut, (const uint8_t**)frame->extended_data, nb);
            if (got > 0) { audioGood(pcm.data(), got); put += got; }
            av_frame_unref(frame);
        }
        return put;
    }

    // ---- DAB+ superframe
    bool superframe(const std::deque<std::vector<uint8_t>>& frames) {
        const int fb = (int)frames.front().size();
        const int total = fb * 5, N = total / 120;
        if (total % 120 || N < 1 || N > 24) return false;
        std::vector<uint8_t> sf((size_t)total), payload((size_t)N * 110);
        for (int f = 0; f < 5; f++) std::memcpy(&sf[(size_t)f * fb], frames[(size_t)f].data(), (size_t)fb);
        // A code word beyond repair does not end the superframe: its bytes are spread over all of it (one in every N), and the access units it
        // did not touch still pass their CRC. The Fire code and the CRCs say what can be used.
        int corrected = 0, failed = 0;
        for (int i = 0; i < N; i++) {
            uint8_t cw[120];
            for (int j = 0; j < 120; j++) cw[j] = sf[(size_t)j * N + i];
            const int r = dab::rsDecode120(cw);
            if (r < 0) failed++;
            else corrected += r;
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
        st.rsUncorrectable += (uint64_t)failed;
        st.aac = dabAacParams(hdr);
        lastNau = nAu;
        auFrames = 5760 / nAu;
        if (openHeader != (hdr & 0x7F)) { if (!openAac(hdr & 0x7F)) return true; openHeader = hdr & 0x7F; }
        for (int k = 0; k < nAu; k++) {
            const uint8_t* au = &payload[(size_t)starts[(size_t)k]];
            const int len = starts[(size_t)k + 1] - starts[(size_t)k];
            if (len >= 3 && dab::crc16(au, len - 2) == (uint16_t)((au[len - 2] << 8) | au[len - 1])) {
                st.auOk++;
                if (auTap) auTap(au, len - 2, hdr);
                pad.accessUnit(au, len - 2);
                if (rec.active) { if (rec.fmt == DabRecFormat::Encoded) recAu(au, len - 2, hdr); recLabel(); }
                hold = play.quiet() && ++goodRun < 2;
                if (!play.quiet()) goodRun++;
                const int got = decodePacket(au, len - 2);
                hold = false;
                if (got <= 0) { st.concealedFrames++; audioLost(auFrames); }
            } else { st.auBad++; st.concealedFrames++; audioLost(auFrames); }
        }
        return true;
    }
};

DabAudio::DabAudio() : p_(new Impl) {}
DabAudio::~DabAudio() { if (p_) { std::lock_guard<std::mutex> lk(p_->mu); p_->recStop(); } }

void DabAudio::select(int sub, bool dabPlus, int bitrate) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& I = *p_;
    I.recStop();
    I.closeDecoder();
    I.frames.clear(); I.mp2buf.clear();
    I.synced = false; I.since = 0; I.consecBad = 0;
    I.dabPlus = dabPlus; I.bitrate = bitrate;
    I.st = DabAudioStats();
    { std::lock_guard<std::mutex> pl(I.pmu); I.ringTotal = 0; }
    I.slideEpoch += I.pad.seq() + 1;
    I.logoEpoch += I.pad.logoSeq() + 1;
    I.pad.reset();
    I.st.sub = sub; I.st.dabPlus = dabPlus; I.st.bitrate = bitrate;
    if (!dabPlus && sub >= 0) I.openMp2();
    I.play.reset();
    I.pout.clear();
    I.everFull = false; I.filling = false;
    I.auFrames = dabPlus ? 1920 : 1152; I.lastNau = 3;
    if (I.outStarted) { I.out.flush(); I.out.setStartThreshold(DabAudio::kStartFrames); }   // AudioOut waits for 0.8 s after a flush otherwise
}

void DabAudio::flushSync() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->frames.clear(); p_->synced = false; p_->since = 0;
}

void DabAudio::push(const uint8_t* frame, int n, bool erased) {
    Impl& I = *p_;
    if (I.st.sub < 0) return;
    if (!I.dabPlus) {
        std::lock_guard<std::mutex> lk(I.mu);
        I.st.frames++;
        // a frame lost on the way (erased by the receiver) or one that does not decode is concealed: 1152 samples at 48 kHz
        if (erased) { I.mp2buf.clear(); if (I.st.pcmFrames) { I.st.concealedFrames++; I.audioLost(1152); } return; }
        // At 48 kHz an MP2 frame is exactly one logical frame; at 24 kHz (MPEG-2 half sampling rate) it is 48 ms and spans two, starting at
        // the first. Its first half is kept and the frame decoded whole with the second; anything else goes to the decoder as it is (a damaged
        // frame fails there and is concealed).
        auto decodeOne = [&](const uint8_t* f, int fl, int units) {
            if (I.rec.active && I.rec.fmt == DabRecFormat::Encoded) I.recMp2(f, fl);
            if (Impl::mp2FrameLen(f, fl) == fl) { I.pad.mp2Frame(f, fl); if (I.rec.active) I.recLabel(); }   // the PAD at the end of the audio frame
            I.hold = I.play.quiet() && ++I.goodRun < 2;
            if (!I.play.quiet()) I.goodRun++;
            const int got = I.decodePacket(f, fl);
            I.hold = false;
            if (got <= 0 && I.st.pcmFrames) { I.st.concealedFrames += (uint64_t)units; I.audioLost(1152 * units); }
        };
        if (!I.mp2buf.empty()) {   // the second half of a 24 kHz frame
            I.mp2buf.insert(I.mp2buf.end(), frame, frame + n);
            decodeOne(I.mp2buf.data(), (int)I.mp2buf.size(), 2);
            I.mp2buf.clear();
            return;
        }
        if (Impl::mp2FrameLen(frame, 1 << 20) == 2 * n) { I.mp2buf.assign(frame, frame + n); return; }
        decodeOne(frame, n, 1);
        return;
    }
    std::deque<std::vector<uint8_t>> window;
    {
        std::lock_guard<std::mutex> lk(I.mu);
        I.st.frames++;
        if (erased) I.frames.emplace_back((size_t)n, (uint8_t)0);   // keeps the superframe phase: the superframe it belongs to is concealed
        else I.frames.emplace_back(frame, frame + n);
        if (I.frames.size() > 5) I.frames.pop_front();
        if (I.frames.size() < 5) return;
        if (I.synced && ++I.since < 5) return;
        window = I.frames;
    }
    const bool ok = I.superframe(window);
    std::lock_guard<std::mutex> lk(I.mu);
    if (ok) { I.st.superframesOk++; I.synced = true; I.since = 0; I.consecBad = 0; }
    else if (I.synced) {
        I.st.superframesBad++; I.since = 0;
        I.st.concealedFrames += (uint64_t)I.lastNau;
        I.st.auBad += (uint64_t)I.lastNau;   // the access units of a superframe that did not decode are lost too
        I.audioLost(5760);                    // 120 ms
        // the superframe phase is only searched again after a long run of bad ones: a fade or an erased stretch keeps it
        if (++I.consecBad >= 8) { I.synced = false; I.consecBad = 0; }
    }
}

void DabAudio::tick() {
    Impl& I = *p_;
    std::lock_guard<std::mutex> lk(I.mu);
    if (I.st.sub < 0 || !I.everFull) return;
    const int fill = I.fillNow();
    if (fill < 0 || fill >= DabPlayout::kLow) return;
    // nothing arriving (the sync is lost, the signal gone): the output is kept from running dry, the concealment fades to silence
    I.audioLost(DabPlayout::kTarget - fill);
    I.filling = true;
}

void DabAudio::setFillProbe(std::function<int()> probe) { std::lock_guard<std::mutex> lk(p_->mu); p_->fillProbe = std::move(probe); }

DabAudioStats DabAudio::stats() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    DabAudioStats s = p_->st;
    s.concealMs = p_->play.concealFrames() / 48;
    s.silenceMs = p_->play.silentFrames() / 48;
    s.rateCorrPpm = (p_->play.ratio() - 1.0) * 1e6;
    if (p_->outStarted) { s.bufferedMs = p_->out.bufferedFrames() / 48; s.underruns = p_->out.underruns(); }
    return s;
}

int DabAudio::recent(std::vector<float>& stereo, int frames) const {
    std::lock_guard<std::mutex> lk(p_->pmu);
    const Impl& I = *p_;
    const int n = (int)std::min<uint64_t>((uint64_t)std::min(frames, Impl::kRing), I.ringTotal);
    stereo.resize((size_t)n * 2);
    for (int i = 0; i < n; i++) {
        const size_t r = (size_t)((I.ringTotal - (uint64_t)n + (uint64_t)i) % Impl::kRing) * 2;
        stereo[(size_t)i * 2] = I.ring[r]; stereo[(size_t)i * 2 + 1] = I.ring[r + 1];
    }
    return n;
}

void DabAudio::setAacBackend(int mode) { gAacBackend = mode; }

void DabAudio::setVolume(float v) { p_->volume = v; if (p_->outStarted) p_->out.setVolume(v); }
void DabAudio::setMuted(bool m) { p_->muted = m; if (p_->outStarted) p_->out.setMuted(m); }
void DabAudio::setMotSink(std::function<void(const MotObject&)> cb) { p_->pad.setMotSink(std::move(cb)); }
void DabAudio::setDataGroupSink(std::function<void(const uint8_t*, size_t)> cb) { p_->pad.setDataGroupSink(std::move(cb)); }
void DabAudio::setAuTap(std::function<void(const uint8_t*, int, uint8_t)> cb) { p_->auTap = std::move(cb); }
void DabAudio::setPcmTap(std::function<void(const float*, int)> cb) { p_->pcmTap = std::move(cb); }
bool DabAudio::slide(DabSlide& out, uint64_t lastSeq) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    const uint64_t seq = p_->slideEpoch + p_->pad.seq();
    if (seq == lastSeq) return false;
    out = p_->pad.slide();
    out.seq = seq;
    return true;
}
bool DabAudio::logos(std::vector<DabLogo>& out, uint64_t lastSeq) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    const uint64_t seq = p_->logoEpoch + p_->pad.logoSeq();
    if (seq == lastSeq) return false;
    out = p_->pad.logos();
    lastSeq = seq;
    return true;
}
uint64_t DabAudio::logoSeq() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->logoEpoch + p_->pad.logoSeq();
}
std::vector<DabDlpTag> DabAudio::dlPlus(bool* running) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (running) *running = p_->pad.dlPlusRunning();
    return p_->pad.dlPlus();
}

bool DabAudio::startRecording(const std::string& base, DabRecFormat fmt, bool dlLog, std::string* err) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& I = *p_;
    auto fail = [&](const char* why) { if (err) *err = why; return false; };
    I.recStop();
    if (I.st.sub < 0) return fail("no service is playing");
    I.rec = Impl::Rec();
    I.rec.fmt = fmt;
    const char* ext = fmt == DabRecFormat::Wav ? ".wav" : I.dabPlus ? ".m4a" : ".mp2";
    I.rec.path = base + ext;
    if (fmt == DabRecFormat::Wav || !I.dabPlus) {
        I.rec.f = std::fopen(I.rec.path.c_str(), "wb");
        if (!I.rec.f) return fail("cannot create the file");
        if (fmt == DabRecFormat::Wav) I.wavHeader(0);
    }
    if (dlLog) I.rec.log = std::fopen((base + ".txt").c_str(), "wb");
    I.rec.active = true;
    return true;
}

void DabAudio::stopRecording() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->recStop();
}

DabRecStats DabAudio::recording() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    const Impl::Rec& r = p_->rec;
    DabRecStats s;
    s.active = r.active; s.path = r.path; s.error = r.error; s.bytes = r.bytes;
    s.seconds = r.fmt == DabRecFormat::Wav ? (double)r.pcmFrames / 48000.0 : (double)r.units * r.unitSec;
    return s;
}

std::string DabAudio::dynamicLabel() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->pad.label();
}
void DabAudio::setSilent(bool s) { p_->silent = s; }

} // namespace dect2
