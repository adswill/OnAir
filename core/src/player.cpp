#include "dect2/player.h"
#include "dect2/conceal.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include "dect2/platform.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace dect2 {

using Clock = std::chrono::steady_clock;
static double nowSec() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

struct SubImage { int x, y, w, h; std::vector<uint8_t> rgba; };
struct Subtitle { double start = 0, end = 0; int canvasW = 720, canvasH = 576; std::vector<SubImage> images; };

struct Player::Impl {
    Player* owner = nullptr;
    // decoded video
    std::mutex vmu;
    std::deque<std::shared_ptr<VideoFrame>> vq;
    std::shared_ptr<VideoFrame> current;
    uint64_t seq = 0;
    // clock
    std::mutex cmu;
    bool baseValid = false;
    double basePts = 0, nextPts = 0;
    uint64_t baseIndex = 0;
    bool wallValid = false;
    double wallPts = 0, wallT0 = 0;
    // stats
    mutable std::mutex smu;
    PlayerStats st;
    std::vector<AudioTrackInfo> tracks;
    int wantTrack = 0;
    std::atomic<int> trackChange{-1};
    std::shared_ptr<Subtitle> sub;
    std::mutex submu;
    std::atomic<bool> lossFlag{false};   // set when the demuxer has read past a transport-stream loss on the video PID

    static int readCb(void* opaque, uint8_t* buf, int size) {
        auto* I = static_cast<Impl*>(opaque);
        Player* p = I->owner;
        std::unique_lock<std::mutex> lk(p->mu_);
        int waits = 0;
        while (p->in_.empty() && !p->stop_ && !p->restart_) { p->cv_.wait_for(lk, std::chrono::milliseconds(100)); if (getenv("DECT2_PLAYDEBUG") && ++waits % 10 == 0) fprintf(stderr, "[play] readCb waiting for data (%d s) sid=%d\n", waits / 10, p->sid_.load()); }
        if (p->stop_ || p->restart_) return AVERROR_EOF;
        int n = (int)std::min<size_t>((size_t)size, p->in_.size());
        for (int i = 0; i < n; i++) { buf[i] = p->in_.front(); p->in_.pop_front(); }
        p->consumed_ += (uint64_t)n;
        while (!p->lossMarks_.empty() && p->consumed_ > p->lossMarks_.front()) { p->lossMarks_.pop_front(); I->lossFlag = true; }
        return n;
    }

    void resetClock() {
        { std::lock_guard<std::mutex> lk(cmu); baseValid = false; wallValid = false; }
        std::lock_guard<std::mutex> lk(vmu);
        vq.clear();
    }
};

Player::Player() : p_(new Impl) {
    for (int& c : lastCc_) c = -1;
    p_->owner = this;
    th_ = std::thread([this] { threadMain(); });
}

Player::~Player() {
    stop_ = true;
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    audio_.stop();
}

void Player::select(int sid) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (sid == sid_) return;
        sid_ = sid;
        filter_.select(sid);
        in_.clear();
        lossMarks_.clear(); pushed_ = consumed_ = 0; videoPid_ = -1;
        for (int& c : lastCc_) c = -1;
        restart_ = true;
    }
    cv_.notify_all();
    p_->resetClock();
    audio_.flush();
    std::lock_guard<std::mutex> lk(p_->smu);
    p_->st = PlayerStats();
    p_->st.serviceId = sid;
    p_->st.active = sid >= 0;
    p_->st.status = sid >= 0 ? "starting..." : "stopped";
    p_->tracks.clear();
    p_->current.reset();
}

