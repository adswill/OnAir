// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tv/cc_decode.h"
#include "dect2/common/thread_name.h"
#include "dect2/tv/player.h"
#include "dect2/tv/hw_guard.h"
#include "dect2/tv/conceal.h"
#include "dect2/tv/tv_dra.h"
#include <functional>
#include "dect2/tv/avsplus_dec.h"
#include "dect2/audio/dolby_audio.h"
#include "dect2/audio/ac4_dec.h"
#include "dect2/common/diag.h"

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
#include "dect2/common/platform.h"
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
#include "dect2/common/timecompat.h"

namespace dect2 {

// ---- the AVS family and DRA (docs/CODECS.md): OnAir ships no codec of its own, it says what its FFmpeg cannot decode
const char* const kDraAudioNote = "DRA audio: install the DRA add-on to hear this service";

int avsProfileId(const uint8_t* p, size_t n) {
    for (size_t i = 0; i + 4 < n; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && p[i + 3] == 0xB0) return p[i + 4];
    return -1;
}

std::string avsVideoNote(const std::string& codec, bool haveDecoder, int profile) {
    if (codec == "avs2") return haveDecoder ? std::string() : "AVS2 video: this FFmpeg has no AVS2 decoder (it needs libdavs2), so there is no picture";
    if (codec == "avs3") return haveDecoder ? std::string() : "AVS3 video: this FFmpeg has no AVS3 decoder (it needs libuavs3d), so there is no picture";
    if (codec != "cavs") return std::string();
    if (!haveDecoder) return "AVS video: this FFmpeg has no AVS decoder, so there is no picture";
    if (profile < 0 || profile == 0x20) return std::string();   // JiZhun: FFmpeg's cavs decoder plays it
    if (profile == 0x48) return "AVS+ video: FFmpeg decodes only the AVS JiZhun profile, not AVS+ (broadcast profile 0x48), so there is no picture";
    char b[160];
    snprintf(b, sizeof b, "AVS video (profile 0x%02X): FFmpeg decodes only the AVS JiZhun profile, so there is no picture", profile);
    return b;
}

std::string avsFamilyName(int streamType, int profile) {
    switch (streamType) {
    case 0x42: return profile == 0x20 ? "AVS" : profile == 0x48 ? "AVS+" : profile < 0 ? "AVS/AVS+" : "AVS";
    case 0xD2: return "AVS2";
    case 0xD4: return "AVS3";
    default: return std::string();
    }
}

bool avsNeverDecodable(int streamType, int profile) { return streamType == 0x42 && profile >= 0 && profile != 0x20; }

std::string avsServiceMessage(int streamType, int profile, bool haveDecoder) {
    const std::string fam = avsFamilyName(streamType, profile);
    if (fam.empty()) return std::string();
    if (streamType == 0x42 && !avsNeverDecodable(streamType, profile) && haveDecoder) return std::string();
    if ((streamType == 0xD2 || streamType == 0xD4) && haveDecoder) return std::string();
    return "Video: " + fam + " (OnAir cannot decode this video) \xE2\x80\x94 audio, subtitles and data still work";
}

using Clock = std::chrono::steady_clock;
static double nowSec() { return dect2::steadyNowSec(); }

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
    std::mutex ccMu; CaptionDecoder cc;      // CEA-608 / 708 captions from the video's user data
    std::mutex submu;
    std::atomic<bool> lossFlag{false};   // set when the demuxer has read past a transport-stream loss on the video PID

