// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Analog TV video enhancer: turns the decoded pictures (AtvFrame RGBA) into a cleaner, square-pixel 720p / 1080p video, live or from a
// recording. CPU only, no model, no dependency, no UI: the picture goes in, the picture comes out; the previous pictures are kept inside.
//   Light: motion-adaptive temporal noise reduction + scaling (bilinear). Cheap.
//   Full : + comb repair in moving areas (motion-adaptive deinterlace of a woven picture), edge-preserving spatial denoise,
//          chroma bleed fix (chroma pulled to the luma edges), Lanczos-3 luma scaling, sharpening with coring and overshoot control.
// Call process() from one thread at a time (the UI runs it on a worker so the decoder is never held up). Same input sequence and
// settings give the same bytes. Per-frame cost and the design: docs/ATV-ENHANCE.md.
#pragma once
#include <cstdint>
#include <vector>

namespace dect2 {

enum class AtvEnhLevel { Off = 0, Light = 1, Full = 2 };

struct AtvEnhParams {
    AtvEnhLevel level = AtvEnhLevel::Light;
    int outHeight = 0;           // 0 as received (same pixel grid), else 720 or 1080 (any height works); width follows displayAspect, square pixels
    double displayAspect = 0;    // width / height the picture is shown with (4/3, 16/9 ...); 0 = the frame's own width / height
    // advanced: 1 = the level's default, 0 = that step off, up to 2
    float temporal = 1, spatial = 1, chroma = 1, sharpen = 1;
};

class AtvEnhancer {
public:
    // One picture in, one picture out (RGBA 8 bit, alpha 255). Returns false (out untouched) for an empty input or Off.
    bool process(const uint8_t* rgba, int w, int h, const AtvEnhParams& p, std::vector<uint8_t>& out, int& ow, int& oh);
    void reset();                       // forget the previous pictures (a channel change, a new file)
    float noiseSigma() const { return sigma_; }   // the noise the last picture was judged to have, in 8 bit levels (luma)
    // the output size for an input and settings
    static void outputSize(int w, int h, const AtvEnhParams& p, int& ow, int& oh);

private:
    int pw_ = 0, ph_ = 0;
    std::vector<uint8_t> pY_, pCb_, pCr_;
    float sigma_ = 3.f;
    bool have_ = false;
};

} // namespace dect2
