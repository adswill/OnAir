// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT image codec (spec: KAIT-SPEC-v0.1): colour transform (BT.601 YCbCr, 4:2:0), CDF 9/7 wavelet (lifting, orthonormal scaling), embedded bitplane coding
// with a binary range coder and neighbourhood contexts. The stream is embedded: any prefix of it decodes to a picture, more bytes give a better one.
// Own code, no external codec and nothing patented (CDF 9/7 is the wavelet of JPEG 2000, published 1992 and free of royalties).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2::kait {

struct Image {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> rgb;   // w * h * 3, interleaved
};

// What the decoder needs besides the bytes (it travels in the burst header).
struct StreamInfo {
    int w = 0;
    int h = 0;
    int levels = 5;             // luma wavelet levels (chroma has levels - 1)
    int topPlane = -1;          // highest coded bitplane, -1 = nothing coded
    uint32_t decisions = 0;     // number of binary decisions in the stream
};

struct Encoded {
    StreamInfo info;
    std::vector<uint8_t> bytes;   // at most maxBytes
};

// Encodes until maxBytes are used or the picture is exact (all planes), whichever comes first.
Encoded encodeImage(const Image& img, size_t maxBytes, int levels = 5);
// Decodes whatever the n bytes hold (a prefix of a stream gives a coarser picture). Never fails for a sane info; returns an empty image otherwise.
Image decodeImage(const uint8_t* bytes, size_t n, const StreamInfo& info);

// A synthetic picture (gradients, text-like strokes, discs, a noise band) for tests and sample files.
Image syntheticPicture(int w = 1920, int h = 1080);

double psnrRgb(const Image& a, const Image& b);   // over all three channels, 255 peak; 99 when equal

}  // namespace dect2::kait