    static int readCb(void* opaque, uint8_t* buf, int size) {
        auto* I = static_cast<Impl*>(opaque);
        Player* p = I->owner;
        std::unique_lock<std::mutex> lk(p->mu_);
        int waits = 0;
        if (p->shifted_ && p->in_.size() < 65536) p->pumpLocked();
        while (p->in_.empty() && !p->stop_ && !p->restart_) { p->cv_.wait_for(lk, std::chrono::milliseconds(p->shifted_ && !p->paused_ ? 20 : 100)); if (p->shifted_ && p->in_.empty()) p->pumpLocked(); if (getenv("DECT2_PLAYDEBUG") && ++waits % 10 == 0) fprintf(stderr, "[play] readCb waiting for data (%d s) sid=%d\n", waits / 10, p->sid_.load()); }
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
    recFilter_.setStrict(true);
    ring_.setCapacityPackets((size_t)bufMb_ * 1024 * 1024 / 188);
    p_->owner = this;
    th_ = dect2::namedThread("onair-tvdec", [this] { threadMain(); });
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
        saveSink_.close();
        recWriter_.close();
        recFilter_.select(sid);
        ring_.clear(); pcrIdx_.clear(); cursor_ = 0; shifted_ = paused_ = false; pcrPid_ = -1;
        sid_ = sid; hwFallback_ = false;
        filter_.select(sid);
        in_.clear();
        lossMarks_.clear(); pushed_ = consumed_ = 0; videoPid_ = -1; draAudio_ = false; draTracks_.clear();
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
    for (const TsService& sv : snap.services) if (sv.id == sid_) { bool d = false; for (const TsStream& st : sv.streams) d = d || st.codec == "DRA"; draAudio_ = d;
        draTracks_.clear();
        for (const TsStream& st : sv.streams) if (st.codec == "DRA" && !st.scrambled) { AudioTrackInfo t; t.pid = st.pid; t.codec = "DRA"; t.lang = st.lang; t.channels = 2; draTracks_.push_back(t); }
        esPid_ = -1;   // the stream "Save stream" writes: DRA, else MPEG audio
        for (const TsStream& st : sv.streams) if (st.codec == "DRA" && !st.scrambled) { esPid_ = st.pid; esCodec_ = st.codec; break; }
        if (esPid_ < 0) for (const TsStream& st : sv.streams) if ((st.codec == "MPEG audio" || st.codec == "MPEG-1 audio") && !st.scrambled) { esPid_ = st.pid; esCodec_ = st.codec; break; } }
    static uint64_t calls = 0, passed = 0;
    uint8_t out[188];
    for (const TsService& sv : snap.services) if (sv.id == sid_) pcrPid_ = sv.pcrPid;
    const bool rec = recWriter_.isOpen();
    std::vector<uint8_t> recBuf;
    for (size_t i = 0; i < n; i++) {
        if (rec && recFilter_.process(pk + i * 188, &snap, out)) recBuf.insert(recBuf.end(), out, out + 188);
        if (filter_.process(pk + i * 188, &snap, out)) {
            double pcr;
            const int pid = ((out[1] & 0x1F) << 8) | out[2];
            if (pid == pcrPid_ && tsPcrSeconds(out, pcr)) {
                pcrIdx_.push_back({ring_.head(), pcr});
                while (!pcrIdx_.empty() && pcrIdx_.front().idx < ring_.oldest()) pcrIdx_.pop_front();
            }
            ring_.push(out);
            if (!shifted_) { feedLocked(out); cursor_ = ring_.head(); }
            passed++;
        }
    }
    if (!recBuf.empty()) recWriter_.write(recBuf.data(), recBuf.size());
    if (rec && recDue()) recWriter_.close();
    if (shifted_) pumpLocked();
    calls++;
    if (getenv("DECT2_PLAYDEBUG") && calls % 8 == 0) fprintf(stderr, "[play] push #%llu: %zu pkts in, %llu passed total, queue %zu\n", (unsigned long long)calls, n, (unsigned long long)passed, in_.size());
    if (in_.size() > 12u * 1024 * 1024) in_.erase(in_.begin(), in_.begin() + (in_.size() - 8u * 1024 * 1024)); // never fall far behind
    cv_.notify_all();
}

void Player::feedLocked(const uint8_t* out) {
    const int pid = ((out[1] & 0x1F) << 8) | out[2];
    if (pid == videoPid_.load() && (out[3] & 0x10)) {   // a payload packet of the video stream: its continuity counter must follow the last one
        const int cc = out[3] & 0x0F;
        if (lastCc_[pid] >= 0 && cc != ((lastCc_[pid] + 1) & 0x0F) && cc != lastCc_[pid]) lossMarks_.push_back(pushed_);
        lastCc_[pid] = cc;
    }
    if (saveSink_.isOpen()) saveEs_.feed(out, saveSink_);
    in_.insert(in_.end(), out, out + 188); pushed_ += 188;
}

double Player::pcrAt(uint64_t idx) const {   // the PCR at or before packet idx (the first one if there is none before)
    if (pcrIdx_.empty()) return 0;
    size_t lo = 0, hi = pcrIdx_.size();
    while (lo < hi) { const size_t m = (lo + hi) / 2; if (pcrIdx_[m].idx <= idx) lo = m + 1; else hi = m; }
    return pcrIdx_[lo ? lo - 1 : 0].pcr;
}

void Player::goLiveLocked() {
    shifted_ = paused_ = false;
    cursor_ = ring_.head();
    in_.clear();
    lossMarks_.push_back(pushed_);   // a jump in the stream: let the repair logic treat it like a loss
    for (int& c : lastCc_) c = -1;
}

// Feeds the player from the ring at the stream's own pace: a packet leaves the ring when the PCR distance from the resume point has passed in real time.
void Player::pumpLocked() {
    if (!shifted_ || paused_) return;
    if (cursor_ < ring_.oldest()) cursor_ = ring_.oldest();            // the buffer overflowed under the play point
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - anchorWall_).count();
    uint8_t pk[188];
    int fed = 0;
    while (cursor_ < ring_.head() && fed < 4096) {
        if (!ring_.read(cursor_, pk)) { cursor_++; continue; }
        double pcr;
        const int pid = ((pk[1] & 0x1F) << 8) | pk[2];
        if (pid == pcrPid_ && tsPcrSeconds(pk, pcr)) {
            if (anchorPending_ || pcr < anchorPcr_ || pcr - anchorPcr_ > elapsed + 3600) { anchorPcr_ = pcr; anchorWall_ = std::chrono::steady_clock::now(); anchorPending_ = false; }
            else if (pcr - anchorPcr_ > elapsed + 0.2) break;      // not due yet (a 0.2 s lead keeps the decoder fed)
        }
        feedLocked(pk);
        cursor_++; fed++;
    }
    if (fed) cv_.notify_all();
    if (cursor_ >= ring_.head()) goLiveLocked();                 // caught up with the live edge
}

void Player::setBufferMB(int mb) {
    mb = std::max(16, std::min(4096, mb));
    std::lock_guard<std::mutex> lk(mu_);
    if (mb == bufMb_) return;
    bufMb_ = mb;
    if (shifted_) goLiveLocked();
    ring_.setCapacityPackets((size_t)mb * 1024 * 1024 / 188);
    pcrIdx_.clear(); cursor_ = 0;
}

std::string Player::latestCaption(size_t* count) const {
    Impl& I = *p_;
    std::lock_guard<std::mutex> lk(I.ccMu);
    if (count) *count = I.cc.cues().size();
    return I.cc.cues().empty() ? std::string() : I.cc.cues().back().text;
}

