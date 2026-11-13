// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HD Radio sound: HDC through the user's own patched FAAD2 ("libfaad_hdc"), loaded at run time. HDC is a patented codec of Xperi and OnAir
// ships none of it, no codec source either (docs/CODECS.md, "HD Radio sound"); without the library the station, program data and pictures
// still work and the audio packets are only counted.
//
// hdc*: finding the library (like mbelib: DECT2_FAAD_HDC_PATH, the OnAir codecs folders, the system's places), checking that it is a FAAD2 whose
// frame info layout OnAir knows, and "Look again".
// HdcDecoder: one decoder state (one program), packet in, 16-bit stereo 44.1 kHz out.
// HdcAudio: everything between the Layer 2 audio packets and the speakers: the per-program packet buffers, the decoders, silence for lost
// packets, 44.1 kHz to 48 kHz, the sound device.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 { namespace hdr {

bool hdcAvailable(std::string* path = nullptr, std::string* problem = nullptr);   // found and usable; else `problem` says why not ("" = no file found)
bool hdcRescan();                                                                // look again (the "Look again" button); true when usable
std::vector<std::string> hdcFileNames();                                         // the names OnAir looks for, no folder
std::vector<std::string> hdcCandidatePaths();                                    // every path tried, in order

constexpr int kHdcFrameSamples = 2048;                      // samples per channel in one HDC audio packet (46.4 ms at 44.1 kHz)
constexpr double kHdcRate = 44100.0;

class HdcDecoder {
public:
    HdcDecoder();
    ~HdcDecoder();
    bool ok() const;                                        // the library is there and a decoder state was made
    // One audio packet (without its CRC byte). Returns the stereo frames put into pcm (interleaved L R, 16-bit), or -1 on a decode error.
    int decode(const uint8_t* pkt, size_t n, std::vector<int16_t>& pcm);
    void reset();                                           // close the state: the next packet starts a new one (after a gap)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

class HdcAudio {
public:
    enum Shape { Full, HalfFront, HalfBack };               // a packet that continues in / from the next PDU (not decoded alone, as in nrsc5)
    HdcAudio();
    ~HdcAudio();
    // Layer 2 side (receiver thread). align(): where the play position of the program's stream sits after this PDU header; push(): one packet.
    void align(int program, int stream, unsigned offset);
    void push(int program, int stream, unsigned seq, Shape shape, bool crcOk, const uint8_t* data, size_t n);
    // Moves the play position of every program on to signal time `sec`: one packet per 2048 / 44100 s.
    void advanceTo(double sec);
    void reset();                                           // a new signal: forget everything
    // Controls (any thread)
    void setProgram(int p) { program_ = p; }                // 0..7: the one to hear (HD1 = 0)
    void setVolume(float v) { volume_ = v < 0 ? 0 : v > 1 ? 1 : v; }
    void setMuted(bool m) { muted_ = m; }
    void setSilent(bool s) { silent_ = s; }                 // decode, never open the sound device (tests, the secondary receiver)
    void setTap(std::function<void(const float* stereo48k, size_t frames)> cb);   // tests: what goes to the device (before the app's leveller and volume)
    // Results (any thread)
    struct Stats {
        bool libFound = false;
        std::string libPath, libProblem;
        int program = 0;                                    // being played
        uint64_t decoded = 0, errors = 0, lost = 0, resets = 0;   // packets decoded, decode errors, slots with nothing usable, decoder restarts
        double lastAudioSec = -1e9;                         // signal time of the last decoded packet
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    std::atomic<int> program_{0};
    std::atomic<float> volume_{1.f};
    std::atomic<bool> muted_{false}, silent_{false};
};

}} // namespace dect2::hdr
