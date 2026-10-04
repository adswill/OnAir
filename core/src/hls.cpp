// HTTP Live Streaming of one service, in memory. See hls.h.
//
// Input: transport stream bytes through a read callback. Output: the FFmpeg "hls" muxer, whose files (playlist and segments) are captured
// in memory through the muxer's io_open / io_close2 hooks. The picture is copied (H.264 and HEVC only), audio that Apple devices do not
// play in a transport stream (MPEG audio layer II, AAC in LATM, ...) is decoded and encoded again as AAC; AC-3, E-AC-3, MP3 and ADTS AAC
// are copied.
#include "dect2/hls.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace dect2 {

namespace {

constexpr int kInBuf = 188 * 128;
constexpr size_t kKeepSegments = 14;   // the playlist lists 6; a few more stay available for slow clients

std::string baseName(const char* url) {
    const char* s = strrchr(url, '/');
    if (!s) s = strrchr(url, ':');   // "mem:index.m3u8"
    return s ? s + 1 : url;
}

} // namespace

struct HlsPipeline::Impl {
    Reader reader;
    std::string token;
    std::atomic<bool> stopReq{false};
    std::thread th;

    mutable std::mutex mu;   // files, error, lastGet
    std::map<std::string, std::string> files;
    std::deque<std::string> segments;
    std::string err;
    std::chrono::steady_clock::time_point lastGet = std::chrono::steady_clock::now();

    // output files in flight: the muxer opens one memory buffer per file
    std::map<AVIOContext*, std::string> open;

    Impl(Reader r, std::string t) : reader(std::move(r)), token(std::move(t)) {}

    void fail(const std::string& m) {
        std::lock_guard<std::mutex> lk(mu);
        if (err.empty()) err = m;
    }

    static int readPacket(void* opaque, uint8_t* buf, int size) {
        Impl* p = static_cast<Impl*>(opaque);
        if (p->stopReq) return AVERROR_EOF;
        const int n = p->reader(buf, size);
        return n > 0 ? n : AVERROR_EOF;
    }
    static int interrupt(void* opaque) { return static_cast<Impl*>(opaque)->stopReq ? 1 : 0; }

    static int ioOpen(AVFormatContext* s, AVIOContext** pb, const char* url, int flags, AVDictionary**) {
        Impl* p = static_cast<Impl*>(s->opaque);
        if (!(flags & AVIO_FLAG_WRITE)) return AVERROR(ENOENT);
        const int r = avio_open_dyn_buf(pb);
        if (r < 0) return r;
        p->open[*pb] = baseName(url);
        return 0;
    }
    static int ioClose(AVFormatContext* s, AVIOContext* pb) {
        Impl* p = static_cast<Impl*>(s->opaque);
        auto it = p->open.find(pb);
        const std::string name = it != p->open.end() ? it->second : std::string();
        if (it != p->open.end()) p->open.erase(it);
        uint8_t* data = nullptr;
        const int size = avio_close_dyn_buf(pb, &data);
        if (size >= 0 && !name.empty()) {
            std::lock_guard<std::mutex> lk(p->mu);
            p->files[name].assign(reinterpret_cast<const char*>(data), (size_t)size);
            if (name.size() > 3 && name.compare(name.size() - 3, 3, ".ts") == 0) {
                p->segments.push_back(name);
                while (p->segments.size() > kKeepSegments) { p->files.erase(p->segments.front()); p->segments.pop_front(); }
            }
        }
        av_free(data);
        return 0;
    }