void Player::pause() {
    std::lock_guard<std::mutex> lk(mu_);
    if (sid_ < 0 || paused_) return;
    if (!shifted_) cursor_ = ring_.head();
    shifted_ = paused_ = true;
    audio_.flush();   // silence at once; the rest of the picture queue freezes with the stream
}

void Player::play() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!paused_) return;
    paused_ = false;
    anchorPending_ = true;
    pumpLocked();
}

void Player::seekBehind(double sec) {
    std::lock_guard<std::mutex> lk(mu_);
    if (sid_ < 0) return;
    if (sec <= 0.05 || pcrIdx_.empty()) { if (shifted_) goLiveLocked(); return; }
    const double target = pcrIdx_.back().pcr - sec;
    size_t k = 0;
    while (k + 1 < pcrIdx_.size() && pcrIdx_[k + 1].pcr <= target) k++;    // the PCR mark at or before the target
    cursor_ = std::max(pcrIdx_[k].idx, ring_.oldest());
    in_.clear();
    lossMarks_.push_back(pushed_);
    for (int& c : lastCc_) c = -1;
    shifted_ = true; paused_ = false; anchorPending_ = true;
    pumpLocked();
}

Player::ShiftStats Player::shift() const {
    std::lock_guard<std::mutex> lk(mu_);
    ShiftStats s;
    s.paused = paused_; s.behind = shifted_;
    s.capacityBytes = (uint64_t)ring_.capacity() * 188;
    s.bufferBytes = (ring_.head() - ring_.oldest()) * 188;
    if (!pcrIdx_.empty()) {
        s.bufferedSec = pcrIdx_.back().pcr - pcrAt(std::max(ring_.oldest(), pcrIdx_.front().idx));
        if (shifted_) s.behindSec = std::max(0.0, pcrIdx_.back().pcr - pcrAt(cursor_));
    }
    return s;
}

bool Player::recDue() const {
    if (recStopAfter_ > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - recStart_).count() >= recStopAfter_) return true;
    return recStopAt_ > 0 && (int64_t)time(nullptr) >= recStopAt_;
}

bool Player::startRecording(const std::string& path, double stopAfterSec, int64_t stopAtUnix) {
    std::lock_guard<std::mutex> lk(mu_);
    recWriter_.close();
    if (sid_ < 0) { DIAG_DEBUG(991865, "A recording of the playing service was requested but no service is playing, so no recording is started."); return false; }
    recFilter_.select(sid_);
    if (!recWriter_.open(path)) { DIAG_ERROR(357592, "The recording file %s could not be created, so the recording of the playing service is not started.", path.c_str()); return false; }
    recStart_ = std::chrono::steady_clock::now(); recStopAfter_ = stopAfterSec; recStopAt_ = stopAtUnix;
    return true;
}

void Player::stopRecording() { std::lock_guard<std::mutex> lk(mu_); recWriter_.close(); }

Player::RecStats Player::recording() const {
    std::lock_guard<std::mutex> lk(mu_);
    RecStats s;
    s.path = recWriter_.path();
    s.active = recWriter_.isOpen();
    s.bytes = recWriter_.bytes(); s.droppedPackets = recWriter_.droppedPackets(); s.failed = recWriter_.failed();
    if (s.active) {
        s.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - recStart_).count();
        if (recDue() || s.failed) { const_cast<TsWriter&>(recWriter_).close(); s.active = false; s.bytes = recWriter_.bytes(); s.droppedPackets = recWriter_.droppedPackets(); }
    }
    return s;
}

std::string Player::saveAudioStream(const std::string& pathNoExt) {
    saveSink_.close();
    if (sid_ < 0) return {};
    std::lock_guard<std::mutex> lk(mu_);
    if (esPid_ < 0) return {};
    const std::string path = pathNoExt + "." + esExtension(esCodec_);
    if (!saveSink_.open(path)) return {};
    saveEs_.setPid(esPid_);
    return path;
}

void Player::setAudioTrack(int i) { p_->trackChange = i; }

std::vector<AudioTrackInfo> Player::audioTracks() const { std::lock_guard<std::mutex> lk(p_->smu); return p_->tracks; }

