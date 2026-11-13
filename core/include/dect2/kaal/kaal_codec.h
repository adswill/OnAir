// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL voice codec (spec v0.1): 16 kHz mono, 20 ms frames of 320 samples, MDCT with the sine window, 16 bands, band energies in 1.5 dB
// steps, adaptive binary arithmetic coding, exactly 320 bits (16 kbit/s) per frame. A port of the reference kaal_codec.py; the output
// follows it to within one LSB of int16. The decoded signal is delayed by kDelay samples against the input; encode() adds one frame so
// the whole input is present: frames = ceil(n / 320) + 1, compare decoded[kDelay .. kDelay + n) with the input.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

class KaalCodec {
public:
    static constexpr size_t kFrame = 320;   // samples per frame
    static constexpr size_t kDelay = 320;   // decoder delay in samples
    static constexpr size_t kBits = 320;    // bits per frame

    // pcm: n samples of 16 kHz mono. Returns (ceil(n / 320) + 1) * 320 bits, one bit (0 or 1) per byte.
    std::vector<uint8_t> encode(const int16_t* pcm, size_t n) const;
    std::vector<uint8_t> encode(const std::vector<int16_t>& pcm) const { return encode(pcm.data(), pcm.size()); }

    // bits: nFrames * 320 bytes of 0 or 1. Returns nFrames * 320 samples. A frame that is not valid (reserved type, an implausible
    // spectrum, a non-binary byte) is concealed from the last good one.
    std::vector<int16_t> decode(const uint8_t* bits, size_t nFrames) const;
    // A flat bit stream; a trailing piece shorter than 320 bytes counts as one lost frame.
    std::vector<int16_t> decode(const std::vector<uint8_t>& bits) const;
    // One vector per frame; a frame whose size is not 320 counts as lost.
    std::vector<int16_t> decode(const std::vector<std::vector<uint8_t>>& frames) const;

    // Frame-by-frame decoding with the same state as decode(): pushing the frames of a stream one at a time gives exactly the samples
    // decode() returns for the whole stream.
    class StreamDecoder {
    public:
        StreamDecoder();
        ~StreamDecoder();
        StreamDecoder(const StreamDecoder&) = delete;
        StreamDecoder& operator=(const StreamDecoder&) = delete;
        void reset();
        // bits: 320 bytes of 0 or 1, or nullptr for a lost frame (concealed). pcm receives kFrame samples (delayed by kDelay as in decode()).
        void push(const uint8_t* bits, int16_t* pcm);
    private:
        struct Impl;
        std::unique_ptr<Impl> p_;
    };
};

}  // namespace dect2