    void run() {
        AVFormatContext* ic = nullptr;
        AVFormatContext* oc = nullptr;
        AVIOContext* ipb = nullptr;
        AVCodecContext* adec = nullptr;
        AVCodecContext* aenc = nullptr;
        SwrContext* swr = nullptr;
        AVAudioFifo* fifo = nullptr;
        AVFrame* frame = nullptr;
        AVFrame* encFrame = nullptr;
        AVPacket* pkt = av_packet_alloc();
        AVPacket* epkt = av_packet_alloc();
        int swrRate = 0, swrFmt = -1, swrCh = 0;
        bool headerWritten = false;

        auto cleanup = [&] {
            if (oc && headerWritten) av_write_trailer(oc);
            if (oc) { avformat_free_context(oc); oc = nullptr; }
            if (swr) swr_free(&swr);
            if (fifo) av_audio_fifo_free(fifo);
            av_frame_free(&frame);
            av_frame_free(&encFrame);
            avcodec_free_context(&adec);
            avcodec_free_context(&aenc);
            av_packet_free(&pkt);
            av_packet_free(&epkt);
            if (ic) avformat_close_input(&ic);
            if (ipb) { av_freep(&ipb->buffer); avio_context_free(&ipb); }
            for (auto& kv : open) { uint8_t* d = nullptr; avio_close_dyn_buf(kv.first, &d); av_free(d); }
            open.clear();
        };

        uint8_t* ibuf = static_cast<uint8_t*>(av_malloc(kInBuf));
        ipb = avio_alloc_context(ibuf, kInBuf, 0, this, &Impl::readPacket, nullptr, nullptr);
        ic = avformat_alloc_context();
        if (!ibuf || !ipb || !ic) { fail("out of memory"); cleanup(); return; }
        ic->pb = ipb;
        ic->flags |= AVFMT_FLAG_CUSTOM_IO;
        ic->interrupt_callback.callback = &Impl::interrupt;
        ic->interrupt_callback.opaque = this;
        ic->probesize = 4 * 1024 * 1024;
        ic->max_analyze_duration = 2 * AV_TIME_BASE;
        if (avformat_open_input(&ic, "", av_find_input_format("mpegts"), nullptr) < 0) { fail("cannot read the stream"); cleanup(); return; }
        if (avformat_find_stream_info(ic, nullptr) < 0) { if (!stopReq) fail("cannot find the picture and sound in the stream"); cleanup(); return; }

        int vi = -1, ai = -1;
        for (unsigned i = 0; i < ic->nb_streams; i++) {
            const AVCodecParameters* cp = ic->streams[i]->codecpar;
            if (cp->codec_type == AVMEDIA_TYPE_VIDEO && vi < 0) vi = (int)i;
            if (cp->codec_type == AVMEDIA_TYPE_AUDIO && ai < 0) ai = (int)i;
        }
        if (vi < 0 && ai < 0) { fail("this service has no picture or sound"); cleanup(); return; }
        if (vi >= 0) {
            const AVCodecID id = ic->streams[vi]->codecpar->codec_id;
            if (id != AV_CODEC_ID_H264 && id != AV_CODEC_ID_HEVC) { fail("the picture format of this service (not H.264 or HEVC) cannot be streamed this way yet"); cleanup(); return; }
        }

        // a name that is not a file keeps the muxer from writing a temporary file and renaming it
        if (avformat_alloc_output_context2(&oc, nullptr, "hls", "mem:index.m3u8") < 0 || !oc) { fail("the HLS muxer is not available"); cleanup(); return; }
        oc->opaque = this;
        oc->io_open = &Impl::ioOpen;
        oc->io_close2 = &Impl::ioClose;

        AVStream* ovs = nullptr;
        AVStream* oas = nullptr;
        bool audioCopy = false;
        if (vi >= 0) {
            ovs = avformat_new_stream(oc, nullptr);
            avcodec_parameters_copy(ovs->codecpar, ic->streams[vi]->codecpar);
            ovs->codecpar->codec_tag = 0;
        }
        if (ai >= 0) {
            const AVCodecParameters* cp = ic->streams[ai]->codecpar;
            const bool adts = cp->codec_id == AV_CODEC_ID_AAC && cp->extradata_size == 0;   // ADTS AAC arrives without a configuration record
            audioCopy = cp->codec_id == AV_CODEC_ID_AC3 || cp->codec_id == AV_CODEC_ID_EAC3 || cp->codec_id == AV_CODEC_ID_MP3 || adts;
            oas = avformat_new_stream(oc, nullptr);
            if (audioCopy) {
                avcodec_parameters_copy(oas->codecpar, cp);
                oas->codecpar->codec_tag = 0;
            } else {
                const AVCodec* dec = avcodec_find_decoder(cp->codec_id);
                const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_AAC);
                if (!dec || !enc) { fail("this audio format cannot be converted"); cleanup(); return; }
                adec = avcodec_alloc_context3(dec);
                avcodec_parameters_to_context(adec, cp);
                if (avcodec_open2(adec, dec, nullptr) < 0) { fail("cannot decode the audio"); cleanup(); return; }
                aenc = avcodec_alloc_context3(enc);
                aenc->sample_rate = 48000;
                av_channel_layout_default(&aenc->ch_layout, 2);
                aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
                aenc->bit_rate = 128000;
                aenc->time_base = AVRational{1, 48000};
                if (oc->oformat->flags & AVFMT_GLOBALHEADER) aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                if (avcodec_open2(aenc, enc, nullptr) < 0) { fail("cannot encode the audio"); cleanup(); return; }
                avcodec_parameters_from_context(oas->codecpar, aenc);
                fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, 4096);
                frame = av_frame_alloc();
                encFrame = av_frame_alloc();
            }
        }

        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "hls_time", "2", 0);
        av_dict_set(&opts, "hls_list_size", "6", 0);
        av_dict_set(&opts, "hls_flags", "independent_segments+omit_endlist", 0);
        av_dict_set(&opts, "hls_segment_filename", ("seg" + token + "%05d.ts").c_str(), 0);
        if (avformat_write_header(oc, &opts) < 0) { av_dict_free(&opts); fail("cannot start the stream"); cleanup(); return; }
        av_dict_free(&opts);
        headerWritten = true;

        const AVStream* ivs = vi >= 0 ? ic->streams[vi] : nullptr;
        const AVStream* ias = ai >= 0 ? ic->streams[ai] : nullptr;
        bool started = vi < 0;       // wait for the first key frame of the picture
        int64_t audioPts = AV_NOPTS_VALUE;   // next audio sample in the encoder's time base

        auto writeCopy = [&](AVPacket* p, const AVStream* in, AVStream* out) {
            p->stream_index = out->index;
            av_packet_rescale_ts(p, in->time_base, out->time_base);
            av_interleaved_write_frame(oc, p);
        };
        auto drainEncoder = [&] {
            while (avcodec_receive_packet(aenc, epkt) == 0) {
                epkt->stream_index = oas->index;
                av_packet_rescale_ts(epkt, aenc->time_base, oas->time_base);
                av_interleaved_write_frame(oc, epkt);
            }
        };

        while (!stopReq) {
            const int r = av_read_frame(ic, pkt);
            if (r < 0) break;
            if (pkt->stream_index == vi && ovs) {
                if (!started) { if (pkt->flags & AV_PKT_FLAG_KEY) started = true; else { av_packet_unref(pkt); continue; } }
                writeCopy(pkt, ivs, ovs);
            } else if (pkt->stream_index == ai && oas && started) {
                if (audioCopy) {
                    writeCopy(pkt, ias, oas);
                } else if (avcodec_send_packet(adec, pkt) == 0) {
                    while (avcodec_receive_frame(adec, frame) == 0) {
                        AVChannelLayout in = frame->ch_layout;
                        if (in.nb_channels <= 0) av_channel_layout_default(&in, std::max(1, adec->ch_layout.nb_channels));
                        if (!swr || swrRate != frame->sample_rate || swrFmt != frame->format || swrCh != in.nb_channels) {
                            if (swr) swr_free(&swr);
                            AVChannelLayout out;
                            av_channel_layout_default(&out, 2);
                            if (swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_FLTP, 48000, &in, (AVSampleFormat)frame->format, frame->sample_rate, 0, nullptr) < 0 || swr_init(swr) < 0) {
                                fail("cannot convert the audio"); av_frame_unref(frame); goto done;
                            }
                            swrRate = frame->sample_rate; swrFmt = frame->format; swrCh = in.nb_channels;
                        }
                        const int maxOut = swr_get_out_samples(swr, frame->nb_samples);
                        uint8_t** conv = nullptr;
                        int linesize = 0;
                        if (maxOut > 0 && av_samples_alloc_array_and_samples(&conv, &linesize, 2, maxOut, AV_SAMPLE_FMT_FLTP, 0) >= 0) {
                            const int n = swr_convert(swr, conv, maxOut, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
                            if (n > 0) {
                                if (frame->pts != AV_NOPTS_VALUE) {
                                    const int64_t want = av_rescale_q(frame->pts, ias->time_base, aenc->time_base) - av_audio_fifo_size(fifo);
                                    // follow the broadcast clock when the stream jumps (lost data), otherwise count samples
                                    if (audioPts == AV_NOPTS_VALUE || std::llabs(want - audioPts) > 24000) audioPts = want;
                                }
                                av_audio_fifo_write(fifo, reinterpret_cast<void**>(conv), n);
                            }
                            av_freep(&conv[0]);
                            av_freep(&conv);
                        }
                        av_frame_unref(frame);
                        while (audioPts != AV_NOPTS_VALUE && av_audio_fifo_size(fifo) >= aenc->frame_size) {
                            encFrame->nb_samples = aenc->frame_size;
                            encFrame->format = AV_SAMPLE_FMT_FLTP;
                            av_channel_layout_copy(&encFrame->ch_layout, &aenc->ch_layout);
                            encFrame->sample_rate = 48000;
                            if (av_frame_get_buffer(encFrame, 0) < 0) break;
                            av_audio_fifo_read(fifo, reinterpret_cast<void**>(encFrame->extended_data), aenc->frame_size);
                            encFrame->pts = audioPts;
                            audioPts += aenc->frame_size;
                            if (avcodec_send_frame(aenc, encFrame) == 0) drainEncoder();
                            av_frame_unref(encFrame);
                        }
                    }
                }
            }
            av_packet_unref(pkt);
        }
    done:
        cleanup();
    }
};

HlsPipeline::HlsPipeline(Reader reader, const std::string& token) : p_(new Impl(std::move(reader), token)) {}
HlsPipeline::~HlsPipeline() { stop(); }

void HlsPipeline::start() {
    if (p_->th.joinable()) return;
    p_->th = std::thread([this] { p_->run(); });
}

void HlsPipeline::stop() {
    p_->stopReq = true;
    if (p_->th.joinable()) p_->th.join();
}

bool HlsPipeline::get(const std::string& name, std::string& body, std::string& contentType) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->lastGet = std::chrono::steady_clock::now();
    auto it = p_->files.find(name);
    if (it == p_->files.end()) return false;
    body = it->second;
    contentType = name.size() > 5 && name.compare(name.size() - 5, 5, ".m3u8") == 0 ? "application/vnd.apple.mpegurl" : "video/MP2T";
    return true;
}

bool HlsPipeline::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->files.count("index.m3u8") != 0;
}

std::string HlsPipeline::error() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->err;
}

double HlsPipeline::idleSeconds() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_->lastGet).count();
}

} // namespace dect2