void Player::push(const uint8_t* pk, size_t n, const TsSnapshot& snap) {
    if (sid_ < 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    static uint64_t calls = 0, passed = 0;
    uint8_t out[188];
    for (size_t i = 0; i < n; i++)
        if (filter_.process(pk + i * 188, &snap, out)) {
            const int pid = ((out[1] & 0x1F) << 8) | out[2];
            if (pid == videoPid_.load() && (out[3] & 0x10)) {   // a payload packet of the video stream: its continuity counter must follow the last one
                const int cc = out[3] & 0x0F;
                if (lastCc_[pid] >= 0 && cc != ((lastCc_[pid] + 1) & 0x0F) && cc != lastCc_[pid]) lossMarks_.push_back(pushed_);
                lastCc_[pid] = cc;
            }
            in_.insert(in_.end(), out, out + 188); passed++; pushed_ += 188;
        }
    calls++;
    if (getenv("DECT2_PLAYDEBUG") && calls % 8 == 0) fprintf(stderr, "[play] push #%llu: %zu pkts in, %llu passed total, queue %zu\n", (unsigned long long)calls, n, (unsigned long long)passed, in_.size());
    if (in_.size() > 12u * 1024 * 1024) in_.erase(in_.begin(), in_.begin() + (in_.size() - 8u * 1024 * 1024)); // never fall far behind
    cv_.notify_all();
}

void Player::setAudioTrack(int i) { p_->trackChange = i; }

std::vector<AudioTrackInfo> Player::audioTracks() const { std::lock_guard<std::mutex> lk(p_->smu); return p_->tracks; }

PlayerStats Player::stats() const {
    // The statistics lock must not be held while the picture-queue lock is taken: the player thread (and videoFrame()) take them in the
    // opposite order, and two threads each holding one and waiting for the other freeze the interface for good.
    PlayerStats s;
    { std::lock_guard<std::mutex> lk(p_->smu); s = p_->st; }
    s.audioBufferMs = audio_.bufferedFrames() * 1000.0 / audio_.sampleRate();
    s.underruns = audio_.underruns();
    { std::lock_guard<std::mutex> l2(p_->vmu); s.videoQueue = (int)p_->vq.size(); }
    return s;
}

bool Player::clockNow(double& clock) {
    std::lock_guard<std::mutex> lk(p_->cmu);
    if (p_->baseValid) {
        uint64_t played = audio_.playedFrames();
        if (played > p_->baseIndex) { clock = p_->basePts + (double)(played - p_->baseIndex) / audio_.sampleRate(); return true; }
        return false; // audio has not started playing yet
    }
    if (p_->wallValid && nowSec() >= p_->wallT0) { clock = p_->wallPts + (nowSec() - p_->wallT0); return true; }
    return false;
}

std::shared_ptr<const VideoFrame> Player::videoFrame(uint64_t& seqIn) {
    double clock = 0;
    bool haveClock = clockNow(clock);
    if (getenv("DECT2_PLAYDEBUG")) { static int c = 0; if ((c++ % 120) == 0) { std::lock_guard<std::mutex> lk(p_->vmu); fprintf(stderr, "[clock] have=%d clock=%.3f front=%.3f back=%.3f vq=%zu played=%llu written=%llu abuf=%d basePts=%.3f baseIdx=%llu\n", haveClock, clock, p_->vq.empty() ? 0.0 : p_->vq.front()->pts, p_->vq.empty() ? 0.0 : p_->vq.back()->pts, p_->vq.size(), (unsigned long long)audio_.playedFrames(), (unsigned long long)audio_.writtenFrames(), audio_.bufferedFrames(), p_->basePts, (unsigned long long)p_->baseIndex); } }
    std::lock_guard<std::mutex> lk(p_->vmu);
    if (!haveClock) {
        if (p_->seq != seqIn) { seqIn = p_->seq; return p_->current; }
        return nullptr;
    }
    // wall-clock mode starts from the first frame
    while (p_->vq.size() > 1 && p_->vq.front()->pts < clock - 0.08) {
        p_->vq.pop_front();
        std::lock_guard<std::mutex> sl(p_->smu);
        p_->st.late++;
    }
    if (!p_->vq.empty() && (p_->vq.front()->pts <= clock + 0.005 || p_->vq.front()->pts > clock + 2.0)) {
        p_->current = p_->vq.front();
        p_->vq.pop_front();
        p_->seq++;
        std::lock_guard<std::mutex> sl(p_->smu);
        p_->st.shown++;
        p_->st.avOffsetMs = (p_->current->pts - clock) * 1000.0;
        if (getenv("DECT2_PLAYDEBUG")) fprintf(stderr, "[shown] #%llu pts %.3f clock %.3f off %+.0f ms vq %zu t=%.3f\n", (unsigned long long)p_->st.shown, p_->current->pts, clock, (p_->current->pts - clock) * 1000.0, p_->vq.size(), nowSec());
    }
    if (p_->seq != seqIn) { seqIn = p_->seq; return p_->current; }
    return nullptr;
}

// ------------------------------------------------------------------ decoding session

#ifdef __APPLE__
static enum AVPixelFormat pickHw(AVCodecContext*, const enum AVPixelFormat* fmts) {
    for (const enum AVPixelFormat* f = fmts; *f != AV_PIX_FMT_NONE; f++) if (*f == AV_PIX_FMT_VIDEOTOOLBOX) return *f;
    return fmts[0];
}
#elif defined(_WIN32)
// Direct3D 11 video decoding (every Windows GPU since about 2012). If the GPU cannot decode this stream, FFmpeg calls this again with the
// list minus the failed format, and the plain software format at the end of the list is taken.
static enum AVPixelFormat pickHw(AVCodecContext*, const enum AVPixelFormat* fmts) {
    for (const enum AVPixelFormat* f = fmts; *f != AV_PIX_FMT_NONE; f++) if (*f == AV_PIX_FMT_D3D11) return *f;
    return fmts[0];
}
static std::atomic<bool> gHwVideoBroken{false};   // set when hardware decoding produced pictures that could not be fetched: software from then on
#endif

void Player::threadMain() {
    setThreadPriority(ThreadPriority::High);
    av_log_set_level(getenv("DECT2_PLAYDEBUG") ? AV_LOG_ERROR : AV_LOG_FATAL); // damaged streams make the H.264 decoder very chatty
    Impl& I = *p_;
    while (!stop_) {
        if (sid_ < 0) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        restart_ = false;

        AVFormatContext* fmt = avformat_alloc_context();
        unsigned char* iobuf = (unsigned char*)av_malloc(3760);
        AVIOContext* io = avio_alloc_context(iobuf, 3760, 0, &I, &Impl::readCb, nullptr, nullptr);
        fmt->pb = io;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_DISCARD_CORRUPT;
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "probesize", "600000", 0);
        av_dict_set(&opts, "analyzeduration", "1500000", 0);
        const AVInputFormat* ts = av_find_input_format("mpegts");
        if (getenv("DECT2_PLAYDEBUG")) fprintf(stderr, "[play] opening input\n");
        int rc = avformat_open_input(&fmt, "", ts, &opts);
        av_dict_free(&opts);
        auto cleanup = [&](AVFormatContext*& f) {
            if (f) { AVIOContext* pb = f->pb; avformat_close_input(&f); if (pb) { av_freep(&pb->buffer); avio_context_free(&pb); } }
            else { av_freep(&iobuf); }
        };
        if (rc < 0 || stop_ || restart_) { cleanup(fmt); if (rc < 0 && !stop_ && !restart_) std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
        if (avformat_find_stream_info(fmt, nullptr) < 0 || restart_) { cleanup(fmt); continue; }

        if (getenv("DECT2_PLAYDEBUG")) fprintf(stderr, "[play] streams found: %u\n", fmt->nb_streams);
        int vIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        videoPid_ = vIdx >= 0 ? fmt->streams[vIdx]->id : -1;
        I.lossFlag = false;
        std::vector<int> aIdx;
        int sIdx = -1;
        for (unsigned i = 0; i < fmt->nb_streams; i++) {
            AVCodecParameters* cp = fmt->streams[i]->codecpar;
            if (cp->codec_type == AVMEDIA_TYPE_AUDIO) aIdx.push_back((int)i);
            if (cp->codec_id == AV_CODEC_ID_DVB_SUBTITLE && sIdx < 0) sIdx = (int)i;
        }
        subsAvail_ = sIdx >= 0;
        {
            std::lock_guard<std::mutex> lk(I.smu);
            I.tracks.clear();
            for (int i : aIdx) {
                AVCodecParameters* cp = fmt->streams[i]->codecpar;
                AudioTrackInfo t;
                t.pid = fmt->streams[i]->id;
                t.codec = avcodec_get_name(cp->codec_id);
                AVDictionaryEntry* l = av_dict_get(fmt->streams[i]->metadata, "language", nullptr, 0);
                t.lang = l ? l->value : "";
                t.channels = cp->ch_layout.nb_channels;
                I.tracks.push_back(t);
            }
        }
        int curTrack = std::min(std::max(0, I.wantTrack), std::max(0, (int)aIdx.size() - 1));

        // ---- video decoder
        AVCodecContext* vctx = nullptr;
        bool hw = false;
        if (vIdx >= 0) {
            const AVCodec* c = avcodec_find_decoder(fmt->streams[vIdx]->codecpar->codec_id);
            if (c) {
                vctx = avcodec_alloc_context3(c);
                avcodec_parameters_to_context(vctx, fmt->streams[vIdx]->codecpar);
                AVBufferRef* hwdev = nullptr;
                const char* swEnv = getenv("DECT2_SWVIDEO");
                // (the user switch and the environment override); a picture size that is not known yet (the stream was joined before its first parameter
                // sets) rules hardware decoding out, because it cannot be opened without it: the software decoder learns the size from the stream
                const bool hwOk = hwAllowed_ && !(swEnv && *swEnv) && fmt->streams[vIdx]->codecpar->width > 0 && fmt->streams[vIdx]->codecpar->height > 0;
                // damaged streams: let the decoder conceal missing slices and keep outputting pictures instead of waiting for a keyframe
                vctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
                vctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
#ifdef __APPLE__   // hardware decoding: VideoToolbox (macOS) or Direct3D 11 (Windows); VAAPI for Linux is still to do (software decoding is used there)
                if (hwOk && av_hwdevice_ctx_create(&hwdev, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0) >= 0) {
                    vctx->hw_device_ctx = av_buffer_ref(hwdev);
                    vctx->get_format = pickHw;
                    hw = true;
                }
#elif defined(_WIN32)
                if (hwOk && !gHwVideoBroken && av_hwdevice_ctx_create(&hwdev, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) >= 0) {
                    vctx->hw_device_ctx = av_buffer_ref(hwdev);
                    vctx->get_format = pickHw;
                    hw = true;
                }
#else
                (void)swEnv;
                (void)hwOk;
#endif
                if (hwdev) av_buffer_unref(&hwdev);
                vctx->thread_count = hw ? 2 : 4;
                if (avcodec_open2(vctx, c, nullptr) < 0) {
                    avcodec_free_context(&vctx);
                    vctx = nullptr;
                    hw = false;
                }
            }
        }
        // ---- audio decoder (reopened when the track changes)
        AVCodecContext* actx = nullptr;
        SwrContext* swr = nullptr;
        auto openAudio = [&](int track) {
            if (actx) avcodec_free_context(&actx);
            if (swr) swr_free(&swr);
            if (track < 0 || track >= (int)aIdx.size()) return;
            AVStream* s = fmt->streams[aIdx[track]];
            const AVCodec* c = avcodec_find_decoder(s->codecpar->codec_id);
            if (!c) return;
            actx = avcodec_alloc_context3(c);
            avcodec_parameters_to_context(actx, s->codecpar);
            if (avcodec_open2(actx, c, nullptr) < 0) avcodec_free_context(&actx);
        };
        openAudio(curTrack);
        if (actx) audio_.start(48000);
        // ---- subtitle decoder
        AVCodecContext* sctx = nullptr;
        if (sIdx >= 0) {
            const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_DVB_SUBTITLE);
            if (c) { sctx = avcodec_alloc_context3(c); avcodec_parameters_to_context(sctx, fmt->streams[sIdx]->codecpar); if (avcodec_open2(sctx, c, nullptr) < 0) avcodec_free_context(&sctx); }
        }
        {
            std::lock_guard<std::mutex> lk(I.smu);
            I.st.status = "playing";
            I.st.hasVideo = vctx != nullptr;
            I.st.hasAudio = actx != nullptr;
            if (vctx) {
                I.st.videoCodec = avcodec_get_name(vctx->codec_id);
                I.st.hardware = hw;
                AVRational fr = fmt->streams[vIdx]->avg_frame_rate.num ? fmt->streams[vIdx]->avg_frame_rate : fmt->streams[vIdx]->r_frame_rate;
                I.st.fps = fr.den ? (double)fr.num / fr.den : 0;
            }
            if (actx) { I.st.audioCodec = avcodec_get_name(actx->codec_id); I.st.audioChannels = actx->ch_layout.nb_channels; }
        }
        audio_.setStartThreshold((int)(1.2 * 48000)); // the stream arrives in 242 ms bursts and decoding time varies: keep a jitter buffer

        AVPacket* pkt = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        AVFrame* sw = av_frame_alloc();
        double lastVPts = 0, vFrameDur = 0.04;
        double recentSteps[32]; int recentN = 0;   // picture intervals seen lately: the stream's own frame rate (the container metadata can claim the field rate)
        std::shared_ptr<VideoFrame> lastVf;
        // picture repair: after a loss on the video stream the pictures are suspect until a clean keyframe; they are held back and
        // replaced by pictures generated from the last good one and the keyframe (if that is not too far apart)
        bool suspect = false;
        std::vector<std::shared_ptr<VideoFrame>> held;
        { std::lock_guard<std::mutex> lk(I.smu); if (I.st.fps > 1) vFrameDur = 1.0 / I.st.fps; }
        bool haveVPts = false;
        SwsContext* sws = nullptr;
        SwsContext* nv12ctx = nullptr;
        std::vector<float> abuf;
        int swrRate = 0; AVSampleFormat swrFmt = AV_SAMPLE_FMT_NONE; int swrCh = 0;
        bool vSusValid = false; double vSusNext = 0;   // the same for picture timestamps
        bool susValid = false; double susD = 0, susNext = 0;   // an audio timestamp jump waiting for the next frame to confirm it
        bool firstVideo = true;

        auto convertVideo = [&](AVFrame* f) {
            AVFrame* src = f;
#ifdef __APPLE__
            if (f->format == AV_PIX_FMT_VIDEOTOOLBOX) {
                av_frame_unref(sw);
                if (av_hwframe_transfer_data(sw, f, 0) < 0) return;
                sw->pts = f->pts; sw->best_effort_timestamp = f->best_effort_timestamp;
                sw->colorspace = f->colorspace; sw->color_range = f->color_range;
                src = sw;
            }
#elif defined(_WIN32)
            if (f->format == AV_PIX_FMT_D3D11) {
                av_frame_unref(sw);
                if (av_hwframe_transfer_data(sw, f, 0) < 0) {
                    // the GPU decoded the picture but it cannot be copied out: after a few of these use the software decoder instead
                    static int fails = 0;
                    if (++fails >= 5) { gHwVideoBroken = true; restart_ = true; }
                    return;
                }
                sw->pts = f->pts; sw->best_effort_timestamp = f->best_effort_timestamp;
                sw->colorspace = f->colorspace; sw->color_range = f->color_range;
                src = sw;
            }
#endif
            const int w = src->width, h = src->height;
            const bool b709 = src->colorspace == AVCOL_SPC_BT709 || (src->colorspace == AVCOL_SPC_UNSPECIFIED && h >= 720);
            auto vf = std::make_shared<VideoFrame>();
            vf->w = w; vf->h = h;
            {   // the frame's own SAR, else the stream's / codec's (HW frames carry it too, but the stream value is a safe fallback)
                AVRational sar = av_guess_sample_aspect_ratio(fmt, fmt->streams[vIdx], f);
                if (sar.num <= 0 || sar.den <= 0) sar = src->sample_aspect_ratio;
                vf->dar = displayAspect(w, h, sar.num, sar.den);
            }
            vf->bt709 = b709; vf->fullRange = src->color_range == AVCOL_RANGE_JPEG;
            vf->interlaced = (src->flags & AV_FRAME_FLAG_INTERLACED) != 0;
            int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
            if (ts != AV_NOPTS_VALUE) {
                vf->pts = ts * av_q2d(fmt->streams[vIdx]->time_base);
                if (getenv("DECT2_BADVPTS")) { static int cnt = 0; if (++cnt % 100 == 0) vf->pts += 11.0; }   // test: a damaged picture timestamp now and then
                // a timestamp far from the previous picture is either a real jump or damage (a bit error in the PES header): one wild
                // value would put the picture seconds off the audio clock, so a jump counts only once the next picture follows on from it
                if (haveVPts && std::fabs(vf->pts - lastVPts) > 1.5) {
                    if (vSusValid && std::fabs(vf->pts - vSusNext) < 0.1) vSusValid = false;   // confirmed: accept the new timeline
                    else { vSusValid = true; vSusNext = vf->pts + vFrameDur; vf->pts = lastVPts + vFrameDur; }
                } else vSusValid = false;
            } else {
                // concealed/damaged pictures often come without a timestamp: extrapolate so the picture is still shown
                if (!haveVPts) return;
                vf->pts = lastVPts + vFrameDur;
                I.st.errors += 0;
            }
            // The metadata can give the field rate (50) for a stream whose pictures come 25 times a second; every picture would then look
            // like a hole in the sequence and be "repaired". So the typical interval is taken from the timestamps themselves.
            if (haveVPts) {
                const double step = vf->pts - lastVPts;
                if (step > 0.005 && step < 0.1) {   // an ordinary picture interval (a hole or a jump is longer)
                    recentSteps[recentN++ % 32] = step;
                    if (recentN >= 8) {
                        double tmp[32];
                        const int cnt = std::min(recentN, 32);
                        std::copy(recentSteps, recentSteps + cnt, tmp);
                        std::nth_element(tmp, tmp + cnt / 2, tmp + cnt);
                        vFrameDur = tmp[cnt / 2];
                    }
                }
            }
            lastVPts = vf->pts; haveVPts = true;
            // DVB subtitle overlay: only frames that carry a subtitle are converted to RGBA on the CPU
            std::shared_ptr<Subtitle> sb;
            if (subsOn_) { std::lock_guard<std::mutex> lk(I.submu); sb = I.sub; }
            const bool needRgba = sb && vf->pts >= sb->start && vf->pts <= sb->end;
            if (needRgba) {
                sws = sws_getCachedContext(sws, w, h, (AVPixelFormat)src->format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!sws) return;
                const int* coef = sws_getCoefficients(b709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
                sws_setColorspaceDetails(sws, coef, vf->fullRange, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
                vf->rgba.resize((size_t)w * h * 4);
                uint8_t* dst[4] = {vf->rgba.data(), nullptr, nullptr, nullptr};
                int ls[4] = {w * 4, 0, 0, 0};
                sws_scale(sws, src->data, src->linesize, 0, h, dst, ls);
                {
                    const double sx = (double)w / sb->canvasW, sy = (double)h / sb->canvasH;
                    for (auto& im : sb->images) {
                        for (int yy = 0; yy < im.h; yy++) {
                            int dy = (int)((im.y + yy) * sy);
                            if (dy < 0 || dy >= h) continue;
                            for (int xx = 0; xx < im.w; xx++) {
                                int dx = (int)((im.x + xx) * sx);
                                if (dx < 0 || dx >= w) continue;
                                const uint8_t* s = &im.rgba[((size_t)yy * im.w + xx) * 4];
                                if (!s[3]) continue;
                                uint8_t* d = &vf->rgba[((size_t)dy * w + dx) * 4];
                                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                            }
                        }
                    }
                }
            } else {
                const int hc = h / 2;
                vf->y.resize((size_t)w * h); vf->uv.resize((size_t)w * hc);
                if (src->format == AV_PIX_FMT_NV12) {
                    for (int yy = 0; yy < h; yy++) memcpy(&vf->y[(size_t)yy * w], src->data[0] + (size_t)yy * src->linesize[0], w);
                    for (int yy = 0; yy < hc; yy++) memcpy(&vf->uv[(size_t)yy * w], src->data[1] + (size_t)yy * src->linesize[1], w);
                } else { // software decoder output (planar): a plain repack to NV12, no scaling and no colour maths
                    nv12ctx = sws_getCachedContext(nv12ctx, w, h, (AVPixelFormat)src->format, w, h, AV_PIX_FMT_NV12, SWS_POINT, nullptr, nullptr, nullptr);
                    if (!nv12ctx) return;
                    uint8_t* dst[4] = {vf->y.data(), vf->uv.data(), nullptr, nullptr};
                    int ls[4] = {w, w, 0, 0};
                    sws_scale(nv12ctx, src->data, src->linesize, 0, h, dst, ls);
                }
            }
            if (firstVideo) {
                firstVideo = false;
                if (conceal_) prepareInterpolation(w, h);
                std::lock_guard<std::mutex> lk(I.smu);
                I.st.width = w; I.st.height = h;
            }
            {   // wall-clock fallback when there is no audio
                std::lock_guard<std::mutex> lk(I.cmu);
                if (!actx && !I.wallValid) { I.wallValid = true; I.wallPts = vf->pts; I.wallT0 = nowSec() + 0.35; }
            }
            const bool keyframe = (f->flags & AV_FRAME_FLAG_KEY) != 0;
            auto pushPlain = [&](const std::shared_ptr<VideoFrame>& p) {
                std::lock_guard<std::mutex> lk(I.vmu);
                I.vq.push_back(p);
                while (I.vq.size() > 80) { I.vq.pop_front(); std::lock_guard<std::mutex> sl(I.smu); I.st.late++; }
            };
            auto flushHeld = [&]() { for (auto& h : held) { pushPlain(h); lastVf = h; } held.clear(); suspect = false; };
            if (suspect && conceal_ && !getenv("DECT2_NOREPAIR") && vf->rgba.empty()) {
                if (keyframe && held.size() >= 2) {
                    // the span ends with a clean keyframe: generate the pictures of the span from the last good picture and this one
                    suspect = false;
                    const double D = lastVf ? vf->pts - lastVf->pts : 0;
                    if (getenv("DECT2_PLAYDEBUG")) fprintf(stderr, "[play] keyframe ends the suspect span: %zu held pictures, %.2f s since the last good one\n", held.size(), D);
                    if (lastVf && lastVf->rgba.empty() && D > 1.5 * vFrameDur && D <= 1.6) {
                        const int cnt = std::min(48, (int)std::lround(D / vFrameDur) - 1);
                        std::vector<std::shared_ptr<VideoFrame>> mid;
                        const auto tI0 = std::chrono::steady_clock::now();
                        const bool madeMid = cnt >= 1 && interpolateGap(*lastVf, *vf, cnt, mid);
                        if (getenv("DECT2_PLAYDEBUG")) fprintf(stderr, "[play] repair: %d pictures between two %dx%d pictures took %.0f ms (%s)\n", cnt, w, h, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tI0).count(), interpolationBackend());
                        if (madeMid) {
                            for (int k = 0; k < (int)mid.size(); k++) mid[(size_t)k]->pts = lastVf->pts + (k + 1) * D / (cnt + 1);
                            for (auto& m : mid) pushPlain(m);
                            lastVf = mid.back();
                            std::lock_guard<std::mutex> sl(I.smu);
                            I.st.repairEvents++; I.st.repairedFrames += (uint64_t)mid.size(); I.st.concealBackend = interpolationBackend();
                            held.clear();
                        } else flushHeld();
                    } else flushHeld();
                } else {
                    held.push_back(vf);
                    if (!lastVf || vf->pts - lastVf->pts > 1.6) flushHeld();   // too long to bridge: show what the decoder made
                    return;
                }
            }
            // A hole in the picture sequence (the stream lost data): synthesise the missing pictures from the ones on both sides
            if (lastVf && conceal_ && !getenv("DECT2_NOCONCEAL") && vf->rgba.empty() && lastVf->rgba.empty()) {
                const double gap = vf->pts - lastVf->pts;
                if (getenv("DECT2_PLAYDEBUG") && std::fabs(gap - vFrameDur) > 0.5 * vFrameDur) fprintf(stderr, "[play] video pts step %.3f s (expected %.3f)\n", gap, vFrameDur);
                if (gap > 1.75 * vFrameDur && gap <= 1.5) {
                    const int cnt = std::min(40, (int)std::lround(gap / vFrameDur) - 1);
                    std::vector<std::shared_ptr<VideoFrame>> mid;
                    const auto tI0 = std::chrono::steady_clock::now();
                    const bool madeMid = cnt >= 1 && interpolateGap(*lastVf, *vf, cnt, mid);
                    if (getenv("DECT2_PLAYDEBUG")) fprintf(stderr, "[play] gap fill: %d pictures took %.0f ms (%s)\n", cnt, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tI0).count(), interpolationBackend());
                    if (madeMid) {
                        std::lock_guard<std::mutex> lk(I.vmu);
                        for (int k = 0; k < (int)mid.size(); k++) { mid[(size_t)k]->pts = lastVf->pts + (k + 1) * gap / (cnt + 1); I.vq.push_back(mid[(size_t)k]); }
                        std::lock_guard<std::mutex> sl(I.smu);
                        I.st.concealedFrames += (uint64_t)mid.size();
                        I.st.concealBackend = interpolationBackend();
                        I.st.concealEvents++;
                    }
                }
            }
            lastVf = vf;
            std::lock_guard<std::mutex> lk(I.vmu);
            I.vq.push_back(vf);
            while (I.vq.size() > 80) { I.vq.pop_front(); std::lock_guard<std::mutex> sl(I.smu); I.st.late++; }
        };

        auto pushAudio = [&](AVFrame* f) {
            if (!actx) return;
            if (!swr || swrRate != f->sample_rate || swrFmt != (AVSampleFormat)f->format || swrCh != f->ch_layout.nb_channels) {
                if (swr) swr_free(&swr);
                AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
                swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_FLT, 48000, &f->ch_layout, (AVSampleFormat)f->format, f->sample_rate, 0, nullptr);
                if (!swr || swr_init(swr) < 0) { swr_free(&swr); return; }
                swrRate = f->sample_rate; swrFmt = (AVSampleFormat)f->format; swrCh = f->ch_layout.nb_channels;
            }
            int maxOut = swr_get_out_samples(swr, f->nb_samples) + 64;
            abuf.resize((size_t)maxOut * 2);
            uint8_t* o[1] = {(uint8_t*)abuf.data()};
            int n = swr_convert(swr, o, maxOut, (const uint8_t**)f->extended_data, f->nb_samples);
            if (n <= 0) return;
            int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
            double pts = ts != AV_NOPTS_VALUE ? ts * av_q2d(fmt->streams[aIdx[curTrack]]->time_base) : I.nextPts;
            if (getenv("DECT2_BADPTS")) { static int cnt = 0; if (++cnt % 150 == 0) pts += 7.3; }   // test: a corrupted audio timestamp now and then
            double gapSec = 0;
            {
                std::lock_guard<std::mutex> lk(I.cmu);
                bool rebase = !I.baseValid;
                if (I.baseValid) {
                    // A timestamp that does not follow on from the previous audio frame is either real (data was lost, or the broadcaster
                    // jumped) or damaged (a bit error in the PES header after a transport loss: one frame with a wild value). Acting on a
                    // damaged one resets the clock by seconds and throws away every queued picture, then does it again when the next frame
                    // is normal. So a jump counts only once the following frame continues from it; until then the frame is played as if
                    // it had arrived on time.
                    const double d = pts - I.nextPts;
                    if (d > 0.02 || std::fabs(d) > 0.15) {
                        if (susValid && std::fabs(pts - susNext) < 0.06) {          // the next frame follows the jump: it is real
                            if (susD > 0.02 && susD <= 4.0) gapSec = susD;           // lost data: fill with silence, keep the timeline
                            else if (std::fabs(susD) > 0.15) rebase = true;           // a genuine discontinuity
                            susValid = false;
                        } else {
                            susValid = true; susD = d; susNext = pts + (double)n / 48000.0;
                            pts = I.nextPts;                                         // play it as continuous for now
                        }
                    } else susValid = false;
                }
                if (rebase) {
                    bool had = I.baseValid;
                    if (getenv("DECT2_PLAYDEBUG")) fprintf(stderr, "[play] audio clock reset: pts %.3f expected %.3f (had=%d)\n", pts, I.nextPts, (int)had);
                    I.basePts = pts;
                    // the device index at which this frame will be played: what has been played plus what is still queued ahead of it.
                    // (Not the number of frames ever written: audio thrown away by a flush at a service change is counted there but never
                    // played, and the clock would then stay invalid for as long as that audio was long, with every picture dropped as late.)
                    I.baseIndex = audio_.playedFrames() + (uint64_t)std::max(0, audio_.bufferedFrames());
                    I.baseValid = true;
                    if (had) { std::lock_guard<std::mutex> l2(I.vmu); I.vq.clear(); } // genuine timestamp jump: drop stale pictures
                }
                I.nextPts = pts + (double)n / 48000.0;
            }
            // A gap in the audio (a deep fade swallowed part of the stream): play silence for the missing time, which keeps the timeline
            // continuous so that the pictures of the gap are not thrown away.
            if (gapSec > 0) {
                if (getenv("DECT2_PLAYDEBUG")) fprintf(stderr, "[play] audio gap %.3f s filled with silence\n", gapSec);
                size_t gf = (size_t)(gapSec * 48000.0);
                std::vector<float> zeros((size_t)4800 * 2, 0.f);
                while (gf > 0 && !stop_ && !restart_) {
                    size_t c = std::min<size_t>(gf, 4800), dn = 0;
                    while (dn < c && !stop_ && !restart_) {
                        size_t w = audio_.write(zeros.data(), (int)(c - dn));
                        dn += w;
                        if (dn < c) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    gf -= dn;
                }
            }
            int done = 0;
            while (done < n && !stop_ && !restart_) {
                done += audio_.write(abuf.data() + (size_t)done * 2, n - done);
                if (done < n) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        };

        // ---- demux / decode loop
        while (!stop_ && !restart_) {
            // back-pressure: do not decode far ahead of the presentation clock. Only while the audio buffer is healthy,
            // otherwise the audio packets that keep the clock running could never be read.
            for (int w = 0; w < 400 && !stop_ && !restart_; w++) {
                double clock = 0, ahead = 0;
                bool running = clockNow(clock);
                { std::lock_guard<std::mutex> lk(I.vmu); if (!I.vq.empty()) ahead = I.vq.back()->pts - clock; }
                const int ab = audio_.bufferedFrames();
                bool block = (running && audio_.playing() && ab >= 38400 && ahead > 2.4) || ab >= 3 * 48000;
                if (!block) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            int tc = I.trackChange.exchange(-1);
            if (tc >= 0 && tc < (int)aIdx.size() && tc != curTrack) {
                curTrack = tc; I.wantTrack = tc;
                openAudio(curTrack);
                audio_.flush();
                { std::lock_guard<std::mutex> lk(I.cmu); I.baseValid = false; }
                std::lock_guard<std::mutex> lk(I.smu);
                if (actx) { I.st.audioCodec = avcodec_get_name(actx->codec_id); I.st.audioChannels = actx->ch_layout.nb_channels; }
            }
            int r = av_read_frame(fmt, pkt);
            if (getenv("DECT2_PLAYDEBUG")) { static int n = 0; if ((n++ % 100) == 0 || r < 0) fprintf(stderr, "[play] read r=%d idx=%d pts=%lld vq=%zu abuf=%d decoded=%llu\n", r, pkt->stream_index, (long long)pkt->pts, I.vq.size(), audio_.bufferedFrames(), (unsigned long long)I.st.decoded); }
            if (r < 0) break;
            if (pkt->stream_index == vIdx && vctx) {
                if (I.lossFlag.exchange(false)) {
                    if (!suspect && conceal_ && !getenv("DECT2_NOREPAIR")) {
                        // the damaged picture may already be queued (the decoder lags the demuxer by a few pictures): take the newest
                        // few back, as long as they are still far from being shown
                        double clk = 0; const bool running = clockNow(clk);
                        std::vector<std::shared_ptr<VideoFrame>> back;
                        {
                            std::lock_guard<std::mutex> lk(I.vmu);
                            while (back.size() < 3 && !I.vq.empty() && I.vq.back()->rgba.empty() && (!running || I.vq.back()->pts > clk + 0.6)) { back.insert(back.begin(), I.vq.back()); I.vq.pop_back(); }
                            if (!I.vq.empty()) lastVf = I.vq.back();
                        }
                        held.insert(held.begin(), back.begin(), back.end());
                    }
                    suspect = true; if (getenv("DECT2_PLAYDEBUG")) fprintf(stderr, "[play] transport loss on the video stream: pictures are suspect until a clean keyframe\n"); }   // the demuxer read past lost transport packets: pictures from here on are suspect
                if (avcodec_send_packet(vctx, pkt) >= 0)
                    while (avcodec_receive_frame(vctx, frame) >= 0) {
                        { std::lock_guard<std::mutex> lk(I.smu); I.st.decoded++; }
                        convertVideo(frame);
                        av_frame_unref(frame);
                    }
                else { std::lock_guard<std::mutex> lk(I.smu); I.st.errors++; }
            } else if (actx && curTrack < (int)aIdx.size() && pkt->stream_index == aIdx[curTrack]) {
                if (avcodec_send_packet(actx, pkt) >= 0)
                    while (avcodec_receive_frame(actx, frame) >= 0) { pushAudio(frame); av_frame_unref(frame); }
            } else if (sctx && pkt->stream_index == sIdx) {
                AVSubtitle as; int got = 0;
                if (avcodec_decode_subtitle2(sctx, &as, &got, pkt) >= 0 && got) {
                    auto sb = std::make_shared<Subtitle>();
                    double base = pkt->pts != AV_NOPTS_VALUE ? pkt->pts * av_q2d(fmt->streams[sIdx]->time_base) : 0;
                    sb->start = base + as.start_display_time / 1000.0;
                    sb->end = base + as.end_display_time / 1000.0;
                    if (sctx->width > 0) { sb->canvasW = sctx->width; sb->canvasH = sctx->height; }
                    for (unsigned k = 0; k < as.num_rects; k++) {
                        AVSubtitleRect* rc = as.rects[k];
                        if (rc->type != SUBTITLE_BITMAP || !rc->data[0] || !rc->data[1]) continue;
                        SubImage im; im.x = rc->x; im.y = rc->y; im.w = rc->w; im.h = rc->h;
                        im.rgba.resize((size_t)rc->w * rc->h * 4);
                        const uint32_t* pal = (const uint32_t*)rc->data[1];
                        for (int yy = 0; yy < rc->h; yy++) for (int xx = 0; xx < rc->w; xx++) {
                            uint32_t c = pal[rc->data[0][yy * rc->linesize[0] + xx]]; // BGRA in memory order of a uint32 on little-endian
                            uint8_t* d = &im.rgba[((size_t)yy * rc->w + xx) * 4];
                            d[0] = (c >> 16) & 0xFF; d[1] = (c >> 8) & 0xFF; d[2] = c & 0xFF; d[3] = (c >> 24) & 0xFF;
                        }
                        sb->images.push_back(std::move(im));
                    }
                    { std::lock_guard<std::mutex> lk(I.submu); I.sub = sb; }
                    avsubtitle_free(&as);
                }
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        av_frame_free(&frame);
        av_frame_free(&sw);
        if (sws) sws_freeContext(sws);
        if (nv12ctx) sws_freeContext(nv12ctx);
        if (swr) swr_free(&swr);
        if (actx) avcodec_free_context(&actx);
        if (vctx) avcodec_free_context(&vctx);
        if (sctx) avcodec_free_context(&sctx);
        cleanup(fmt);
        if (restart_ || stop_) { /* new session starts or we quit */ }
    }
}

} // namespace dect2
