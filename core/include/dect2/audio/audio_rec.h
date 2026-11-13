// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Record audio": saves the audio you hear (before the volume slider) from any mode to a WAV, FLAC or MP3 file. Every AudioOut::write() offers its
// frames to this recorder; the first output that writes after Start owns the recording (other outputs are ignored until it has been silent for 2 s,
// so a mode change hands over to the new mode's output). The audio thread only copies into a lock-free ring; a writer thread gates, encodes and writes.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace dect2 {

enum class AudioRecFormat { Wav = 0, Flac = 1, Mp3 = 2 };

struct AudioRecStats {
    bool active = false;
    std::string path;            // the file being written, or the last one
    int files = 0;               // files written so far in this recording (more than one when split by transmission)
    double seconds = 0;          // audio saved so far (the gaps skipped by the gate are not counted)
    uint64_t bytes = 0;          // size of the files so far
    int rate = 0, channels = 0;  // of the audio (0 until the first audio arrives)
    bool gateOpen = false;       // audio is present (always true when ungated)
    uint64_t dropped = 0;        // frames lost because the disk could not keep up
    std::string error;           // why it ended by itself
};

struct AudioRecOptions {
    AudioRecFormat format = AudioRecFormat::Wav;
    bool gated = false;          // save only while audio is present (level above gateDb), with a pre-roll and a hang
    bool split = false;          // with gating: one file per transmission instead of one file with the silence cut out
    double gateDb = -55;         // block RMS (dBFS) that counts as audio
    double hangSec = 1.5, preRollSec = 0.3, splitAfterSec = 3.0;
    int mp3Kbps = 128;
    std::string tag;             // a comment in the file's tags (the mode and frequency)
};

class AudioRecorder {
public:
    static AudioRecorder& instance();
    // path: the file (with the extension of the format); with split on, "_2", "_3" ... are put before the extension for the next files.
    bool start(const std::string& path, const AudioRecOptions& o, std::string& err);
    void stop();
    bool active() const { return active_.load(std::memory_order_acquire); }
    AudioRecStats stats() const;
    // For AudioOut::write(): never blocks, never allocates. src identifies the output.
    static void offer(const void* src, const float* stereo, int frames, int rate) {
        AudioRecorder& r = instance();
        if (r.active_.load(std::memory_order_relaxed)) r.push(src, stereo, frames, rate);
    }
    static std::string fileName(const std::string& modeId, double freqHz, AudioRecFormat f);   // "onair_audio_fm_93.900MHz_20261017-101500.wav"
    static const char* extension(AudioRecFormat f);
    AudioRecorder();
    ~AudioRecorder();

    struct Impl;

private:
    void push(const void* src, const float* stereo, int frames, int rate);
    std::unique_ptr<Impl> p_;
    std::atomic<bool> active_{false};
};

}  // namespace dect2
