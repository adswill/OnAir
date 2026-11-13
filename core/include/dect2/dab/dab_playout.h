// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB / DAB+ playout (docs/DAB-AUDIT.md): what goes to the audio output between the decoder and the sound card.
//  - concealment: a lost access unit or MP2 frame is replaced by the last audio, repeated with a crossfade at every splice and faded out after
//    kHoldFrames, so a short loss is not heard and a long one goes quiet without a click; audio that comes back is spliced in the same way
//    (faded in when the concealment had gone quiet);
//  - rate control: the output buffer is kept near kTarget by resampling by up to kMaxRatio (the sound card's clock is never the transmitter's);
//  - gap filling: when no audio comes at all (sync lost), the buffer is kept from running dry, so the output never stops and never re-buffers.
#pragma once
#include <cstdint>
#include <vector>

namespace dect2 {

class DabPlayout {
public:
    static constexpr int kTarget = 19200;      // 400 ms queued at 48 kHz: above the spacing of the audio bursts (120 ms superframes in 96 ms frames)
    static constexpr int kLow = 4800;          // with less than 100 ms queued and nothing arriving, the gap is filled
    static constexpr int kXfade = 96;          // 2 ms: the overlap of every splice
    static constexpr int kPeriod = 1920;       // 40 ms: the length of the audio repeated
    static constexpr int kHoldFrames = 1920;   // full level for 40 ms of concealment, then
    static constexpr int kFadeFrames = 5760;   // a fade to silence over 120 ms
    static constexpr double kMaxRatio = 0.002; // rate control: at most 0.2 % faster or slower (3.5 cents, not heard)

    DabPlayout();
    void reset();                              // a new service: no history, no rate correction
    // n frames (48 kHz stereo, interleaved) decoded: appended to out, after the splice from a concealment
    void good(const float* st, int n, std::vector<float>& out);
    // n frames lost: concealment appended to out
    void lost(int n, std::vector<float>& out);
    // the output now holds `fill` frames: updates the rate correction
    void setFill(int fill);
    double ratio() const { return ratio_; }
    bool concealing() const { return conceal_; }
    bool quiet() const;                        // concealing, and faded out to silence
    uint64_t silentFrames() const { return silent_; }      // frames put out as silence by the concealment
    uint64_t concealFrames() const { return concealed_; }  // frames put out by the concealment (repeated or silent)

private:
    void put(const float* st, int n, std::vector<float>& out);   // the rate correction, then out
    void remember(const float* st, int n);
    float concealSample(int ch);
    std::vector<float> hist_;                  // the last frames given to good() (stereo), the newest kXfade of them not yet put out
    std::vector<float> tmp_;
    bool haveTail_ = false;
    bool conceal_ = false;
    int64_t ct_ = 0;                           // frames concealed in this run
    int segLen_ = 0, segPos_ = 0;
    std::vector<float> seg_;                   // the repeated stretch: kXfade frames before it, then segLen_ frames
    double ratio_ = 1.0, fillAvg_ = -1;
    double frac_ = 0;                          // resampler position between in_[1] and in_[2]
    float in_[4][2] = {};                      // the last four input frames of the resampler
    bool primed_ = false;
    uint64_t silent_ = 0, concealed_ = 0;
};

} // namespace dect2
