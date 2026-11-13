// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal workbench: a recording opened for analysis. A Source gives random access to complex samples of any size:
//  - raw IQ files (.cs8 .cu8 .cf32 .ci16 .cu16, SigMF, IQ WAV) are read from disk on demand and never held in memory,
//  - audio (WAV, MP3, FLAC, OGG, M4A ... decoded with the FFmpeg libraries OnAir links) is decoded once into memory; it is a real signal
//    (imaginary part 0) or, for a stereo file, I (left) and Q (right) when treat-stereo-as-IQ is on,
//  - synthetic signals from memory (tests, the generator).
// read() is thread safe.
#pragma once
#include "dect2/source/source.h"
#include "dect2/common/ring.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace wb {

struct LoadOptions {
    int format = -1;               // FileFormat for raw files, -1: by the extension / the SigMF meta
    bool audio = false;            // force the FFmpeg decoder
    double sampleRate = 0;         // raw files that do not say (and a name such as x_2Msps.cs8 does not either)
    double centerHz = 0;           // RF frequency of the samples' centre, for the frequency read-outs
    bool bigEndian = false;
    bool stereoIq = false;         // audio with two channels: left = I, right = Q
};

class Source {
public:
    virtual ~Source() {}
    virtual uint64_t length() const = 0;              // complex samples
    virtual double rate() const = 0;
    virtual size_t read(uint64_t start, size_t n, cf32* out) const = 0;   // samples copied (fewer at the end)
    double centerHz() const { return centerHz_; }
    void setCenterHz(double hz) { centerHz_ = hz; }
    bool real() const { return real_; }                // the signal is real (audio): only 0 .. rate / 2 is meaningful
    const std::string& name() const { return name_; }
    const std::string& description() const { return desc_; }
    double seconds() const { return rate() > 0 ? (double)length() / rate() : 0; }
    int channels() const { return channels_; }         // audio: the decoded channel count (1 when not audio)
    virtual void setStereoIq(bool) {}
    virtual bool stereoIq() const { return false; }
    virtual bool isAudio() const { return false; }
    virtual const std::string& path() const { return path_; }

    // Opens a file; null with err set when it cannot be read
    static std::shared_ptr<Source> open(const std::string& path, const LoadOptions& o, std::string& err);
    // A recording held in memory (real: the imaginary part is ignored by the displays)
    static std::shared_ptr<Source> fromSamples(std::vector<cf32> x, double rate, bool real = false, double centerHz = 0, const std::string& name = "memory");

protected:
    double centerHz_ = 0;
    bool real_ = false;
    int channels_ = 1;
    std::string name_, desc_, path_;
};

// Extensions: true for the ones decoded as audio
bool isAudioExtension(const std::string& path);
// "treat as raw IQ": the extension is one of the IQ ones
bool isRawIqExtension(const std::string& path);

} // namespace wb
} // namespace dect2
