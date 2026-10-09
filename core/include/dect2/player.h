// Live player: decodes the selected service of the transport stream (video with VideoToolbox when possible, audio
// through CoreAudio) and keeps audio and video in sync using the stream's timestamps.
#pragma once
#include "audioout.h"
#include "ts.h"
#include <atomic>
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

struct AudioTrackInfo { int pid = 0; std::string codec, lang; int channels = 0; };

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
};

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
    void setSubtitles(bool on) { subsOn_ = on; }
    void setConceal(bool on) { conceal_ = on; }    // bridge gaps in the stream: continue the audio, interpolate missing pictures
    bool conceal() const { return conceal_; }
    // Decode video on the graphics chip (VideoToolbox / Direct3D 11) when it can; off = always on the CPU. Takes effect by restarting the stream.
    void setHardwareDecode(bool on) { if (hwAllowed_.exchange(on) != on) restart_ = true; }
    bool hardwareDecode() const { return hwAllowed_; }
    bool subtitlesAvailable() const { return subsAvail_; }

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
    std::atomic<bool> conceal_{false};   // off by default: interpolating pictures costs CPU that slower computers need for decoding
    std::atomic<bool> hwAllowed_{true};
    ServiceFilter filter_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<uint8_t> in_;
    // transport-stream losses on the video PID (continuity counter jumps), as positions in the byte stream fed to the demuxer
    std::deque<uint64_t> lossMarks_;
    uint64_t pushed_ = 0, consumed_ = 0;
    int lastCc_[8192];
    std::atomic<int> videoPid_{-1};
};

} // namespace dect2