PlayerStats Player::stats() const {
    // The statistics lock must not be held while the picture-queue lock is taken: the player thread (and videoFrame()) take them in the
    // opposite order, and two threads each holding one and waiting for the other freeze the interface for good.
    PlayerStats s;
    { std::lock_guard<std::mutex> lk(p_->smu); s = p_->st; }
    // FFmpeg has no DRA decoder: a service whose sound is DRA only plays without it (another audio track, if there is one, plays)
    if (draAudio_ && !s.hasAudio && s.status == "playing") {
        std::string prob;
        s.audioNote = draTvNote(cdr::draAvailable(nullptr, &prob), prob);
        if (s.audioNote.empty()) s.audioNote = "DRA audio: starting the DRA add-on";   // found; it is used when the stream restarts or the track is picked again
    }
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

// True when an HEVC access unit (Annex B, as carried in MPEG-TS) holds a random-access picture (NAL types 16..23: BLA, IDR, CRA).
// The packet's key flag is not enough: RASL/trailing pictures can be flagged too, and a GPU session started on one never recovers.
[[maybe_unused]] static bool hevcHasIrap(const uint8_t* d, int n) {
    for (int i = 0; i + 3 < n; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            const int type = (d[i + 3] >> 1) & 0x3F;
            if (type >= 16 && type <= 23) return true;
            i += 2;
        }
    }
    return false;
}

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
        if (rc < 0 || stop_ || restart_) { if (rc < 0 && !stop_ && !restart_) DIAG_DEBUG_EVERY(142335, 10.0, "FFmpeg could not open the transport stream input (error %d), so opening is retried every 200 ms.", rc); cleanup(fmt); if (rc < 0 && !stop_ && !restart_) std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
        { const int sr = avformat_find_stream_info(fmt, nullptr); if (sr < 0 && !restart_) DIAG_DEBUG_EVERY(498634, 10.0, "FFmpeg could not find the streams in the input (error %d), so the input is opened again.", sr); if (sr < 0 || restart_) { cleanup(fmt); continue; } }

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
        // DRA sound: FFmpeg has no decoder and lists the stream as data or unknown; its packets still come out of the demuxer with their times.
        // They are played through the user's DRA add-on (tv_dra.h) as extra audio tracks after the ones FFmpeg can decode.
        std::vector<char> aDra(aIdx.size(), 0);
        std::vector<AudioTrackInfo> draInfo;
        { std::lock_guard<std::mutex> lk(mu_); draInfo = draTracks_; }
        for (unsigned i = 0; i < fmt->nb_streams; i++) {
            const AVMediaType mt = fmt->streams[i]->codecpar->codec_type;
            if (mt == AVMEDIA_TYPE_AUDIO || mt == AVMEDIA_TYPE_VIDEO || mt == AVMEDIA_TYPE_SUBTITLE) continue;
            for (const AudioTrackInfo& d : draInfo) if (d.pid == fmt->streams[i]->id) { aIdx.push_back((int)i); aDra.push_back(1); break; }
        }
        subsAvail_ = sIdx >= 0;
        {
            std::lock_guard<std::mutex> lk(I.smu);
            I.tracks.clear();
            for (size_t k = 0; k < aIdx.size(); k++) {
                const int i = aIdx[k];
                AVCodecParameters* cp = fmt->streams[i]->codecpar;
                AudioTrackInfo t;
                t.pid = fmt->streams[i]->id;
                AVDictionaryEntry* l = av_dict_get(fmt->streams[i]->metadata, "language", nullptr, 0);
                t.lang = l ? l->value : "";
                if (aDra[k]) {
                    t.codec = "DRA"; t.channels = 2;
                    if (t.lang.empty()) for (const AudioTrackInfo& d : draInfo) if (d.pid == t.pid) t.lang = d.lang;
                } else {
                    t.codec = avcodec_get_name(cp->codec_id);
                    t.channels = cp->ch_layout.nb_channels;
                }
                I.tracks.push_back(t);
            }
        }
        int curTrack = std::min(std::max(0, I.wantTrack), std::max(0, (int)aIdx.size() - 1));
        // Dolby sound: each AC-3 / E-AC-3 track's own headers say what it is (Atmos JOC flag, bed); read from every packet, cheap
        std::vector<DolbyStreamInfo> dInfo(aIdx.size());
        std::vector<char> aDolby(aIdx.size(), 0);
        for (size_t k = 0; k < aIdx.size(); k++) {
            const AVCodecID id = fmt->streams[aIdx[k]]->codecpar->codec_id;
            aDolby[k] = !aDra[k] && (id == AV_CODEC_ID_AC3 || id == AV_CODEC_ID_EAC3);
        }
        auto noteDolby = [&](size_t k) {
            const DolbyStreamSummary& sm = dInfo[k].summary();   // the same bits FFmpeg 7.1+ reports as AV_PROFILE_EAC3_DDP_ATMOS
            const std::string line = dolbyFormatLine(sm);
            std::lock_guard<std::mutex> lk(I.smu);
            if (k < I.tracks.size()) {
                AudioTrackInfo& t = I.tracks[k];
                t.format = line; t.bed = sm.bed; t.atmos = sm.atmos ? 1 : 0; t.complexity = sm.complexity;
                if (sm.channels > 0) t.channels = sm.channels;
            }
            if ((int)k == curTrack) { I.st.audioFormat = line; I.st.audioBed = sm.bed; I.st.atmos = sm.atmos ? 1 : 0; I.st.jocComplexity = sm.complexity; }
        };

        // ---- video decoder
        AVCodecContext* vctx = nullptr;
        bool hw = false;
        // the AVS family (DTMB in China): FFmpeg decodes AVS1-P2 in its JiZhun profile only, not AVS+; AVS2 and AVS3 need libdavs2 / libuavs3d
        const AVCodecID avsId = vIdx >= 0 ? fmt->streams[vIdx]->codecpar->codec_id : AV_CODEC_ID_NONE;
        const bool avs = avsId == AV_CODEC_ID_CAVS || avsId == AV_CODEC_ID_AVS2 || avsId == AV_CODEC_ID_AVS3;
        int avsProfile = -1;
        std::unique_ptr<AvsPlusDecoder> avd;       // the user's AVS+ add-on, when the picture needs it and it is installed
        bool avsNeedsAddon = false;                // the AVS profile is one FFmpeg's own decoder cannot show
        AVFrame* avf = av_frame_alloc();           // a borrowed-plane frame handed to convertVideo for the add-on's pictures
        const std::string avsNote = avs ? avsServiceMessage(avsId == AV_CODEC_ID_CAVS ? 0x42 : avsId == AV_CODEC_ID_AVS2 ? 0xD2 : 0xD4, -1, avcodec_find_decoder(avsId) != nullptr) : std::string();
        if (vIdx >= 0) {
            const AVCodec* c = avcodec_find_decoder(fmt->streams[vIdx]->codecpar->codec_id);
            if (c) {
                vctx = avcodec_alloc_context3(c);
                avcodec_parameters_to_context(vctx, fmt->streams[vIdx]->codecpar);
                AVBufferRef* hwdev = nullptr;
                const char* swEnv = getenv("DECT2_SWVIDEO");
                // (the user switch and the environment override); a picture size that is not known yet (the stream was joined before its first parameter
                // sets) rules hardware decoding out, because it cannot be opened without it: the software decoder learns the size from the stream
                const bool hwOk = hwAllowed_ && !hwFallback_ && !(swEnv && *swEnv) && fmt->streams[vIdx]->codecpar->width > 0 && fmt->streams[vIdx]->codecpar->height > 0;
                // damaged streams: let the decoder conceal missing slices and keep outputting pictures instead of waiting for a keyframe
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
                // (software only: the GPU decoders drop undecodable leading pictures themselves, and SHOW_ALL would push RASL pictures
                // that lack their IRAP into them)
                if (!hw) { vctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT; vctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL; }
                vctx->thread_count = hw ? 2 : 4;
                if (avcodec_open2(vctx, c, nullptr) < 0) {
                    DIAG(283841, "The %s video decoder %s did not open, so this service plays without a picture.", hw ? "hardware" : "software", c->name);
                    avcodec_free_context(&vctx);
                    vctx = nullptr;
                    hw = false;
                }
            }
        }
        // ---- audio decoder (reopened when the track changes)
        AVCodecContext* actx = nullptr;
        SwrContext* swr = nullptr;
        TvDraDecoder draDec;
        bool draOn = false;
        // what goes to the sound device (dolby_audio.h): the channel count for the device's speakers and the user's layout choice, or the
        // untouched AC-3 / E-AC-3 as an IEC 61937 bitstream (passthrough, experimental)
        int wantCh = 2, passKind = 0;
        bool matrixStereo = false;
        std::string passNote;
        Iec61937Packer packer;
        std::vector<IecBurst> bursts;
        DolbyOutputConfig& dcfg = DolbyOutputConfig::global();
        auto openAudio = [&](int track) {
            if (actx) avcodec_free_context(&actx);
            if (swr) swr_free(&swr);
            draOn = false;
            wantCh = 2; passKind = 0; matrixStereo = false; passNote.clear(); packer.reset();
            if (track < 0 || track >= (int)aIdx.size()) return;
            if (aDra[track]) { draDec.reset(); draOn = draDec.usable(); return; }
            AVStream* s = fmt->streams[aIdx[track]];
            const AVCodec* c = avcodec_find_decoder(s->codecpar->codec_id);
            if (!c) {
                DIAG_DEBUG(678258, "This FFmpeg build has no decoder for audio codec id %d, so that audio track is not played.", (int)s->codecpar->codec_id);
                // DVB AC-4 (descriptor 0x7F / 0x15): the FFmpeg inside OnAir has no AC-4 decoder; the receiver tab says what is needed
                if (s->codecpar->codec_id == AV_CODEC_ID_AC4) { ac4NoteSeen("AC-4 (DVB)"); ac4NoteDropped(); }
                return;
            }
            actx = avcodec_alloc_context3(c);
            avcodec_parameters_to_context(actx, s->codecpar);
            if (avcodec_open2(actx, c, nullptr) < 0) { DIAG(753637, "The %s audio decoder did not open, so that audio track is not played.", c->name); avcodec_free_context(&actx); return; }
            const int layout = dcfg.layout.load();
            const bool dolby = s->codecpar->codec_id == AV_CODEC_ID_AC3 || s->codecpar->codec_id == AV_CODEC_ID_EAC3;
            if (dcfg.passthrough.load() && dolby) {
                if (s->codecpar->sample_rate == 48000 || s->codecpar->sample_rate == 0) passKind = s->codecpar->codec_id == AV_CODEC_ID_EAC3 ? 2 : 1;
                else passNote = "passthrough needs 48 kHz sound; this track is decoded";
            }
            wantCh = dolbyOutputChannels(layout, s->codecpar->ch_layout.nb_channels, layout == 0 ? AudioOut::deviceChannels(audio_.device().empty() ? AudioOut::defaultDevice() : audio_.device()) : 0);
            matrixStereo = wantCh == 2 && layout == 2 && s->codecpar->ch_layout.nb_channels > 2;
        };
        auto startAudio = [&]() {
            if (!(actx || draOn)) return;
            bool pass = false;
            if (passKind && actx) {
                std::string err;
                // the output: the one set for passthrough, else (Windows, macOS) the TV sound's own; Linux names an ALSA device ("hdmi" when empty)
                std::string bdev = dcfg.device();
#if defined(_WIN32) || defined(__APPLE__)
                if (bdev.empty()) bdev = audio_.device().empty() ? AudioOut::defaultDevice() : audio_.device();
#endif
                pass = audio_.startBitstream(48000, passKind == 2 ? 192000 : 48000, passKind, bdev, &err);
                if (!pass) passNote = "passthrough: " + err + "; playing the decoded sound";
            }
            if (!pass) audio_.start(48000, draOn ? 2 : wantCh);
            std::lock_guard<std::mutex> lk(I.smu);
            I.st.passthrough = pass;
            I.st.passthroughNote = passNote;
            I.st.audioOutChannels = audio_.channels();
            I.st.matrixStereo = !pass && matrixStereo;
        };
        openAudio(curTrack);
        startAudio();
        // ---- subtitle decoder
        AVCodecContext* sctx = nullptr;
        if (sIdx >= 0) {
            const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_DVB_SUBTITLE);
            if (c) { sctx = avcodec_alloc_context3(c); avcodec_parameters_to_context(sctx, fmt->streams[sIdx]->codecpar); if (avcodec_open2(sctx, c, nullptr) < 0) { DIAG_DEBUG(978603, "The FFmpeg DVB subtitle decoder did not open, so subtitles are not shown."); avcodec_free_context(&sctx); } }
        }
        {
            std::lock_guard<std::mutex> lk(I.smu);
            I.st.status = "playing";
            I.st.hasVideo = vctx != nullptr;
            I.st.hasAudio = actx != nullptr || draOn;
            I.st.videoNote = avsNote;
            I.st.videoStreamType = avsId == AV_CODEC_ID_CAVS ? 0x42 : avsId == AV_CODEC_ID_AVS2 ? 0xD2 : avsId == AV_CODEC_ID_AVS3 ? 0xD4 : -1;
            I.st.avsProfile = -1;
            if (vctx) {
                I.st.videoCodec = avcodec_get_name(vctx->codec_id);
                I.st.hardware = hw;
                AVRational fr = fmt->streams[vIdx]->avg_frame_rate.num ? fmt->streams[vIdx]->avg_frame_rate : fmt->streams[vIdx]->r_frame_rate;
                I.st.fps = fr.den ? (double)fr.num / fr.den : 0;
            }
            if (actx) { I.st.audioCodec = avcodec_get_name(actx->codec_id); I.st.audioChannels = actx->ch_layout.nb_channels; }
            else if (draOn) { I.st.audioCodec = "DRA"; I.st.audioChannels = 2; }
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
        int swrRate = 0; AVSampleFormat swrFmt = AV_SAMPLE_FMT_NONE; int swrCh = 0, swrOutCh = 0;
        bool vSusValid = false; double vSusNext = 0;   // the same for picture timestamps
        bool susValid = false; double susD = 0, susNext = 0;   // an audio timestamp jump waiting for the next frame to confirm it
        bool firstVideo = true;
        HwStartGuard hwGuard; const auto hwT0 = std::chrono::steady_clock::now();
        auto hwNow = [&]() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - hwT0).count(); };
        hwGuard.arm(0);
        bool sawClean = false;   // a picture without missing references has come out of the video decoder since it was opened

        auto convertVideo = [&](AVFrame* f) {
            AVFrame* src = f;
            // a stream joined between keyframes: until the first clean picture the decoder (software; hardware drops them itself) outputs
            // pictures whose reference was never received, a grey field with moving blocks. Nothing has been shown yet, so they are skipped
            if (!sawClean) { if (f->flags & AV_FRAME_FLAG_CORRUPT) return; sawClean = true; }
#ifdef __APPLE__
            if (f->format == AV_PIX_FMT_VIDEOTOOLBOX) {
                av_frame_unref(sw);
                if (av_hwframe_transfer_data(sw, f, 0) < 0) { hwGuard.fetchFailed(); return; }
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
            if (AVFrameSideData* cc = av_frame_get_side_data(f, AV_FRAME_DATA_A53_CC)) { std::lock_guard<std::mutex> lk(I.ccMu); I.cc.feed(cc->data, (int)(cc->size / 3), vf->pts); }
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
                if (!actx && !draOn && !I.wallValid) { I.wallValid = true; I.wallPts = vf->pts; I.wallT0 = nowSec() + 0.35; }
            }
            const bool keyframe = (f->flags & AV_FRAME_FLAG_KEY) != 0;
            // queue cap from the frame rate and the picture size; over the cap the late pictures go first, then the newest (never the next due)
            auto trimQ = [&](const std::shared_ptr<VideoFrame>& pic) {
                const size_t cap = videoQueueCap(vFrameDur > 0 ? 1.0 / vFrameDur : 25.0, pic->y.size() + pic->uv.size() + pic->rgba.size());
                if (I.vq.size() <= cap) return;
                double clk = 0; const bool have = clockNow(clk);
                const size_t n = trimVideoQueue(I.vq, cap, clk, have);
                if (n) { std::lock_guard<std::mutex> sl(I.smu); I.st.late += n; }
            };
            auto pushPlain = [&](const std::shared_ptr<VideoFrame>& p) {
                std::lock_guard<std::mutex> lk(I.vmu);
                I.vq.push_back(p);
                trimQ(p);
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
            trimQ(vf);
        };

        std::function<void(const float*, int, double)> emitAudio;
        auto pushAudio = [&](AVFrame* f) {
            if (!actx) return;
            const int outCh = audio_.channels();
            if (!swr || swrRate != f->sample_rate || swrFmt != (AVSampleFormat)f->format || swrCh != f->ch_layout.nb_channels || swrOutCh != outCh) {
                if (swr) swr_free(&swr);
                // the bed as 5.1 / 7.1 when the device has the speakers (FFmpeg's order is AudioOut's), else a stereo downmix; 7.1 from a
                // 5.1 stream is never asked for (dolbyOutputChannels)
                AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
                if (outCh == 6) out = AV_CHANNEL_LAYOUT_5POINT1;
                else if (outCh == 8) out = AV_CHANNEL_LAYOUT_7POINT1;
                swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_FLT, 48000, &f->ch_layout, (AVSampleFormat)f->format, f->sample_rate, 0, nullptr);
                if (swr && outCh == 2 && matrixStereo && !audio_.bitstream()) av_opt_set_int(swr, "matrix_encoding", AV_MATRIX_ENCODING_DOLBY, 0);   // Lt/Rt
                if (!swr || swr_init(swr) < 0) { swr_free(&swr); return; }
                swrRate = f->sample_rate; swrFmt = (AVSampleFormat)f->format; swrCh = f->ch_layout.nb_channels; swrOutCh = outCh;
            }
            int maxOut = swr_get_out_samples(swr, f->nb_samples) + 64;
            abuf.resize((size_t)maxOut * (size_t)outCh);
            uint8_t* o[1] = {(uint8_t*)abuf.data()};
            int n = swr_convert(swr, o, maxOut, (const uint8_t**)f->extended_data, f->nb_samples);
            if (n <= 0) return;
            int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
            double pts = ts != AV_NOPTS_VALUE ? ts * av_q2d(fmt->streams[aIdx[curTrack]]->time_base) : I.nextPts;
            if (getenv("DECT2_BADPTS")) { static int cnt = 0; if (++cnt % 150 == 0) pts += 7.3; }   // test: a corrupted audio timestamp now and then
            emitAudio(abuf.data(), n, pts);
        };
        // the part both FFmpeg-decoded and DRA-decoded sound goes through: timeline checks against the clock, gap filling, then the sound device
        emitAudio = [&](const float* pcmData, int n, double pts) {
            if (pts < 0) pts = I.nextPts;
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
                std::vector<float> zeros((size_t)4800 * 8, 0.f);
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
                done += audio_.write(pcmData + (size_t)done * (size_t)audio_.channels(), n - done);
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
                {
                    std::lock_guard<std::mutex> lk(I.smu);
                    if (actx) { I.st.audioCodec = avcodec_get_name(actx->codec_id); I.st.audioChannels = actx->ch_layout.nb_channels; }
                    else if (draOn) { I.st.audioCodec = "DRA"; I.st.audioChannels = 2; }
                    I.st.hasAudio = actx != nullptr || draOn;
                    if (curTrack < (int)I.tracks.size()) { const AudioTrackInfo& t = I.tracks[(size_t)curTrack]; I.st.audioFormat = t.format; I.st.audioBed = t.bed; I.st.atmos = t.atmos; I.st.jocComplexity = t.complexity; }
                }
                startAudio();
            } else if (reopenAudio_.exchange(false)) {   // Settings > Audio changed the layout or passthrough: the same track, opened again
                openAudio(curTrack);
                audio_.flush();
                { std::lock_guard<std::mutex> lk(I.cmu); I.baseValid = false; }
                startAudio();
            }
            if (!draOn && curTrack < (int)aDra.size() && aDra[curTrack] && draDec.usable()) {   // the add-on was installed while this service played
                draDec.reset(); draOn = true;
                wantCh = 2; passKind = 0;
                startAudio();
                std::lock_guard<std::mutex> lk(I.smu);
                I.st.hasAudio = true; I.st.audioCodec = "DRA"; I.st.audioChannels = 2;
            }
            int r = av_read_frame(fmt, pkt);
            if (getenv("DECT2_PLAYDEBUG")) { static int n = 0; if ((n++ % 100) == 0 || r < 0) fprintf(stderr, "[play] read r=%d idx=%d pts=%lld vq=%zu abuf=%d decoded=%llu\n", r, pkt->stream_index, (long long)pkt->pts, I.vq.size(), audio_.bufferedFrames(), (unsigned long long)I.st.decoded); }
            if (r < 0) break;
            for (size_t k = 0; k < aIdx.size(); k++) {
                if (pkt->stream_index != aIdx[k] || !aDolby[k]) continue;
                const DolbyStreamSummary before = dInfo[k].summary();
                dInfo[k].feed(pkt->data, (size_t)pkt->size);
                const DolbyStreamSummary& now = dInfo[k].summary();
                if (now.valid != before.valid || now.atmos != before.atmos || now.speakers != before.speakers || now.complexity != before.complexity) noteDolby(k);
                // passthrough: the packet as it came, framed for IEC 61937, beside the decoded sound that keeps the clock
                if ((int)k == curTrack && audio_.bitstream()) {
                    bursts.clear();
                    packer.push(pkt->data, (size_t)pkt->size, bursts);
                    for (const IecBurst& b : bursts) audio_.writeIec(b.words.data(), (int)(b.words.size() / 2));
                }
                break;
            }
            auto openAddon = [&]() {   // true when the picture is now decoded by the add-on
                auto d = std::make_unique<AvsPlusDecoder>();
                if (!d->open()) { DIAG_DEBUG(775879, "The AVS+ decoder add-on did not open, so this service plays without a picture."); return false; }
                if (vctx) avcodec_free_context(&vctx);
                avd = std::move(d);
                AVRational fr = fmt->streams[vIdx]->avg_frame_rate.num ? fmt->streams[vIdx]->avg_frame_rate : fmt->streams[vIdx]->r_frame_rate;
                std::lock_guard<std::mutex> lk(I.smu);
                I.st.videoNote.clear(); I.st.hasVideo = true; I.st.hardware = false;
                I.st.videoCodec = "AVS+ (add-on)";
                I.st.fps = fr.den ? (double)fr.num / fr.den : 0;
                return true;
            };
            if (avsId == AV_CODEC_ID_CAVS && avsProfile < 0 && pkt->stream_index == vIdx) {
                // the profile from the first sequence header: AVS+ (and anything else but JiZhun) is refused picture by picture by FFmpeg's own
                // decoder, so the user's AVS+ add-on takes it when installed, and otherwise the player says so
                avsProfile = avsProfileId(pkt->data, (size_t)pkt->size);
                { std::lock_guard<std::mutex> lk(I.smu); I.st.avsProfile = avsProfile; }
                avsNeedsAddon = avsProfile >= 0 && (avsNeverDecodable(0x42, avsProfile) || !vctx);
                if (avsNeedsAddon && !(avsPlusPoll(0) && openAddon())) {
                    const bool hadDecoder = vctx != nullptr;
                    if (vctx) avcodec_free_context(&vctx);
                    const AvsPlusLibInfo li = avsPlusLibrary();
                    std::lock_guard<std::mutex> lk(I.smu);
                    I.st.videoNote = avsProfile == 0x48 ? avsPlusNote(li) : avsServiceMessage(0x42, avsProfile, hadDecoder);
                    I.st.hasVideo = false;
                }
            } else if (avsNeedsAddon && !avd && pkt->stream_index == vIdx && avsPlusPoll(3.0)) {
                openAddon();   // the add-on was installed while the service was playing; the decoder waits for the next sequence header
            }
            if (pkt->stream_index == vIdx && avd) {
                I.lossFlag.store(false);
                const int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
                if (!avd->send(pkt->data, (size_t)pkt->size, ts == AV_NOPTS_VALUE ? INT64_MIN : ts, (pkt->flags & AV_PKT_FLAG_KEY) != 0)) { std::lock_guard<std::mutex> lk(I.smu); I.st.errors++; }
                AvsPicture pic;
                while (avf && avd->receive(pic)) {
                    if (pic.format != AV_PIX_FMT_YUV420P && pic.format != AV_PIX_FMT_YUVJ420P) {
                        std::lock_guard<std::mutex> lk(I.smu);
                        I.st.videoNote = "AVS+ video: the add-on gave a picture format OnAir does not handle (" + std::to_string(pic.format) + ")";
                        break;
                    }
                    for (int k = 0; k < 4; k++) { avf->data[k] = const_cast<uint8_t*>(pic.data[k]); avf->linesize[k] = pic.linesize[k]; }
                    avf->width = pic.w; avf->height = pic.h; avf->format = pic.format;
                    avf->pts = avf->best_effort_timestamp = pic.pts == INT64_MIN ? AV_NOPTS_VALUE : pic.pts;
                    avf->flags = pic.key ? AV_FRAME_FLAG_KEY : 0;
                    avf->colorspace = AVCOL_SPC_UNSPECIFIED;
                    avf->color_range = pic.format == AV_PIX_FMT_YUVJ420P ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
                    avf->sample_aspect_ratio = AVRational{0, 1};
                    { std::lock_guard<std::mutex> lk(I.smu); I.st.decoded++; }
                    convertVideo(avf);
                    for (int k = 0; k < 4; k++) { avf->data[k] = nullptr; avf->linesize[k] = 0; }
                }
            } else if (pkt->stream_index == vIdx && vctx) {
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
                if (hw) {
                    // GPU decoding: start on a keyframe; if the GPU stays silent for seconds, switch this service to the CPU decoder
                    const bool irap = vctx->codec_id == AV_CODEC_ID_HEVC ? hevcHasIrap(pkt->data, pkt->size) : (pkt->flags & AV_PKT_FLAG_KEY) != 0;
                    const bool feedIt = hwGuard.feed(irap, hwNow());
                    // checked even for a packet held back: an H.264 stream with no flagged keyframe (I pictures with a recovery-point SEI) never
                    // starts the GPU session, so audio plays and no picture ever comes (#410); the CPU decoder shows those
                    if (hwGuard.giveUp(hwNow())) {
                        fprintf(stderr, "player: hardware video decoding gave no picture after %.1f s (%d packets fed, %d held back before a random-access picture): switching to software decoding\n", hwNow(), hwGuard.fed(), hwGuard.skipped());
                        DIAG(382108, "Hardware video decoding gave no picture after %.1f s, so playback restarts with software decoding.", hwNow());
                        hwFallback_ = true; restart_ = true;
                        av_packet_unref(pkt);
                        continue;
                    }
                    if (!feedIt) { av_packet_unref(pkt); continue; }
                }
                if (avcodec_send_packet(vctx, pkt) >= 0)
                    while (avcodec_receive_frame(vctx, frame) >= 0) {
                        hwGuard.gotPicture();
                        { std::lock_guard<std::mutex> lk(I.smu); I.st.decoded++; }
                        convertVideo(frame);
                        av_frame_unref(frame);
                    }
                else { std::lock_guard<std::mutex> lk(I.smu); I.st.errors++; }
            } else if (actx && curTrack < (int)aIdx.size() && pkt->stream_index == aIdx[curTrack]) {
                if (avcodec_send_packet(actx, pkt) >= 0)
                    while (avcodec_receive_frame(actx, frame) >= 0) { pushAudio(frame); av_frame_unref(frame); }
            } else if (draOn && curTrack < (int)aIdx.size() && aDra[curTrack] && pkt->stream_index == aIdx[curTrack]) {
                const AVStream* ds = fmt->streams[pkt->stream_index];
                const double dpts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts * av_q2d(ds->time_base) : -1.0;
                std::vector<DraPcm> pcm;
                draDec.feed(pkt->data, (size_t)pkt->size, dpts, pcm);
                for (const DraPcm& c : pcm) emitAudio(c.stereo.data(), (int)(c.stereo.size() / 2), c.pts);
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
        av_frame_free(&avf);
        avd.reset();
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
