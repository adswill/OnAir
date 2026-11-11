// Live player: decodes the selected service of the transport stream (video with VideoToolbox when possible, audio
// through CoreAudio) and keeps audio and video in sync using the stream's timestamps.
#pragma once
#include "dect2/audio/audioout.h"
#include "ts.h"
#include "es_save.h"
#include "ts_record.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

namespace dect2 {

// Display aspect ratio (width / height) of a picture of w x h stored pixels whose pixels have the sample aspect ratio sarN:sarD.
// An unknown or invalid SAR (0/0, a zero or negative part) means square pixels. Never returns less than a tiny positive value.
inline double displayAspect(int w, int h, int sarN, int sarD) {
    if (w <= 0 || h <= 0) return 1.0;
    double sar = (sarN > 0 && sarD > 0) ? (double)sarN / sarD : 1.0;
    if (sar < 0.05 || sar > 20.0) sar = 1.0;     // nonsense in the stream: treat as square
    return (double)w * sar / h;
}

struct VideoFrame {
    double pts = 0;
    int w = 0, h = 0;
    double dar = 0;                      // display aspect ratio (width / height) of the picture; 0 = unknown, draw w / h
    std::vector<uint8_t> rgba;           // RGBA8, only when a subtitle was burnt in; otherwise empty
    std::vector<uint8_t> y, uv;          // NV12 planes (w*h and w*(h/2) bytes, tightly packed); converted to RGB on the GPU
    bool bt709 = true, fullRange = false, interlaced = false;
};

// How many decoded pictures the player may queue. A broadcaster that muxes the video well ahead of the audio (1.6 s is seen on
// DVB-T2 HEVC 1080p50) needs about 3.5 s of pictures, so the cap follows the frame rate (never below 80) and is bounded by memory:
// at most about 768 MB of NV12 pictures (never below 30, so 4K still has a second of queue).
inline size_t videoQueueCap(double fps, size_t frameBytes) {
    if (!(fps >= 1.0) || fps > 240.0) fps = 25.0;
    size_t cap = (size_t)(fps * 3.5 + 0.999);
    if (cap < 80) cap = 80;
    if (frameBytes > 0) {
        size_t byMem = (size_t)(768ull * 1024 * 1024) / frameBytes;
        if (byMem < 30) byMem = 30;
        if (cap > byMem) cap = byMem;
    }
    return cap;
}

// Brings a queue of pictures (ordered by pts) back to at most cap entries. Pictures that are already late are dropped first (oldest
// first); the pictures that are due next are kept, so when the queue is still too long the NEWEST ones go (the decoder is too far ahead,
// the back-pressure in the read loop holds it back). Returns the number dropped.
template <class Q>
inline size_t trimVideoQueue(Q& q, size_t cap, double clock, bool haveClock) {
    size_t n = 0;
    while (q.size() > cap && haveClock && q.size() > 1 && q.front()->pts < clock) { q.pop_front(); n++; }
    while (q.size() > cap) { q.pop_back(); n++; }
    return n;
}

// format: the Dolby line read from the stream's own headers (dolby_audio.h), e.g. "Dolby Atmos (E-AC-3 JOC), 5.1 bed"; empty for other codecs.
// atmos: 1 E-AC-3 with JOC objects (FFmpeg plays its channel bed; the objects need a receiver: passthrough), 0 none.
struct AudioTrackInfo { int pid = 0; std::string codec, lang; int channels = 0; std::string format, bed; int atmos = 0; int complexity = 0; };

struct PlayerStats {
    bool active = false;
    int serviceId = -1;
    std::string status;
    std::string videoCodec, audioCodec;
    int width = 0, height = 0;
    double fps = 0;
    bool hardware = false;
    uint64_t decoded = 0, shown = 0, late = 0, errors = 0;
    double avOffsetMs = 0;
    double audioBufferMs = 0;
    int videoQueue = 0;
    int audioChannels = 0;
    int underruns = 0;
    bool hasVideo = false, hasAudio = false;
    // gaps in the stream (lost data) that were bridged: audio continued, missing pictures interpolated
    uint64_t concealEvents = 0, concealedFrames = 0;
    double concealedAudioSec = 0;
    uint64_t repairEvents = 0, repairedFrames = 0;   // damaged pictures replaced by generated ones
    std::string concealBackend;          // how missing pictures are made: "Apple ML" or "motion search"
    // what of the service the FFmpeg in OnAir cannot show or play (AVS+ / AVS2 / AVS3 video, DRA sound; docs/CODECS.md), one sentence, else empty
    std::string videoNote, audioNote;
    int videoStreamType = -1;   // PMT stream_type of the video (0x42 AVS family, 0xD2 AVS2, 0xD4 AVS3), -1 unknown
    // Dolby sound (dolby_audio.h): the format line of the playing track, whether it carries Atmos (1 E-AC-3 JOC), its bed and object count
    std::string audioFormat, audioBed;
    int atmos = 0, jocComplexity = 0;
    int audioOutChannels = 0;   // what goes to the sound device: 2, 6 (5.1) or 8 (7.1); 0 while nothing plays
    bool matrixStereo = false;  // the stereo is Dolby Surround (Lt/Rt) matrix-encoded
    bool passthrough = false;   // the AC-3 / E-AC-3 goes out untouched as an IEC 61937 bitstream (experimental)
    std::string passthroughNote;   // why passthrough is not in use although it is switched on (empty otherwise)
    int avsProfile = -1;        // profile_id of the AVS sequence header (0x20 JiZhun, 0x48 Guangdian = AVS+), -1 not seen yet
};

// The profile_id of the first AVS1-P2 / AVS+ sequence header (start code 00 00 01 B0) in p, -1 when there is none
int avsProfileId(const uint8_t* p, size_t n);
// Why an AVS family video stream gives no picture: codec as FFmpeg names it ("cavs", "avs2", "avs3"), whether this FFmpeg has a decoder for
// it, the profile from avsProfileId() (-1 not seen yet). Empty when it can be decoded (or nothing is known yet).
std::string avsVideoNote(const std::string& codec, bool haveDecoder, int profile);
// Which member of the Chinese AVS family a video stream is, from the PMT stream_type and (when seen) the sequence header profile_id:
// "AVS" (JiZhun, 0x20), "AVS+" (0x48), "AVS2" (0xD2), "AVS3" (0xD4); "AVS/AVS+" when stream_type 0x42 has no profile yet; empty for any other stream.
std::string avsFamilyName(int streamType, int profile);
// true when FFmpeg's cavs decoder cannot decode it whatever the build (AVS+ and other non-JiZhun AVS1 profiles); AVS2/AVS3 depend on the build (false here)
bool avsNeverDecodable(int streamType, int profile);
// The plain sentence for the service list and the player: "Video: AVS+ (OnAir cannot decode this video) - audio, subtitles and data still work"
// (also for an AVS2/AVS3 or AVS stream whose decoder is missing: pass haveDecoder). Empty when the video can be decoded.
std::string avsServiceMessage(int streamType, int profile, bool haveDecoder);
extern const char* const kDraAudioNote;   // a service whose sound is DRA only

class Player {
public:
    Player();
    ~Player();
    void select(int serviceId);            // -1 stops
    int selected() const { return sid_; }
    void push(const uint8_t* packets, size_t nPackets, const TsSnapshot& snap);
    // Newest frame whose presentation time has arrived; null if nothing new since `seq`.
    std::shared_ptr<const VideoFrame> videoFrame(uint64_t& seq);
    PlayerStats stats() const;
    std::vector<AudioTrackInfo> audioTracks() const;
    void setVolume(float v) { audio_.setVolume(v); }
    void setMuted(bool m) { audio_.setMuted(m); }
    void setAudioTrack(int index);         // index into audioTracks()
    void reopenAudio() { reopenAudio_ = true; }   // the speaker layout or passthrough setting changed (DolbyOutputConfig): open the sound again
    void setSubtitles(bool on) { subsOn_ = on; }
    void setConceal(bool on) { conceal_ = on; }    // bridge gaps in the stream: continue the audio, interpolate missing pictures
    bool conceal() const { return conceal_; }
    // Decode video on the graphics chip (VideoToolbox / Direct3D 11) when it can; off = always on the CPU. Takes effect by restarting the stream.
    void setHardwareDecode(bool on) { hwFallback_ = false; if (hwAllowed_.exchange(on) != on) restart_ = true; }
    bool hardwareDecode() const { return hwAllowed_; }
    bool subtitlesAvailable() const { return subsAvail_; }
    // Save the raw audio elementary stream (DRA, else MPEG audio) of the selected service to a file FFmpeg can play. Returns the file
    // actually written (the extension is set from the codec), empty when the service has no such stream yet or the file cannot be opened.
    std::string saveAudioStream(const std::string& pathNoExt);
    void stopSaveAudioStream() { saveSink_.close(); }
    bool savingAudioStream() const { return saveSink_.isOpen(); }
    uint64_t savedAudioBytes() const { return saveSink_.bytes(); }

