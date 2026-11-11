// Audio output (CoreAudio on macOS, miniaudio elsewhere) with a lock-free ring buffer; keeps count of frames written and played for A/V sync.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

class AudioOut {
public:
    AudioOut();
    ~AudioOut();
    // Output devices by name (the system's list; the first is the default, shown as ""). setDevice() before start(): "" = the system default.
    // A named device is opened through miniaudio (Windows, Linux) or a HAL unit (macOS); one that is not found falls back to the default.
    static std::vector<std::string> devices();
    void setDevice(const std::string& name) { device_ = name; }
    const std::string& device() const { return device_; }
    // The device every output without one of its own uses at start() (the user's choice for the current mode, audio_control.h); a name that is not
    // present any more falls back to the system default.
    static void setDefaultDevice(const std::string& name);
    static std::string defaultDevice();
    // A monitor output (audio_monitor.h) is not "the mode's sound": the monitor yields while any other, unmuted AudioOut has written lately.
    void setMonitorRole(bool m) { monitor_ = m; }
    static int msSinceModeWrite();      // milliseconds since an unmuted non-monitor output last wrote (large when never)
    bool start(int sampleRate = 48000) { return start(sampleRate, 2); }
    // `channels` 2, 6 (5.1) or 8 (7.1), interleaved in the WAVE / FFmpeg order: FL FR FC LFE SL SR (5.1), FL FR FC LFE BL BR SL SR (7.1).
    // A running output with another channel count or in bitstream mode is restarted.
    bool start(int sampleRate, int channels);
    void stop();
    int channels() const { return ch_; }
    // The speakers of an output device (the system's configuration), 0 when unknown (Linux: ALSA does not say; Settings can force 5.1 / 7.1)
    static int deviceChannels(const std::string& name);
    // Interleaved float, channels() per frame (stereo unless started with more). Returns frames accepted (drops if the ring is full).
    int write(const float* stereo, int frames);
    // ---- bitstream passthrough (EXPERIMENTAL, untested on hardware; dolby_audio.h, docs/ATMOS-3.9.md). The device gets IEC 61937 words
    // (writeIec) at the link rate; the decoded stereo that write() still receives is not played but keeps the clock, so playedFrames()
    // and the A/V sync work as before. The bursts follow the decoded sound in lockstep: what the device plays is what the clock says.
    // kind: 1 AC-3, 2 E-AC-3. false (and *err) when the device does not take a bitstream; the output is then stopped.
    bool startBitstream(int pcmRate, int linkRate, int kind, const std::string& device, std::string* err);
    bool bitstream() const { return bitstream_; }
    int writeIec(const int16_t* words, int frames);   // IEC 60958 frames (2 words each); returns frames accepted
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
    static void bitstreamStop(Impl* I);
    std::unique_ptr<Impl> p_;
    int rate_ = 48000;
    bool running_ = false;
    bool bitstream_ = false;
    int ch_ = 2;
    bool monitor_ = false;
    std::string device_;
};

} // namespace dect2
