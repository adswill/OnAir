// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Monitor audio": listen to the tuned channel of ANY mode. Most decoders have no sound path of their own (they decode data), so a signal you
// are trying to recognise stays silent. The monitor takes a copy of the radio's samples (like the second mode), shifts the tuned frequency to
// 0 Hz, demodulates it (USB, LSB, narrow FM or AM; the demodulators of the ham decoders, hfdig::UsbAudio) and sends the audio to
//   1. the speaker (any output device, chosen by name),
//   2. a second output device (for example a virtual audio cable that fldigi, WSJT-X or multimon-ng read), and
//   3. optionally a TCP stream (16-bit stereo, 48 kHz; a WAV header or raw), bound to 127.0.0.1 unless told otherwise.
// It yields while the mode plays sound of its own (AudioOut::msSinceModeWrite), so nothing is heard twice. The outputs are AudioOut objects, so
// "Record audio" (audio_rec.h) records what the monitor plays. The radio's thread only copies samples into a queue (dropped when full).
#pragma once
#include "dect2/common/ring.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct MonitorConfig {
    bool on = false;
    int demod = 0;                 // HfDemod: USB 0, LSB 1, NFM 2, AM 3 (Auto is resolved by the caller from the frequency)
    float volume = 1.f;            // output 1 (0..1, the slider position)
    std::string device;            // output 1: device name, "" = the system default
    bool out2 = false;             // a second output device
    std::string device2;
    float volume2 = 1.f;
    int latencyMs = 100;           // audio queued before playback starts, and the longest it may run behind before it catches up
    bool yield = true;             // stay silent while the mode plays its own sound
    bool net = false;              // TCP audio stream
    int port = 7355;
    bool bindAll = false;          // false: 127.0.0.1 only. true: every interface (anyone on the network can listen)
    bool wav = true;               // true: a WAV header first (ffplay, VLC, sox read it directly). false: raw 16-bit little-endian stereo 48 kHz
};

struct MonitorStats {
    bool running = false;          // the thread is up and samples are coming in
    bool yielded = false;          // silent now because the mode plays its own sound
    float levelDb = -120;          // of the audio last produced
    uint64_t dropped = 0;          // input samples lost because the monitor fell behind
    int clients = 0;               // TCP listeners
    std::string error;             // output device or port that could not be opened
};

class AudioMonitor {
public:
    AudioMonitor();
    ~AudioMonitor();
    void configure(const MonitorConfig& c);
    MonitorConfig config() const;
    // The app's mute: the speaker (output 1) goes silent. The second device and the TCP stream carry on, they feed other programs.
    // Without it the monitor took over when the mode was muted (a muted mode does not count as playing) and the station stayed audible.
    void setMuted(bool m);
    bool enabled() const;          // cheap: for the radio's thread
    // The radio's thread: a block of samples at `rate`, the wanted channel at shiftHz from their centre (negative: below). Never waits.
    void push(const cf32* x, size_t n, double rate, double shiftHz);
    MonitorStats stats() const;
    void stop();
    // Tests: the audio goes here (interleaved stereo 48 kHz float) instead of the devices and the network; processBlock() runs synchronously.
    void setSink(std::function<void(const float*, int)> sink);
    void processBlock(const cf32* x, size_t n, double rate, double shiftHz);
    static const char* demodName(int d);

    struct Impl;

private:
    std::unique_ptr<Impl> p_;
};

}  // namespace dect2
