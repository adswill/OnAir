// Audio output (CoreAudio on macOS, miniaudio elsewhere) with a lock-free ring buffer; keeps count of frames written and played for A/V sync.
#pragma once
#include <cstdint>
#include <memory>

namespace dect2 {

class AudioOut {
public:
    AudioOut();
    ~AudioOut();
    bool start(int sampleRate = 48000);
    void stop();
    // Interleaved stereo float. Returns frames accepted (drops if the ring is full).
    int write(const float* stereo, int frames);
    void flush();                       // discard queued audio
    int bufferedFrames() const;
    uint64_t writtenFrames() const;
    uint64_t playedFrames() const;      // real frames consumed by the device (silence is not counted)
    void setVolume(float v);
    void setMuted(bool m);
    int sampleRate() const { return rate_; }
    bool running() const { return running_; }
    int underruns() const;
    bool playing() const;               // false while (re)buffering
    // Playback only begins once this many frames are queued (and again after an underrun).
    void setStartThreshold(int frames);

    struct Impl;

private:
    std::unique_ptr<Impl> p_;
    int rate_ = 48000;
    bool running_ = false;
};

} // namespace dect2
