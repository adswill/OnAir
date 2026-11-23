// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Synthetic content": the picture, video and sound that the built-in test signals carry can come from the user's own files instead of
// the built-in test card and beep (SynthConfig::mediaKind / mediaPath / mediaAudio; the default is the built-in content and changes nothing).
//   transport stream modes   the file is encoded to MPEG-2 video + MP2 sound (demo_ts.cpp) and looped in the multiplex
//   analog TV                 the picture / video and sound of the file modulate the carrier (pullVideo / pullAudio of the ATV generator)
//   FM, AM, NFM, WFM, DSB     the sound file is the programme (pull48k / the general generator)
//   SSTV                      the picture is sent as the image
// Modes that need their own audio codec (DAB, DRM, HD Radio, ATSC 3.0, the digital voice modes) keep the built-in content: OnAir ships no
// encoder for them. synthMediaSupport() says what a configuration can carry. Everything is decoded with FFmpeg, which the build already has.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct SynthConfig;

enum { kSynthMediaPicture = 1, kSynthMediaAudio = 2 };   // bits of synthMediaSupport()

// What the test signal of this configuration (cfg.mode, DAB / ATSC flags, ...) can carry: kSynthMediaPicture | kSynthMediaAudio, or 0.
// note (optional) gets a sentence for the user: what the mode takes, or why it keeps the built-in content.
int synthMediaSupport(const SynthConfig& cfg, std::string* note = nullptr);

struct SynthMedia {
    int kind = 0;           // 0 built-in, 1 picture, 2 video
    std::string path, audio;
    bool active() const { return (kind != 0 && !path.empty()) || !audio.empty(); }
    std::string key() const;   // names the content (paths and file times) for caches
};
SynthMedia synthMediaOf(const SynthConfig& cfg);   // empty (built-in) when the mode cannot carry it

// The content that demoTsSource() / the generators made inside the scope use (thread-local); the synthetic source opens one around the
// construction of a generator. Nothing outside a scope sees media, so the transmitter keeps its own content.
class SynthMediaScope {
public:
    explicit SynthMediaScope(const SynthConfig& cfg);
    explicit SynthMediaScope(const SynthMedia& m);
    ~SynthMediaScope();
    SynthMediaScope(const SynthMediaScope&) = delete;
    SynthMediaScope& operator=(const SynthMediaScope&) = delete;
    static const SynthMedia* current();   // nullptr: built-in content
private:
    SynthMedia m_;
    const SynthMedia* prev_;
};

// What happened with the last file: "" when built-in, else a sentence ("Carrying holiday.mp4", or why the built-in content is used).
std::string synthMediaStatus();
void synthMediaSetStatus(const std::string& s);

// Frames of a picture or a video file, looped. Not thread safe: one reader per user.
class SynthMediaVideo {
public:
    SynthMediaVideo();
    ~SynthMediaVideo();
    SynthMediaVideo(const SynthMediaVideo&) = delete;
    SynthMediaVideo& operator=(const SynthMediaVideo&) = delete;
    bool open(const std::string& path, std::string& err);
    bool isStill() const;          // a picture (one frame)
    double duration() const;       // seconds of one pass; 0 for a picture or when FFmpeg cannot tell
    int width() const;
    int height() const;
    // The picture at time t (looped), fitted in dw x dh with black bars, no distortion. False when there is no frame.
    bool rgb(double t, int dw, int dh, std::vector<uint8_t>& out, bool stretch = false);   // RGB 8 bit packed; stretch: fill dw x dh (the caller fits the aspect itself)
    bool yuv420(double t, int dw, int dh, uint8_t* y, int ys, uint8_t* u, int us, uint8_t* v, int vs);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The sound of a file (or of a video), 48 kHz stereo, looped.
// A file up to the memory limit is decoded whole; a longer one is decoded a packet at a time while it plays (a small window of samples is kept,
// what was played is freed), so a 3 hour video has sound and the memory stays flat.
class SynthMediaAudio {
public:
    SynthMediaAudio();
    ~SynthMediaAudio();
    SynthMediaAudio(const SynthMediaAudio&) = delete;
    SynthMediaAudio& operator=(const SynthMediaAudio&) = delete;
    // The chosen sound: media.audio, else the video's own sound. At most maxSeconds are kept. False with err when there is none.
    bool load(const SynthMedia& m, double maxSeconds, std::string& err);
    // As load(), but with no limit on the length: up to memorySeconds is decoded whole, a longer file (or one that does not say how long it is
    // and does not end within memorySeconds) is streamed from the file.
    bool loadStreaming(const SynthMedia& m, double memorySeconds, std::string& err);
    bool ok() const;
    bool streaming() const;                  // decoded while it plays, not held whole
    double seconds() const;                  // one pass (a streamed file: from the container until the end has been reached once)
    int read(float* stereo, int frames);     // interleaved L R; wraps around at the end; returns frames
    float mono(double t) const;              // (L + R) / 2 at time t, looped, interpolated
    void seek(double seconds);               // where read() goes on from (looped into the file)
    void setPeak(float peak);                // scale so the loudest sample is peak (default: as decoded); a streamed file looks at its first 15 minutes
    size_t bufferedBytes() const;            // the memory the sound holds now
private:
    struct Stream;
    std::unique_ptr<Stream> s_;
    std::vector<float> pcm_;
    size_t pos_ = 0;
};

// The in-memory limit of the sound of a video file (about 23 MB per minute): a file shorter than this keeps its sound whole; a longer one is
// streamed (it used to be cut at this length, 120 s before 4.4.1, which made the sound start over while the picture played on).
constexpr double kSynthMediaMaxSoundSeconds = 900;

// One shared decoded copy per (file, time, length) for the generators that rebuild themselves on an option change
std::shared_ptr<SynthMediaAudio> synthMediaAudioFor(const SynthConfig& cfg, double maxSeconds);
std::shared_ptr<SynthMediaVideo> synthMediaVideoFor(const SynthConfig& cfg);
// A picture (or the first frame of a video) as w x h RGB, letterboxed; empty when the configuration has none or it cannot be read.
std::vector<uint8_t> synthMediaPicture(const SynthConfig& cfg, int w, int h);

} // namespace dect2