    // ---- record the selected service to a .ts file: only its own PIDs (PAT/PMT single-program, video, audio, subtitles, teletext).
    // The disk is written on its own thread; when it is too slow packets are dropped and counted. stopAfterSec > 0 or stopAtUnix > 0 end it by themselves.
    struct RecStats { bool active = false; double seconds = 0; uint64_t bytes = 0, droppedPackets = 0; bool failed = false; std::string path; };
    bool startRecording(const std::string& path, double stopAfterSec = 0, int64_t stopAtUnix = 0);
    void stopRecording();
    RecStats recording() const;
    // ---- pause / live buffer: the service's packets always fill a ring; Pause freezes the picture, Play resumes from that point paced by
    // the stream's PCR (the broadcaster's clock), so sound and picture stay in step. Overflow drops the oldest.
    struct ShiftStats { bool paused = false, behind = false; double behindSec = 0, bufferedSec = 0; uint64_t bufferBytes = 0, capacityBytes = 0; };
    void setBufferMB(int mb);
    int bufferMB() const { return bufMb_; }
    void pause();
    std::string latestCaption(size_t* count = nullptr) const;   // newest CEA-608 / 708 caption decoded from the video (empty: none so far)
    void play();                       // resume after Pause (stays behind live)
    void seekBehind(double seconds);   // play from this far behind live (0 = live)
    void goLive() { seekBehind(0); }
    ShiftStats shift() const;

private:
    struct Impl;
    friend struct Impl;
    bool clockNow(double& clock);
    void threadMain();
    std::unique_ptr<Impl> p_;
    std::atomic<int> sid_{-1};
    AudioOut audio_;
    std::thread th_;
    std::atomic<bool> stop_{false}, restart_{false};
    std::atomic<bool> subsOn_{true}, subsAvail_{false};
    std::atomic<bool> reopenAudio_{false};
    std::atomic<bool> conceal_{false};   // off by default: interpolating pictures costs CPU that slower computers need for decoding
    std::atomic<bool> hwAllowed_{true};
    std::atomic<bool> hwFallback_{false};   // the GPU decoder stayed silent on this service: software decoding until the service or the setting changes
    ServiceFilter filter_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<uint8_t> in_;
    // transport-stream losses on the video PID (continuity counter jumps), as positions in the byte stream fed to the demuxer
    std::deque<uint64_t> lossMarks_;
    uint64_t pushed_ = 0, consumed_ = 0;
    int lastCc_[8192];
    std::atomic<int> videoPid_{-1};
    // recording and live buffer, guarded by mu_
    TsWriter recWriter_;
    ServiceFilter recFilter_;
    std::chrono::steady_clock::time_point recStart_;
    double recStopAfter_ = 0; int64_t recStopAt_ = 0;
    bool recDue() const;
    void feedLocked(const uint8_t* out);
    void pumpLocked();
    void goLiveLocked();
    TsRing ring_;
    int bufMb_ = 256;
    int pcrPid_ = -1;
    struct PcrMark { uint64_t idx; double pcr; };
    std::deque<PcrMark> pcrIdx_;
    bool shifted_ = false, paused_ = false, anchorPending_ = true;
    uint64_t cursor_ = 0;
    double anchorPcr_ = 0;
    std::chrono::steady_clock::time_point anchorWall_;
    double pcrAt(uint64_t idx) const;
    EsSink saveSink_;
    TsEsExtractor saveEs_;   // guarded by mu_
    int esPid_ = -1;         // the selected service's DRA / MPEG audio stream, guarded by mu_
    std::string esCodec_;
    public:
    bool hasSavableAudio() const { std::lock_guard<std::mutex> lk(mu_); return esPid_ >= 0; }
    private:
    std::atomic<bool> draAudio_{false};   // the selected service carries DRA sound (from the service list)
    std::vector<AudioTrackInfo> draTracks_;   // its DRA audio streams (pid, language), guarded by mu_: played through the DRA add-on (tv_dra.h)
};

} // namespace dect2
