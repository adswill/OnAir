// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL voice codec v3 (port of kaal_codec_v3.py): 16 kHz mono, 20 ms frames of 320 samples, MDCT with the Vorbis power-sine window,
// 26 bands, band energies in 4.5 dB steps, context-modelled adaptive binary arithmetic coding with trained start probabilities,
// high-band noise fill. Four profiles: 12, 16, 24 and 32 kbit/s = 240, 320, 480 and 640 bits per frame. Every frame starts with a
// 2-bit profile id (16 -> 0, 24 -> 1, 32 -> 2, 12 -> 3), then a 2-bit type and a 5-bit step offset, so a receiver can tell the profile
// from the frame itself. The frames are independent (no state between frames except the MDCT overlap).
// The bit stream is the one of the Python prototype; the decoded samples follow it to within one LSB.
// Decoded output is delayed by kDelay samples against the input; encode() adds one frame: frames = ceil(n / 320) + 1.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

class KaalCodec3 {
public:
    static constexpr size_t kFrame = 320;   // samples per frame
    static constexpr size_t kDelay = 320;   // decoder delay in samples

    static bool validBitrate(int kbit);                  // 12, 16, 24 or 32
    static size_t frameBits(int kbit);                   // kbit * 20, 0 for an invalid bitrate
    // The bitrate (kbit/s) named by the first two bits of a frame, 0 for none (cannot happen: all four ids are used).
    static int bitrateOfHeader(const uint8_t* bits);

    explicit KaalCodec3(int kbit = 16) : kbit_(validBitrate(kbit) ? kbit : 16) {}
    int bitrate() const { return kbit_; }
    size_t frameBits() const { return frameBits(kbit_); }

    // pcm: n samples of 16 kHz mono. Returns (ceil(n / 320) + 1) * frameBits() bits, one bit (0 or 1) per byte.
    std::vector<uint8_t> encode(const int16_t* pcm, size_t n) const;
    std::vector<uint8_t> encode(const std::vector<int16_t>& pcm) const { return encode(pcm.data(), pcm.size()); }

    // bits: nFrames * frameBits() bytes of 0 or 1. A frame of another profile, with a reserved type, an implausible spectrum or a
    // non-binary byte is concealed from the last good one.
    std::vector<int16_t> decode(const uint8_t* bits, size_t nFrames) const;
    // One vector per frame; a frame whose size is not frameBits() counts as lost.
    std::vector<int16_t> decode(const std::vector<std::vector<uint8_t>>& frames) const;

    // Frame-by-frame decoding with the state of decode(). kbit 0 accepts any profile (the profile is read from the frame header and
    // must agree with the frame length); otherwise only that profile.
    class StreamDecoder {
    public:
        explicit StreamDecoder(int kbit = 0);
        ~StreamDecoder();
        StreamDecoder(const StreamDecoder&) = delete;
        StreamDecoder& operator=(const StreamDecoder&) = delete;
        void reset();
        // bits: nbits bytes of 0 or 1 (240, 320, 480 or 640), or nullptr for a lost frame (concealed). pcm receives kFrame samples
        // (delayed by kDelay as in decode()). Returns the profile (kbit/s) of the frame when it was decoded, 0 when it was concealed.
        int push(const uint8_t* bits, size_t nbits, int16_t* pcm);
    private:
        struct Impl;
        std::unique_ptr<Impl> p_;
    };

private:
    int kbit_;
};

}  // namespace dect2
