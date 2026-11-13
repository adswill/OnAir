// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TV sound in DRA (GB/T 22726, DTMB / CMMB in China): the elementary stream of a DRA audio PID goes through the DRA add-on the user installs
// (cdr_dra.h, "libonair_dra"; the same loader as the CDR radio path). OnAir ships no DRA decoder. FFmpeg does not decode DRA, so the player
// takes the PES payloads of the DRA stream from the demuxer (with their presentation times), cuts them into DRA frames, feeds the add-on one frame at
// a time and hands out stereo float PCM at 48 kHz stamped with the stream's own times, which the player then plays like any other audio.
#pragma once
#include "dect2/cdr/cdr_dra.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

// PMT stream_type used for DRA sound in Chinese transport streams (also announced by the registration descriptor "DRA1", which the stream parser
// handles too). From memory of GY/T 220.1; not checked against a real broadcast.
constexpr int kDraStreamType = 0x14;

// The text for a DTMB service whose sound is DRA and cannot be played: one sentence for the player and the service list.
std::string draTvNote(bool addonFound, const std::string& problem);

// The length in bytes of the DRA frame that starts at p (0x7FFF sync, then the header's frame size in 32-bit words); 0 when p does not hold a whole
// valid frame yet. `needMore` (optional) is set when it starts like a frame but more bytes are needed.
size_t draFrameLength(const uint8_t* p, size_t n, bool* needMore = nullptr);

struct DraPcm {
    double pts = -1;                 // presentation time of the first sample, seconds on the stream's clock (-1: none known)
    int rate = 48000;                // always the output rate
    std::vector<float> stereo;       // interleaved L R, rate Hz
};

class TvDraDecoder {
public:
    // `api` null: the user's add-on from cdr::draApi() (looked up again whenever it is missing, so installing it while running works).
    explicit TvDraDecoder(const cdr::DraApi* api = nullptr, int outRate = 48000);
    ~TvDraDecoder();
    TvDraDecoder(const TvDraDecoder&) = delete;
    TvDraDecoder& operator=(const TvDraDecoder&) = delete;

    bool usable();                   // an add-on is there
    void reset();                    // after a seek / track change / lost data: a clean decoder, nothing carried over
    // One PES payload (the elementary stream bytes) with its presentation time in seconds (< 0 when the PES had none). A frame may continue in the
    // next payload. Returns the number of PCM chunks appended to `out`.
    int feed(const uint8_t* es, size_t n, double pts, std::vector<DraPcm>& out);

    uint64_t framesIn = 0, framesBad = 0, pcmFrames = 0, resets = 0;
    int sourceRate = 0, sourceChannels = 0;

private:
    const cdr::DraApi* explicitApi_ = nullptr;
    const cdr::DraApi* api_ = nullptr;
    void* h_ = nullptr;
    int outRate_;
    std::vector<uint8_t> buf_;       // bytes of an unfinished frame / bytes not yet cut into frames
    double nextPts_ = -1;            // the time the next frame is expected at
    double pendPts_ = -1;            // a PES time not used by a frame yet, and where that PES starts in buf_
    size_t pendOff_ = 0;
    int sincePcm_ = 0;
    // linear resampler state
    double rsPos_ = 0;
    float lastL_ = 0, lastR_ = 0;
    bool haveLast_ = false;
    void drop();
    bool open();
    void emit(const int16_t* s, int ch, int frames, int rate, double pts, std::vector<DraPcm>& out);
};

} // namespace dect2
